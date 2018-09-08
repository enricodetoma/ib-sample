#include "maindialog.h"
#include "trading.h"
#include <QApplication>

int main(int argc, char* argv[])
{
	QApplication a(argc, argv);

	Trading t;
	_thread = boost::thread(&Trading::run, &t);
    
	MainDialog d;
	return d.exec();
}
