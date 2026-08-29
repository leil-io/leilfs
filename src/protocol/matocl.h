/*
   Copyright 2013-2014 EditShare
   Copyright 2013-2017 Skytechnology sp. z o.o.
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

#include "common/access_control_list.h"
#include "common/attributes.h"
#include "common/chunk_type_with_address.h"
#include "common/chunk_with_address_and_label.h"
#include "common/chunks_availability_state.h"
#include "common/defective_file_info.h"
#include "common/inotifier_list_entry.h"
#include "common/io_limits_database.h"
#include "common/job_info.h"
#include "common/legacy_acl.h"
#include "common/legacy_string.h"
#include "common/legacy_vector.h"
#include "common/metadataserver_list_entry.h"
#include "common/richacl.h"
#include "common/serialization_macros.h"
#include "common/serialized_goal.h"
#include "common/sessions_file.h"
#include "protocol/SFSCommunication.h"
#include "protocol/chunkserver_list_entry.h"
#include "protocol/directory_entry.h"
#include "protocol/handle_inode_entry.h"
#include "protocol/lock_info.h"
#include "protocol/mount_info_entry.h"
#include "protocol/named_inode_entry.h"
#include "protocol/packet.h"
#include "protocol/quota.h"

SAUNAFS_DEFINE_PACKET_SERIALIZATION(matocl, updateCredentials, SAU_MATOCL_UPDATE_CREDENTIALS, 0,
		uint32_t, messageId,
		uint8_t, status)

// SAU_MATOCL_FUSE_MKNOD
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseMknod, kStatusPacketVersion, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseMknod, kResponsePacketVersion, 1)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseMknod, SAU_MATOCL_FUSE_MKNOD, kStatusPacketVersion,
		uint32_t, messageId,
		uint8_t, status)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseMknod, SAU_MATOCL_FUSE_MKNOD, kResponsePacketVersion,
		uint32_t, messageId,
		inode_t, inode,
		Attributes, attributes)

// SAU_MATOCL_FUSE_CREATE (fused create+open, mirrors fuseMknod's reply shape)
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseCreate, kStatusPacketVersion, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseCreate, kResponsePacketVersion, 1)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseCreate, SAU_MATOCL_FUSE_CREATE, kStatusPacketVersion,
		uint32_t, messageId,
		uint8_t, status)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseCreate, SAU_MATOCL_FUSE_CREATE, kResponsePacketVersion,
		uint32_t, messageId,
		inode_t, inode,
		Attributes, attributes)

// SAU_MATOCL_FUSE_MKDIR
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseMkdir, kStatusPacketVersion, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseMkdir, kResponsePacketVersion, 1)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseMkdir, SAU_MATOCL_FUSE_MKDIR, kStatusPacketVersion,
		uint32_t, messageId,
		uint8_t, status)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseMkdir, SAU_MATOCL_FUSE_MKDIR, kResponsePacketVersion,
		uint32_t, messageId,
		inode_t, inode,
		Attributes, attributes)

// SAU_MATOCL_FUSE_DELETE_ACL
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseDeleteAcl, SAU_MATOCL_FUSE_DELETE_ACL, 0,
		uint32_t, messageId,
		uint8_t, status)

// SAU_MATOCL_FUSE_GET_ACL
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseGetAcl, kStatusPacketVersion, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseGetAcl, kLegacyResponsePacketVersion, 1)
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseGetAcl, kResponsePacketVersion, 2)
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseGetAcl, kRichACLResponsePacketVersion, 3)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseGetAcl, SAU_MATOCL_FUSE_GET_ACL, kStatusPacketVersion,
		uint32_t, messageId,
		uint8_t, status)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseGetAcl, SAU_MATOCL_FUSE_GET_ACL, kLegacyResponsePacketVersion,
		uint32_t, messageId,
		legacy::AccessControlList, acl)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseGetAcl, SAU_MATOCL_FUSE_GET_ACL, kResponsePacketVersion,
		uint32_t, messageId,
		AccessControlList, acl)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseGetAcl, SAU_MATOCL_FUSE_GET_ACL, kRichACLResponsePacketVersion,
		uint32_t, messageId,
		uint32_t, owner,
		RichACL, acl)

// SAU_MATOCL_FUSE_SET_ACL
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseSetAcl, SAU_MATOCL_FUSE_SET_ACL, 0,
		uint32_t, messageId,
		uint8_t, status)

// SAU_MATOCL_IOLIMITS_CONFIG
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, iolimitsConfig, SAU_MATOCL_IOLIMITS_CONFIG, 0,
		uint32_t, configVersion,
		uint32_t, period_us,
		std::string, subsystem,
		std::vector<std::string>, groups)

// SAU_MATOCL_IOLIMIT
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, iolimit, SAU_MATOCL_IOLIMIT, 0,
		uint32_t, msgid,
		uint32_t, configVersion,
		std::string, group,
		uint64_t, grantedBytes)

// SAU_MATOCL_FUSE_SET_QUOTA
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseSetQuota, SAU_MATOCL_FUSE_SET_QUOTA, 0,
		uint32_t, messageId,
		uint8_t, status)

// SAU_MATOCL_FUSE_DELETE_QUOTA
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseDeleteQuota, SAU_MATOCL_FUSE_DELETE_QUOTA, 0,
		uint32_t, messageId,
		uint8_t, status)

// SAU_MATOCL_FUSE_GET_QUOTA
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseGetQuota, kStatusPacketVersion, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseGetQuota, kResponsePacketVersion, 1)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseGetQuota, SAU_MATOCL_FUSE_GET_QUOTA, kStatusPacketVersion,
		uint32_t, messageId,
		uint8_t, status)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseGetQuota, SAU_MATOCL_FUSE_GET_QUOTA, kResponsePacketVersion,
		uint32_t, messageId,
		std::vector<QuotaEntry>, quotaEntries,
		std::vector<std::string>, quotaInfo)

// SAU_MATOCL_IOLIMITS_STATUS
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, iolimitsStatus, SAU_MATOCL_IOLIMITS_STATUS, 0,
		uint32_t, messageId,
		uint32_t, configId,
		uint32_t, period_us,
		uint32_t, accumulate_ms,
		std::string, subsystem,
		std::vector<IoGroupAndLimit>, groupsAndLimits)

// SAU_MATOCL_METADATASERVER_STATUS
SAUNAFS_DEFINE_PACKET_VERSION(matocl, metadataserverStatus, kLegacyResponse, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocl, metadataserverStatus, kWithIdentityResponse, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, metadataserverStatus, SAU_MATOCL_METADATASERVER_STATUS, kLegacyResponse,
		uint32_t, messageId,
		uint8_t, status,
		uint64_t, metadataVersion)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, metadataserverStatus, SAU_MATOCL_METADATASERVER_STATUS, kWithIdentityResponse,
		uint32_t, messageId,
		uint8_t, status,
		uint64_t, metadataVersion,
		uint32_t, mdsId)

// SAU_MATOCL_FUSE_GETGOAL
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseGetGoal, kStatusPacketVersion, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseGetGoal, kResponsePacketVersion, 1)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseGetGoal, SAU_MATOCL_FUSE_GETGOAL, kStatusPacketVersion,
		uint32_t, messageId,
		uint8_t, status)

SAUNAFS_DEFINE_SERIALIZABLE_CLASS(FuseGetGoalStats,
		std::string, goalName,
		inode_t, files,
		inode_t, directories);

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseGetGoal, SAU_MATOCL_FUSE_GETGOAL, kResponsePacketVersion,
		uint32_t, messageId,
		std::vector<FuseGetGoalStats>, goalsStats)

// SAU_MATOCL_FUSE_SETGOAL
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseSetGoal, kStatusPacketVersion, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseSetGoal, kResponsePacketVersion, 1)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseSetGoal, SAU_MATOCL_FUSE_SETGOAL, kStatusPacketVersion,
		uint32_t, messageId,
		uint8_t, status)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseSetGoal, SAU_MATOCL_FUSE_SETGOAL, kResponsePacketVersion,
		uint32_t, messageId,
		inode_t, changed,
		inode_t, notChanged,
		inode_t, notPermitted)

// SAU_MATOCL_LIST_GOALS
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, listGoals, SAU_MATOCL_LIST_GOALS, 0,
		std::vector<SerializedGoal>, serializedGoals)

// SAU_MATOCL_CHUNKS_HEALTH
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, chunksHealth, SAU_MATOCL_CHUNKS_HEALTH, 0,
		bool, regularChunksOnly,
		ChunksAvailabilityState, availability,
		ChunksReplicationState, replication)

// SAU_MATOCL_CSERV_LIST
SAUNAFS_DEFINE_PACKET_VERSION(matocl, cservList, kStandard, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocl, cservList, kWithMessageId, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, cservList, SAU_MATOCL_CSERV_LIST, kStandard,
		std::vector<ChunkserverListEntry>, cservList)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, cservList, SAU_MATOCL_CSERV_LIST, kWithMessageId,
		uint32_t, message_id,
		std::vector<ChunkserverListEntry>, cservList)

// SAU_MATOCL_METADATASERVERS_LIST
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, metadataserversList, SAU_MATOCL_METADATASERVERS_LIST, 0,
		uint32_t, masterVersion,
		std::vector<MetadataserverListEntry>, shadowList)

// SAU_MATOCL_INOTIFIER_LIST
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, inotifierList, SAU_MATOCL_INOTIFIER_LIST, 0,
		std::vector<INotifierListEntry>, inotifierList)

// SAU_MATOCL_CHUNKS_INFO
namespace matocl {
namespace chunksInfo {
	static constexpr uint32_t kMaxNumberOfResultEntries = 4096;
}
}

SAUNAFS_DEFINE_PACKET_VERSION(matocl, chunksInfo, kStatusPacketVersion, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocl, chunksInfo, kResponsePacketVersion, 1)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, chunksInfo, SAU_MATOCL_CHUNKS_INFO, kStatusPacketVersion,
		uint32_t, message_id,
		uint8_t, status)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, chunksInfo, SAU_MATOCL_CHUNKS_INFO, kResponsePacketVersion,
		uint32_t, message_id,
		std::vector<ChunkWithAddressAndLabel>, chunks)

// SAU_MATOCL_HOSTNAME
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, hostname, SAU_MATOCL_HOSTNAME, 0,
		std::string, hostname)

// SAU_MATOCL_ADMIN_REGISTER_CHALLENGE
typedef std::array<uint8_t, 32> SauMatoclAdminRegisterChallengeData;
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, adminRegisterChallenge, SAU_MATOCL_ADMIN_REGISTER_CHALLENGE, 0,
		SauMatoclAdminRegisterChallengeData, data)

// SAU_MATOCL_ADMIN_REGISTER_RESPONSE
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, adminRegisterResponse, SAU_MATOCL_ADMIN_REGISTER_RESPONSE, 0,
		uint8_t, status)

// SAU_MATOCL_ADMIN_BECOME_MASTER
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, adminBecomeMaster, SAU_MATOCL_ADMIN_BECOME_MASTER, 0,
		uint8_t, status)

// SAU_MATOCL_ADMIN_STOP_WITHOUT_METADATA_DUMP
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, adminStopWithoutMetadataDump, SAU_MATOCL_ADMIN_STOP_WITHOUT_METADATA_DUMP, 0,
		uint8_t, status)

// SAU_MATOCL_ADMIN_DUMP_CONFIG
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, adminDumpConfiguration, SAU_MATOCL_ADMIN_DUMP_CONFIG, 0,
		std::string, config)

// SAU_MATOCL_ADMIN_RELOAD
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, adminReload, SAU_MATOCL_ADMIN_RELOAD, 0,
		uint8_t, status)

// SAU_MATOCL_ADMIN_SAVE_METADATA
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, adminSaveMetadata, SAU_MATOCL_ADMIN_SAVE_METADATA, 0,
		uint8_t, status)

// SAU_MATOCL_ADMIN_RECALCULATE_METADATA_CHECKSUM
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, adminRecalculateMetadataChecksum, SAU_MATOCL_ADMIN_RECALCULATE_METADATA_CHECKSUM, 0,
		uint8_t, status)

// SAU_MATOCL_UNLOCK_CHUNK_NOTICE
SAUNAFS_DEFINE_PACKET_VERSION(matocl, unlockChunkNotice, kInodeAndChunkIndex, 0)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, unlockChunkNotice, SAU_MATOCL_UNLOCK_CHUNK_NOTICE, kInodeAndChunkIndex,
		inode_t, inode,
		uint32_t, chunkIndex)

// SAU_MATOCL_FUSE_TRUNCATE
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseTruncate, kStatusPacketVersion, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseTruncate, kFinishedPacketVersion, 1)
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseTruncate, kInProgressPacketVersion, 2)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseTruncate, SAU_MATOCL_FUSE_TRUNCATE, kStatusPacketVersion,
		uint32_t, messageId,
		uint8_t, status)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseTruncate, SAU_MATOCL_FUSE_TRUNCATE, kFinishedPacketVersion,
		uint32_t, messageId,
		Attributes, attributes)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseTruncate, SAU_MATOCL_FUSE_TRUNCATE, kInProgressPacketVersion,
		uint32_t, messageId,
		uint64_t, oldLength,
		uint32_t, lockId)

// SAU_MATOCL_FUSE_TRUNCATE_END
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseTruncateEnd, kStatusPacketVersion, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseTruncateEnd, kResponsePacketVersion, 1)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseTruncateEnd, SAU_MATOCL_FUSE_TRUNCATE_END, kStatusPacketVersion,
		uint32_t, messageId,
		uint8_t, status)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseTruncateEnd, SAU_MATOCL_FUSE_TRUNCATE_END, kResponsePacketVersion,
		uint32_t, messageId,
		Attributes, attributes)

// SAU_MATOCL_FUSE_FLOCK
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseFlock, SAU_MATOCL_FUSE_FLOCK, 0,
		uint32_t, messageId,
		uint8_t, status)

// SAU_MATOCL_FUSE_GETLK
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseGetlk, kStatusPacketVersion, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseGetlk, kResponsePacketVersion, 1)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseGetlk, SAU_MATOCL_FUSE_GETLK, kStatusPacketVersion,
		uint32_t, message_id,
		uint8_t, status)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseGetlk, SAU_MATOCL_FUSE_GETLK, kResponsePacketVersion,
		uint32_t, message_id,
		safs_locks::FlockWrapper, lock)

// SAU_MATOCL_FUSE_SETLK
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseSetlk, SAU_MATOCL_FUSE_SETLK, 0,
		uint32_t, messageId,
		uint8_t, status)

// SAU_MATOCL_MANAGE_LOCKS
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, manageLocksList, SAU_MATOCL_MANAGE_LOCKS_LIST, 0,
		std::vector<safs_locks::Info>, locks
		)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, manageLocksUnlock, SAU_MATOCL_MANAGE_LOCKS_UNLOCK, 0,
		uint8_t, status)

// SAU_MATOCL_WHOLE_PATH_LOOKUP
SAUNAFS_DEFINE_PACKET_VERSION(matocl, wholePathLookup, kStatusPacketVersion, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocl, wholePathLookup, kResponsePacketVersion, 1)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, wholePathLookup, SAU_MATOCL_WHOLE_PATH_LOOKUP, kStatusPacketVersion,
		uint32_t, messageId,
		uint8_t, status)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, wholePathLookup, SAU_MATOCL_WHOLE_PATH_LOOKUP, kResponsePacketVersion,
		uint32_t, messageId,
		inode_t, inode,
		Attributes, attr)

// SAU_MATOCL_FULL_PATH_BY_INODE
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fullPathByInode, kStatusPacketVersion, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fullPathByInode, kResponsePacketVersion, 1)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fullPathByInode, SAU_MATOCL_FULL_PATH_BY_INODE, kStatusPacketVersion,
		uint32_t, messageId,
		uint8_t, status)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fullPathByInode, SAU_MATOCL_FULL_PATH_BY_INODE, kResponsePacketVersion,
		uint32_t, messageId,
		std::string, fullPath)

// SAU_MATOCL_FUSE_GET_SELF_QUOTA
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseGetSelfQuota, kStatusPacketVersion, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseGetSelfQuota, kResponsePacketVersion, 1)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseGetSelfQuota, SAU_MATOCL_FUSE_GET_SELF_QUOTA, kStatusPacketVersion,
		uint32_t, messageId,
		uint8_t, status)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseGetSelfQuota, SAU_MATOCL_FUSE_GET_SELF_QUOTA, kResponsePacketVersion,
		uint32_t, messageId,
		std::vector<QuotaEntry>, quotaEntries)

// SAU_MATOCL_MOUNT_INFO_LIST
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, mountInfoList, SAU_MATOCL_MOUNT_INFO_LIST, 0,
		std::vector<MountInfoEntry>, mountInfoList)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, recursiveRemove, SAU_MATOCL_RECURSIVE_REMOVE, 0,
		uint32_t, msgid,
		uint8_t, status)

// SAU_MATOCL_FUSE_GETDIR
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseGetDir, kStatus, 0)
// Not used: just kept to document the historical version numbering
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseGetDirLegacy, kLegacyResponse, 1)
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseGetDir, kResponseWithDirentIndex, 2)

namespace matocl {
namespace fuseGetDir {
	const uint64_t kMaxNumberOfDirectoryEntries = 1 << 13;
}
}

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseGetDir, SAU_MATOCL_FUSE_GETDIR, kStatus,
		uint32_t, messageId,
		uint8_t, status)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseGetDir, SAU_MATOCL_FUSE_GETDIR, kResponseWithDirentIndex,
		uint32_t, message_id,
		uint64_t, first_entry_index, //TODO remove (not needed)
		std::vector<DirectoryEntry>, dir_entry)

SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseGetReserved, kResponseNamedInodeEntry, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseGetReserved, kResponseHandleInodeEntry, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseGetReserved, SAU_MATOCL_FUSE_GETRESERVED, kResponseNamedInodeEntry,
		uint32_t, msgid,
		std::vector<NamedInodeEntry>, entries)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseGetReserved, SAU_MATOCL_FUSE_GETRESERVED, kResponseHandleInodeEntry,
		uint32_t, msgid,
		std::vector<HandleInodeEntry>, entries)

SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseGetTrash, kResponseNamedInodeEntry, 0)
SAUNAFS_DEFINE_PACKET_VERSION(matocl, fuseGetTrash, kResponseHandleInodeEntry, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseGetTrash, SAU_MATOCL_FUSE_GETTRASH, kResponseNamedInodeEntry,
		uint32_t, msgid,
		std::vector<NamedInodeEntry>, entries)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, fuseGetTrash, SAU_MATOCL_FUSE_GETTRASH, kResponseHandleInodeEntry,
		uint32_t, msgid,
		std::vector<HandleInodeEntry>, entries)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, listTasks, SAU_MATOCL_LIST_TASKS, 0,
		std::vector<JobInfo>, jobs_info)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, stopTask, SAU_MATOCL_STOP_TASK, 0,
		uint32_t, msgid,
		uint8_t, status)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, requestTaskId, SAU_MATOCL_REQUEST_TASK_ID, 0,
		uint32_t, msgid,
		uint32_t, taskid)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, snapshot, SAU_MATOCL_FUSE_SNAPSHOT, 0,
		uint32_t, msgid,
		uint8_t, status)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, listDefectiveFiles, SAU_MATOCL_LIST_DEFECTIVE_FILES, 0,
		uint64_t, last_entry_index,
		std::vector<DefectiveFileInfo>, files_info)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, listSessions, SAU_MATOCL_SESSION_FILES, 0,
		std::vector<SessionFiles>, sessions)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		matocl, deleteSession, SAU_MATOCL_DELETE_SESSION, 0,
		uint8_t, status)

namespace matocl {

namespace fuseReadChunk {

const PacketVersion kStatusPacketVersion = 0;
const PacketVersion kResponsePacketVersion = 1;
const PacketVersion kECChunks_ResponsePacketVersion = 2;

inline void serialize(std::vector<uint8_t>& destination, uint32_t messageId, uint8_t status) {
	serializePacket(destination, SAU_MATOCL_FUSE_READ_CHUNK, kStatusPacketVersion,
			messageId, status);
}

inline void deserialize(const std::vector<uint8_t>& source, uint8_t& status) {
	uint32_t dummyMessageId;
	verifyPacketVersionNoHeader(source, kStatusPacketVersion);
	deserializeAllPacketDataNoHeader(source, dummyMessageId, status);
}

inline void serialize(std::vector<uint8_t>& destination,
		uint32_t messageId, uint64_t fileLength, uint64_t chunkId, uint32_t chunkVersion,
		const std::vector<legacy::ChunkTypeWithAddress>& serversList) {
	serializePacket(destination, SAU_MATOCL_FUSE_READ_CHUNK, kResponsePacketVersion,
			messageId, fileLength, chunkId, chunkVersion, serversList);
}

inline void deserialize(const std::vector<uint8_t>& source,
		uint64_t& fileLength, uint64_t& chunkId, uint32_t& chunkVersion,
		std::vector<legacy::ChunkTypeWithAddress>& serversList) {
	uint32_t dummyMessageId;
	verifyPacketVersionNoHeader(source, kResponsePacketVersion);
	deserializeAllPacketDataNoHeader(source, dummyMessageId,
			fileLength, chunkId, chunkVersion, serversList);
}

inline void serialize(std::vector<uint8_t>& destination,
		uint32_t messageId, uint64_t fileLength, uint64_t chunkId, uint32_t chunkVersion,
		const std::vector<ChunkTypeWithAddress>& serversList) {
	serializePacket(destination, SAU_MATOCL_FUSE_READ_CHUNK, kECChunks_ResponsePacketVersion,
			messageId, fileLength, chunkId, chunkVersion, serversList);
}

inline void deserialize(const std::vector<uint8_t>& source,
		uint64_t& fileLength, uint64_t& chunkId, uint32_t& chunkVersion,
		std::vector<ChunkTypeWithAddress>& serversList) {
	uint32_t dummyMessageId;
	verifyPacketVersionNoHeader(source, kECChunks_ResponsePacketVersion);
	deserializeAllPacketDataNoHeader(source, dummyMessageId,
			fileLength, chunkId, chunkVersion, serversList);
}

} // namespace fuseReadChunk

namespace fuseWriteChunk {

const PacketVersion kStatusPacketVersion = 0;
const PacketVersion kResponsePacketVersion = 1;
const PacketVersion kECChunks_ResponsePacketVersion = 2;
const PacketVersion kECChunksWithGrant_ResponsePacketVersion = 3;

inline void serialize(std::vector<uint8_t>& destination, uint32_t messageId, uint8_t status) {
	serializePacket(destination, SAU_MATOCL_FUSE_WRITE_CHUNK, kStatusPacketVersion,
			messageId, status);
}

inline void deserialize(const std::vector<uint8_t>& source, uint8_t& status) {
	uint32_t dummyMessageId;
	verifyPacketVersionNoHeader(source, kStatusPacketVersion);
	deserializeAllPacketDataNoHeader(source, dummyMessageId, status);
}

inline void serialize(std::vector<uint8_t>& destination,
		uint32_t messageId, uint64_t fileLength,
		uint64_t chunkId, uint32_t chunkVersion, uint32_t lockId,
		const std::vector<legacy::ChunkTypeWithAddress>& serversList) {
	serializePacket(destination, SAU_MATOCL_FUSE_WRITE_CHUNK, kResponsePacketVersion,
			messageId, fileLength, chunkId, chunkVersion, lockId, serversList);
}

inline void deserialize(const std::vector<uint8_t>& source,
		uint64_t& fileLength, uint64_t& chunkId, uint32_t& chunkVersion, uint32_t& lockId,
		std::vector<legacy::ChunkTypeWithAddress>& serversList) {
	uint32_t dummyMessageId;
	verifyPacketVersionNoHeader(source, kResponsePacketVersion);
	deserializeAllPacketDataNoHeader(source, dummyMessageId,
			fileLength, chunkId, chunkVersion, lockId, serversList);
}

inline void serialize(std::vector<uint8_t>& destination,
		uint32_t messageId, uint64_t fileLength,
		uint64_t chunkId, uint32_t chunkVersion, uint32_t lockId,
		const std::vector<ChunkTypeWithAddress>& serversList) {
	serializePacket(destination, SAU_MATOCL_FUSE_WRITE_CHUNK, kECChunks_ResponsePacketVersion,
			messageId, fileLength, chunkId, chunkVersion, lockId, serversList);
}

inline void deserialize(const std::vector<uint8_t>& source,
		uint64_t& fileLength, uint64_t& chunkId, uint32_t& chunkVersion, uint32_t& lockId,
		std::vector<ChunkTypeWithAddress>& serversList) {
	uint32_t dummyMessageId;
	verifyPacketVersionNoHeader(source, kECChunks_ResponsePacketVersion);
	deserializeAllPacketDataNoHeader(source, dummyMessageId,
			fileLength, chunkId, chunkVersion, lockId, serversList);
}

inline void serialize(std::vector<uint8_t> &destination, uint32_t messageId, uint64_t fileLength,
                      uint64_t chunkId, uint32_t chunkVersion, uint32_t lockId,
                      uint64_t grantGeneration, uint64_t grantRandom,
                      const std::vector<ChunkTypeWithAddress> &serversList) {
	serializePacket(destination, SAU_MATOCL_FUSE_WRITE_CHUNK,
	                kECChunksWithGrant_ResponsePacketVersion, messageId, fileLength, chunkId,
	                chunkVersion, lockId, grantGeneration, grantRandom, serversList);
}

inline void deserialize(const std::vector<uint8_t> &source, uint64_t &fileLength,
                        uint64_t &chunkId, uint32_t &chunkVersion, uint32_t &lockId,
                        uint64_t &grantGeneration, uint64_t &grantRandom,
                        std::vector<ChunkTypeWithAddress> &serversList) {
	uint32_t dummyMessageId;
	verifyPacketVersionNoHeader(source, kECChunksWithGrant_ResponsePacketVersion);
	deserializeAllPacketDataNoHeader(source, dummyMessageId, fileLength, chunkId, chunkVersion,
	                                 lockId, grantGeneration, grantRandom, serversList);
}

} //namespace fuseWriteChunk

namespace fuseWriteChunkEnd {

inline void serialize(std::vector<uint8_t>& destination, uint32_t messageId, uint8_t status) {
	serializePacket(destination, SAU_MATOCL_FUSE_WRITE_CHUNK_END, 0, messageId, status);
}

inline void deserialize(const std::vector<uint8_t>& source, uint8_t& status) {
	uint32_t dummyMessageId;
	verifyPacketVersionNoHeader(source, 0);
	deserializeAllPacketDataNoHeader(source, dummyMessageId, status);
}

} //namespace fuseWriteChunkEnd

} // namespace matocl
