/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "common/platform.h"

#include <map>

#include "common/special_inode_defs.h"
#include "common/quota_database.h"
#include "master/acl_storage.h"
#include "master/filesystem_checksum_background_updater.h"
#include "master/filesystem_node_types.h"
#include "master/filesystem_xattr.h"
#include "master/id_pool_detainer.h"
#include "master/locks.h"
#include "master/task_manager.h"

using XAttributeInodeEntryPointer = std::unique_ptr<XAttributeInodeEntry>;
using XAttributeInodeEntryVector = std::vector<XAttributeInodeEntryPointer>;

using XAttributeDataEntryPointer = std::unique_ptr<XAttributeDataEntry>;
using XAttributeDataEntryVector = std::vector<XAttributeDataEntryPointer>;

using FSNodePointerVector = std::vector<FSNode*>;

/** Metadata of the filesystem.
 *  All the static variables managed by function in this file which form metadata of the filesystem.
 */
struct FilesystemMetadata {
public:
	std::array<XAttributeInodeEntryVector, XATTR_INODE_HASH_SIZE> xattrInodeHash;
	std::array<XAttributeDataEntryVector, XATTR_DATA_HASH_SIZE> xattrDataHash;
	// TODO(Guillex): Check implications of using 64 bits for inode_t in this structure.
	IdPoolDetainer<inode_t, uint32_t> inodePool;
	AclStorage aclStorage;
	TrashPathContainer trash;
	ReservedPathContainer reserved;
	FSNodeDirectory *root{};
	std::array<FSNodePointerVector, NODEHASHSIZE> nodeHash;
	TaskManager taskManager;
	FileLocks flockLocks;
	FileLocks posixLocks;

	inode_t maxInodeId{};
	uint32_t nextSessionId{};
	inode_t nodes{};
	uint64_t metadataVersion{};
	uint64_t trashSpace{};
	uint64_t reservedSpace{};
	inode_t trashNodes{};
	inode_t reservedNodes{};
	inode_t fileNodes{};
	inode_t dirNodes{};
	inode_t linkNodes{};

	QuotaDatabase quotaDatabase;

	uint64_t fsNodesChecksum{};
	uint64_t xattrChecksum{};
	uint64_t quotaChecksum{quotaDatabase.checksum()};

	FilesystemMetadata()
	    : inodePool{SFS_INODE_REUSE_DELAY,
	                12,
	                MAX_REGULAR_INODE,
	                MAX_REGULAR_INODE,
	                32 * 8 * 1024,
	                8 * 1024,
	                10} {}

	~FilesystemMetadata() {
		// Free memory allocated in xattr_inode_hash hashmap
		for (uint32_t i = 0; i < XATTR_INODE_HASH_SIZE; ++i) {
			xattrInodeHash[i].clear();
		}

		// Free memory allocated in xattr_data_hash hashmap
		for (uint32_t i = 0; i < XATTR_DATA_HASH_SIZE; ++i) {
			xattrDataHash[i].clear();
		}

		// Free memory allocated in nodehash hashmap
		for (uint32_t i = 0; i < NODEHASHSIZE; ++i) {
			for (const auto &node : nodeHash[i]) {
				FSNode::destroy(node);
			}
			nodeHash[i].clear();
		}
	}
};

extern FilesystemMetadata *gMetadata;
extern ChecksumBackgroundUpdater gChecksumBackgroundUpdater;
extern bool gDisableChecksumVerification;
extern uint32_t gTestStartTime;
extern bool gDisableEmptyFoldersMetadataOnFullDisk;

#ifndef METARESTORE
extern std::map<int, Goal> gGoalDefinitions;
extern bool gAtimeDisabled;
extern bool gMagicAutoFileRepair;
#endif
