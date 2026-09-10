/*

   Copyright 2016 Skytechnology sp. z o.o.
   Copyright 2023 Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include <gtest/gtest.h>
#include <random>

#include "mount/readahead_adviser.h"

constexpr uint32_t kOneSecondInMs = 1000;
constexpr int kStillSequential = 65000;

int getRandomTolerableDelta() {
	static std::random_device seed;
	static std::mt19937 generator{seed()};
	static std::uniform_int_distribution<> distribution{-kStillSequential,
	                                                    kStillSequential};
	return distribution(generator);
}

TEST(ReadaheadTests, ReadSequential) {
	ReadaheadAdviser readAdviser(kOneSecondInMs);

	constexpr int kEnoughIterations = 32;
	constexpr uint64_t kOffsetStep = 65536;

	int lastWindow = 0;

	for (int i = 0; i < kEnoughIterations; ++i) {
		int tolerableDelta = getRandomTolerableDelta();
		readAdviser.feed(i * kOffsetStep, kOffsetStep + tolerableDelta);
		ASSERT_GE(readAdviser.window(), lastWindow);
		lastWindow = readAdviser.window();
	}
}

constexpr uint64_t kNotTolerableDelta = 100000;
constexpr uint64_t kOffsetStep = 6553600;

TEST(ReadaheadTests, ReadHoles) {
	ReadaheadAdviser readAdviser(kOneSecondInMs);

	constexpr int kSequentialIterations = 8;
	constexpr int kHoledIterations = 16;

	int i = 0;
	int lastWindow = readAdviser.window();

	for (; i < kSequentialIterations; ++i) {
		readAdviser.feed(i * kOffsetStep, kOffsetStep);
		ASSERT_GE(readAdviser.window(), lastWindow);
		lastWindow = readAdviser.window();
	}

	for (; i < kHoledIterations; ++i) {
		readAdviser.feed(i * kOffsetStep, kOffsetStep - kNotTolerableDelta * i);
		ASSERT_LE(readAdviser.window(), lastWindow);
		lastWindow = readAdviser.window();
	}
}

TEST(ReadaheadTests, ReadOverlapping) {
	ReadaheadAdviser readAdviser(kOneSecondInMs);

	constexpr int kSequentialIterations = 8;
	constexpr int kHoledIterations = 16;

	int i = 0;
	int lastWindow = readAdviser.window();

	for (; i < kSequentialIterations; ++i) {
		readAdviser.feed(i * kOffsetStep, kOffsetStep);
		ASSERT_GE(readAdviser.window(), lastWindow);
		lastWindow = readAdviser.window();
	}
	for (; i < kHoledIterations; ++i) {
		readAdviser.feed(i * kOffsetStep, kOffsetStep + kNotTolerableDelta * i);
		ASSERT_LE(readAdviser.window(), lastWindow);
		lastWindow = readAdviser.window();
	}
}

TEST(ReadaheadTests, ReadSequentialThenHolesThenSequential) {
	ReadaheadAdviser readAdviser(kOneSecondInMs);

	int i = 0;
	int lastWindow = 0;

	constexpr int kSequentialIterations = 16;
	constexpr int kHoledIterations = 48;
	constexpr int kFinalSequentialIterations = 64;
	const auto kOppositeThreshold = ReadaheadAdviser::kOppositeRequestThreshold;

	for (; i < kSequentialIterations; ++i) {
		readAdviser.feed(i * kOffsetStep, kOffsetStep);
		ASSERT_GE(readAdviser.window(), lastWindow);
		lastWindow = readAdviser.window();
	}

	for (; i < kSequentialIterations + kOppositeThreshold; ++i) {
		readAdviser.feed(i * kOffsetStep, kOffsetStep - kNotTolerableDelta * i);
	}
	for (; i < kHoledIterations; ++i) {
		readAdviser.feed(i * kOffsetStep, kOffsetStep - kNotTolerableDelta * i);
		ASSERT_LE(readAdviser.window(), lastWindow);
		lastWindow = readAdviser.window();
	}

	for (; i < kHoledIterations + kOppositeThreshold; ++i) {
		readAdviser.feed(i * kOffsetStep, kOffsetStep);
	}
	for (; i < kFinalSequentialIterations; ++i) {
		readAdviser.feed(i * kOffsetStep, kOffsetStep);
		ASSERT_GE(readAdviser.window(), lastWindow);
		lastWindow = readAdviser.window();
	}
}

TEST(ReadaheadTests, SwitchReadaheadSuggestion) {
	const auto threshold = ReadaheadAdviser::kOppositeRequestThreshold;
	ReadaheadAdviser readAdviser(kOneSecondInMs,
	                             SFSBLOCKSIZE * SFSBLOCKSINCHUNK, threshold);

	int step = 0;
	constexpr int kNumberOfSwitches = 10;

	for (int iteration = 0; iteration < kNumberOfSwitches; iteration++) {
		// Start sequential workload number
		for (unsigned i = 0; i < threshold + (iteration != 0); i++, step++) {
			// At the beginning readAhead mechanism is off
			ASSERT_FALSE(readAdviser.shouldUseReadahead());
			int tolerableDelta = getRandomTolerableDelta();
			readAdviser.feed(step * kOffsetStep, kOffsetStep + tolerableDelta);
		}

		// After feeding with tolerable deltha, readAhead should be true
		ASSERT_TRUE(readAdviser.shouldUseReadahead());

		// Start non-sequential workload
		for (unsigned i = 0; i <= threshold; i++, step++) {
			// After sequential workload readAhead mechanism should be enabled
			ASSERT_TRUE(readAdviser.shouldUseReadahead());
			bool isSequential = true;
			readAdviser.feed(step * kOffsetStep,
			                 kOffsetStep + kNotTolerableDelta * step,
			                 isSequential);
		}

		// After feeding with not tolerable deltha, readAhead should be false
		ASSERT_FALSE(readAdviser.shouldUseReadahead());
	}
}
