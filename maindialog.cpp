#include "maindialog.h"

MainDialog::MainDialog(QWidget* parent)
	: QDialog(parent)
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
}

void MainDialog::slotSell()
{
}
