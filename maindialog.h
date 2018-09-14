#pragma once

#include "ui_maindialog.h"

class MainDialog : public QDialog
{
Q_OBJECT

public:
	MainDialog(QWidget* parent = Q_NULLPTR);
	~MainDialog();

	Ui::MainDialog ui;

public Q_SLOTS:
	void slotSetPrice(double bid_price, double ask_price);

protected Q_SLOTS:
	void slotBuy();
	void slotSell();
};
