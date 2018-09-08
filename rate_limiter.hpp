#pragma once

#include <boost/circular_buffer.hpp>

class RateLimiter
{
public:
	RateLimiter(unsigned long rate, unsigned long period_ms);

	// Ritorna il numero di ms da attendere prima di poter inserire il prossimo messaggio 
	long add_message();

private:
	unsigned long rate_;
	unsigned long period_ms_;
	boost::circular_buffer<double> queue_;
};
