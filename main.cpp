#include "maindialog.h"
#include "mytrading.h"
#include <QApplication>
#include <boost/thread.hpp>

int main(int argc, char* argv[])
{
	QApplication a(argc, argv);

	MainDialog d;

	// Trading object will run its own boost::asio::io_service loop in a worker thread
	MyTrading t(&d);
	boost::thread thr(&MyTrading::run, &t);
    
	d.exec();

	// Stop the boost::asio::io_service then wait for the worker thread to end
	t.stop();
	thr.join();

	return 0;
}
