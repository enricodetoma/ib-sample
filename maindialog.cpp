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

void MainDialog::slotBuy()
{
}

void MainDialog::slotSell()
{
}
