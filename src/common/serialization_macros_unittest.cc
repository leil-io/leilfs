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

#include "common/serialization_macros.h"

#include <tuple>
#include <gtest/gtest.h>

#include "common/legacy_string.h"
#include "common/type_defs.h"
#include "unittests/inout_pair.h"
#include "unittests/packet.h"

#define SAU_STATIC_ASSERT(cond) static_assert(cond, #cond)

SAU_STATIC_ASSERT(MORE_THEN_ONE_ARG(0, 1, "ala")                 == 0);
SAU_STATIC_ASSERT(MORE_THEN_ONE_ARG(0, 1, "ala", "ma")           == 1);
SAU_STATIC_ASSERT(MORE_THEN_ONE_ARG(0, 1, "ala", "ma", "costam") == 1);

SAU_STATIC_ASSERT(COUNT_ARGS(a, b, c) == 3);
SAU_STATIC_ASSERT(COUNT_ARGS(a, b)    == 2);
SAU_STATIC_ASSERT(COUNT_ARGS(a)       == 1);
// This doesn't work :(
// SAU_STATIC_ASSERT(COUNT_ARGS()        == 0);

class Base {};
SERIALIZABLE_CLASS_BEGIN(SomeClass : public Base)
SERIALIZABLE_CLASS_BODY(
	SomeClass,
	int   , fieldA,
	short , fieldB,
	int64_t , fieldC
)
	void myMethod() {
		fieldA = 5;
	};
SERIALIZABLE_CLASS_END;
TEST(SerializableClassTests, SimpleClass) {
	SomeClass a;
	(void) a.fieldA;
	(void) a.fieldB;
	(void) a.fieldC;
	a.myMethod();
}

SERIALIZABLE_CLASS_BEGIN(Class)
SERIALIZABLE_CLASS_BODY(
	Class,
	int   , A,
	short , B,
	int64_t , C,
	std::string             , D,
	std::vector<std::string>, E
)
	bool operator==(const Class& o) const {
		return std::make_tuple(A, B, C) == std::make_tuple(o.A, o.B, o.C);
	}
	bool operator!=(const Class& o) const {
		return !(*this == o);
	}
SERIALIZABLE_CLASS_END;
TEST(SerializableClassTests, Serialize) {
	std::vector<std::string> tmpVector {"kogo", "ma", "ala", "?"};
	Class tmpC {1, 20, 300, "ala ma kota", tmpVector};

	SAUNAFS_DEFINE_INOUT_PAIR(Class, c, tmpC, Class());
	ASSERT_NE(cIn, cOut);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(serialize(buffer, cIn));
	ASSERT_NO_THROW(deserialize(buffer, cOut));
	SAUNAFS_VERIFY_INOUT_PAIR(c);
}

SAUNAFS_DEFINE_PACKET_VERSION(somebodyToSomebodyElse, communicate, kNonEmptyVersion, 3210)
SAUNAFS_DEFINE_PACKET_VERSION(somebodyToSomebodyElse, communicate, kEmptyVersion, 3211)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		somebodyToSomebodyElse, communicate, SAU_CLTOMA_FUSE_MKNOD, kNonEmptyVersion,
		uint32_t, messageId,
		inode_t, inode,
		LegacyString<uint8_t>, name,
		uint8_t, nodeType,
		uint16_t, mode,
		uint16_t, umask,
		uint32_t, uid,
		uint32_t, gid,
		uint32_t, rdev)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		somebodyToSomebodyElse, communicate, SAU_CLTOMA_FUSE_MKNOD, kEmptyVersion)

TEST(PacketSerializationTests, SerializeAndDeserialize) {
	ASSERT_EQ(3210U, somebodyToSomebodyElse::communicate::kNonEmptyVersion);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, messageId, 65432, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(inode_t, inode, 36, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(LegacyString<uint8_t>, name, "kobyla ma maly bok", "");
	SAUNAFS_DEFINE_INOUT_PAIR(uint8_t, nodeType, 0xF1, 0x00);
	SAUNAFS_DEFINE_INOUT_PAIR(uint16_t, mode, 0725, 0000);
	SAUNAFS_DEFINE_INOUT_PAIR(uint16_t, umask, 0351, 0000);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, uid, 1235, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, gid, 531, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, rdev, 398, 0);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(somebodyToSomebodyElse::communicate::serialize(buffer, messageIdIn, inodeIn,
			nameIn, nodeTypeIn, modeIn, umaskIn, uidIn, gidIn, rdevIn));

	verifyHeader(buffer, SAU_CLTOMA_FUSE_MKNOD);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(somebodyToSomebodyElse::communicate::deserialize(buffer.data(), buffer.size(),
			messageIdOut, inodeOut, nameOut, nodeTypeOut, modeOut, umaskOut, uidOut, gidOut,
			rdevOut));

	SAUNAFS_VERIFY_INOUT_PAIR(messageId);
	SAUNAFS_VERIFY_INOUT_PAIR(inode);
	SAUNAFS_VERIFY_INOUT_PAIR(name);
	SAUNAFS_VERIFY_INOUT_PAIR(nodeType);
	SAUNAFS_VERIFY_INOUT_PAIR(mode);
	SAUNAFS_VERIFY_INOUT_PAIR(umask);
	SAUNAFS_VERIFY_INOUT_PAIR(uid);
	SAUNAFS_VERIFY_INOUT_PAIR(gid);
	SAUNAFS_VERIFY_INOUT_PAIR(rdev);
}

TEST(PacketSerializationTests, EmptyPacket) {
	std::vector<uint8_t> buffer;
	somebodyToSomebodyElse::communicate::serialize(buffer);
	verifyHeader(buffer, SAU_CLTOMA_FUSE_MKNOD);
	removeHeaderInPlace(buffer);
	somebodyToSomebodyElse::communicate::deserialize(buffer);
}

SAUNAFS_DEFINE_SERIALIZABLE_ENUM_CLASS(TestEnum, value0, value1, value2)

TEST(EnumClassSerializationTests, SerializeAndDeserialize) {
	SAUNAFS_DEFINE_INOUT_VECTOR_PAIR(TestEnum, enums);
	enumsIn  = {TestEnum::value0, TestEnum::value1, TestEnum::value2};
	enumsOut = {TestEnum::value1, TestEnum::value2, TestEnum::value0};

	for (uint8_t i = 0; i < enumsIn.size(); ++i) {
		EXPECT_EQ(i, static_cast<uint8_t>(enumsIn[i]));
		std::vector<uint8_t> buffer;
		ASSERT_NO_THROW(serialize(buffer, enumsIn[i]));
		ASSERT_NO_THROW(deserialize(buffer, enumsOut[i]));
	}
	SAUNAFS_VERIFY_INOUT_PAIR(enums);
}

TEST(EnumClassSerializationTests, DeserializeImproperValue) {
	std::vector<uint8_t> buffer;
	uint8_t in = 1 + static_cast<uint8_t>(TestEnum::value2);
	ASSERT_NO_THROW(serialize(buffer, in));
	TestEnum out;
	ASSERT_THROW(deserialize(buffer, out), IncorrectDeserializationException);
}
