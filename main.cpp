#include "maindialog.h"
#include "trading.h"
#include <QApplication>
#include <boost/thread.hpp>

int main(int argc, char* argv[])
{
	QApplication a(argc, argv);

	// Trading object will run its own boost::asio::io_service loop in a worker thread
	Trading t;
	boost::thread thr(&Trading::run, &t);
    
	MainDialog d;
	d.exec();

	// Stop the boost::asio::io_service then wait for the worker thread to end
	t.stop();
	thr.join();

	return 0;
}
