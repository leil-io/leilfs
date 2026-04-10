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

#include "common/platform.h"
#include "common/time_utils.h"

#include <sys/time.h>
#include <unistd.h>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

// without at least millisecond precision a timesource is pretty much useless for us
TEST(TimeUtilsTests, SteadyClockResolutionAndArithmetic) {
	const SteadyTimePoint now = SteadyClock::now();
	const SteadyDuration dur =
		std::chrono::duration_cast<SteadyDuration>(std::chrono::milliseconds(123));
	const SteadyTimePoint later = now + dur;
	EXPECT_EQ(dur, later - now);
}

TEST(TimeUtilsTests, TimerAndTimeout) {
	Timer timer;
	Timeout timeout(std::chrono::milliseconds(200));

#if defined(__APPLE__) || defined(__FreeBSD__)
	long long accuracy = 15;
#else
	long long accuracy = 10;
#endif

	std::this_thread::sleep_for(std::chrono::microseconds(50 * 1000 - timer.elapsed_us()));

	EXPECT_EQ(0, timer.elapsed_s());
	EXPECT_NEAR(timer.elapsed_ms(), 50, accuracy);
	EXPECT_NEAR(timer.elapsed_us(), 50 * 1000, accuracy * 1000);
	EXPECT_NEAR(timer.elapsed_ns(), 50 * 1000 * 1000, accuracy * 1000 * 1000);

	EXPECT_EQ(0, timeout.remaining_s());
	EXPECT_NEAR(timeout.remaining_ms(), 150, accuracy);
	EXPECT_NEAR(timeout.remaining_us(), 150 * 1000, accuracy * 1000);
	EXPECT_NEAR(timeout.remaining_ns(), 150 * 1000 * 1000, accuracy * 1000 * 1000);
	EXPECT_FALSE(timeout.expired());

	std::this_thread::sleep_for(std::chrono::microseconds(210 * 1000 - timer.elapsed_us()));

	EXPECT_EQ(0, timer.elapsed_s());
	EXPECT_NEAR(timer.elapsed_ms(), 210, accuracy);
	EXPECT_NEAR(timer.elapsed_us(), 210 * 1000, accuracy * 1000);
	EXPECT_NEAR(timer.elapsed_ns(), 210 * 1000 * 1000, accuracy * 1000 * 1000);

	EXPECT_EQ(0, timeout.remaining_s());
	EXPECT_EQ(0, timeout.remaining_ms());
	EXPECT_EQ(0, timeout.remaining_us());
	EXPECT_EQ(0, timeout.remaining_ns());
	EXPECT_TRUE(timeout.expired());
}

TEST(TimeUtilsTests, InfiniteTimeout) {
	Timeout timeout(std::chrono::milliseconds(-1));

	EXPECT_FALSE(timeout.expired());
	EXPECT_EQ(timeout.remaining_ms(), -1);
	EXPECT_EQ(timeout.remaining_us(), -1);
	EXPECT_EQ(timeout.remaining_ns(), -1);
	EXPECT_EQ(timeout.remaining_s(), -1);

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	EXPECT_FALSE(timeout.expired());
	EXPECT_EQ(timeout.remaining_ms(), -1);
	EXPECT_EQ(timeout.remaining_us(), -1);
	EXPECT_EQ(timeout.remaining_ns(), -1);
	EXPECT_EQ(timeout.remaining_s(), -1);
}

TEST(TimeUtilsTests, TimeDiffUsecHandlesMicrosecondBorrow) {
	const timeval current{.tv_sec = 10, .tv_usec = 100};
	const timeval previous{.tv_sec = 8, .tv_usec = 900000};

	EXPECT_EQ(timeDiffUsec(current, previous), 1100100U);
}

TEST(TimeUtilsTests, TimeDiffUsecClampsNegativeDeltaToZero) {
	const timeval current{.tv_sec = 5, .tv_usec = 0};
	const timeval previous{.tv_sec = 6, .tv_usec = 0};

	EXPECT_EQ(timeDiffUsec(current, previous), 0U);
}
