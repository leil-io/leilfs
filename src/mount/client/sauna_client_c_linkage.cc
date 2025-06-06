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

#include "common/platform.h"

#include "client/sauna_client_c_linkage.h"

typedef SaunaClient::EntryParam EntryParam;
typedef SaunaClient::Context Context;
typedef SaunaClient::AttrReply AttrReply;
typedef SaunaClient::FileInfo FileInfo;
typedef SaunaClient::BytesWritten BytesWritten;
typedef SaunaClient::DirEntry DirEntry;
typedef SaunaClient::RequestException RequestException;

int saunafs_fs_init(SaunaClient::FsInitParams &params) {
	try {
		SaunaClient::fs_init(params);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_lookup(Context &ctx, inode_t parent, const char *name, EntryParam &param) {
	try {
		param = SaunaClient::lookup(ctx, parent, name);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_mknod(Context &ctx, inode_t parent, const char *name, mode_t mode, dev_t rdev,
		EntryParam &param) {
	try {
		param = SaunaClient::mknod(ctx, parent, name, mode, rdev);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_link(Context ctx, inode_t inode, inode_t parent, const char *name,
		EntryParam &param) {
	try {
		param = SaunaClient::link(ctx, inode, parent, name);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_symlink(Context ctx, const char *link, inode_t parent, const char *name,
		EntryParam &param) {
	try {
		param = SaunaClient::symlink(ctx, link, parent, name);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_unlink(Context &ctx, inode_t parent, const char *name) {
	try {
		SaunaClient::unlink(ctx, parent, name);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_undel(Context &ctx, inode_t ino) {
	try {
		SaunaClient::undel(ctx, ino);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_open(Context &ctx, inode_t ino, FileInfo *fi) {
	try {
		SaunaClient::open(ctx, ino, fi);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_getattr(Context &ctx, inode_t ino, AttrReply &reply) {
	try {
		reply = SaunaClient::getattr(ctx, ino);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_makesnapshot(Context &ctx, inode_t ino, inode_t dst_parent,
                         const std::string &dst_name, bool can_overwrite,
                         SaunaClient::JobId &job_id) {
	try {
		job_id = SaunaClient::makesnapshot(ctx, ino, dst_parent, dst_name,
		                                   can_overwrite);
		return SAUNAFS_STATUS_OK;
	} catch (RequestException &e) {
		job_id = 0;
		return e.saunafs_error_code;
	} catch (...) {
		job_id = 0;
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_getgoal(Context &ctx, inode_t ino, std::string &goal) {
	try {
		goal = SaunaClient::getgoal(ctx, ino);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_setattr(Context &ctx, inode_t ino, struct stat *stbuf, int to_set,
	             AttrReply &reply) {
	try {
		reply = SaunaClient::setattr(ctx, ino, stbuf, to_set);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_setgoal(Context &ctx, inode_t ino, const std::string &goal_name, uint8_t smode) {
	try {
		SaunaClient::setgoal(ctx, ino, goal_name, smode);
		return SAUNAFS_STATUS_OK;
	} catch (RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_read(Context &ctx, inode_t ino, size_t size, off_t off,
                 FileInfo *fi, ReadCache::Result &result) {
	try {
		result = SaunaClient::read(ctx, ino, size, off, fi);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		result = ReadCache::Result();
		return e.saunafs_error_code;
	} catch (...) {
		result = ReadCache::Result();
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_read_special_inode(Context &ctx, inode_t ino, size_t size, off_t off,
                               FileInfo *fi,
                               std::vector<uint8_t> &special_inode) {
	try {
		special_inode =
		    SaunaClient::read_special_inode(ctx, ino, size, off, fi);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		special_inode.clear();
		return e.saunafs_error_code;
	} catch (...) {
		special_inode.clear();
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_write(Context &ctx, inode_t ino, const char *buf, size_t size,
                  off_t off, FileInfo *fi, ssize_t &bytes_written) {
	try {
		bytes_written = SaunaClient::write(ctx, ino, buf, size, off, fi);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		bytes_written = 0;
		return e.saunafs_error_code;
	} catch (...) {
		bytes_written = 0;
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_release(inode_t ino, FileInfo *fi) {
	try {
		SaunaClient::release(ino, fi);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_flush(Context &ctx, inode_t ino, FileInfo *fi) {
	try {
		SaunaClient::flush(ctx, ino, fi);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_fsync(Context &ctx, inode_t ino, int datasync, FileInfo* fi) {
	try {
		SaunaClient::fsync(ctx, ino, datasync, fi);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_statfs(uint64_t *totalspace, uint64_t *availspace, uint64_t *trashspace,
                   uint64_t *reservedspace, inode_t *inodes) {
	try {
		SaunaClient::statfs(totalspace, availspace, trashspace, reservedspace, inodes);
		return SAUNAFS_STATUS_OK;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

void saunafs_fs_term() {
	try {
		SaunaClient::fs_term();
	} catch (...) {
		// ignore
	}
}

bool saunafs_isSpecialInode(inode_t ino) {
	return SaunaClient::isSpecialInode(ino);
}

int saunafs_readdir(Context &ctx, uint64_t opendirSessionID, inode_t ino,
                    off_t off, size_t max_entries,
                    std::vector<DirEntry> &entries) {
	try {
		entries = SaunaClient::readdir(ctx, opendirSessionID, ino, off, max_entries);
		uint64_t nextEntryIno = (entries.empty()) ? 0 : entries.back().attr.st_ino;
		SaunaClient::update_readdir_session(opendirSessionID, nextEntryIno);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		entries.clear();
		return e.saunafs_error_code;
	} catch (...) {
		entries.clear();
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_readlink(Context &ctx, inode_t ino, std::string &link) {
	try {
		link = SaunaClient::readlink(ctx, ino);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_readreserved(Context &ctx, SaunaClient::NamedInodeOffset off,
                         SaunaClient::NamedInodeOffset max_entries,
                         std::vector<NamedInodeEntry> &inode_entries) {
	try {
		inode_entries = SaunaClient::readreserved(ctx, off, max_entries);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		inode_entries.clear();
		return e.saunafs_error_code;
	} catch (...) {
		inode_entries.clear();
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_readtrash(Context &ctx, SaunaClient::NamedInodeOffset off,
                      SaunaClient::NamedInodeOffset max_entries,
                      std::vector<NamedInodeEntry> &trash_entries) {
	try {
		trash_entries = SaunaClient::readtrash(ctx, off, max_entries);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		trash_entries.clear();
		return e.saunafs_error_code;
	} catch (...) {
		trash_entries.clear();
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_opendir(Context &ctx, inode_t ino) {
	try {
		SaunaClient::opendir(ctx, ino);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_releasedir(inode_t ino, uint64_t opendirSessionID) {
	try {
		SaunaClient::releasedir(ino);
		SaunaClient::drop_readdir_session(opendirSessionID);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_mkdir(Context &ctx, inode_t parent, const char *name, mode_t mode,
		EntryParam &entry_param) {
	try {
		entry_param = SaunaClient::mkdir(ctx, parent, name, mode);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_rmdir(Context &ctx, inode_t parent, const char *name) {
	try {
		SaunaClient::rmdir(ctx, parent, name);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_rename(Context &ctx, inode_t parent, const char *name, inode_t new_parent,
	            const char *new_name) {
	try {
		SaunaClient::rename(ctx, parent, name, new_parent, new_name);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_update_groups(Context &ctx) {
	try {
		SaunaClient::updateGroups(ctx);
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch(...) {
		return SAUNAFS_ERROR_IO;
	}
	return SAUNAFS_STATUS_OK;
}

int saunafs_setxattr(Context ctx, inode_t ino, const char *name, const char *value,
		size_t size, int flags) {
	try {
		SaunaClient::setxattr(ctx, ino, name, value, size, flags, 0);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_getchunksinfo(Context &ctx, inode_t ino, uint32_t chunk_index,
                          uint32_t chunk_count,
                          std::vector<ChunkWithAddressAndLabel> &chunks) {
	try {
		chunks = SaunaClient::getchunksinfo(ctx, ino, chunk_index, chunk_count);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		chunks.clear();
		return e.saunafs_error_code;
	} catch (...) {
		chunks.clear();
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_getchunkservers(std::vector<ChunkserverListEntry> &chunkservers) {
	try {
		chunkservers = SaunaClient::getchunkservers();
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		chunkservers.clear();
		return e.saunafs_error_code;
	} catch (...) {
		chunkservers.clear();
		return SAUNAFS_ERROR_IO;
	}
}


int saunafs_getlk(Context &ctx, inode_t ino,
	           SaunaClient::FileInfo *fi, safs_locks::FlockWrapper &lock) {
	try {
		SaunaClient::getlk(ctx, ino, fi, lock);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_setlk_send(Context &ctx, inode_t ino, SaunaClient::FileInfo *fi,
                       safs_locks::FlockWrapper &lock, uint32_t &reqid) {
	try {
		reqid = SaunaClient::setlk_send(ctx, ino, fi, lock);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		reqid = 0;
		return e.saunafs_error_code;
	} catch (...) {
		reqid = 0;
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_setlk_recv() {
	try {
		SaunaClient::setlk_recv();
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_setlk_interrupt(const safs_locks::InterruptData &data) {
	try {
		SaunaClient::setlk_interrupt(data);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_getxattr(Context ctx, inode_t ino, const char *name,
	              size_t size, SaunaClient::XattrReply &xattr_reply) {
	try {
		xattr_reply = SaunaClient::getxattr(ctx, ino, name, size, 0);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}
int saunafs_listxattr(Context ctx, inode_t ino, size_t size,
	               SaunaClient::XattrReply &xattr_reply) {
	try {
		xattr_reply = SaunaClient::listxattr(ctx, ino, size);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}

int saunafs_removexattr(Context ctx, inode_t ino, const char *name) {
	try {
		SaunaClient::removexattr(ctx, ino, name);
		return SAUNAFS_STATUS_OK;
	} catch (const RequestException &e) {
		return e.saunafs_error_code;
	} catch (...) {
		return SAUNAFS_ERROR_IO;
	}
}
