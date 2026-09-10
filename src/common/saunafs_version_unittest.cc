/*
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

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
#include "common/saunafs_version.h"

#include <gtest/gtest.h>

TEST(SaunaFsVersionTests, SaunaFsVersion) {
	EXPECT_EQ(0x01061BU, saunafsVersion(1, 6, 27));
	EXPECT_EQ(0x01061CU, saunafsVersion(1, 6, 28));
	EXPECT_EQ(0x01071BU, saunafsVersion(1, 7, 27));
	EXPECT_EQ(0x02061BU, saunafsVersion(2, 6, 27));
}
