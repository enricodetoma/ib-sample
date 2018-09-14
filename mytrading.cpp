#include "mytrading.h"
#include <QMetaObject>

MyTrading::MyTrading(MainDialog *dlg)
	: _dlg(dlg)
	, time_epoch_(boost::gregorian::date(1970, 1, 1))
	, last_time_send_gui_(0.)
{
}

void MyTrading::setupTickers()
{
	// Example, single ticker
	setNumberOfTickers(1);
}

void MyTrading::onMarketDataUpdated()
{
	// Implement reaction to market data updated here or in a derived class
	// Here is an example which sends prices to the GUI every second
	double now  = (boost::posix_time::microsec_clock::local_time() - time_epoch_).total_milliseconds();
	if (now - last_time_send_gui_ > 1000.)
	{
		QMetaObject::invokeMethod( _dlg, "slotSetPrice", Q_ARG( double, bid_price[0] ), Q_ARG( double, ask_price[0] ) );
		last_time_send_gui_ = now;
	}
}
