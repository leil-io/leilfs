/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2018 Skytechnology sp. z o.o.
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

#include "common/platform.h"

#include "mount/fuse/sfs_fuse.h"

#include <atomic>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "common/lru_cache.h"
#include "common/massert.h"
#include "common/small_vector.h"
#include "common/special_inode_defs.h"
#include "common/time_utils.h"
#include "mount/fuse/lock_conversion.h"
#include "mount/fuse/sfs_fuselib/metadata.h"
#include "mount/group_cache.h"
#include "mount/sauna_client.h"
#include "mount/sauna_client_context.h"
#include "mount/thread_safe_map.h"
#include "protocol/SFSCommunication.h"
#include "protocol/cltoma.h"

#include "grp.h"
#include "pwd.h"
#include "unistd.h"

#if SPECIAL_INODE_ROOT != FUSE_ROOT_ID
#error FUSE_ROOT_ID is not equal to SPECIAL_INODE_ROOT
#endif

/**
 * Function checking if types are equal, ignoring constness
 */
template <class A, class B>
void checkTypesEqual(const A& a, const B& b) {
	static_assert(std::is_same<decltype(a), decltype(b)>::value,
			"Types don't match");
}

#if defined(__FreeBSD__)
#include <sys/user.h>
#endif

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/sysctl.h>

static SaunaClient::Context getSecondaryGroups(fuse_req_t &req) {
	auto fuse_ctx = fuse_req_ctx(req);
	SaunaClient::Context ctx(fuse_ctx->uid, fuse_ctx->gid, fuse_ctx->pid, 0);

	int mib_path[4];
	struct kinfo_proc kinfo;
	size_t len;

	len = 4;
	sysctlnametomib("kern.proc.pid", mib_path, &len);
	mib_path[3] = ctx.pid;

	len = sizeof(kinfo);
	memset(&kinfo, 0, sizeof(kinfo));
	if (sysctl(mib_path, 4, &kinfo, &len, nullptr, 0) == 0) {
#if defined(__APPLE__)
		ctx.gids.resize(kinfo.kp_eproc.e_ucred.cr_ngroups + 1);
		ctx.gids[0] = ctx.gid;
		for (int i = 0; i < kinfo.kp_eproc.e_ucred.cr_ngroups; ++i) {
			ctx.gids[i + 1] = kinfo.kp_eproc.e_ucred.cr_groups[i];
		}
#else
		ctx.gids.resize(kinfo.ki_ngroups + 1);
		ctx.gids[0] = ctx.gid;
		for (int i = 0; i < kinfo.ki_ngroups; ++i) {
			ctx.gids[i + 1] = kinfo.ki_groups[i];
		}
#endif
	}

	return ctx;
}

#else

static SaunaClient::Context getSecondaryGroups(fuse_req_t &req) {
	static const int kMaxGroups = GroupCache::kDefaultGroupsSize - 1;

	auto fuse_ctx = fuse_req_ctx(req);
	SaunaClient::Context ctx(fuse_ctx->uid, fuse_ctx->gid, fuse_ctx->pid, 0);

	assert(ctx.gids.size() == 1);
	ctx.gids.resize(kMaxGroups + 1);

	int getgroups_ret = fuse_req_getgroups(req, kMaxGroups, ctx.gids.data() + 1);
	ctx.gids.resize(std::max(1, getgroups_ret + 1));
	if (getgroups_ret > kMaxGroups) {
		getgroups_ret = fuse_req_getgroups(req, ctx.gids.size() - 1, ctx.gids.data() + 1);

		// we include check for case when number of groups has been changed between
		// calls to fuse_req_getgroups
		ctx.gids.resize(std::max(1, std::min<int>(getgroups_ret + 1, ctx.gids.size())));
	}

	return ctx;
}

#endif

typedef LruCache<
	LruCacheOption::UseTreeMap,
	LruCacheOption::Reentrant,
	SaunaClient::Context,
	uint32_t> PidToContextCache;

static PidToContextCache gPidToContextCache(std::chrono::seconds(10), 1024);

static void updateGroupsForContext(fuse_req_t &req, SaunaClient::Context &ctx) {
	static_assert(sizeof(gid_t) == sizeof(SaunaClient::Context::IdType),
	              "Invalid IdType to call fuse_req_getgroups");

	SaunaClient::Context cached_context = gPidToContextCache.get(SteadyClock::now(), ctx.pid,
		[&req](SaunaClient::Context::IdType /*pid*/) { return getSecondaryGroups(req); });
	if (!cached_context.isValid()) {
		safs::log_warn("Failed to extract context information (secondary groups)");
		return;
	}
	if (cached_context.uid != ctx.uid || cached_context.gid != ctx.gid) {
		// uid and/or gid changed - this may happen due to setuid or other changes,
		// so let's invalidate cache and try again
		gPidToContextCache.erase(ctx.uid);
		cached_context = gPidToContextCache.get(SteadyClock::now(), ctx.pid,
			[&req](SaunaClient::Context::IdType /*pid*/) { return getSecondaryGroups(req); });
	}
	ctx.gids = std::move(cached_context.gids);
	SaunaClient::updateGroups(ctx);
}

/**
 * A function converting fuse_ctx to SaunaClient::Context (without secondary groups update)
 */
SaunaClient::Context get_reduced_context(fuse_req_t& req) {
	auto fuse_ctx = fuse_req_ctx(req);
	mode_t umask = fuse_ctx->umask;
	auto ret = SaunaClient::Context(fuse_ctx->uid, fuse_ctx->gid, fuse_ctx->pid, umask);
	return ret;
}

/**
 * A function converting fuse_ctx to SaunaClient::Context (with secondary groups update)
 */
SaunaClient::Context get_context(fuse_req_t& req) {
	auto ret = get_reduced_context(req);
	if (ret.pid > 0) {
		updateGroupsForContext(req, ret);
	}
	return ret;
}


/**
 * A wrapper that allows one to use fuse_file_info as if it was SaunaClient::FileInfo object.
 *  During construction, SaunaClient::FileInfo object is initialized with information from the
 *  provided fuse_file_info.
 *  During destruction, the fuse_file_info is updated to reflect any changes made to the
 *  SaunaClient::FileInfo object.
 */
class fuse_file_info_wrapper {
public:
	fuse_file_info_wrapper(fuse_file_info* fi)
			: fuse_fi_(fi), fs_fi_(fuse_fi_
					? new SaunaClient::FileInfo(fi->flags, fi->direct_io, fi->keep_cache, fi->fh,
					fi->lock_owner)
					: nullptr) {
		if (fs_fi_) {
			assert(fuse_fi_);
		} else {
			assert(!fuse_fi_);
		}
	}
	operator SaunaClient::FileInfo*() {
		return fs_fi_.get();
	}
	~fuse_file_info_wrapper() {
		if (fs_fi_) {
			fuse_fi_->direct_io  = fs_fi_->direct_io;
			fuse_fi_->fh         = fs_fi_->fh;
			fuse_fi_->flags      = fs_fi_->flags;
			fuse_fi_->keep_cache = fs_fi_->keep_cache;
			checkTypesEqual(fuse_fi_->direct_io , fs_fi_->direct_io);
			checkTypesEqual(fuse_fi_->fh        , fs_fi_->fh);
			checkTypesEqual(fuse_fi_->flags     , fs_fi_->flags);
			checkTypesEqual(fuse_fi_->keep_cache, fs_fi_->keep_cache);
		}
	}

private:
	fuse_file_info* fuse_fi_;
	std::unique_ptr<SaunaClient::FileInfo> fs_fi_;
};

/**
 * A function converting SaunaFS::EntryParam to fuse_entry_param
 */
fuse_entry_param make_fuse_entry_param(const SaunaClient::EntryParam& e) {
	fuse_entry_param ret;
	memset(&ret, 0, sizeof(ret));
	checkTypesEqual(ret.generation,    e.generation);
	checkTypesEqual(ret.attr,          e.attr);
	checkTypesEqual(ret.attr_timeout,  e.attr_timeout);
	checkTypesEqual(ret.entry_timeout, e.entry_timeout);
	ret.ino           = e.ino;
	ret.generation    = e.generation;
	ret.attr          = e.attr;
	ret.attr_timeout  = e.attr_timeout;
	ret.entry_timeout = e.entry_timeout;
	return ret;
}

#ifndef EDQUOT
# define EDQUOT ENOSPC
#endif
#ifndef ENOATTR
# ifdef ENODATA
#  define ENOATTR ENODATA
# else
#  define ENOATTR ENOENT
# endif
#endif

ThreadSafeMap<std::uintptr_t, safs_locks::InterruptData> gLockInterruptData;

void sfs_statfs(fuse_req_t req,fuse_ino_t ino) {
	try {
		auto ctx = get_context(req);
		auto a = SaunaClient::statfs(ctx, ino);
		fuse_reply_statfs(req, &a);
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

void sfs_access(fuse_req_t req, fuse_ino_t ino, int mask) {
	try {
		auto ctx = get_context(req);
		SaunaClient::access(ctx, ino, mask);
		fuse_reply_err(req, 0);
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

void sfs_lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
	try {
		auto ctx = get_context(req);
		auto fuseEntryParam = make_fuse_entry_param(
				SaunaClient::lookup(ctx, parent, name));
		fuse_reply_entry(req, &fuseEntryParam);
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

void sfs_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *) {
	try {
		// FileInfo not needed, not conducive to optimization
		auto ctx = get_context(req);
		auto a = SaunaClient::getattr(ctx, ino);
		fuse_reply_attr(req, &a.attr, a.attrTimeout);
	} catch (SaunaClient::RequestException &e) {
		if (e.saunafs_error_code == SAUNAFS_ERROR_ENOENT) {
			// if the file is not found despite having the inode, it's probably
			// a stale file handle. For example, from a bind mount.
			fuse_reply_err(req, ESTALE);
		} else {
			fuse_reply_err(req, e.system_error_code);
		}
	}
}

void sfs_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *stbuf, int to_set,
		struct fuse_file_info *) {
	try {
		static_assert(SAUNAFS_SET_ATTR_MODE      == FUSE_SET_ATTR_MODE,      "incompatible");
		static_assert(SAUNAFS_SET_ATTR_UID       == FUSE_SET_ATTR_UID,       "incompatible");
		static_assert(SAUNAFS_SET_ATTR_GID       == FUSE_SET_ATTR_GID,       "incompatible");
		static_assert(SAUNAFS_SET_ATTR_SIZE      == FUSE_SET_ATTR_SIZE,      "incompatible");
		static_assert(SAUNAFS_SET_ATTR_ATIME     == FUSE_SET_ATTR_ATIME,     "incompatible");
		static_assert(SAUNAFS_SET_ATTR_MTIME     == FUSE_SET_ATTR_MTIME,     "incompatible");
		static_assert(SAUNAFS_SET_ATTR_ATIME_NOW == FUSE_SET_ATTR_ATIME_NOW, "incompatible");
		static_assert(SAUNAFS_SET_ATTR_MTIME_NOW == FUSE_SET_ATTR_MTIME_NOW, "incompatible");
		auto ctx = get_context(req);
		auto a = SaunaClient::setattr(ctx, ino, stbuf, to_set);
		fuse_reply_attr(req, &a.attr, a.attrTimeout);
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

void sfs_mknod(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode, dev_t rdev) {
	try {
		auto ctx = get_context(req);
		auto fuseEntryParam = make_fuse_entry_param(
				SaunaClient::mknod(ctx, parent, name, mode, rdev));
		fuse_reply_entry(req, &fuseEntryParam);
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

void sfs_unlink(fuse_req_t req, fuse_ino_t parent, const char *name) {
	try {
		auto ctx = get_context(req);
		SaunaClient::unlink(ctx, parent, name);
		fuse_reply_err(req, 0);
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

void sfs_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode) {
	try {
		auto ctx = get_context(req);
		auto fuseEntryParam = make_fuse_entry_param(
				SaunaClient::mkdir(ctx, parent, name, mode));
		fuse_reply_entry(req, &fuseEntryParam);
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

void sfs_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name) {
	try {
		auto ctx = get_context(req);
		SaunaClient::rmdir(ctx, parent, name);
		fuse_reply_err(req, 0);
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

void sfs_symlink(fuse_req_t req, const char *path, fuse_ino_t parent, const char *name) {
	try {
		auto ctx = get_context(req);
		auto fuseEntryParam = make_fuse_entry_param(
				SaunaClient::symlink(ctx, path, parent, name));
		fuse_reply_entry(req, &fuseEntryParam);
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

void sfs_readlink(fuse_req_t req, fuse_ino_t ino) {
	try {
		auto ctx = get_context(req);
		fuse_reply_readlink(req,
				SaunaClient::readlink(ctx, ino).c_str());
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

void sfs_rename(fuse_req_t req, fuse_ino_t parent, const char *name, fuse_ino_t newparent,
		const char *newname, unsigned int flags) {
	(void)flags; // FIXME(haze) Add handling of RENAME_EXCHANGE FLAG
	try {
		auto ctx = get_context(req);
		SaunaClient::rename(ctx, parent, name, newparent, newname);
		fuse_reply_err(req, 0);
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

void sfs_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent, const char *newname) {
	try {
		auto ctx = get_context(req);
		auto fuseEntryParam = make_fuse_entry_param(
				SaunaClient::link(ctx, ino, newparent, newname));
		fuse_reply_entry(req, &fuseEntryParam);
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

void sfs_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	try {
		auto ctx = get_context(req);
		SaunaClient::opendir(ctx, ino);
		//opendir can be called asynchronously
		static std::atomic<std::uint64_t> opendirSessionID{0};
		fi->fh = opendirSessionID++;
		SaunaClient::update_readdir_session(fi->fh, 0);
		fuse_reply_open(req, fi);
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

void sfs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
		struct fuse_file_info* fi) {
	try {
		char buffer[READDIR_BUFFSIZE];
		if (size > READDIR_BUFFSIZE) {
			size = READDIR_BUFFSIZE;
		}
		size_t bytesInBuffer = 0;
		bool end = false;
		uint64_t nextEntryIno = 0;
		while (!end) {
			// Calculate approximated number of entries which will fit in the buffer. If this
			// number is smaller than the actual value, SaunaClient::readdir will be called more
			// than once for a single sfs_readdir (this will eg. generate more oplog entries than
			// one might expect). If it's bigger, the code will be slightly less optimal because
			// superfluous entries will be extracted by SaunaClient::readdir and then discarded by
			// us. Using maxEntries=+inf makes the complexity of the getdents syscall O(n^2).
			// The expression below generates some upper bound of the actual number of entries
			// to be returned (because fuse adds 24 bytes of metadata to each file name in
			// fuse_add_direntry and aligns size up to 8 bytes), so SaunaClient::readdir
			// should be called only once.
			size_t maxEntries = 1 + size / 32;
			// Now extract some entries and rewrite them into the buffer.
			auto ctx = get_context(req);
			auto fsDirEntries = SaunaClient::readdir(ctx, fi->fh, ino, off, maxEntries);
			if (fsDirEntries.empty()) {
				break; // no more entries (we don't need to set 'end = true' here to end the loop)
			}
			for (const auto& e : fsDirEntries) {
				size_t entrySize = fuse_add_direntry(req,
						buffer + bytesInBuffer, size,
						e.name.c_str(), &(e.attr), e.nextEntryOffset);
				nextEntryIno = e.attr.st_ino;
				if (entrySize > size) {
					end = true; // buffer is full
					break;
				}
				off = e.nextEntryOffset; // update offset of the next call to SaunaClient::readdir
				bytesInBuffer += entrySize;
				size -= entrySize; // decrease remaining buffer size
			}
		}
		SaunaClient::update_readdir_session(fi->fh, nextEntryIno);
		fuse_reply_buf(req, buffer, bytesInBuffer);
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

void sfs_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
	try {
		SaunaClient::releasedir(ino);
		SaunaClient::drop_readdir_session(fi->fh);
		fuse_reply_err(req, 0);
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

void sfs_create(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode,
		struct fuse_file_info *fi) {
	try {
		auto ctx = get_context(req);
		auto e = make_fuse_entry_param(SaunaClient::create(
				ctx, parent, name, mode, fuse_file_info_wrapper(fi)));
		if (fuse_reply_create(req, &e, fi) == -ENOENT) {
			SaunaClient::remove_file_info(fuse_file_info_wrapper(fi));
		}
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

void sfs_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	try {
		auto ctx = get_context(req);
		SaunaClient::open(ctx, ino, fuse_file_info_wrapper(fi));
		if (fuse_reply_open(req, fi) == -ENOENT) {
			SaunaClient::remove_file_info(fuse_file_info_wrapper(fi));
		}
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

void sfs_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	try {
		SaunaClient::release(ino, fuse_file_info_wrapper(fi));
		fuse_reply_err(req, 0);
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

void sfs_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi) {
	try {
		auto ctx = get_reduced_context(req);
		if (SaunaClient::isSpecialInode(ino)) {
			auto ret = SaunaClient::read_special_inode(
					ctx, ino, size, off, fuse_file_info_wrapper(fi));
			fuse_reply_buf(req, (char*) ret.data(), ret.size());
		} else {
			ReadCache::Result ret = SaunaClient::read(
					ctx, ino, size, off, fuse_file_info_wrapper(fi));

			small_vector<struct iovec, 8> reply;
			ret.toIoVec(reply, off, size);
			fuse_reply_iov(req, reply.data(), reply.size());
		}
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

void sfs_write(fuse_req_t req, fuse_ino_t ino, const char *buf, size_t size, off_t off,
		struct fuse_file_info *fi) {
	try {
		auto ctx = get_reduced_context(req);
		fuse_reply_write(req, SaunaClient::write(
				ctx, ino, buf, size, off, fuse_file_info_wrapper(fi)));
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

void sfs_flush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	try {
		auto ctx = get_reduced_context(req);
		SaunaClient::flush(ctx, ino, fuse_file_info_wrapper(fi));
		fuse_reply_err(req, 0);
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

void sfs_fsync(fuse_req_t req, fuse_ino_t ino, int datasync, struct fuse_file_info *fi) {
	try {
		auto ctx = get_reduced_context(req);
		SaunaClient::fsync(ctx, ino, datasync, fuse_file_info_wrapper(fi));
		fuse_reply_err(req, 0);
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

#if defined(__APPLE__)
void sfs_setxattr (fuse_req_t req, fuse_ino_t ino, const char *name, const char *value,
		size_t size, int flags, uint32_t position) {
#else
void sfs_setxattr (fuse_req_t req, fuse_ino_t ino, const char *name, const char *value,
		size_t size, int flags) {
	uint32_t position=0;
#endif
	try {
		auto ctx = get_context(req);
		SaunaClient::setxattr(ctx, ino, name, value, size, flags, position);
		fuse_reply_err(req, 0);
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

#if defined(__APPLE__)
void sfs_getxattr (fuse_req_t req, fuse_ino_t ino, const char *name, size_t size,
		uint32_t position) {
#else
void sfs_getxattr (fuse_req_t req, fuse_ino_t ino, const char *name, size_t size) {
	uint32_t position=0;
#endif /* __APPLE__ */
	try {
		auto ctx = get_context(req);
		auto a = SaunaClient::getxattr(ctx, ino, name, size, position);
		if (size == 0) {
			fuse_reply_xattr(req, a.valueLength);
		} else {
			fuse_reply_buf(req,(const char*)a.valueBuffer.data(), a.valueLength);
		}
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

void sfs_listxattr (fuse_req_t req, fuse_ino_t ino, size_t size) {
	try {
		auto ctx = get_context(req);
		auto a = SaunaClient::listxattr(ctx, ino, size);
		if (size == 0) {
			fuse_reply_xattr(req, a.valueLength);
		} else {
			fuse_reply_buf(req,(const char*)a.valueBuffer.data(), a.valueLength);
		}
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

void sfs_removexattr (fuse_req_t req, fuse_ino_t ino, const char *name) {
	try {
		auto ctx = get_context(req);
		SaunaClient::removexattr(ctx, ino, name);
		fuse_reply_err(req, 0);
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

void safs_flock_interrupt(fuse_req_t req, void *data) {
	auto interrupt_data = gLockInterruptData.take(reinterpret_cast<std::uintptr_t>(data));

	// if there was any data
	if (interrupt_data.first) {
		// handle interrupt
		SaunaClient::flock_interrupt(interrupt_data.second);
		fuse_reply_err(req, EINTR);
	}
}

void safs_setlk_interrupt(fuse_req_t req, void *data) {
	auto interrupt_data = gLockInterruptData.take(reinterpret_cast<std::uintptr_t>(data));

	// if there was any data
	if (interrupt_data.first) {
		// handle interrupt
		SaunaClient::setlk_interrupt(interrupt_data.second);
		fuse_reply_err(req, EINTR);
	}
}

void safs_getlk(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi, struct flock *lock) {
	try {
		if (!safs_locks::posixOpValid(lock->l_type)) {
			fuse_reply_err(req, EINVAL);
			return;
		}

		safs_locks::FlockWrapper safslock = safs_locks::convertPLock(*lock, 1);
		auto ctx = get_context(req);
		SaunaClient::getlk(ctx, ino, fuse_file_info_wrapper(fi), safslock);
		struct flock retlock = safs_locks::convertToFlock(safslock);
		fuse_reply_lock(req, &retlock);
	} catch (SaunaClient::RequestException& e) {
		fuse_reply_err(req, e.system_error_code);
	}
}

void safs_setlk(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi, struct flock *lock, int sleep) {
	std::uintptr_t interrupt_data_key = gLockInterruptData.generateKey();
	try {
		if (fuse_req_interrupted(req)) {
			fuse_reply_err(req, EINTR);
			return;
		}

		if (!safs_locks::posixOpValid(lock->l_type)) {
			fuse_reply_err(req, EINVAL);
			return;
		}

		safs_locks::FlockWrapper safslock = safs_locks::convertPLock(*lock, sleep);
		auto ctx = get_context(req);
		uint32_t reqid = SaunaClient::setlk_send(ctx, ino, fuse_file_info_wrapper(fi),
				safslock);

		// save interrupt data in static structure
		gLockInterruptData.put(interrupt_data_key,
				       safs_locks::InterruptData(fi->lock_owner, ino, reqid));

		// register interrupt handle
		if (safslock.l_type == safs_locks::kShared || safslock.l_type == safs_locks::kExclusive) {
			fuse_req_interrupt_func(req, safs_setlk_interrupt,
					reinterpret_cast<void*>(interrupt_data_key));
		}

		// setlk_recv() must run on the same thread as setlk_send().
		SaunaClient::setlk_recv();

		// release the memory
		auto interrupt_data = gLockInterruptData.take(interrupt_data_key);
		if (interrupt_data.first) {
			fuse_reply_err(req, 0);
		}
	} catch (SaunaClient::RequestException& e) {
		// release the memory
		auto interrupt_data = gLockInterruptData.take(interrupt_data_key);
		if (interrupt_data.first) {
			fuse_reply_err(req, e.system_error_code);
		}
	}
}

void safs_flock(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi, int op) {
	std::uintptr_t interrupt_data_key = gLockInterruptData.generateKey();
	try {
		if (fuse_req_interrupted(req)) {
			fuse_reply_err(req, EINTR);
			return;
		}

		if (!safs_locks::flockOpValid(op)) {
			fuse_reply_err(req, EINVAL);
			return;
		}

		uint32_t safs_op = safs_locks::flockOpConv(op);
		auto ctx = get_context(req);
		uint32_t reqid = SaunaClient::flock_send(ctx, ino,
			fuse_file_info_wrapper(fi), safs_op);

		// save interrupt data in static structure
		gLockInterruptData.put(interrupt_data_key,
				       safs_locks::InterruptData(fi->lock_owner, ino, reqid));
		// register interrupt handle
		if (safs_op == safs_locks::kShared || safs_op == safs_locks::kExclusive) {
			fuse_req_interrupt_func(req, safs_flock_interrupt,
					reinterpret_cast<void*>(interrupt_data_key));
		}

		SaunaClient::flock_recv();

		// release the memory
		auto interrupt_data = gLockInterruptData.take(interrupt_data_key);
		if (interrupt_data.first) {
			fuse_reply_err(req, 0);
		}
	} catch (SaunaClient::RequestException& e) {
		// release the memory
		auto interrupt_data = gLockInterruptData.take(interrupt_data_key);
		if (interrupt_data.first) {
			fuse_reply_err(req, e.system_error_code);
		}
	}
}
