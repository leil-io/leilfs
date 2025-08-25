/*
   Copyright 2025      Leil Storage OÜ

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

// NOLINT

#include "common/capabilities.h"
#include "common/platform.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

TEST(CapabilitiesTest, ToStringMinimal) {  // NOLINT
	const std::string expectedOutput = "mountInfo: (0, 1)";
	const CapabilityList singleCap = {{"mountInfo", {.minimum = 0, .latest = 1}}};
	auto caps = Capabilities(singleCap);
	EXPECT_EQ(caps.toString(), expectedOutput);
}

TEST(CapabilitiesTest, ToStringMultiple) {  // NOLINT
	const std::vector<std::string> expectedLines = {
	    "mountInfo: (0, 1)",
	    "masterInfo: (5, 6)",
	};

	const CapabilityList singleCap = {
	    {"masterInfo", {.minimum = 5, .latest = 6}},
	    {"mountInfo", {.minimum = 0, .latest = 1}},
	};

	auto caps = Capabilities(singleCap);
	std::vector<std::string> actualLines;
	{
		std::istringstream iss(caps.toString());
		std::string line;
		while (std::getline(iss, line)) { actualLines.push_back(line); }
	}

	EXPECT_THAT(actualLines, ::testing::UnorderedElementsAreArray(expectedLines));
}

TEST(CapabilitiesTest, FromString) {  // NOLINT
	const std::string input = "mountInfo: (0, 1)\nmasterInfo: (5, 6)";
	auto caps = Capabilities(input);
	EXPECT_TRUE(caps.hasCapability("mountInfo", 0));
	EXPECT_TRUE(caps.hasCapability("mountInfo", 1));
	EXPECT_FALSE(caps.hasCapability("mountInfo", 2));
	EXPECT_TRUE(caps.hasCapability("masterInfo", 5));
	EXPECT_FALSE(caps.hasCapability("masterInfo", 4));
}

TEST(CapabilitiesTest, InvalidFormat) {  // NOLINT
	const std::string input1 = "mountInfo (0, 1)";
	EXPECT_THROW(Capabilities varname(input1), InvalidCapString); // NOLINT
	const std::string input2 = "masterInfo 5, 6";
	EXPECT_THROW(Capabilities varname(input2), InvalidCapString); // NOLINT
	const std::string input3 = "masterInfo: (6, 5)";
	EXPECT_THROW(Capabilities varname(input3), WrongCapVersion); // NOLINT
}
