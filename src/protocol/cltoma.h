/*
   Copyright 2013-2017 EditShare
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

#include <array>
#include <cstdint>

#include "common/access_control_list.h"
#include "common/acl_type.h"
#include "common/legacy_acl.h"
#include "common/legacy_string.h"
#include "common/richacl.h"
#include "common/serialization_macros.h"
#include "common/small_vector.h"
#include "common/type_defs.h"
#include "protocol/lock_info.h"
#include "protocol/SFSCommunication.h"
#include "protocol/packet.h"
#include "protocol/quota.h"

namespace cltoma::updateCredentials {
constexpr uint32_t kDefaultGroupsSize = 16;
using GroupsContainer = small_vector<uint32_t, kDefaultGroupsSize>;
}  // namespace cltoma::updateCredentials

SAUNAFS_DEFINE_PACKET_SERIALIZATION(cltoma, updateCredentials, SAU_CLTOMA_UPDATE_CREDENTIALS, 0,
		uint32_t, messageId,
		uint32_t, index,
		GroupsContainer, gids)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(cltoma, fuseMknod, SAU_CLTOMA_FUSE_MKNOD, 0,
		uint32_t, messageId,
		inode_t, inode,
		LegacyString<uint8_t>, name,
		uint8_t, nodeType,
		uint16_t, mode,
		uint16_t, umask,
		uint32_t, uid,
		uint32_t, gid,
		uint32_t, rdev)

// Fused create+open: server does mknod (type file) + acquire for the session.
// flags carries the open flags (WANT_READ/WANT_WRITE), matching fuseOpen.
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cltoma, fuseCreate, SAU_CLTOMA_FUSE_CREATE, 0, uint32_t,
                                    messageId, inode_t, inode, LegacyString<uint8_t>, name,
                                    uint16_t, mode, uint16_t, umask, uint32_t, uid, uint32_t, gid,
                                    uint8_t, flags)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(cltoma, fuseMkdir, SAU_CLTOMA_FUSE_MKDIR, 0,
		uint32_t, messageId,
		inode_t, inode,
		LegacyString<uint8_t>, name,
		uint16_t, mode,
		uint16_t, umask,
		uint32_t, uid,
		uint32_t, gid,
		bool, copySgid)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, fuseDeleteAcl, SAU_CLTOMA_FUSE_DELETE_ACL, 0,
		uint32_t, messageId,
		inode_t, inode,
		uint32_t, uid,
		uint32_t, gid,
		AclType, type)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, fuseGetAcl, SAU_CLTOMA_FUSE_GET_ACL, 0,
		uint32_t, messageId,
		inode_t, inode,
		uint32_t, uid,
		uint32_t, gid,
		AclType, type)

SAUNAFS_DEFINE_PACKET_VERSION(cltoma, fuseSetAcl, kLegacyACL, 0)
SAUNAFS_DEFINE_PACKET_VERSION(cltoma, fuseSetAcl, kPosixACL, 1)
SAUNAFS_DEFINE_PACKET_VERSION(cltoma, fuseSetAcl, kRichACL, 2)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, fuseSetAcl, SAU_CLTOMA_FUSE_SET_ACL, kLegacyACL,
		uint32_t, messageId,
		inode_t, inode,
		uint32_t, uid,
		uint32_t, gid,
		AclType, type,
		legacy::AccessControlList, acl)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, fuseSetAcl, SAU_CLTOMA_FUSE_SET_ACL, kPosixACL,
		uint32_t, messageId,
		inode_t, inode,
		uint32_t, uid,
		uint32_t, gid,
		AclType, type,
		AccessControlList, acl)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, fuseSetAcl, SAU_CLTOMA_FUSE_SET_ACL, kRichACL,
		uint32_t, messageId,
		inode_t, inode,
		uint32_t, uid,
		uint32_t, gid,
		RichACL, acl)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, iolimit, SAU_CLTOMA_IOLIMIT, 0,
		uint32_t, msgid,
		uint32_t, configVersion,
		std::string, group,
		uint64_t, requestedBytes)

// SAU_CLTOMA_FUSE_SET_QUOTA
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, fuseSetQuota, SAU_CLTOMA_FUSE_SET_QUOTA, 0,
		uint32_t, messageId,
		uint32_t, uid,
		uint32_t, gid,
		std::vector<QuotaEntry>, quotaEntries)

// SAU_CLTOMA_FUSE_DELETE_QUOTA
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, fuseDeleteQuota, SAU_CLTOMA_FUSE_DELETE_QUOTA, 0,
		uint32_t, messageId,
		uint32_t, uid,
		uint32_t, gid,
		std::vector<QuotaEntryKey>, quotaEntriesKeys)

// SAU_CLTOMA_FUSE_GET_QUOTA
SAUNAFS_DEFINE_PACKET_VERSION(cltoma, fuseGetQuota, kAllLimits, 0)
SAUNAFS_DEFINE_PACKET_VERSION(cltoma, fuseGetQuota, kSelectedLimits, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, fuseGetQuota, SAU_CLTOMA_FUSE_GET_QUOTA, kAllLimits,
		uint32_t, messageId,
		uint32_t, uid,
		uint32_t, gid)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, fuseGetQuota, SAU_CLTOMA_FUSE_GET_QUOTA, kSelectedLimits,
		uint32_t, messageId,
		uint32_t, uid,
		uint32_t, gid,
		std::vector<QuotaOwner>, owners)

// SAU_CLTOMA_IOLIMITS_STATUS
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, iolimitsStatus, SAU_CLTOMA_IOLIMITS_STATUS, 0,
		uint32_t, messageId)

// SAU_CLTOMA_METADATASERVER_STATUS
SAUNAFS_DEFINE_PACKET_VERSION(cltoma, metadataserverStatus, kLegacyRequest, 0)
SAUNAFS_DEFINE_PACKET_VERSION(cltoma, metadataserverStatus, kWithIdentityRequest, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, metadataserverStatus, SAU_CLTOMA_METADATASERVER_STATUS, kLegacyRequest,
		uint32_t, messageId)
// requestsIdentity only needs to exist so this version's build/deserialize overload has a
// different arity from kLegacyRequest's; the version tag itself is already the real signal.
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, metadataserverStatus, SAU_CLTOMA_METADATASERVER_STATUS, kWithIdentityRequest,
		uint32_t, messageId,
		uint8_t, requestsIdentity)

// SAU_CLTOMA_METADATASERVERS_LIST
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, metadataserversList, SAU_CLTOMA_METADATASERVERS_LIST, 0)

// SAU_CLTOMA_INOTIFIER_LIST
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, inotifierList, SAU_CLTOMA_INOTIFIER_LIST, 0)

// SAU_CLTOMA_FUSE_GETGOAL
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, fuseGetGoal, SAU_CLTOMA_FUSE_GETGOAL, 0,
		uint32_t, messageId,
		inode_t, inode,
		uint8_t, gmode)

// SAU_CLTOMA_FUSE_SETGOAL
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, fuseSetGoal, SAU_CLTOMA_FUSE_SETGOAL, 0,
		uint32_t, messageId,
		inode_t, inode,
		uint32_t, uid,
		std::string, goalName,
		uint8_t, smode)

// SAU_CLTOMA_LIST_GOALS
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, listGoals, SAU_CLTOMA_LIST_GOALS, 0,
		bool, dummy)

// SAU_CLTOMA_CHUNKS_HEALTH
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, chunksHealth, SAU_CLTOMA_CHUNKS_HEALTH, 0,
		bool, regularChunksOnly)

// SAU_CLTOMA_CSERV_LIST
SAUNAFS_DEFINE_PACKET_VERSION(cltoma, cservList, kStandard, 0)
SAUNAFS_DEFINE_PACKET_VERSION(cltoma, cservList, kWithMessageId, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, cservList, SAU_CLTOMA_CSERV_LIST, kStandard,
		bool, dummy)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, cservList, SAU_CLTOMA_CSERV_LIST, kWithMessageId,
		uint32_t, message_id,
		bool, dummy)

// SAU_CLTOMA_CHUNK_INFO
SAUNAFS_DEFINE_PACKET_VERSION(cltoma, chunksInfo, kSingleChunk, 0) // deprecated
SAUNAFS_DEFINE_PACKET_VERSION(cltoma, chunksInfo, kMultiChunk, 1)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, chunksInfo, SAU_CLTOMA_CHUNKS_INFO, kMultiChunk,
		uint32_t, message_id,
		uint32_t, uid,
		uint32_t, gid,
		inode_t, inode,
		uint32_t, chunk_index,
		uint32_t, chunk_count)

// SAU_CLTOMA_HOSTNAME
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, hostname, SAU_CLTOMA_HOSTNAME, 0)

// SAU_CLTOMA_ADMIN_REGISTER
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, adminRegister, SAU_CLTOMA_ADMIN_REGISTER_CHALLENGE, 0)

// SAU_CLTOMA_ADMIN_REGISTER_RESPONSE
constexpr size_t kAdminRegisterResponseDataSize = 16;
using SauCltomaAdminRegisterResponseData = std::array<uint8_t, kAdminRegisterResponseDataSize>;
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, adminRegisterResponse, SAU_CLTOMA_ADMIN_REGISTER_RESPONSE, 0,
		SauCltomaAdminRegisterResponseData, data)

// SAU_CLTOMA_ADMIN_BECOME_MASTER
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, adminBecomeMaster, SAU_CLTOMA_ADMIN_BECOME_MASTER, 0)

// SAU_CLTOMA_ADMIN_STOP_WITHOUT_METADATA_DUMP
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, adminStopWithoutMetadataDump, SAU_CLTOMA_ADMIN_STOP_WITHOUT_METADATA_DUMP, 0)

// SAU_CLTOMA_ADMIN_RELOAD
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, adminReload, SAU_CLTOMA_ADMIN_RELOAD, 0)

// SAU_CLTOMA_ADMIN_DUMP_CONFIG
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, adminDumpConfiguration, SAU_CLTOMA_ADMIN_DUMP_CONFIG, 0)

// SAU_CLTOMA_ADMIN_SAVE_METADATA
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, adminSaveMetadata, SAU_CLTOMA_ADMIN_SAVE_METADATA, 0,
		bool, asynchronous)

// SAU_CLTOMA_ADMIN_RECALCULATE_METADATA_CHECKSUM
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, adminRecalculateMetadataChecksum, SAU_CLTOMA_ADMIN_RECALCULATE_METADATA_CHECKSUM, 0,
		bool, asynchronous)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, fuseTruncate, SAU_CLTOMA_FUSE_TRUNCATE, 0,
		uint32_t, messageId,
		inode_t, inode,
		bool, isOpened,
		uint32_t, uid,
		uint32_t, gid,
		uint64_t, length)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, fuseTruncateEnd, SAU_CLTOMA_FUSE_TRUNCATE_END, 0,
		uint32_t, messageId,
		inode_t, inode,
		uint32_t, uid,
		uint32_t, gid,
		uint64_t, length,
		uint32_t, lockid)

// FILE LOCKS
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, fuseFlock, SAU_CLTOMA_FUSE_FLOCK, 0,
		uint32_t, messageId,
		inode_t, inode,
		uint64_t, owner,
		uint32_t, requestId,
		uint16_t, operation)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, fuseFlock, SAU_CLTOMA_FUSE_FLOCK_INTERRUPT, 0,
		uint32_t, messageId,
		safs_locks::InterruptData, interruptData)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, fuseGetlk, SAU_CLTOMA_FUSE_GETLK, 0,
		uint32_t, message_id,
		inode_t, inode,
		uint64_t, owner,
		safs_locks::FlockWrapper, lock)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, fuseSetlk, SAU_CLTOMA_FUSE_SETLK, 0,
		uint32_t, messageId,
		inode_t, inode,
		uint64_t, owner,
		uint32_t, requestId,
		safs_locks::FlockWrapper, lock)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, fuseSetlk, SAU_CLTOMA_FUSE_SETLK_INTERRUPT, 0,
		uint32_t, messageId,
		safs_locks::InterruptData, interruptData)

SAUNAFS_DEFINE_PACKET_VERSION(cltoma, manageLocksList, kAll, 0)
SAUNAFS_DEFINE_PACKET_VERSION(cltoma, manageLocksList, kInode, 1)

constexpr uint64_t SAU_CLTOMA_MANAGE_LOCKS_LIST_LIMIT = 1024;

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, manageLocksList, SAU_CLTOMA_MANAGE_LOCKS_LIST, kAll,
		safs_locks::Type, type,
		bool, pending,
		uint64_t, start,
		uint64_t, max)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, manageLocksList, SAU_CLTOMA_MANAGE_LOCKS_LIST, kInode,
		inode_t, inode,
		safs_locks::Type, type,
		bool, pending,
		uint64_t, start,
		uint64_t, max)

SAUNAFS_DEFINE_PACKET_VERSION(cltoma, manageLocksUnlock, kSingle, 0)
SAUNAFS_DEFINE_PACKET_VERSION(cltoma, manageLocksUnlock, kInode, 1)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, manageLocksUnlock, SAU_CLTOMA_MANAGE_LOCKS_UNLOCK, kSingle,
		safs_locks::Type, type,
		inode_t, inode,
		uint32_t, sessionid,
		uint64_t, owner,
		uint64_t, start,
		uint64_t, end)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, manageLocksUnlock, SAU_CLTOMA_MANAGE_LOCKS_UNLOCK, kInode,
		safs_locks::Type, type,
		inode_t, inode)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(cltoma, wholePathLookup, SAU_CLTOMA_WHOLE_PATH_LOOKUP, 0,
		uint32_t, messageId,
		inode_t, inode,
		std::string, name,
		uint32_t, uid,
		uint32_t, gid)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(cltoma, fullPathByInode, SAU_CLTOMA_FULL_PATH_BY_INODE, 0,
		uint32_t, messageId,
		inode_t, inode,
		uint32_t, uid,
		uint32_t, gid)

SAUNAFS_DEFINE_PACKET_VERSION(cltoma, fuseGetSelfQuota, kGetSelfQuota, 0)
SAUNAFS_DEFINE_PACKET_VERSION(cltoma, fuseGetSelfQuota, kGetSelfQuotaWithInode, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, fuseGetSelfQuota, SAU_CLTOMA_FUSE_GET_SELF_QUOTA, kGetSelfQuota,
		uint32_t, messageId,
		uint32_t, uid,
		uint32_t, gid)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, fuseGetSelfQuota, SAU_CLTOMA_FUSE_GET_SELF_QUOTA, kGetSelfQuotaWithInode,
		uint32_t, messageId,
		uint32_t, uid,
		uint32_t, gid,
		inode_t, inode)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, recursiveRemove, SAU_CLTOMA_RECURSIVE_REMOVE, 0,
		uint32_t, msgid,
		uint32_t, jobId,
		inode_t, inode,
		std::string, file_name,
		uint32_t, uid,
		uint32_t, gid)

// Not used: just kept to document the historical version numbering
SAUNAFS_DEFINE_PACKET_VERSION(cltoma, fuseGetDirLegacy, kLegacyClient, 0)
SAUNAFS_DEFINE_PACKET_VERSION(cltoma, fuseGetDir, kClientAbleToProcessDirentIndex, 1)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(cltoma, fuseGetDir, SAU_CLTOMA_FUSE_GETDIR, kClientAbleToProcessDirentIndex,
		uint32_t, message_id,
		inode_t, inode,
		uint32_t, uid,
		uint32_t, gid,
		uint64_t, first_entry,
		uint64_t, number_of_entries)

SAUNAFS_DEFINE_PACKET_VERSION(cltoma, fuseGetReserved, kClientPositionOffset, 0)
SAUNAFS_DEFINE_PACKET_VERSION(cltoma, fuseGetReserved, kClientHandleOffset, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cltoma, fuseGetReserved, SAU_CLTOMA_FUSE_GETRESERVED, kClientPositionOffset,
		uint32_t, msgid,
		uint32_t, off,
		uint32_t, max_entries)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cltoma, fuseGetReserved, SAU_CLTOMA_FUSE_GETRESERVED, kClientHandleOffset,
		uint32_t, msgid,
		uint64_t, handleOffset,
		uint32_t, maxEntries)

SAUNAFS_DEFINE_PACKET_VERSION(cltoma, fuseGetTrash, kClientPositionOffset, 0)
SAUNAFS_DEFINE_PACKET_VERSION(cltoma, fuseGetTrash, kClientHandleOffset, 1)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cltoma, fuseGetTrash, SAU_CLTOMA_FUSE_GETTRASH, kClientPositionOffset,
		uint32_t, msgid,
		uint32_t, off,
		uint32_t, max_entries)
SAUNAFS_DEFINE_PACKET_SERIALIZATION(cltoma, fuseGetTrash, SAU_CLTOMA_FUSE_GETTRASH, kClientHandleOffset,
		uint32_t, msgid,
		uint64_t, handleOffset,
		uint32_t, maxEntries)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, listTasks, SAU_CLTOMA_LIST_TASKS, 0,
		bool, dummy)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, stopTask, SAU_CLTOMA_STOP_TASK, 0,
		uint32_t, msgid,
		uint32_t, taskid)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, listSessions, SAU_CLTOMA_SESSION_FILES, 0)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, deleteSession, SAU_CLTOMA_DELETE_SESSION, 0,
		uint64_t, sessionId)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, requestTaskId, SAU_CLTOMA_REQUEST_TASK_ID, 0,
		uint32_t, msgid)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, snapshot, SAU_CLTOMA_FUSE_SNAPSHOT, 0,
		uint32_t, msgid,
		uint32_t, jobid,
		inode_t, inode,
		inode_t, inode_dst,
		std::string, name_dst,
		uint32_t, uid,
		uint32_t, gid,
		uint8_t, canoverwrite,
		uint8_t, ignore_missing_src,
		uint32_t, initial_batch)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, listDefectiveFiles, SAU_CLTOMA_LIST_DEFECTIVE_FILES, 0,
		uint8_t, flags,
		uint64_t, first_entry,
		uint64_t, number_of_entries)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, registerConfig, SAU_CLTOMA_REGISTER_CONFIG, 0,
		std::string, config)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, updateMountInfo, SAU_CLTOMA_UPDATE_MOUNT_INFO, 0,
		std::string, mount_info)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		cltoma, mountInfoList, SAU_CLTOMA_MOUNT_INFO_LIST, 0)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(cltoma, writeGrantRenew, SAU_CLTOMA_WRITE_GRANT_RENEW, 0,
                                    uint32_t, messageId, uint64_t, chunkId, uint64_t,
                                    grantGeneration, uint64_t, grantRandom)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(cltoma, startTls, SAU_CLTOMA_STARTTLS, 0)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(cltoma, endTls, SAU_CLTOMA_ENDTLS, 0)

namespace cltoma {

namespace fuseReadChunk {

inline void serialize(std::vector<uint8_t>& destination,
		uint32_t messageId, inode_t inode, uint32_t chunkIndex) {
	serializePacket(destination, SAU_CLTOMA_FUSE_READ_CHUNK, 0, messageId, inode,
			chunkIndex);
}

inline void deserialize(const std::vector<uint8_t>& source,
		uint32_t& messageId, inode_t& inode, uint32_t& chunkIndex) {
	verifyPacketVersionNoHeader(source, 0);
	deserializeAllPacketDataNoHeader(source, messageId, inode, chunkIndex);
}

} // namespace fuseReadChunk

namespace fuseWriteChunk {

inline void serialize(std::vector<uint8_t>& destination,
		uint32_t messageId, inode_t inode, uint32_t chunkIndex, uint32_t lockId) {
	serializePacket(destination, SAU_CLTOMA_FUSE_WRITE_CHUNK, 0,
			messageId, inode, chunkIndex, lockId);
}

inline void deserialize(const std::vector<uint8_t>& source,
		uint32_t& messageId, inode_t& inode, uint32_t& chunkIndex, uint32_t& lockId) {
	verifyPacketVersionNoHeader(source, 0);
	deserializeAllPacketDataNoHeader(source, messageId, inode, chunkIndex, lockId);
}

} // namespace fuseWriteChunk

namespace fuseWriteChunkEnd {

const PacketVersion kLegacyPacketVersion = 0;
const PacketVersion kWithWriteGrantPacketVersion = 1;

inline void serialize(std::vector<uint8_t>& destination,
		uint32_t messageId, uint64_t chunkId, uint32_t lockId,
		inode_t inode, uint64_t fileLength) {
	serializePacket(destination, SAU_CLTOMA_FUSE_WRITE_CHUNK_END, kLegacyPacketVersion,
			messageId, chunkId, lockId, inode, fileLength);
}

inline void serialize(std::vector<uint8_t> &destination, uint32_t messageId, uint64_t chunkId,
                      uint32_t lockId, inode_t inode, uint64_t fileLength,
                      uint64_t grantGeneration, uint64_t grantRandom) {
	serializePacket(destination, SAU_CLTOMA_FUSE_WRITE_CHUNK_END, kWithWriteGrantPacketVersion,
	                messageId, chunkId, lockId, inode, fileLength, grantGeneration, grantRandom);
}

inline void deserialize(const std::vector<uint8_t>& source,
		uint32_t& messageId, uint64_t& chunkId, uint32_t& lockId,
		inode_t& inode, uint64_t& fileLength) {
	verifyPacketVersionNoHeader(source, kLegacyPacketVersion);
	deserializeAllPacketDataNoHeader(source, messageId, chunkId, lockId, inode, fileLength);
}

inline void deserialize(const std::vector<uint8_t> &source, uint32_t &messageId,
                        uint64_t &chunkId, uint32_t &lockId, inode_t &inode,
                        uint64_t &fileLength, uint64_t &grantGeneration,
                        uint64_t &grantRandom) {
	verifyPacketVersionNoHeader(source, kWithWriteGrantPacketVersion);
	deserializeAllPacketDataNoHeader(source, messageId, chunkId, lockId, inode, fileLength,
	                                 grantGeneration, grantRandom);
}

} // namespace fuseWriteChunkEnd

} // namespace cltoma
