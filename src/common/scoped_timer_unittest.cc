/*
	Copyright 2023-2024 Leil Storage OÜ

	This file is part of LeilFS.

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


#include "common/scoped_timer.h"
#include "gtest/gtest.h"

TEST(ScopedTimerTest, DefaultConstructor) {
	{
		util::ScopedTimer timer;
		EXPECT_EQ(timer.sayElapsed(), "elapsed time: 0s");
	}
}

TEST(ScopedTimerTest, ConstructorWithMessage) {
	{
		util::ScopedTimer timer("test message");
		EXPECT_EQ(timer.sayElapsed(), "test message: 0s");
	}
}

TEST(ScopedTimerTest, ConstructorWithTimeUnit) {
	{
		util::ScopedTimer timer(util::ScopedTimer::TimeUnit::TU_MILLI);
		EXPECT_EQ(timer.sayElapsed(), "elapsed time: 0ms");
	}
}

TEST(ScopedTimerTest, ConstructorWithMessageAndTimeUnit) {
	{
		util::ScopedTimer timer("test message",
		                        util::ScopedTimer::TimeUnit::TU_MILLI);
		EXPECT_EQ(timer.sayElapsed(), "test message: 0ms");
	}
}

TEST(ScopedTimerTest, SetTimeUnit) {
	{
		util::ScopedTimer timer;
		timer.setTimeUnit(util::ScopedTimer::TimeUnit::TU_MILLI);
		EXPECT_EQ(timer.sayElapsed(), "elapsed time: 0ms");
	}
}
