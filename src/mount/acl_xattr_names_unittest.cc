/*

   Copyright 2026 Leil Storage OÜ

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

#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <initializer_list>
#include <string>
#include <vector>

#include "mount/acl_xattr_names.h"

/// Builds the NUL separated name list that the master returns for listxattr.
static std::vector<uint8_t> nameList(std::initializer_list<const char *> names) {
	std::vector<uint8_t> list;
	for (const char *name : names) {
		list.insert(list.end(), name, name + strlen(name) + 1);
	}
	return list;
}

TEST(AclXattrNamesTests, RecognisesAclNames) {
	EXPECT_TRUE(isAclXattrName("system.posix_acl_access"));
	EXPECT_TRUE(isAclXattrName("system.posix_acl_default"));
	EXPECT_TRUE(isAclXattrName("system.nfs4_acl"));
	EXPECT_TRUE(isAclXattrName("system.richacl"));
}

TEST(AclXattrNamesTests, LeavesOtherNamesAlone) {
	EXPECT_FALSE(isAclXattrName("user.comment"));
	EXPECT_FALSE(isAclXattrName("security.selinux"));
	EXPECT_FALSE(isAclXattrName(""));
	// Prefixes and extensions of an ACL name are not ACL names.
	EXPECT_FALSE(isAclXattrName("system.posix_acl"));
	EXPECT_FALSE(isAclXattrName("system.posix_acl_access_x"));
}

TEST(AclXattrNamesTests, FilterDropsOnlyAclNames) {
	const std::vector<uint8_t> list = nameList({"user.a", "system.posix_acl_access", "user.b",
	                                            "system.posix_acl_default", "system.richacl",
	                                            "system.nfs4_acl", "user.c"});
	EXPECT_EQ(nameList({"user.a", "user.b", "user.c"}), filterAclXattrNames(list.data(),
	                                                                       list.size()));
}

TEST(AclXattrNamesTests, FilterKeepsListWithoutAclNames) {
	const std::vector<uint8_t> list = nameList({"user.a", "security.selinux"});
	EXPECT_EQ(list, filterAclXattrNames(list.data(), list.size()));
}

TEST(AclXattrNamesTests, FilterEmptiesListOfOnlyAclNames) {
	const std::vector<uint8_t> list = nameList({"system.posix_acl_access", "system.richacl"});
	EXPECT_TRUE(filterAclXattrNames(list.data(), list.size()).empty());
}

TEST(AclXattrNamesTests, FilterHandlesEmptyInput) {
	EXPECT_TRUE(filterAclXattrNames(nullptr, 0).empty());
}

TEST(AclXattrNamesTests, FilterStopsAtAnUnterminatedTail) {
	// A malformed list must not be read past its end; whatever parsed cleanly is kept.
	std::vector<uint8_t> list = nameList({"user.a"});
	const std::string tail = "user.truncated";
	list.insert(list.end(), tail.begin(), tail.end());
	EXPECT_EQ(nameList({"user.a"}), filterAclXattrNames(list.data(), list.size()));
}

TEST(AclXattrNamesTests, FilterIsExactAboutNameBoundaries) {
	// An ACL name appearing as a substring of a longer name must survive.
	const std::vector<uint8_t> list = nameList({"user.system.posix_acl_access",
	                                            "system.posix_acl_access"});
	EXPECT_EQ(nameList({"user.system.posix_acl_access"}),
	          filterAclXattrNames(list.data(), list.size()));
}
