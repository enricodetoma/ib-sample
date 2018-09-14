#include "maindialog.h"
#include "mytrading.h"
#include <boost/bind.hpp>

MainDialog::MainDialog(QWidget* parent)
	: QDialog(parent)
	, trading(nullptr)
{
	ui.setupUi(this);

	connect(ui.pushButtonBuy, SIGNAL(clicked()), this, SLOT(slotBuy()));
	connect(ui.pushButtonSell, SIGNAL(clicked()), this, SLOT(slotSell()));
}

MainDialog::~MainDialog()
{
}

void MainDialog::slotSetPrice(double bid_price, double ask_price)
{
	ui.labelBidPrice->setText(QString("%1").arg(bid_price));
	ui.labelAskPrice->setText(QString("%1").arg(ask_price));
}

void MainDialog::slotBuy()
{
	if (trading)
	{
		// We need to use io_service::post to dispatch the call to the worker thread
		trading->getIoService().post(boost::bind(&MyTrading::buy, trading));
	}
}

void MainDialog::slotSell()
{
	if (trading)
	{
		// We need to use io_service::post to dispatch the call to the worker thread
		trading->getIoService().post(boost::bind(&MyTrading::sell, trading));
	}
}
