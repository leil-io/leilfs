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
#include "common/serialization.h"

#include <gtest/gtest.h>

#include "unittests/serialization.h"

TEST(SerializationTests, SerializeString) {
	serializeTest<std::string>("scrambled eggs with chives");
}

TEST(SerializationTests, SerializeUint32Vector) {
	serializeTest(std::vector<uint32_t>{1, 2, 3, 4});
}

TEST(SerializationTests, SerializeStringVector) {
	serializeTest(std::vector<std::string>{"scrambled", "eggs", "with", "chives"});
}

TEST(SerializationTests, SerializeMapOfMapsOfSets) {
	std::map<uint32_t, std::map<int, std::set<uint32_t>>> mapOfMapsOfSets;
	mapOfMapsOfSets[3][4] = {5, 6, 7};
	mapOfMapsOfSets[3][5] = {9, 6};
	mapOfMapsOfSets[1][4] = {};
	mapOfMapsOfSets[2];
	serializeTest(mapOfMapsOfSets);
}

struct MyStringAllocator : public std::allocator<std::string> {
	template<class ... Args>
	MyStringAllocator(Args... args) : std::allocator<std::string>(args...) {
	}

	template<typename U>
	struct rebind {
		using other = MyStringAllocator;
	};
};
TEST(SerializationTests, SerializeVectorWithCustomAllocator) {
	serializeTest<std::vector<std::string, MyStringAllocator>>(
			std::vector<std::string, MyStringAllocator>{"two", "eggs", "on", "sausage"});
}

TEST(SerializationTests, DeserializeStringNonEmptyVariable) {
	SAUNAFS_DEFINE_INOUT_PAIR(std::string, stringVariable, "good!", "BAD!");

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(serialize(buffer, stringVariableIn));
	ASSERT_ANY_THROW(deserialize(buffer, stringVariableOut));
}

TEST(SerializationTests, SerializeUniquePtr) {
	SAUNAFS_DEFINE_INOUT_PAIR(std::unique_ptr<std::string>, ptr,
			new std::string("chocolate-like product"), nullptr);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(serialize(buffer, ptrIn));
	ASSERT_NO_THROW(deserialize(buffer, ptrOut));
	ASSERT_FALSE(!ptrOut);
	ASSERT_EQ(*ptrIn, *ptrOut);
}

TEST(SerializationTests, SerializeEmptyUniquePtr) {
	SAUNAFS_DEFINE_INOUT_PAIR(std::unique_ptr<std::string>, emptyPtr, nullptr, nullptr);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(serialize(buffer, emptyPtrIn));
	ASSERT_NO_THROW(deserialize(buffer, emptyPtrOut));
	ASSERT_TRUE(!emptyPtrOut);
}

TEST(SerializationTests, SerializeStringArray) {
	std::string arrayIn[3] = {"ala", "ma", "xxxxx"};
	std::string arrayOut[3];

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(serialize(buffer, arrayIn));
	ASSERT_NO_THROW(deserialize(buffer, arrayOut));

	for (auto i = 0; i < 3; ++i) {
		ASSERT_EQ(arrayIn[i], arrayOut[i]);
	}
}

TEST(SerializationTests, SerializeSet) {
	serializeTest<std::set<std::string>>(
			std::set<std::string>{"I", "like", "to", "eat", "-", "pancakes"});
}

TEST(SerializationTests, SerializeMap) {
	serializeTest<std::map<std::string, std::string>>(
			std::map<std::string, std::string>{
					{"I", "like"}, {"to", "eat"}, {"-", "pancakes"}});
}
