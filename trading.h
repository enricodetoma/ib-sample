#pragma once

#ifndef TWSAPIDLLEXP
#define TWSAPIDLLEXP
#endif
#include <EClientSocket.h>
#include <DefaultEWrapper.h>
#include <Order.h>
#include <deque>
#include <string>
#include <memory>
#include <vector>
#include <boost/asio.hpp>
#include "rate_limiter.hpp"

class Trading : public EClientSocket, public DefaultEWrapper
{
public:
    explicit Trading(const char* ib_host, int ib_port, int ib_client_id);

    void run();
	void stop();
	void startReceiving();
	void stopReceiving();
	bool isConnected() const { return _connection_state == CLIENT_CS_CONNECTED; }

	bool eConnect(const char* host, unsigned int port, int clientId = 0, bool extraAuth = false);
	void eDisconnect(bool resetState = true) override;
	bool isSocketOK() const override { return _connection_state == CLIENT_CS_CONNECTED; }

	boost::asio::io_service& getIoService() { return _io_service; }
	int getNextOrderId() { return _order_id >= 0 ? _order_id++ : -1; }

	// override virtual funcs from EClient
private:
	int receive(char* buf, size_t sz) override;
protected:
	int bufferedSend(const std::string& message) override;

	// override virtual funcs from EClientMsgSink
public:
	void redirect(const char* host, int port) override;

public:
	// override virtual funcs from EWrapper
	void tickPrice(TickerId tickerId, TickType field, double price, const TickAttrib& attrib) override;
	void tickSize(TickerId tickerId, TickType field, int size) override;
	void tickGeneric(TickerId tickerId, TickType tickType, double value) override;
	void orderStatus(OrderId orderId, const std::string& status, double filled,
	                 double remaining, double avgFillPrice, int permId, int parentId,
	                 double lastFillPrice, int clientId, const std::string& whyHeld, double mktCapPrice) override;
	void openOrder(OrderId orderId, const Contract&, const Order&, const OrderState&) override;
	void updateAccountValue(const std::string& key, const std::string& val,
	                        const std::string& currency, const std::string& accountName) override;
	void nextValidId(OrderId orderId) override;
	void contractDetails(int reqId, const ContractDetails& contractDetails) override;
	void error(int id, int errorCode, const std::string& errorString) override;
	void currentTime(long time) override;
	void position(const std::string& account, const Contract& contract, double position, double avgCost) override;
	void positionEnd() override;

private:
	bool eConnectImpl(int clientId, bool extraAuth);
	bool checkMessages();

	void clearWriteBuffer();
	void clearReadBuffer();

	void readStart(void);
	void readHandler(const boost::system::error_code& error, size_t bytes_transferred);
	void writeStart();
	void writeHandler(const boost::system::error_code& error, size_t bytesTransferred);

	void waitForReconnect();
	void reconnectHandler(const boost::system::error_code& error);
	void pingHandler(const boost::system::error_code& error);
	void pingDeadline(const boost::system::error_code& error);
	void pingDeadlineConnect(const boost::system::error_code& error);
	void rateHandler(const boost::system::error_code& error);

	void reqAllContractDetails();
	void restartSubscriptions();

	int processMsgImpl(const char*& ptr, const char* endPtr);
	int processMsg(const char*& ptr, const char* endPtr);
	int processOnePrefixedMsg(const char*& ptr, const char* endPtr);

protected:
	int clientIdTickerOffset() { return 10000*clientId(); }
	void setNumberOfTickers(int tickers);
	void subscribeTicker(const Contract &contract);
	virtual void setupTickers() = 0;
	virtual void onMarketDataUpdated() = 0;

	// One element for each ticker
	std::vector<int> bid_size;
	std::vector<double> bid_price;
	std::vector<int> ask_size;
	std::vector<double> ask_price;
	std::vector<double> last_price;
	std::vector<int> volume;
	std::vector<ContractDetails> contract_details;
	int contract_details_requested;
	int contract_details_received;

private:
	enum ClientConnState
	{
		CLIENT_CS_DISCONNECTED,
		CLIENT_CS_WAITING_FOR_CONNECT,
		CLIENT_CS_CONNECTING,
		CLIENT_CS_CONNECTED,
	};
	boost::asio::io_service _io_service;
	boost::asio::io_service::work _work;
	std::unique_ptr<boost::asio::ip::tcp::socket> _socket;
	boost::asio::streambuf _inbox;
	std::deque<std::string> _outbox;
	std::string _current_message;
	bool _async_send_active;
	bool _wait_for_rate_limiter;
	boost::asio::deadline_timer _reconnect_timer;
	boost::asio::deadline_timer _ping_timer;
	boost::asio::deadline_timer _ping_deadline;
	boost::asio::deadline_timer _rate_timer;
	OrderId _order_id;
	RateLimiter _rate_limiter;
	bool _priority_message;
	enum ClientConnState _connection_state;
	int _time_between_reconnects;
	bool _has_error;
	bool _receiving;
	bool _market_data_updated;
};

