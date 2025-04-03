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

#pragma once

#include "common/platform.h"

#include <memory>

#include "common/exception.h"
#include "mount/mastercomm.h"

SAUNAFS_CREATE_EXCEPTION_CLASS_MSG(AclAcquisitionException, Exception, "ACL acquiring");

struct RichACLWithOwner {
	RichACL acl;
	uint32_t owner_id;
};

typedef std::shared_ptr<RichACLWithOwner> AclCacheEntry;

typedef LruCache<
		LruCacheOption::UseTreeMap,
		LruCacheOption::Reentrant,
		AclCacheEntry,
		inode_t, uint32_t, uint32_t> AclCache;

inline AclCacheEntry getAcl(inode_t inode, uint32_t uid, uint32_t gid) {
	AclCacheEntry entry(new RichACLWithOwner());
	uint8_t status = fs_getacl(inode, uid, gid, entry->acl, entry->owner_id);
	if (status == SAUNAFS_STATUS_OK) {
		return entry;
	} else if (status == SAUNAFS_ERROR_ENOATTR) {
		return AclCacheEntry();
	} else {
		throw AclAcquisitionException(status);
	}
}
