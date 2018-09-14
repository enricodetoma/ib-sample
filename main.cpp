#include "maindialog.h"
#include "mytrading.h"
#include <QApplication>
#include <boost/thread.hpp>
#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/attributes/named_scope.hpp>

int main(int argc, char* argv[])
{
    boost::log::add_common_attributes();
    boost::log::core::get()->set_filter(
        boost::log::trivial::severity >= boost::log::trivial::trace
    );

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
