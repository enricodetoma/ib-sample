#include <cmath>

#include "rate_limiter.hpp"

RateLimiter::RateLimiter(unsigned long rate, unsigned long period_ms)
	: rate_(rate)
	  , period_ms_(period_ms)
	  // sovradimensiona la coda per non sbagliare nella valutazione della rate in caso eventuali imprecisioni nei timer
	  , queue_(rate + 100)
	  , time_epoch_(boost::gregorian::date(1970, 1, 1))
{
	assert(rate > 0);
}

long RateLimiter::add_message()
{
	double now  = (boost::posix_time::microsec_clock::local_time() - time_epoch_).total_milliseconds();
	queue_.push_back(now);
	// Se la coda non contiene almeno rate_ elementi, non deve aspettare nulla
	if (queue_.size() < rate_)
		return 0;
	// Se la coda contiene piu' di rate_ elementi, elimina eventuali elementi vecchi che non interessano piu' per il calcolo
	while (queue_.size() > rate_ && now > queue_.front() + period_ms_)
		queue_.pop_front();
	double future_allowable_time = queue_.front() + (double)period_ms_ * (double)queue_.size() / (double)rate_;
	if (future_allowable_time < now)
		return 0;
	return ceil(future_allowable_time - now);
}
