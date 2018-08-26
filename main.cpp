#include "maindialog.h"
#include <QApplication>

int main(int argc, char* argv[])
{
	QApplication a(argc, argv);

	//start(QThread::TimeCriticalPriority);
    
	MainDialog d;
	return d.exec();
}
