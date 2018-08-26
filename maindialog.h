#pragma once

#include "ui_maindialog.h"

class MainDialog : public QDialog
{
Q_OBJECT

public:
	MainDialog(QWidget* parent = Q_NULLPTR);
	~MainDialog();

	Ui::MainDialog ui;

protected Q_SLOTS:
	void slotBuy();
	void slotSell();
};
