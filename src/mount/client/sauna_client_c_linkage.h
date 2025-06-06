/*

   Copyright 2017 Skytechnology sp. z o.o.
   Copyright 2023 Leil Storage OÜ

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

#include <utility>
#include "mount/sauna_client.h"
#include "protocol/lock_info.h"

/*
 * This file exists in order to provide unmangled names that can be easily
 * linked with dlsym() call.
 *
 * Implementations of all functions below should not throw anything,
 * as throwing exceptions in dynamically linked code is a dangerous idea.
 */

extern "C" {

int saunafs_fs_init(SaunaClient::FsInitParams &params);
void saunafs_fs_term();
int saunafs_lookup(SaunaClient::Context &ctx, inode_t parent, const char *name,
                   SaunaClient::EntryParam &param);
int saunafs_mknod(SaunaClient::Context &ctx, inode_t parent, const char *name, mode_t mode,
                  dev_t rdev, SaunaClient::EntryParam &param);
int saunafs_link(SaunaClient::Context ctx, inode_t inode, inode_t parent, const char *name,
                 SaunaClient::EntryParam &param);
int saunafs_symlink(SaunaClient::Context ctx, const char *link, inode_t parent, const char *name,
                    SaunaClient::EntryParam &param);
int saunafs_mkdir(SaunaClient::Context &ctx, inode_t parent, const char *name, mode_t mode,
                  SaunaClient::EntryParam &entry_param);
int saunafs_rmdir(SaunaClient::Context &ctx, inode_t parent, const char *name);
int saunafs_unlink(SaunaClient::Context &ctx, inode_t parent, const char *name);
int saunafs_undel(SaunaClient::Context &ctx, inode_t ino);
int saunafs_open(SaunaClient::Context &ctx, inode_t ino, SaunaClient::FileInfo* fi);
int saunafs_opendir(SaunaClient::Context &ctx, inode_t ino);
int saunafs_release(inode_t ino, SaunaClient::FileInfo* fi);
int saunafs_getattr(SaunaClient::Context &ctx, inode_t ino, SaunaClient::AttrReply &reply);
int saunafs_releasedir(inode_t ino, uint64_t opendirSessionID);
int saunafs_setattr(SaunaClient::Context &ctx, inode_t ino, struct stat *stbuf, int to_set,
                    SaunaClient::AttrReply &attr_reply);

int saunafs_read(SaunaClient::Context &ctx, inode_t ino, size_t size,
                 off_t off, SaunaClient::FileInfo *fi, ReadCache::Result &result);

int saunafs_read_special_inode(SaunaClient::Context &ctx,
                               inode_t ino, size_t size, off_t off,
                               SaunaClient::FileInfo *fi,
                               std::vector<uint8_t> &special_inode);

int saunafs_readdir(SaunaClient::Context &ctx, uint64_t opendirSessionID,
                    inode_t ino, off_t off, size_t max_entries,
                    std::vector<SaunaClient::DirEntry> &entries);

int saunafs_readlink(SaunaClient::Context &ctx, inode_t ino, std::string &link);

int saunafs_readreserved(SaunaClient::Context &ctx,
                         SaunaClient::NamedInodeOffset off,
                         SaunaClient::NamedInodeOffset max_entries,
                         std::vector<NamedInodeEntry> &inode_entries);

int saunafs_readtrash(SaunaClient::Context &ctx,
                      SaunaClient::NamedInodeOffset off,
                      SaunaClient::NamedInodeOffset max_entries,
                      std::vector<NamedInodeEntry> &trash_entries);

int saunafs_write(SaunaClient::Context &ctx, inode_t ino,
                  const char *buf, size_t size, off_t off,
                  SaunaClient::FileInfo *fi, ssize_t &bytes_written);

int saunafs_flush(SaunaClient::Context &ctx, inode_t ino, SaunaClient::FileInfo* fi);
int saunafs_fsync(SaunaClient::Context &ctx, inode_t ino, int datasync, SaunaClient::FileInfo* fi);
bool saunafs_isSpecialInode(inode_t ino);
int saunafs_update_groups(SaunaClient::Context &ctx);

int saunafs_makesnapshot(SaunaClient::Context &ctx, inode_t ino,
                         inode_t dst_parent,
                         const std::string &dst_name, bool can_overwrite,
                         SaunaClient::JobId &job_id);

int saunafs_getgoal(SaunaClient::Context &ctx, inode_t ino, std::string &goal);
int saunafs_setgoal(SaunaClient::Context &ctx, inode_t ino, const std::string &goal_name,
                    uint8_t smode);
int saunafs_rename(SaunaClient::Context &ctx, inode_t parent, const char *name, inode_t newparent,
                   const char *newname);
int saunafs_statfs(uint64_t *totalspace, uint64_t *availspace, uint64_t *trashspace,
                   uint64_t *reservedspace, inode_t *inodes);
int saunafs_setxattr(SaunaClient::Context ctx, inode_t ino, const char *name, const char *value,
                     size_t size, int flags);
int saunafs_getxattr(SaunaClient::Context ctx, inode_t ino, const char *name, size_t size,
                     SaunaClient::XattrReply &xattr_reply);
int saunafs_listxattr(SaunaClient::Context ctx, inode_t ino, size_t size,
                      SaunaClient::XattrReply &xattr_reply);
int saunafs_removexattr(SaunaClient::Context ctx, inode_t ino, const char *name);

int saunafs_getchunksinfo(SaunaClient::Context &ctx, inode_t ino, uint32_t chunk_index,
                          uint32_t chunk_count, std::vector<ChunkWithAddressAndLabel> &chunks);

int saunafs_getchunkservers(std::vector<ChunkserverListEntry> &chunkservers);

int saunafs_getlk(SaunaClient::Context &ctx, inode_t ino, SaunaClient::FileInfo *fi,
                  safs_locks::FlockWrapper &lock);

int saunafs_setlk_send(SaunaClient::Context &ctx, inode_t ino, SaunaClient::FileInfo *fi,
                       safs_locks::FlockWrapper &lock, uint32_t &reqid);

int saunafs_setlk_recv();
int saunafs_setlk_interrupt(const safs_locks::InterruptData &data);

} // extern "C"
