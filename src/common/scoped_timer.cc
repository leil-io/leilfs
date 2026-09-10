/*
    Copyright 2023-2024 Leil Storage OÜ

    LeilFS is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, version 3.

    LeilFS is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with LeilFS  If not, see <http://www.gnu.org/licenses/>.
*/

#include "scoped_timer.h"
#include "common/platform.h"

#include <sys/syslog.h>

#include "slogger/slogger.h"
#include "common/time_utils.h"

namespace util {

class ScopedTimer::Impl {
public:
	//	friend class ScopedTimer;
	using Clock = Timer;
	using ClockPtr = std::unique_ptr<Clock>;

	/**
	 * Constructor
	 */
	Impl() { init(std::string{"elapsed time"}, TimeUnit::TU_SEC); }

	/**
	 * Constructor
	 * @param message The message to log, it is prepended with the elapsed time
	 */
	explicit Impl(const std::string &message) {
		init(message, TimeUnit::TU_SEC);
	}

	/**
	 * Constructor
	 * @param time_unit The time unit to use for the elapsed time
	 */
	explicit Impl(TimeUnit timeUnit) {
		init(std::string{"elapsed time"}, timeUnit);
	}

	/**
	 * Constructor
	 * @param message The message to log, it is prepended with the elapsed time
	 * @param time_unit The time unit to use for the elapsed time
	 */
	Impl(const std::string &message, TimeUnit timeUnit) {
		init(message, timeUnit);
	}

	/**
	 * Constructor
	 * @param message The message to log, it is prepended with the elapsed time
	 * @param time_unit The time unit to use for the elapsed time
	 * @param timer The timer to use
	 */
	Impl(const std::string &message, TimeUnit timeUnit, ClockPtr timer) {
		init(message, timeUnit, std::move(timer));
	}

	/**
	 * Destructor
	 * Logs the elapsed time to syslog
	 */
	virtual ~Impl() {
		safs_pretty_syslog(LOG_INFO, "%s", sayElapsed().c_str());
	}

	/**
	 * We don't want to allow copying or moving
	 */
	Impl(const Impl &) = delete;
	Impl &operator=(const Impl &) = delete;
	Impl(Impl &&) noexcept = delete;
	Impl &operator=(Impl &&) noexcept = delete;

	/**
	 * Set the time unit to use
	 * @param time_unit The time unit to use for the elapsed time
	 */
	void setTimeUnit(TimeUnit timeUnit) { timeUnit_ = timeUnit; }

	/**
	 * Get the elapsed time as a string
	 * @return The elapsed time as a string
	 */
	std::string sayElapsed() {
		if (timer_ == nullptr) { return "Warning: timer not initialized"; }
		int64_t elapsedTime;
		std::string unit;
		switch (timeUnit_) {
		case TimeUnit::TU_NANO:
			elapsedTime = timer_->elapsed_ns();
			unit = "ns";
			break;
		case TimeUnit::TU_MICRO:
			elapsedTime = timer_->elapsed_us();
			unit = "us";
			break;
		case TimeUnit::TU_MILLI:
			elapsedTime = timer_->elapsed_ms();
			unit = "ms";
			break;
		case TimeUnit::TU_SEC:
		default:
			elapsedTime = timer_->elapsed_s();
			unit = "s";
			break;
		}
		return message_ + ": " + std::to_string(elapsedTime) + unit;
	}

private:
	ClockPtr timer_ = nullptr;
	std::string message_{"elapsed time"};
	TimeUnit timeUnit_{TimeUnit::TU_SEC};

	void init(const std::string &message, TimeUnit timeUnit,
	          ClockPtr timer = nullptr) {
		timer_ =
		    (timer == nullptr) ? std::make_unique<Clock>() : std::move(timer);
		message_ = message;
		timeUnit_ = timeUnit;

		timer_->reset();
	}
};

void ScopedTimer::setTimeUnit(util::ScopedTimer::TimeUnit time_unit) {
	pimpl_->setTimeUnit(time_unit);
}

ScopedTimer::ScopedTimer() { pimpl_ = std::make_unique<Impl>(); }

ScopedTimer::ScopedTimer(const std::string &message) {
	pimpl_ = std::make_unique<Impl>(message);
}

ScopedTimer::ScopedTimer(TimeUnit timeUnit) {
	pimpl_ = std::make_unique<Impl>(timeUnit);
}

ScopedTimer::ScopedTimer(const std::string &message, TimeUnit timeUnit) {
	pimpl_ = std::make_unique<Impl>(message, timeUnit);
}

std::string ScopedTimer::sayElapsed() { return pimpl_->sayElapsed(); }

ScopedTimer::~ScopedTimer() = default;

}  // namespace util
