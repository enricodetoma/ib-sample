#include "trading.h"
#include <TwsSocketClientErrors.h>
#include <EDecoder.h>
#include <boost/log/trivial.hpp>
#include <boost/bind.hpp>
#include <boost/format.hpp>

const int MAX_TIME_BETWEEN_RECONNECTS = 10; // seconds
const int TIME_BETWEEN_PINGS = 30; // seconds
const int PING_DEADLINE = 3; // seconds
const int CONNECT_DEADLINE = 10; // seconds

const int TIME_BETWEEN_WRITE_CHECK = 100; // milliseconds

const char	*IB_DEFAULT_TICK_TYPES = "100,101,104,105,106,107,165,221,225,233,258,293,294,295,318";

using boost::asio::ip::tcp;

Trading::Trading(const char* ib_host, int ib_port, int ib_client_id): EClientSocket(this, nullptr)
    , _work(_io_service)
    , _socket(new tcp::socket(_io_service))
    , _async_send_active(false)
    , _wait_for_rate_limiter(false)
    , _reconnect_timer(_io_service)
    , _ping_timer(_io_service)
    , _ping_deadline(_io_service)
    , _rate_timer(_io_service)
    , _has_error(false)
    , _rate_limiter(45, 1000) // Max 45 messages per second to avoid being blocked by IB
    , _priority_message(false)
	, _connection_state(CLIENT_CS_DISCONNECTED)
    , _time_between_reconnects(0)
	, _receiving(false)
	, _market_data_updated(false)
	, _order_id(-1)
	, contract_details_requested(0)
	, contract_details_received(0)
{
	// use local machine if no host passed in
	if (!(ib_host && *ib_host))
	{
		ib_host = "127.0.0.1";
	}

	setHost(ib_host);
	setPort(ib_port);
	setClientId(ib_client_id);
}

void Trading::run()
{
	setupTickers();

	startReceiving();

    // https://stackoverflow.com/questions/17156541/why-do-we-need-to-use-boostasioio-servicework
	_io_service.run();
}

void Trading::stop()
{
	stopReceiving();
	_io_service.stop();
}

void Trading::startReceiving()
{
    if (!_receiving)
	{
		_receiving = true;

		// Make first connection
		reconnectHandler(boost::system::error_code());
	}
}

void Trading::stopReceiving()
{
   	_receiving = false;
	_reconnect_timer.cancel();
	_ping_timer.cancel();
	_ping_deadline.cancel();
	_rate_timer.cancel();
	eDisconnect();
}

void Trading::readStart(void)
{
	if (_receiving && _connection_state != CLIENT_CS_DISCONNECTED)
	{
		boost::asio::streambuf::mutable_buffers_type buf = _inbox.prepare(8192);

		// Start an asynchronous read and call readHandler when it completes or fails
		_socket->async_read_some(buf,
		                         boost::bind(&Trading::readHandler,
		                                     this,
		                                     boost::asio::placeholders::error,
		                                     boost::asio::placeholders::bytes_transferred));
	}
}

void Trading::readHandler(const boost::system::error_code& error, size_t bytes_transferred)
{
	if (_receiving)
	{
		if ((!error) && (bytes_transferred > 0))
		{
			// Mark to buffer how much was actually read
			_inbox.commit(bytes_transferred);

			checkMessages();

			// Wait for next data
			readStart();
		}
		else
		{
			eDisconnect();
		}
	}
}

void Trading::reconnectHandler(const boost::system::error_code& error)
{
	if (error != boost::asio::error::operation_aborted && _receiving &&
		(_connection_state == CLIENT_CS_DISCONNECTED || _connection_state == CLIENT_CS_WAITING_FOR_CONNECT))
	{
		eConnect(this->host().c_str(), this->port(), this->clientId());
	}
}

bool Trading::eConnect(const char* host, unsigned int port, int clientId, bool extraAuth)
{
	// use local machine if no host passed in
	if (!(host && *host))
	{
		host = "127.0.0.1";
	}

	// initialize host and port
	setHost(host);
	setPort(port);

	return eConnectImpl(clientId, extraAuth);
}

bool Trading::eConnectImpl(int clientId, bool extraAuth)
{
	if (_receiving && _connection_state != CLIENT_CS_CONNECTING && _connection_state != CLIENT_CS_CONNECTED)
	{
		boost::system::error_code error;

		_connection_state = CLIENT_CS_CONNECTING;

		// reset errno
		//errno = 0;

		// already connected?
		//if(_socket->is_open()) {
		//	errno = EISCONN;
		//	getWrapper()->error( NO_VALID_ID, ALREADY_CONNECTED.code(), ALREADY_CONNECTED.msg());
		//	return false;
		//}

		eDisconnectBase();
		clearWriteBuffer();
		clearReadBuffer();

		// allocate new socket
		_socket.reset(new tcp::socket(_io_service));

		// resolve address
		tcp::resolver resolver(_io_service);
		char s_port[16];
		sprintf(s_port, "%u", (unsigned int)port());
		tcp::resolver::query query(host(), s_port);
		tcp::resolver::iterator endpoint_iterator = resolver.resolve(query, error);
		if (error)
		{
			BOOST_LOG_TRIVIAL(error) << "could not resolve host " << host() << ": " << error.message();
			eDisconnect();
			return false;
		}

		// connect to server
		boost::asio::connect(*_socket, endpoint_iterator, error);
		if (error)
		{
			BOOST_LOG_TRIVIAL(error) << "could not connect: " << error.message();
			eDisconnect();
			return false;
		}

		_socket->set_option(tcp::no_delay(true), error);
		if (error)
			BOOST_LOG_TRIVIAL(error) << "could not set no_delay option on socket: " << error.message();

		// set client id
		setClientId(clientId);
		setExtraAuth(extraAuth);

		sendConnectRequest();

		// start async read
		readStart();

		_ping_deadline.expires_from_now(boost::posix_time::seconds(CONNECT_DEADLINE));
		_ping_deadline.async_wait(boost::bind(&Trading::pingDeadlineConnect, this, boost::asio::placeholders::error));

		// http://www.boost.org/doc/libs/1_52_0/doc/html/boost_asio/example/timeouts/blocking_tcp_client.cpp
		do
			_io_service.run_one();
		while ((!EClient::isConnected() || EClient::serverVersion() == 0) && _ping_deadline.expires_at() > boost::asio::deadline_timer::traits_type::now());

		_ping_deadline.cancel();

		if (!EClient::isConnected() || EClient::serverVersion() == 0)
		{
			_connection_state = CLIENT_CS_DISCONNECTED;
			getWrapper()->error(NO_VALID_ID, CONNECT_FAIL.code(), CONNECT_FAIL.msg());
			eDisconnect();
			return false;
		}

		_connection_state = CLIENT_CS_CONNECTED;

		_time_between_reconnects = 0;

		reqAllContractDetails();

		// Avvia il timer per il ping periodico
		_ping_timer.expires_from_now(boost::posix_time::seconds(TIME_BETWEEN_PINGS));
		_ping_timer.async_wait(boost::bind(&Trading::pingHandler, this, boost::asio::placeholders::error));

		// successfully connected
		return true;
	}
	return false;
}

void Trading::redirect(const char* host, int port)
{
	// use local machine if no host passed in
	if (!(host && *host))
	{
		host = "127.0.0.1";
	}

	// handle redirect
	if ((host != this->host() || (port > 0 && port != this->port())))
	{
		setHost(host);
		setPort(port);

		eDisconnect(false);
		eConnectImpl(clientId(), extraAuth());
	}
}

void Trading::eDisconnect(bool resetState)
{
	if (_socket->is_open())
	{
		boost::system::error_code error;
		_socket->shutdown(tcp::socket::shutdown_both, error);
		_socket->close(error);
	}

	_connection_state = CLIENT_CS_DISCONNECTED;
	_ping_timer.cancel();
	_rate_timer.cancel();
    if (resetState) {
	    eDisconnectBase();
    }
	clearWriteBuffer();
	clearReadBuffer();
	BOOST_LOG_TRIVIAL(info) << "disconnected";
	waitForReconnect();
}

void Trading::clearWriteBuffer()
{
	_outbox.clear();
	_async_send_active = false;
	_wait_for_rate_limiter = false;
}

void Trading::writeStart()
{
	if (_receiving && _connection_state != CLIENT_CS_DISCONNECTED)
	{
		if (_async_send_active || _wait_for_rate_limiter || _outbox.empty())
		{
			return;
		}

		_current_message = _outbox.front();
		_outbox.pop_front();
		_async_send_active = true;
		_socket->async_send(
			boost::asio::buffer(_current_message.c_str(), _current_message.size()),
			boost::bind(
				&Trading::writeHandler,
				this,
				boost::asio::placeholders::error,
				boost::asio::placeholders::bytes_transferred
			)
		);

		unsigned long wait_milliseconds = _rate_limiter.add_message();
		if (wait_milliseconds > 0)
		{
			// Start timer waiting for rate limiter
			_wait_for_rate_limiter = true;
			_rate_timer.expires_from_now(boost::posix_time::milliseconds(wait_milliseconds));
			_rate_timer.async_wait(boost::bind(&Trading::rateHandler, this, boost::asio::placeholders::error));
		}
	}
}

void Trading::clearReadBuffer()
{
	_inbox.consume(_inbox.size());
}

void Trading::writeHandler(
	const boost::system::error_code& error,
	const size_t bytesTransferred)
{
	_async_send_active = false;
	if (_receiving)
	{
		if (error)
		{
			BOOST_LOG_TRIVIAL(error) << "could not write: " << boost::system::system_error(error).what();
			return;
		}

		this->writeStart();
	}
}

int Trading::receive(char* buf, size_t sz)
{
	if (_receiving)
	{
		size_t rsz = std::min(_inbox.size(), sz);
		if (rsz > 0)
		{
			const char* beginPtr = boost::asio::buffer_cast<const char*>(_inbox.data());
			memcpy(buf, beginPtr, rsz);
			_inbox.consume(rsz);
		}
		return rsz;
	}
	return -1;
}

int Trading::bufferedSend(const std::string& message)
{
	if (_receiving && _connection_state != CLIENT_CS_DISCONNECTED)
	{
		if (_priority_message)
		{
			_priority_message = false;
			_outbox.push_front(message);
		}
		else
			_outbox.push_back(message);

		this->writeStart();

		return message.size();
	}

	return -1;
}

bool Trading::checkMessages()
{
	if (_receiving)
	{
		// Use some ugly parsing to extract whole lines.
		const char* beginPtr = boost::asio::buffer_cast<const char*>(_inbox.data());
		const char* ptr = beginPtr;
		const char* endPtr = ptr + _inbox.size();

		// Message dump to disk (for debugging purposes)
		// FILE *f = fopen("ib_dump.log", "a");
		// fwrite(ptr, 1, _inbox.size(), f);
		// fclose(f);

		_market_data_updated = false;
		try
		{
			while (processMsg(ptr, endPtr) > 0)
			{
				if ((ptr - beginPtr) >= _inbox.size())
					break;
			}
		}
		catch (...)
		{
			_inbox.consume(ptr - beginPtr);
			if (_market_data_updated)
				onMarketDataUpdated();
			throw;
		}

		_inbox.consume(ptr - beginPtr);
		if (_market_data_updated)
			onMarketDataUpdated();
		return true;
	}
	return false;
}

int Trading::processMsgImpl(const char*& beginPtr, const char* endPtr)
{
	EDecoder decoder(EClient::serverVersion(), this, this);

	return decoder.parseAndProcessMsg(beginPtr, endPtr);
}

int Trading::processMsg(const char*& beginPtr, const char* endPtr)
{
	if (!m_useV100Plus)
	{
		return processMsgImpl(beginPtr, endPtr);
	}
	return processOnePrefixedMsg(beginPtr, endPtr);
}

int Trading::processOnePrefixedMsg(const char*& beginPtr, const char* endPtr)
{
	if (beginPtr + HEADER_LEN >= endPtr)
		return 0;

	assert(sizeof(unsigned) == HEADER_LEN);

	unsigned netLen = 0;
	memcpy(&netLen, beginPtr, HEADER_LEN);

	const unsigned msgLen = ntohl(netLen);

	// shold never happen, but still....
	if (!msgLen)
	{
		beginPtr += HEADER_LEN;
		return HEADER_LEN;
	}

	// enforce max msg len limit
	if (msgLen > MAX_MSG_LEN)
	{
		error(NO_VALID_ID, BAD_LENGTH.code(), BAD_LENGTH.msg());
		eDisconnect();
		connectionClosed();
		return 0;
	}

	const char* msgStart = beginPtr + HEADER_LEN;
	const char* msgEnd = msgStart + msgLen;

	// handle incomplete messages
	if (msgEnd > endPtr)
	{
		return 0;
	}

	int decoded = processMsgImpl(msgStart, msgEnd);
	if (decoded <= 0)
	{
		// this would mean something went real wrong
		// and message was incomplete from decoder POV
		error(NO_VALID_ID, BAD_MESSAGE.code(), BAD_MESSAGE.msg());
		eDisconnect();
		connectionClosed();
		return 0;
	}

	int consumed = msgEnd - beginPtr;
	beginPtr = msgEnd;
	return consumed;
}

void Trading::waitForReconnect()
{
	if (_receiving && _connection_state == CLIENT_CS_DISCONNECTED)
	{
		if (_time_between_reconnects < MAX_TIME_BETWEEN_RECONNECTS)
			_time_between_reconnects++;
		_connection_state = CLIENT_CS_WAITING_FOR_CONNECT;
		BOOST_LOG_TRIVIAL(info) << "sleeping " << _time_between_reconnects <<
			" seconds before next attempt";
		_reconnect_timer.expires_from_now(boost::posix_time::seconds(_time_between_reconnects));
		_reconnect_timer.async_wait(boost::bind(&Trading::reconnectHandler, this, boost::asio::placeholders::error));
	}
}

void Trading::reqAllContractDetails()
{
	if (contract_details_received == contract_details.size())
	{
		// Contract details were already received. Just resubscribe if necessary
		restartSubscriptions();
		return;
	}

	for (size_t i = 0; i < contract_details.size(); i++)
	{
		ContractDetails& details = contract_details[i];
		reqContractDetails(i + clientIdTickerOffset(), details.contract);
	}
}

void Trading::pingHandler(const boost::system::error_code& error)
{
	// Check for operation_aborted because retriggering _ping_timer with expires_from_now
	// causes pingHandler being called with error == operation_aborted
	// See http://stackoverflow.com/questions/10165352/how-do-you-discriminate-a-cancelled-from-a-retriggered-boost-deadline-timer
	if (error != boost::asio::error::operation_aborted && _receiving)
	{
		if (_connection_state == CLIENT_CS_CONNECTED)
		{
			_priority_message = true;
			reqCurrentTime();
			_priority_message = false;
			_ping_deadline.expires_from_now(boost::posix_time::seconds(PING_DEADLINE));
			_ping_deadline.async_wait(boost::bind(&Trading::pingDeadline, this, boost::asio::placeholders::error));
		}
		_ping_timer.expires_from_now(boost::posix_time::seconds(TIME_BETWEEN_PINGS));
		_ping_timer.async_wait(boost::bind(&Trading::pingHandler, this, boost::asio::placeholders::error));
	}
}

void Trading::rateHandler(const boost::system::error_code& error)
{
	// Check for operation_aborted because retriggering _rate_timer with expires_from_now
	// causes pingHandler being called with error == operation_aborted
	// See http://stackoverflow.com/questions/10165352/how-do-you-discriminate-a-cancelled-from-a-retriggered-boost-deadline-timer
	_wait_for_rate_limiter = false;
	if (error != boost::asio::error::operation_aborted && _receiving && _connection_state == CLIENT_CS_CONNECTED)
	{
		writeStart();
	}
}

void Trading::pingDeadline(const boost::system::error_code& error)
{
	// Check for operation_aborted
	if (error != boost::asio::error::operation_aborted && _receiving)
		eDisconnect();
}

void Trading::pingDeadlineConnect(const boost::system::error_code& error)
{
	// Non faccio niente qui perche' tutti i controlli li faccio nella eConnect
}

//////////////////////////////////////////////////////////////////
// methods

// bool Trading::sendOrder(OpenOrder& open_order)
// {
// 	if (_order_id < 0 || open_order.instrument == nullptr)
// 		return false;

// 	Order order;
// 	if (open_order.action == LuaEnums::BUY)
// 		order.action = ACTION_BUY;
// 	else if (open_order.action == LuaEnums::SELL)
// 		order.action = ACTION_SELL;
// 	else return false;
// 	order.totalQuantity = std::lround(open_order.quantity);
// 	if (open_order.type == LuaEnums::LIMIT)
// 		order.orderType = "LMT";
// 	else if (open_order.type == LuaEnums::MARKET)
// 		order.orderType = "MKT";
// 	else
// 	{
// 		BOOST_LOG_TRIVIAL(error) << "invalid order type: " << open_order.type;
// 		return false;
// 	}
// 	order.lmtPrice = open_order.limit_price;

// 	// Assegna qui l'id e poi lo incrementa per il prossimo ordine
// 	open_order.broker_id = _order_id;
// 	BOOST_LOG_TRIVIAL(info) << "placing order id " << open_order.id << ", broker id " << open_order.
// 		broker_id << ": " << order.action << " " <<
// 		std::lround(open_order.quantity) << " " << open_order.instrument->name << " at " << open_order.limit_price;
// 	_has_error = false;
// 	placeOrder(_order_id, open_order.instrument->details.contract, order);
// 	_order_id++;
// 	return !_has_error;
// }

// bool Trading::modifyOrder(const OpenOrder& open_order, double quantity, double limit_price)
// {
// 	if (open_order.broker_id < 0 || open_order.instrument == nullptr)
// 		return false;

// 	Order order;
// 	if (open_order.action == LuaEnums::BUY)
// 		order.action = ACTION_BUY;
// 	else if (open_order.action == LuaEnums::SELL)
// 		order.action = ACTION_SELL;
// 	else return false;
// 	// Nella modifyOrder uso le nuove quantity e limit_price
// 	order.totalQuantity = std::lround(quantity);
// 	if (open_order.type == LuaEnums::LIMIT)
// 		order.orderType = "LMT";
// 	else if (open_order.type == LuaEnums::MARKET)
// 		order.orderType = "MKT";
// 	else
// 	{
// 		BOOST_LOG_TRIVIAL(error) << "invalid order type: " << open_order.type;
// 		return false;
// 	}
// 	order.lmtPrice = limit_price;

// 	BOOST_LOG_TRIVIAL(info) << "modifying order id " << open_order.id << ", broker id " << open_order.
// 		broker_id << ": " << order.action << " " <<
// 		std::lround(quantity) << " " << open_order.instrument->details.contract.symbol << " at " << limit_price;
// 	_has_error = false;
// 	placeOrder(open_order.broker_id, open_order.instrument->details.contract, order);
// 	return !_has_error;
// }

// bool Trading::cancelOrder(const OpenOrder& open_order)
// {
// 	if (open_order.broker_id <= 0)
// 		return false;

// 	BOOST_LOG_TRIVIAL(info) << "cancelling order id " << open_order.id << ", broker id " << open_order.
// 		broker_id;

// 	_has_error = false;
// 	EClient::cancelOrder((OrderId)open_order.broker_id);
// 	return !_has_error;
// }

///////////////////////////////////////////////////////////////////
// events
void Trading::orderStatus(OrderId orderId, const std::string& status, double filled,
                           double remaining, double avgFillPrice, int permId, int parentId,
                           double lastFillPrice, int clientId, const std::string& whyHeld, double mktCapPrice)
{
}

void Trading::nextValidId(OrderId orderId)
{
	_order_id = orderId;
}

void Trading::currentTime(long time)
{
	_ping_deadline.cancel();

	BOOST_LOG_TRIVIAL(debug) << "the current date/time is: " << time;
}

void Trading::error(int id, int errorCode, const std::string& errorString)
{
	// Warning: Approaching max rate of 50 messages per second
	if (id == -1 && errorCode == 0)
	{
		boost::format fmt = boost::format("error id=%1%, errorCode=%2%, msg=%3%") % id % errorCode %
			errorString;
		BOOST_LOG_TRIVIAL(warning) << fmt;
		return;
	}

	// Ignore error code 300 ("Can't find EId with ticker Id") which is given by cancelMktData
	// Ignore error code 202 ("Order cancelled - Reason:") which is given when canceling orders
	if ((id != -1 || errorCode < 2103 || errorCode > 2108) && errorCode != 300 && errorCode != 202)
	{
		boost::format fmt = boost::format("error id=%1%, errorCode=%2%, msg=%3%") % id % errorCode %
			errorString;
		BOOST_LOG_TRIVIAL(error) << fmt;
		_has_error = true;
	}

	// No security definition has been found for the request
	if (errorCode == 200)
	{
		_has_error = false;
		contractDetails(id, ContractDetails());
	}

	if (id == -1 && (errorCode == 1101 || errorCode == 1300)) // if "Connectivity between IB and TWS has been lost"
	{
		// See http://groups.yahoo.com/neo/groups/TWSAPI/conversations/topics/29797
		// With error codes 1101 and 1300 subscriptions are lost, so it's better to disconnect and reconnect
		// to restart all subscriptions upon reconnection
		eDisconnect();
		_has_error = true;
	}
}

void Trading::tickPrice(TickerId tickerId, TickType field, double price, const TickAttrib& attrib)
{
	tickerId -= clientIdTickerOffset();
	if (tickerId >= 0 && tickerId < bid_price.size())
	{
		switch (field)
		{
		case BID:
			if (bid_price[tickerId] != price)
			{
				bid_price[tickerId] = price;
				_market_data_updated = true;
			}
			break;
		case ASK:
			if (ask_price[tickerId] != price)
			{
				ask_price[tickerId] = price;
				_market_data_updated = true;
			}
			break;
		case LAST:
			if (last_price[tickerId] != price)
			{
				last_price[tickerId] = price;
				_market_data_updated = true;
			}
			break;
		}
	}
}

void Trading::tickSize(TickerId tickerId, TickType field, int size)
{
	tickerId -= clientIdTickerOffset();
	if (tickerId >= 0 && tickerId < bid_size.size())
	{
		switch (field)
		{
		case BID_SIZE:
			if (bid_size[tickerId] != size)
			{
				bid_size[tickerId] = size;
				_market_data_updated = true;
			}
			break;
		case ASK_SIZE:
			if (ask_size[tickerId] != size)
			{
				ask_size[tickerId] = size;
				_market_data_updated = true;
			}
			break;
		case VOLUME:
			if (volume[tickerId] != size)
			{
				ask_size[tickerId] = size;
				_market_data_updated = true;
			}
			break;
		}
	}
}

void Trading::tickGeneric(TickerId tickerId, TickType tickType, double value)
{
}

void Trading::openOrder(OrderId orderId, const Contract& contract, const Order& order, const OrderState& ostate)
{
}

void Trading::updateAccountValue(const std::string& key, const std::string& val,
                                  const std::string& currency, const std::string& accountName)
{
}

void Trading::restartSubscriptions()
{
	// Market data subscriptions
	for (size_t i = 0; i < contract_details.size(); i++)
	{
		ContractDetails& details = contract_details[i];
		cancelMktData(i + clientIdTickerOffset());
		reqMktData(i + clientIdTickerOffset(), details.contract, IB_DEFAULT_TICK_TYPES, false, false, TagValueListSPtr());
	}

	// Open orders
	reqOpenOrders();

	// Portfolio
	reqPositions();

	// Account updates
	//reqAccountUpdates(true, std::string());
}

void Trading::contractDetails(int reqId, const ContractDetails& contractDetails)
{
	reqId -= clientIdTickerOffset();
	if (reqId >= 0 && reqId < contract_details.size())
	{
		ContractDetails& details = contract_details[reqId];
		details = contractDetails;
		// Fix for bug in API 9.73.06: https://groups.io/g/insync/topic/7790846?p=,,,20,0,0,0::recentpostdate%2Fsticky,,,20,2,0,7790846
		// If lastTradeDateOrContractMonth contains the hour, it gets stripped
		details.contract.lastTradeDateOrContractMonth = details.contract.lastTradeDateOrContractMonth.substr(0, 8);
		contract_details_received++;
		if (contract_details_received == contract_details.size())
		{
			BOOST_LOG_TRIVIAL(info) << "contract details completed";
			restartSubscriptions();
		}
	}
}

void Trading::position(const std::string& account, const Contract& contract, double position, double avgCost)
{
	BOOST_LOG_TRIVIAL(info) << contract.symbol << " position: " << position;
}

void Trading::positionEnd()
{
	BOOST_LOG_TRIVIAL(info) << "positions completed";
}

void Trading::setNumberOfTickers(int tickers)
{
	bid_size.resize(tickers);
	bid_price.resize(tickers);
	ask_size.resize(tickers);
	ask_price.resize(tickers);
	last_price.resize(tickers);
	volume.resize(tickers);
	contract_details.resize(tickers);
	contract_details_requested = 0;
	contract_details_received = 0;
}

void Trading::subscribeTicker(const Contract &contract)
{
	if (contract_details_requested < contract_details.size())
	{
		contract_details[contract_details_requested].contract = contract;
		contract_details_requested++;
	}
}
