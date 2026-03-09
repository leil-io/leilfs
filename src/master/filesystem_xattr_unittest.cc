/*
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

#include <gtest/gtest.h>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "master/filesystem_metadata.h"
#include "master/filesystem_xattr.h"

// gMetadata is defined in filesystem.cc, which is linked into the master library used by the unit
// test.

class XAttrHashCollisionTest : public ::testing::Test {
protected:
	void SetUp() override { gMetadata = new FilesystemMetadata; }
	void TearDown() override {
		delete gMetadata;
		gMetadata = nullptr;
	}

	static void setXAttr(inode_t inode, const std::string &name, const std::string &value) {
		ASSERT_EQ(SAUNAFS_STATUS_OK, xattr_setattr(inode, static_cast<uint8_t>(name.size()),
		                                           reinterpret_cast<const uint8_t *>(name.data()),
		                                           static_cast<uint32_t>(value.size()),
		                                           reinterpret_cast<const uint8_t *>(value.data()),
		                                           XATTR_SMODE_CREATE_OR_REPLACE));
	}

	/// Retrieves the value of a single xattr by name. Returns empty string if not found.
	static std::string getXAttr(inode_t inode, const std::string &name) {
		uint32_t valueLength = 0;
		uint8_t *valuePtr = nullptr;
		uint8_t status =
		    xattr_getattr(inode, static_cast<uint8_t>(name.size()),
		                  reinterpret_cast<const uint8_t *>(name.data()), &valueLength, &valuePtr);
		if (status != SAUNAFS_STATUS_OK) { return {}; }
		if (valueLength == 0) { return {}; }
		return {reinterpret_cast<const char *>(valuePtr), valueLength};
	}

	/// Returns the total attribute name list size for the inode via xattrInodeHash
	static uint32_t listSize(inode_t inode) {
		void *node = nullptr;
		uint32_t size = 0;
		EXPECT_EQ(SAUNAFS_STATUS_OK, get_xattrs_length_for_inode(inode, &node, &size));
		return size;
	}

	/// Returns the set of attribute names listed for the inode via xattrInodeHash.
	/// This exercises the same code path as `attr -ql` (listing xattrs).
	static std::set<std::string> listNames(inode_t inode) {
		void *node = nullptr;
		uint32_t size = 0;

		if (get_xattrs_length_for_inode(inode, &node, &size) != SAUNAFS_STATUS_OK ||
		    node == nullptr || size == 0) {
			return {};
		}

		std::vector<uint8_t> xattrBuffer(size);
		xattr_listattr_data(node, xattrBuffer.data());

		// The buffer contains NUL-separated names
		std::set<std::string> names;
		const char *xattrBufferPointer = reinterpret_cast<const char *>(xattrBuffer.data());
		const char *end = xattrBufferPointer + size;

		while (xattrBufferPointer < end) {
			std::string name(xattrBufferPointer);
			const auto nameLength = name.size();
			if (nameLength > 0) { names.insert(std::move(name)); }
			xattrBufferPointer += nameLength + 1;
		}

		return names;
	}

public:
	// The two inodes are designed to collide in get_xattr_inode_hash:
	//   (1      * 0x72B5F387) & 0xFFFF == 62343
	//   (65537  * 0x72B5F387) & 0xFFFF == 62343
	static constexpr inode_t kInodeA = 1;
	static constexpr inode_t kInodeB = 65537;
};

// Confirms compile-time that the two inodes do collide.
static_assert(get_xattr_inode_hash(XAttrHashCollisionTest::kInodeA) ==
                  get_xattr_inode_hash(XAttrHashCollisionTest::kInodeB),
              "Test inodes must collide in xattrInodeHash");

TEST_F(XAttrHashCollisionTest, ListingIsCorrectForCollidingInodes) {
	const std::string attributeNameA = "user.alpha";
	const std::string attributeNameB = "user.beta";

	setXAttr(kInodeA, attributeNameA, "val_a");
	setXAttr(kInodeB, attributeNameB, "val_b");

	// Each inode has exactly one attribute name (+1 for the NUL separator)
	EXPECT_EQ(attributeNameA.size() + 1, listSize(kInodeA));
	EXPECT_EQ(attributeNameB.size() + 1, listSize(kInodeB));

	// Verify listed names are correct for each inode
	EXPECT_EQ(std::set<std::string>({attributeNameA}), listNames(kInodeA));
	EXPECT_EQ(std::set<std::string>({attributeNameB}), listNames(kInodeB));

	// Verify values are retrievable and correct
	EXPECT_EQ("val_a", getXAttr(kInodeA, attributeNameA));
	EXPECT_EQ("val_b", getXAttr(kInodeB, attributeNameB));

	// Cross-inode lookups must fail
	EXPECT_EQ("", getXAttr(kInodeA, attributeNameB));
	EXPECT_EQ("", getXAttr(kInodeB, attributeNameA));
}

TEST_F(XAttrHashCollisionTest, MultipleAttrsOnCollidingInodes) {
	const std::string attributeNameA1 = "user.a1";
	const std::string attributeNameA2 = "user.a2";
	const std::string attributeNameB1 = "user.b1";

	setXAttr(kInodeA, attributeNameA1, "x");
	setXAttr(kInodeA, attributeNameA2, "y");
	setXAttr(kInodeB, attributeNameB1, "z");

	uint32_t expectedA = attributeNameA1.size() + 1 + attributeNameA2.size() + 1;
	uint32_t expectedB = attributeNameB1.size() + 1;

	EXPECT_EQ(expectedA, listSize(kInodeA));
	EXPECT_EQ(expectedB, listSize(kInodeB));

	// Verify listed names
	EXPECT_EQ((std::set<std::string>({attributeNameA1, attributeNameA2})), listNames(kInodeA));
	EXPECT_EQ(std::set<std::string>({attributeNameB1}), listNames(kInodeB));

	// Verify values
	EXPECT_EQ("x", getXAttr(kInodeA, attributeNameA1));
	EXPECT_EQ("y", getXAttr(kInodeA, attributeNameA2));
	EXPECT_EQ("z", getXAttr(kInodeB, attributeNameB1));

	// Cross-inode must not return values
	EXPECT_EQ("", getXAttr(kInodeA, attributeNameB1));
	EXPECT_EQ("", getXAttr(kInodeB, attributeNameA1));
	EXPECT_EQ("", getXAttr(kInodeB, attributeNameA2));
}
