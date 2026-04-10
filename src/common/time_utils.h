/*
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include <chrono>
#include <cstdint>
#include <ratio>

struct timeval;

/*!
 * @brief Returns elapsed microseconds between two timeval snapshots.
 *
 * Handles microsecond borrow and clamps negative deltas to zero.
 * Use this helper instead of raw unsigned subtraction to avoid wraparound
 * when @p current.tv_usec is smaller than @p previous.tv_usec.
 *
 * @param current Newer timeval snapshot.
 * @param previous Older timeval snapshot.
 * @return Elapsed time in microseconds, clamped to zero when negative.
 */
uint64_t timeDiffUsec(const struct timeval &current, const struct timeval &previous);

// SteadyClock is an alias for std::chrono::steady_clock or compatible class.
// SteadyTimePoint is a matching time_point, SteadyDuration - matching duration.

#ifndef SAUNAFS_HAVE_STD_CHRONO_STEADY_CLOCK

// For platforms known to lack std::chrono::steady_clock support.
class SteadyClock {
public:
	typedef int64_t rep;
	typedef std::nano period;
	typedef std::chrono::duration<rep, period> duration;
	typedef std::chrono::time_point<SteadyClock> time_point;

	static constexpr bool is_steady = true;

	static time_point now();
};

#else

// Assume std::chrono::steady_clock is present.
typedef std::chrono::steady_clock SteadyClock;

#endif

typedef SteadyClock::time_point SteadyTimePoint;
typedef SteadyClock::duration SteadyDuration;

// Measures time from creation or last call to reset()
//
class Timer {
public:
	Timer();
	void reset();

	// Returns time since last reset
	SteadyTimePoint startTime() const;
	SteadyDuration elapsedTime() const;

	// Returns time since last reset and resets the timer
	SteadyDuration lap();

	int64_t elapsed_ns() const;
	int64_t elapsed_us() const;
	int64_t elapsed_ms() const;
	int64_t elapsed_s() const;
	int64_t lap_ns();
	int64_t lap_us();
	int64_t lap_ms();
	int64_t lap_s();

private:
	SteadyTimePoint now() const;
	SteadyTimePoint startTime_;
};

// Measures time from creation or reset, "expires" after predefined time.
//
class Timeout : public Timer {
public:
	Timeout(std::chrono::nanoseconds);
	SteadyTimePoint deadline() const;
	SteadyDuration remainingTime() const;
	int64_t remaining_ns() const;
	int64_t remaining_us() const;
	int64_t remaining_ms() const;
	int64_t remaining_s() const;
	bool expired() const;
private:
	SteadyDuration timeout_;
	bool infinite_ = false;
};
