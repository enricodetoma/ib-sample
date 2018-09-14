#pragma once

#include "trading.h"
#include "maindialog.h"

class MyTrading : public Trading
{
public:
    MyTrading(MainDialog *dlg);

protected:
	void setupTickers() override;
	void onMarketDataUpdated() override;

private:
	MainDialog *_dlg;
	const boost::posix_time::ptime time_epoch_;
	double last_time_send_gui_;
};

