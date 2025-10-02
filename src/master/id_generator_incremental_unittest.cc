/*
   Copyright 2023      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/platform.h"

#include <cstdint>

#include <gtest/gtest.h>

#include "master/id_generator_incremental.h"

TEST(IdGeneratorIncrementalTests, BasicFunctionality) {
	IdGeneratorIncremental<uint64_t> idGen;

	EXPECT_TRUE(idGen.initialize());

	// Test initial ID
	EXPECT_EQ(idGen.getCurrentId(), 1ULL);
	EXPECT_EQ(idGen.getNextId(), 1ULL);
	EXPECT_EQ(idGen.getCurrentId(), 2ULL);

	// Test sequential ID generation
	for (uint64_t i = 2ULL; i <= 10ULL; ++i) {
		EXPECT_EQ(idGen.getNextId(), i);
		EXPECT_EQ(idGen.getCurrentId(), i + 1ULL);
	}

	// Test setting current ID to a higher value
	idGen.setCurrentId(20ULL);
	EXPECT_EQ(idGen.getCurrentId(), 20ULL);
	EXPECT_EQ(idGen.getNextId(), 20ULL);
	EXPECT_EQ(idGen.getCurrentId(), 21ULL);

	// Test setting current ID to a lower value (should not change)
	idGen.setCurrentId(15ULL);
	EXPECT_EQ(idGen.getCurrentId(), 21ULL);

	// Tests numbers beyond 32-bit range
	idGen.setCurrentId(0xFFFFFFFFULL); // 2^32 - 1
	EXPECT_EQ(idGen.getNextId(), 0xFFFFFFFFULL);
	EXPECT_EQ(idGen.getCurrentId(), 0x100000000ULL); // 2^32
	EXPECT_EQ(idGen.getNextId(), 0x100000000ULL);
	EXPECT_EQ(idGen.getCurrentId(), 0x100000001ULL); // 2^32 + 1
}
