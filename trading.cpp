#include "trading.h"
#include <boost/log/trivial.hpp>

#define IB_HOST         "127.0.0.1"
#define IB_PORT         4002
#define IB_CLIENT_ID    1

Trading::Trading(): EClientSocket(this, nullptr)
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
{

}

void Trading::run()
{
    // https://stackoverflow.com/questions/17156541/why-do-we-need-to-use-boostasioio-servicework
	_io_service.run();
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

void Trading::reconnectHandler(const boost::system::error_code& error)
{
	if (error != boost::asio::error::operation_aborted && _receiving &&
		(_connection_state == CLIENT_CS_DISCONNECTED || _connection_state == CLIENT_CS_WAITING_FOR_CONNECT))
	{
		eConnect(IB_HOST, IB_PORT, IB_CLIENT_ID);
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
		marketDataConnected();

		_time_between_reconnects = 0;

		reqInstrumentDetails();

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
	marketDataDisconnected();
	clearAccountNames();
	_ping_timer.cancel();
	_rate_timer.cancel();
    if (resetState) {
	    eDisconnectBase();
    }
	clearWriteBuffer();
	clearReadBuffer();
	BOOST_LOG_TRIVIAL(info) << "disconnected";
#ifdef QT_CORE_LIB
	Q_EMIT sigConnectionStatus(TraderManager::DISCONNECTED);
#endif
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
#ifdef QT_CORE_LIB
			Q_EMIT sigConnectionStatus(TraderManager::RATE_LIMITER);
#endif
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

		// Dump su disco dei messaggi IB
		//FILE *f = fopen("ib_dump.log", "a");
		//fwrite(ptr, 1, _inbox.size(), f);
		//fclose(f);

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
				updateMarketData(TraderManager::BaseClientKey());
			throw;
		}

		_inbox.consume(ptr - beginPtr);
		if (_market_data_updated)
			updateMarketData(TraderManager::BaseClientKey());
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

void Trading::reconnectHandler(const boost::system::error_code& error)
{
	// Non deve chiedere i dati dei contratti al server quando e' in market replay
	if (_market_replay)
	{
		_all_instruments_valid = true;
		return;
	}

	// Il primo controllo deve essere error != boost::asio::error::operation_aborted
	// perche' l'oggetto potrebbe gia' essere distrutto
	if (error != boost::asio::error::operation_aborted && _receiving &&
		(_connection_state == CLIENT_CS_DISCONNECTED || _connection_state == CLIENT_CS_WAITING_FOR_CONNECT))
	{
		eConnect(IB_HOST, IB_PORT, IB_CLIENT_ID);
	}
}

void Trading::reqInstrumentDetails()
{
	// if (_all_instruments_valid)
	// {
	// 	// Resubscribe if necessary
	// 	restartSubscriptions();
	// 	return;
	// }

	// // Load non-combo instruments only
	// boost::ptr_vector<Instrument>& instruments = getInstruments(
	// 	TraderManager::InstrumentsMarketDataPositionsKey());
	// for (size_t i = 0; i < instruments.size(); i++)
	// {
	// 	Instrument& instrument = instruments[i];
	// 	if (!instrument.ib_received && !instrument.simulated_combo && (instrument.broker_mkt_data == brokerCode() ||
	// 		instrument.broker_execute == brokerCode()))
	// 		reqContractDetails(i + CLIENT_ID_TICKER_OFFSET, instrument.details.contract);
	// }
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
#ifdef QT_CORE_LIB
		Q_EMIT sigConnectionStatus(TraderManager::CONNECTED);
#endif
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

bool Trading::sendOrder(OpenOrder& open_order)
{
	if (m_orderId < 0 || open_order.instrument == nullptr)
		return false;

	Order order;
	if (open_order.action == LuaEnums::BUY)
		order.action = ACTION_BUY;
	else if (open_order.action == LuaEnums::SELL)
		order.action = ACTION_SELL;
	else return false;
	order.totalQuantity = std::lround(open_order.quantity);
	if (open_order.type == LuaEnums::LIMIT)
		order.orderType = "LMT";
	else if (open_order.type == LuaEnums::MARKET)
		order.orderType = "MKT";
	else
	{
		BOOST_LOG_TRIVIAL(error) << "invalid order type: " << open_order.type;
		return false;
	}
	order.lmtPrice = open_order.limit_price;

	// Assegna qui l'id e poi lo incrementa per il prossimo ordine
	open_order.broker_id = m_orderId;
	BOOST_LOG_TRIVIAL(info) << "placing order id " << open_order.id << ", broker id " << open_order.
		broker_id << ": " << order.action << " " <<
		std::lround(open_order.quantity) << " " << open_order.instrument->name << " at " << open_order.limit_price;
	_has_error = false;
	placeOrder(m_orderId, open_order.instrument->details.contract, order);
	m_orderId++;
	return !_has_error;
}

bool Trading::modifyOrder(const OpenOrder& open_order, double quantity, double limit_price)
{
	if (open_order.broker_id < 0 || open_order.instrument == nullptr)
		return false;

	Order order;
	if (open_order.action == LuaEnums::BUY)
		order.action = ACTION_BUY;
	else if (open_order.action == LuaEnums::SELL)
		order.action = ACTION_SELL;
	else return false;
	// Nella modifyOrder uso le nuove quantity e limit_price
	order.totalQuantity = std::lround(quantity);
	if (open_order.type == LuaEnums::LIMIT)
		order.orderType = "LMT";
	else if (open_order.type == LuaEnums::MARKET)
		order.orderType = "MKT";
	else
	{
		BOOST_LOG_TRIVIAL(error) << "invalid order type: " << open_order.type;
		return false;
	}
	order.lmtPrice = limit_price;

	BOOST_LOG_TRIVIAL(info) << "modifying order id " << open_order.id << ", broker id " << open_order.
		broker_id << ": " << order.action << " " <<
		std::lround(quantity) << " " << open_order.instrument->details.contract.symbol << " at " << limit_price;
	_has_error = false;
	placeOrder(open_order.broker_id, open_order.instrument->details.contract, order);
	return !_has_error;
}

bool Trading::cancelOrder(const OpenOrder& open_order)
{
	if (open_order.broker_id <= 0)
		return false;

	BOOST_LOG_TRIVIAL(info) << "cancelling order id " << open_order.id << ", broker id " << open_order.
		broker_id;

	_has_error = false;
	EClient::cancelOrder((OrderId)open_order.broker_id);
	return !_has_error;
}

bool Trading::requestHistoricalData(Instrument* instrument, int bar_size_sec, int past_duration_sec)
{
	std::stringstream stream;
	boost::posix_time::time_facet* facet = new boost::posix_time::time_facet();
	facet->format("%Y%m%d %H:%M:%S");
	stream.imbue(std::locale(std::locale::classic(), facet));
	stream << boost::posix_time::microsec_clock::universal_time() << " GMT";
	// TODO finire
	// reqHistoricalData(instrument->details., const Contract &contract, const IBString &endDateTime, const IBString &durationStr, const IBString &barSizeSetting, const IBString &whatToShow, int useRTH, int formatDate, const TagValueListSPtr& chartOptions);
	return true;
}

static enum LUA_ENUMS ORDER_STATUS mapIBStatusToInternalStatus(const std::string& status)
{
	if (status == "PendingSubmit")
		return LuaEnums::PENDING_SUBMIT;
	if (status == "PendingCancel")
		return LuaEnums::PENDING_CANCEL;
	if (status == "PreSubmitted")
		return LuaEnums::PRE_SUBMITTED;
	if (status == "Submitted")
		return LuaEnums::SUBMITTED;
	if (status == "Cancelled")
		return LuaEnums::CANCELLED;
	if (status == "Filled")
		return LuaEnums::FILLED;
	if (status == "Inactive")
		return LuaEnums::INACTIVE;
	return LuaEnums::UNKNOWN_STATUS;
}

///////////////////////////////////////////////////////////////////
// events
void Trading::orderStatus(OrderId orderId, const std::string& status, double filled,
                           double remaining, double avgFillPrice, int permId, int parentId,
                           double lastFillPrice, int clientId, const std::string& whyHeld, double mktCapPrice)
{
	if (!_all_instruments_valid)
		return;

	_order_manager->updateOrder(brokerCode(), orderId, mapIBStatusToInternalStatus(status), filled,
	                                            remaining, avgFillPrice);
}

void Trading::nextValidId(OrderId orderId)
{
	m_orderId = orderId;
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
		boost::format fmt = boost::format("%1%: error id=%2%, errorCode=%3%, msg=%4%") % brokerName() % id % errorCode %
			errorString;
		BOOST_LOG_TRIVIAL(warning) << fmt;
		return;
	}

	// Ignore error code 300 ("Can't find EId with ticker Id") which is given by cancelMktData
	// Ignore error code 202 ("Order cancelled - Reason:") which is given when canceling orders
	if ((id != -1 || errorCode < 2103 || errorCode > 2108) && errorCode != 300 && errorCode != 202)
	{
		boost::format fmt = boost::format("%1%: error id=%2%, errorCode=%3%, msg=%4%") % brokerName() % id % errorCode %
			errorString;
		BOOST_LOG_TRIVIAL(error) << fmt;
		_has_error = true;
	}

	// No security definition has been found for the request
	if (errorCode == 200)
	{
		_has_error = false;
		// Lo gestisce come uno strumento ricevuto, ma non valido
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
	tickerId -= CLIENT_ID_TICKER_OFFSET;
	boost::ptr_vector<Instrument>& instruments = getInstruments(
		TraderManager::InstrumentsMarketDataPositionsKey());
	if (tickerId >= 0 && tickerId < instruments.size())
	{
		Instrument& instrument = instruments[tickerId];
		MarketData& market_data = getMarketData(TraderManager::InstrumentsMarketDataPositionsKey())[tickerId
		];
		switch (field)
		{
		case BID:
			if (instrument.broker_mkt_data == brokerCode() && market_data.bid_price[0] != price)
			{
				double current_time = getCurrentTime();
				market_data.setBidPrice(0, price, current_time);
				_market_data_updated = true;
				market_data.setUpdated(true, current_time);
				instrument.bidAskPricesSizesChanged();
			}
			break;
		case ASK:
			if (instrument.broker_mkt_data == brokerCode() && market_data.ask_price[0] != price)
			{
				double current_time = getCurrentTime();
				market_data.setAskPrice(0, price, current_time);
				_market_data_updated = true;
				market_data.setUpdated(true, current_time);
				instrument.bidAskPricesSizesChanged();
			}
			break;
		case LAST:
			if (instrument.broker_mkt_data == brokerCode() && market_data.last_price != price)
			{
				double current_time = getCurrentTime();
				market_data.setLastPrice(price);
				_market_data_updated = true;
				market_data.setUpdated(true, current_time);
			}
			break;
		case OPEN:
			// TODO: verificare come IB riporta i prezzi in asta
			if (instrument.broker_mkt_data == brokerCode() && market_data.open_price != price)
			{
				double current_time = getCurrentTime();
				market_data.setOpenPrice(price);
				_market_data_updated = true;
				market_data.setUpdated(true, current_time);
			}
			break;
		case HIGH:
			if (instrument.broker_mkt_data == brokerCode() && market_data.day_high != price)
			{
				double current_time = getCurrentTime();
				market_data.day_high = price;
				_market_data_updated = true;
				market_data.setUpdated(true, current_time);
			}
			break;
		case LOW:
			if (instrument.broker_mkt_data == brokerCode() && market_data.day_low != price)
			{
				double current_time = getCurrentTime();
				market_data.day_low = price;
				_market_data_updated = true;
				market_data.setUpdated(true, current_time);
			}
			break;
		case CLOSE:
			// TODO: verificare come IB riporta i prezzi in asta
			if (instrument.broker_mkt_data == brokerCode() && market_data.close_price != price)
			{
				double current_time = getCurrentTime();
				market_data.setClosePrice(price);
				_market_data_updated = true;
				market_data.setUpdated(true, current_time);
			}
			break;
		case AUCTION_PRICE:
			// TODO: verificare come IB riporta i prezzi in asta
			if (instrument.broker_mkt_data == brokerCode() && market_data.auction_price != price)
			{
				double current_time = getCurrentTime();
				market_data.setAuctionPrice(price);
				_market_data_updated = true;
				market_data.setUpdated(true, current_time);
			}
			break;
		}
	}
}

void Trading::tickSize(TickerId tickerId, TickType field, int size)
{
	tickerId -= CLIENT_ID_TICKER_OFFSET;
	boost::ptr_vector<Instrument>& instruments = getInstruments(
		TraderManager::InstrumentsMarketDataPositionsKey());
	if (tickerId >= 0 && tickerId < instruments.size())
	{
		Instrument& instrument = instruments[tickerId];
		MarketData& market_data = getMarketData(TraderManager::InstrumentsMarketDataPositionsKey())[tickerId
		];
		switch (field)
		{
		case BID_SIZE:
			if (instrument.broker_mkt_data == brokerCode() && market_data.bid_size[0] != size)
			{
				double current_time = getCurrentTime();
				market_data.setBidSize(0, size, current_time);
				_market_data_updated = true;
				market_data.setUpdated(true, current_time);
				instrument.bidAskPricesSizesChanged();
			}
			break;
		case ASK_SIZE:
			if (instrument.broker_mkt_data == brokerCode() && market_data.ask_size[0] != size)
			{
				double current_time = getCurrentTime();
				market_data.setAskSize(0, size, current_time);
				_market_data_updated = true;
				market_data.setUpdated(true, current_time);
				instrument.bidAskPricesSizesChanged();
			}
			break;
		case VOLUME:
			if (instrument.broker_mkt_data == brokerCode() && market_data.volume != size)
			{
				double current_time = getCurrentTime();
				market_data.setVolume(size);
				_market_data_updated = true;
				market_data.setUpdated(true, current_time);
			}
			break;
		case AUCTION_IMBALANCE:
			if (instrument.broker_mkt_data == brokerCode() && market_data.imbalance != size)
			{
				double current_time = getCurrentTime();
				market_data.setImbalance(size);
				_market_data_updated = true;
				market_data.setUpdated(true, current_time);
			}
			break;
		}
	}
}

void Trading::tickGeneric(TickerId tickerId, TickType tickType, double value)
{
	tickerId -= CLIENT_ID_TICKER_OFFSET;
	boost::ptr_vector<Instrument>& instruments = getInstruments(
		TraderManager::InstrumentsMarketDataPositionsKey());
	if (tickerId >= 0 && tickerId < instruments.size())
	{
		Instrument& instrument = instruments[tickerId];
		MarketData& market_data = getMarketData(TraderManager::InstrumentsMarketDataPositionsKey())[tickerId
		];
		switch (tickType)
		{
		case SHORTABLE:
			if (instrument.broker_execute == brokerCode() && instrument.secType == LuaEnums::STK && market_data.shortable != (
				value > 2.5 || instrument.force_shortable))
			{
				double current_time = getCurrentTime();
				market_data.shortable = (value > 2.5 || instrument.force_shortable);
				_market_data_updated = true;
				market_data.setUpdated(true, current_time);
			}
			break;
		case HALTED:
			// value == 2.0 e' il volatility halt
			if (instrument.broker_mkt_data == brokerCode())
			{
				if (market_data.ib_halted != value)
				{
					double current_time = getCurrentTime();
					market_data.ib_halted = value;
					bool volatility_halt = (fabs(value - 2.0) < PRICE_EPSILON);
					market_data.setBrokerInAuction(MarketData::ClientKey(), volatility_halt);
					_market_data_updated = true;
					market_data.setUpdated(true, current_time);
				}
			}
			break;
		}
	}
}

void Trading::openOrder(OrderId orderId, const Contract& contract, const Order& order, const OrderState& ostate)
{
	if (!_all_instruments_valid)
		return;

	enum LUA_ENUMS ORDER_STATUS order_status = mapIBStatusToInternalStatus(ostate.status);
	// Cerco lo strumento
	boost::ptr_vector<Instrument>& instruments = getInstruments(
		TraderManager::InstrumentsMarketDataPositionsKey());
	for (auto it = instruments.begin(); it != instruments.end(); ++it)
	{
		if (it->details.contract.conId == contract.conId)
		{
			enum LUA_ENUMS ORDER_TYPES order_type = LuaEnums::MARKET;
			if (order.orderType == "LMT")
				order_type = LuaEnums::LIMIT;
			else if (order.orderType == "MKT")
				order_type = LuaEnums::MARKET;
			if (m_orderId <= orderId)
				m_orderId = orderId + 1;
			_order_manager->addOrChangeOrder(brokerCode(), orderId, &*it, order_status,
			                                                 order.action == ACTION_BUY ? LuaEnums::BUY : LuaEnums::SELL,
			                                                 order.totalQuantity, order_type, order.lmtPrice);
			break;
		}
	}
}

void Trading::updateAccountValue(const std::string& key, const std::string& val,
                                  const std::string& currency, const std::string& accountName)
{
	addAccountName(accountName.c_str());
}

void Trading::restartSubscriptions()
{
	// Market data subscription
	if (!_market_replay)
	{
		boost::ptr_vector<Instrument>& instruments = getInstruments(
			TraderManager::InstrumentsMarketDataPositionsKey());
		for (size_t i = 0; i < instruments.size(); i++)
		{
			Instrument& instrument = instruments[i];
			// Esclude gli strumenti invalidi
			if (instrument.req_mkt_data && instrument.secType != LuaEnums::INVSEC)
			{
				cancelMktData(i + CLIENT_ID_TICKER_OFFSET);
				if (instrument.broker_mkt_data != brokerCode())
				{
					// Se vengono richiesti dati di mercato alternativi (Sella, IWBank), e sto eseguendo su IB, da IB chiedo solo il flag shortable
					if (instrument.broker_execute == brokerCode() && instrument.secType == LuaEnums::STK)
						reqMktData(i + CLIENT_ID_TICKER_OFFSET, instrument.details.contract, "236", false, false, TagValueListSPtr());
					// For stocks, ask for shortable flag
				}
				else
					// Il flag shortable e' richiesto solo se IB e' il broker di esecuzione per lo strumento
					reqMktData(i + CLIENT_ID_TICKER_OFFSET, instrument.details.contract, (std::string(IB_DEFAULT_TICK_TYPES) +
						           (instrument.broker_execute == brokerCode() && instrument.secType == LuaEnums::STK ? ",236" : "")).
					           c_str(), false, false, TagValueListSPtr());
			}
			//std::stringstream stream;
			//boost::posix_time::time_facet* facet = new boost::posix_time::time_facet();
			//facet->format("%Y%m%d %H:%M:%S");
			//stream.imbue(std::locale(std::locale::classic(), facet));
			//stream << boost::posix_time::microsec_clock::universal_time() << " GMT";
			//reqHistoricalData(i + CLIENT_ID_TICKER_OFFSET, instrument.details.contract, stream.str(), "1 D", "30 mins", "MIDPOINT", 1, 2, TagValueListSPtr());
		}
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
	reqId -= CLIENT_ID_TICKER_OFFSET;
	boost::ptr_vector<Instrument>& instruments = getInstruments(
		TraderManager::InstrumentsMarketDataPositionsKey());
	if (reqId >= 0 && reqId < instruments.size())
	{
		Instrument& reqInstrument = instruments[reqId];
		reqInstrument.details = contractDetails;
		// Inserisce lo strumento nella mappa ordinata per contract id, per localizzarlo facilmente quando arrivano gli aggiornamenti di ptf
		if (contractDetails.contract.conId > 0)
			_instruments_by_contract_id[contractDetails.contract.conId] = &reqInstrument;
		// Riporta i dati essenziali sui campi di Instrument a beneficio di Lua
		copyAllocateString(reqInstrument.symbol_ib, contractDetails.contract.symbol.c_str());
		if (contractDetails.contract.secType == "STK")
			reqInstrument.secType = LuaEnums::STK;
		else if (contractDetails.contract.secType == "OPT")
			reqInstrument.secType = LuaEnums::OPT;
		else if (contractDetails.contract.secType == "FUT")
			reqInstrument.secType = LuaEnums::FUT;
		else if (contractDetails.contract.secType == "IND")
			reqInstrument.secType = LuaEnums::IND;
		else if (contractDetails.contract.secType == "FOP")
			reqInstrument.secType = LuaEnums::FOP;
		else if (contractDetails.contract.secType == "WAR")
			reqInstrument.secType = LuaEnums::WAR;
		else if (contractDetails.contract.secType == "CASH")
			reqInstrument.secType = LuaEnums::CASH;
		else if (contractDetails.contract.secType == "FUND")
			reqInstrument.secType = LuaEnums::FUND;
		else if (contractDetails.contract.secType == "EFP")
			reqInstrument.secType = LuaEnums::EFP;
		else if (contractDetails.contract.secType == "BAG")
			reqInstrument.secType = LuaEnums::BAG;
		else
			reqInstrument.secType = LuaEnums::INVSEC;
		copyAllocateString(reqInstrument.exchange, contractDetails.contract.exchange.c_str());
		copyAllocateString(reqInstrument.currency, contractDetails.contract.currency.c_str());
		// Fix per bug nelle API 9.73.06: https://groups.io/g/insync/topic/7790846?p=,,,20,0,0,0::recentpostdate%2Fsticky,,,20,2,0,7790846
		// Se lastTradeDateOrContractMonth contiene anche l'ora, viene tagliata via
		reqInstrument.details.contract.lastTradeDateOrContractMonth = reqInstrument
		                                                             .details.contract.lastTradeDateOrContractMonth.substr(
			                                                             0, 8);
		copyAllocateString(reqInstrument.expiry, reqInstrument.details.contract.lastTradeDateOrContractMonth.c_str());
		reqInstrument.strike = contractDetails.contract.strike;
		copyAllocateString(reqInstrument.right, contractDetails.contract.right.c_str());
		try
		{
			long multiplier = boost::lexical_cast<long>(contractDetails.contract.multiplier.c_str());
			if (multiplier > 0)
				reqInstrument.multiplier = multiplier;
		}
		catch (boost::bad_lexical_cast)
		{
		}
		reqInstrument.ib_received = true;
		BOOST_LOG_TRIVIAL(debug) << "contract " << reqId << ":" << dumpContractDetails(contractDetails);
		bool tmp_all_non_combo_instruments_valid = true;
		bool tmp_all_instruments_valid = true;
		for (auto it = instruments.begin(); it != instruments.end(); ++it)
		{
			if (it->broker_mkt_data == brokerCode() || it->broker_execute == brokerCode())
			{
				if (!it->combo)
					tmp_all_non_combo_instruments_valid = tmp_all_non_combo_instruments_valid && it->ib_received;
				tmp_all_instruments_valid = tmp_all_instruments_valid && it->ib_received;
			}
		}
		_all_non_combo_instruments_valid = tmp_all_non_combo_instruments_valid;
		_all_instruments_valid = tmp_all_instruments_valid;
		if (_all_non_combo_instruments_valid && !_all_instruments_valid)
		{
			// When all non combo instruments are received, copy contract ids into combo legs
			for (auto it = instruments.begin(); it != instruments.end(); ++it)
			{
				if (it->combo)
				{
					for (size_t j = 0; j < it->combo_legs.size(); j++)
					{
						(*it->details.contract.comboLegs)[j]->conId =
							it->combo_legs[j]->details.contract.conId;
					}
				}
			}
			_all_instruments_valid = true;
		}
		if (_all_instruments_valid)
		{
#ifdef QT_GUI_LIB
			updateInstruments(TraderManager::InstrumentsMarketDataPositionsKey());
#endif
			BOOST_LOG_TRIVIAL(info) << "contract details completed";
			restartSubscriptions();
		}
	}
}

void Trading::position(const std::string& account, const Contract& contract, double position, double avgCost)
{
	addAccountName(account.c_str());
	// TODO: filtrare sull'accountName in caso di account multipli?
	auto it = _instruments_by_contract_id.find(contract.conId);
	if (it == _instruments_by_contract_id.end())
	{
		// Non e' uno degli strumenti gestiti
		return;
	}
	Instrument* instrument = it->second;
	if (instrument->broker_execute == brokerCode())
	{
		// TODO: cost_basis e' il pmc o l'average cost? Cioe' devo dividere per il motiplicatore gia' qui?
		setInstrumentBrokerPosition(instrument, position,
		                                             avgCost / (instrument->multiplier > 1.0 ? instrument->multiplier : 1.0));
	}
}

void Trading::positionEnd()
{
	setPortfolioComplete();
	BOOST_LOG_TRIVIAL(info) << "positions completed";
}
