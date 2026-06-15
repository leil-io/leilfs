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

#include <inttypes.h>
#include <vector>

#include "common/access_control_list.h"
#include "common/acl_type.h"
#include "common/attributes.h"
#include "common/chunk_type_with_address.h"
#include "mount/group_cache.h"
#include "mount/sauna_client.h"
#ifdef _WIN32
#include "mount/acquired_files_last_time_used.h"
#endif
#include "protocol/directory_entry.h"
#include "protocol/handle_inode_entry.h"
#include "protocol/lock_info.h"
#include "protocol/named_inode_entry.h"
#include "protocol/packet.h"

#ifdef _WIN32
inline std::atomic_bool gIsDisconnectedFromMaster;
#endif

inline std::atomic<uint32_t> masterVersion;

inline std::mutex acquiredFileMutex;
// <inode, cnt> and sorted by inode
using AcquiredFileMap = std::map<inode_t, uint32_t>;
inline AcquiredFileMap acquiredFiles;

void fs_getmasterlocation(uint8_t loc[14]);
uint32_t fs_getsrcip(void);

void fs_statfs(uint64_t *totalspace, uint64_t *availspace, uint64_t *trashspace,
               uint64_t *reservedspace, inode_t *inodes);
uint8_t fs_access(inode_t inode, uint32_t uid, uint32_t gid, uint8_t modemask);
uint8_t fs_lookup(inode_t parent, const std::string &path, uint32_t uid, uint32_t gid,
                  inode_t *inode, Attributes &attr);
uint8_t fs_getattr(inode_t inode, uint32_t uid, uint32_t gid, Attributes &attr);
uint8_t fs_setattr(inode_t inode, uint32_t uid, uint32_t gid, uint8_t setmask, uint16_t attrmode,
                   uint32_t attruid, uint32_t attrgid, uint32_t attratime, uint32_t attrmtime,
                   uint8_t sugidclearmode, Attributes &attr);
uint8_t fs_truncate(inode_t inode, bool opened, uint32_t uid, uint32_t gid, uint64_t length,
                    bool &clientPerforms, Attributes &attr, uint64_t &oldLength, uint32_t &lockId);
uint8_t fs_truncateend(inode_t inode, uint32_t uid, uint32_t gid, uint64_t length, uint32_t lockId,
                       Attributes &attr);
uint8_t fs_readlink(inode_t inode, const uint8_t **path);
uint8_t fs_symlink(inode_t parent, uint8_t nleng, const uint8_t *name, const uint8_t *path,
                   uint32_t uid, uint32_t gid, inode_t *inode, Attributes &attr);
uint8_t fs_mknod(inode_t parent, uint8_t nleng, const uint8_t *name, uint8_t type, uint16_t mode,
                 uint16_t umask, uint32_t uid, uint32_t gid, uint32_t rdev, inode_t &inode,
                 Attributes &attr);
uint8_t fs_create(inode_t parent, uint8_t nleng, const uint8_t *name, uint16_t mode, uint16_t umask,
                  uint32_t uid, uint32_t gid, uint8_t flags, inode_t &inode, Attributes &attr);
uint8_t fs_mkdir(inode_t parent, uint8_t nleng, const uint8_t *name, uint16_t mode, uint16_t umask,
                 uint32_t uid, uint32_t gid, uint8_t copysgid, inode_t &inode, Attributes &attr);
uint8_t fs_unlink(inode_t parent, uint8_t nleng, const uint8_t *name, uint32_t uid, uint32_t gid);
uint8_t fs_rmdir(inode_t parent, uint8_t nleng, const uint8_t *name, uint32_t uid, uint32_t gid);
uint8_t fs_rename(inode_t parent_src, uint8_t nleng_src, const uint8_t *name_src, inode_t parent_dst,
                  uint8_t nleng, const uint8_t *name_dst, uint32_t uid, uint32_t gid, inode_t *inode,
                  Attributes &attr);
uint8_t fs_link(inode_t inode_src, inode_t parent_dst, uint8_t nleng_dst, const uint8_t *name_dst,
                uint32_t uid, uint32_t gid, inode_t *inode, Attributes &attr);
uint8_t fs_getdir_plus(inode_t inode, uint32_t uid, uint32_t gid, uint8_t addtocache,
                       const uint8_t **dbuff, uint32_t *dbuffsize);
uint8_t fs_getdir(inode_t inode, uint32_t uid, uint32_t gid, uint64_t first_entry,
                  uint64_t max_entries, std::vector<DirectoryEntry> &dir_entries);

uint8_t fs_opencheck(inode_t inode, uint32_t uid, uint32_t gid, uint8_t flags, Attributes &attr);
uint8_t fs_update_credentials(uint32_t key, const GroupCache::Groups &gids);
void fs_release(inode_t inode);

uint8_t fs_saureadchunk(std::vector<ChunkTypeWithAddress> &serverList, uint64_t &chunkId,
                        uint32_t &chunkVersion, uint64_t &fileLength, inode_t inode, uint32_t index);
uint8_t fs_sauwritechunk(inode_t inode, uint32_t chunkIndex, uint32_t &lockId, uint64_t &fileLength,
                         uint64_t &chunkId, uint32_t &chunkVersion,
                         std::vector<ChunkTypeWithAddress> &chunkservers);
uint8_t fs_sauwriteend(uint64_t chunkId, uint32_t lockId, inode_t inode, uint64_t length);

uint8_t fs_getxattr(inode_t inode, uint8_t opened, uint32_t uid, uint32_t gid, uint8_t nleng,
                    const uint8_t *name, uint8_t mode, const uint8_t **vbuff, uint32_t *vleng);
uint8_t fs_listxattr(inode_t inode, uint8_t opened, uint32_t uid, uint32_t gid, uint8_t mode,
                     const uint8_t **dbuff, uint32_t *dleng);
uint8_t fs_setxattr(inode_t inode, uint8_t opened, uint32_t uid, uint32_t gid, uint8_t nleng,
                    const uint8_t *name, uint32_t vleng, const uint8_t *value, uint8_t mode);
uint8_t fs_removexattr(inode_t inode, uint8_t opened, uint32_t uid, uint32_t gid, uint8_t nleng,
                       const uint8_t *name);

uint8_t fs_deletacl(inode_t inode, uint32_t uid, uint32_t gid, AclType type);
uint8_t fs_getacl(inode_t inode, uint32_t uid, uint32_t gid, RichACL &acl, uint32_t &owner_id);
uint8_t fs_setacl(inode_t inode, uint32_t uid, uint32_t gid, const RichACL &acl);
uint8_t fs_setacl(inode_t inode, uint32_t uid, uint32_t gid, AclType type,
                  const AccessControlList &acl);
uint8_t fs_fullpath(inode_t inode, uint32_t uid, uint32_t gid, std::string &fullPath);

uint8_t fs_getreserved(const uint8_t **dbuff, uint32_t *dbuffsize);
template <typename OffsetT, typename EntryT>
uint8_t fs_getreserved(OffsetT off, uint32_t max_entries, std::vector<EntryT> &entries);
uint8_t fs_gettrash(const uint8_t **dbuff, uint32_t *dbuffsize);
template <typename OffsetT, typename EntryT>
uint8_t fs_gettrash(OffsetT off, uint32_t max_entries, std::vector<EntryT> &entries);
uint8_t fs_getdetachedattr(inode_t inode, Attributes &attr);
uint8_t fs_gettrashpath(inode_t inode, const uint8_t **path);
uint8_t fs_settrashpath(inode_t inode, const uint8_t *path);
uint8_t fs_undel(inode_t inode);
uint8_t fs_purge(inode_t inode);

uint8_t fs_getlk(inode_t inode, uint64_t owner, safs_locks::FlockWrapper &lock);
uint8_t fs_setlk_send(inode_t inode, uint64_t owner, uint32_t reqid,
                      const safs_locks::FlockWrapper &lock);
uint8_t fs_setlk_recv();
uint8_t fs_flock_send(inode_t inode, uint64_t owner, uint32_t reqid, uint16_t op);
uint8_t fs_flock_recv();
void fs_flock_interrupt(const safs_locks::InterruptData &data);
void fs_setlk_interrupt(const safs_locks::InterruptData &data);

uint8_t fs_makesnapshot(inode_t src_inode, inode_t dst_parent, const std::string &dst_name,
                        uint32_t uid, uint32_t gid, uint8_t can_overwrite, uint32_t &job_id);
uint8_t fs_get_self_quota(uint32_t uid, uint32_t gid, inode_t inode, std::vector<QuotaEntry> &quota_entries);
uint8_t fs_getgoal(inode_t inode, std::string &goal);
uint8_t fs_setgoal(inode_t inode, uint32_t uid, const std::string &goal_name, uint8_t smode);

uint8_t fs_custom(MessageBuffer &buffer);
uint8_t fs_raw_sendandreceive(MessageBuffer &buffer, PacketHeader::Type expectedType);
uint8_t fs_send_custom(MessageBuffer buffer);
uint8_t fs_getchunksinfo(uint32_t uid, uint32_t gid, inode_t inode, uint32_t chunk_index,
                         uint32_t chunk_count, std::vector<ChunkWithAddressAndLabel> &chunks);
uint8_t fs_getchunkservers(std::vector<ChunkserverListEntry> &chunkservers);

// called after fork
int fs_init_master_connection(SaunaClient::FsInitParams &params
#ifdef _WIN32
, uint8_t &session_flags, int &mounting_uid, int &mounting_gid
#endif
);
void fs_init_threads(uint32_t retries, uint32_t maxWaitTimeForRetry, uint32_t sleepTimeDivisor);
void fs_term(void);

class PacketHandler {
public:
	virtual bool handle(MessageBuffer buffer) = 0;
	virtual ~PacketHandler() {}
};

bool fs_register_packet_type_handler(PacketHeader::Type type, PacketHandler *handler);
bool fs_unregister_packet_type_handler(PacketHeader::Type type, PacketHandler *handler);
