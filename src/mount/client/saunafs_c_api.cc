/*

   Copyright 2017 Skytechnology sp. z o.o.
   Copyright 2023 Leil Storage OÜ

   This file is part of SaunaFS.

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

#include <cassert>

#include <system_error>

#include "saunafs_c_api.h"
#include "errors/saunafs_error_codes.h"
#include "common/md5.h"
#include "common/small_vector.h"
#include "mount/client/iovec_traits.h"
#include "mount/fuse/lock_conversion.h"

#include "client.h"

using namespace saunafs;

void sau_set_default_init_params(struct sau_init_params *params,
                                 const char *host, const char *port,
                                 const char *mountpoint) {
	assert(params != nullptr);
	params->bind_host = nullptr;
	params->host = host;
	params->port = port;
	params->meta = false;
	params->mountpoint = mountpoint;
	params->subfolder = SaunaClient::FsInitParams::kDefaultSubfolder;
	params->password = nullptr;
	params->md5_pass = nullptr;
	params->do_not_remember_password = SaunaClient::FsInitParams::kDefaultDoNotRememberPassword;
	params->delayed_init = SaunaClient::FsInitParams::kDefaultDelayedInit;
	params->report_reserved_period = SaunaClient::FsInitParams::kDefaultReportReservedPeriod;

	params->io_retries = SaunaClient::FsInitParams::kDefaultIoRetries;
	params->chunkserver_latency_sort = SaunaClient::FsInitParams::kDefaultChunkserverLatencySort;
	params->chunkserver_round_time_ms = SaunaClient::FsInitParams::kDefaultRoundTime;
	params->chunkserver_connect_timeout_ms = SaunaClient::FsInitParams::kDefaultChunkserverConnectTo;
	params->chunkserver_wave_read_timeout_ms = SaunaClient::FsInitParams::kDefaultChunkserverWaveReadTo;
	params->total_read_timeout_ms = SaunaClient::FsInitParams::kDefaultChunkserverTotalReadTo;
	params->cache_expiration_time_ms = SaunaClient::FsInitParams::kDefaultCacheExpirationTime;
	params->readahead_max_window_size_kB = SaunaClient::FsInitParams::kDefaultReadaheadMaxWindowSize;
	params->read_cache_max_size_percentage = SaunaClient::FsInitParams::kDefaultReadCacheMaxSizePercentage;
	params->prefetch_xor_stripes = SaunaClient::FsInitParams::kDefaultPrefetchXorStripes;
	params->bandwidth_overuse = SaunaClient::FsInitParams::kDefaultBandwidthOveruse;

	params->write_cache_size = SaunaClient::FsInitParams::kDefaultWriteCacheSize;
	params->write_workers = SaunaClient::FsInitParams::kDefaultWriteWorkers;
	params->write_window_size = SaunaClient::FsInitParams::kDefaultWriteWindowSize;
	params->chunkserver_write_timeout_ms = SaunaClient::FsInitParams::kDefaultChunkserverWriteTo;
	params->cache_per_inode_percentage = SaunaClient::FsInitParams::kDefaultCachePerInodePercentage;
	params->symlink_cache_timeout_s = SaunaClient::FsInitParams::kDefaultSymlinkCacheTimeout;

	params->debug_mode = SaunaClient::FsInitParams::kDefaultDebugMode;
	params->keep_cache = SaunaClient::FsInitParams::kDefaultKeepCache;
	params->direntry_cache_timeout = SaunaClient::FsInitParams::kDefaultDirentryCacheTimeout;
	params->direntry_cache_size = SaunaClient::FsInitParams::kDefaultDirentryCacheSize;
	params->entry_cache_timeout = SaunaClient::FsInitParams::kDefaultEntryCacheTimeout;
	params->attr_cache_timeout = SaunaClient::FsInitParams::kDefaultAttrCacheTimeout;
	params->mkdir_copy_sgid = SaunaClient::FsInitParams::kDefaultMkdirCopySgid;
	params->sugid_clear_mode = (sau_sugid_clear_mode)SaunaClient::FsInitParams::kDefaultSugidClearMode;
	params->use_rw_lock = SaunaClient::FsInitParams::kDefaultUseRwLock;
	params->acl_cache_timeout = SaunaClient::FsInitParams::kDefaultAclCacheTimeout;
	params->acl_cache_size = SaunaClient::FsInitParams::kDefaultAclCacheSize;

	params->verbose = SaunaClient::FsInitParams::kDefaultVerbose;

	params->io_limits_config_file = nullptr;

	// statically assert that all SugidClearMode value are covered and consistent
	static_assert(sizeof(sau_sugid_clear_mode) >= sizeof(SugidClearMode), "");
	for (int dummy = SAUNAFS_SUGID_CLEAR_MODE_NEVER; dummy < SAUNAFS_SUGID_CLEAR_MODE_END_; ++dummy) {
		switch (SugidClearMode(dummy)) { // exhaustive match check thanks to 'enum class'
			#define STATIC_ASSERT_SUGID_CLEAR_MODE(KNAME,CENUM) \
					case SugidClearMode::KNAME: \
						static_assert((sau_sugid_clear_mode)SugidClearMode::KNAME  == CENUM, \
							"sau_sugid_clear_mode incompatible with SugidClearMode");
				STATIC_ASSERT_SUGID_CLEAR_MODE(kNever, SAUNAFS_SUGID_CLEAR_MODE_NEVER);
				STATIC_ASSERT_SUGID_CLEAR_MODE(kAlways, SAUNAFS_SUGID_CLEAR_MODE_ALWAYS);
				STATIC_ASSERT_SUGID_CLEAR_MODE(kOsx, SAUNAFS_SUGID_CLEAR_MODE_OSX);
				STATIC_ASSERT_SUGID_CLEAR_MODE(kBsd, SAUNAFS_SUGID_CLEAR_MODE_BSD);
				STATIC_ASSERT_SUGID_CLEAR_MODE(kExt, SAUNAFS_SUGID_CLEAR_MODE_EXT);
				STATIC_ASSERT_SUGID_CLEAR_MODE(kSfs, SAUNAFS_SUGID_CLEAR_MODE_SFS);
			#undef STATIC_ASSERT_SUGID_CLEAR_MODE
		}
	}
}

static thread_local sau_err_t gLastErrorCode(SAUNAFS_STATUS_OK);

static void to_entry(const Client::EntryParam &param, sau_entry *entry) {
	assert(entry);
	entry->ino = param.ino;
	entry->generation = param.generation;
	entry->attr = param.attr;
	entry->attr_timeout = param.attr_timeout;
	entry->entry_timeout = param.entry_timeout;
}

static void to_attr_reply(const Client::AttrReply &attr_reply,
                          sau_attr_reply *reply) {
	assert(reply);
	reply->attr = attr_reply.attr;
	reply->attr_timeout = attr_reply.attrTimeout;
}

static void to_stat(const Client::Stats &stats, sau_stat_t *buf) {
	assert(buf);
	buf->total_space = stats.total_space;
	buf->avail_space = stats.avail_space;
	buf->trash_space = stats.trash_space;
	buf->reserved_space = stats.reserved_space;
	buf->inodes = stats.inodes;
}

sau_err_t sau_last_err() {
	return gLastErrorCode;
}

sau_err_t sau_error_conv(sau_err_t saunafs_error_code) {
	if (saunafs_error_code < 0) {
		return EINVAL;
	} else {
		return saunafs_error_conv(saunafs_error_code);
	}
}

const char *sau_error_string(sau_err_t saunafs_error_code) {
	return saunafs_error_string(saunafs_error_code);
}

sau_context_t *sau_create_context() {
	try {
		Client::Context *ret = new Client::Context(getuid(), getgid(),
		                                           getpid(), 0);
		return (sau_context_t *)ret;
	} catch (...) {
		gLastErrorCode = SAUNAFS_ERROR_OUTOFMEMORY;
		return nullptr;
	}
}

sau_context_t *sau_create_user_context(uid_t uid, gid_t gid, pid_t pid,
                                       mode_t umask) {
	try {
		Client::Context *ret = new Client::Context(uid, gid, pid, umask);
		return (sau_context_t *)ret;
	} catch (...) {
		gLastErrorCode = SAUNAFS_ERROR_OUTOFMEMORY;
		return nullptr;
	}
}

void sau_destroy_context(sau_context_t **ctx) {
	Client::Context *client_ctx = (Client::Context *) *ctx;
	delete client_ctx;
}

void sau_set_lock_owner(sau_fileinfo_t *fileinfo, uint64_t lock_owner) {
	if (fileinfo == nullptr) {
		return;
	}
	Client::FileInfo *fi = (Client::FileInfo *)fileinfo;
	fi->lock_owner = lock_owner;
}

sau_t *sau_init(const char *host, const char *port, const char *mountpoint) {
	try {
		Client *ret = new Client(host, port, mountpoint);
		gLastErrorCode = SAUNAFS_STATUS_OK;
		return (sau_t *)ret;
	} catch (...) {
		gLastErrorCode = SAUNAFS_ERROR_CANTCONNECT;
		return nullptr;
	}
}

sau_t *sau_init_with_params(struct sau_init_params *params) {
	assert(params != nullptr);
	assert(params->host != nullptr);
	assert(params->port != nullptr);
	assert(params->mountpoint != nullptr);
	assert(params->sugid_clear_mode >= 0);
	assert(params->sugid_clear_mode < SAUNAFS_SUGID_CLEAR_MODE_END_);

	Client::FsInitParams init_params(
	    params->bind_host != nullptr ? params->bind_host : "", params->host,
	    params->port, params->mountpoint);

	init_params.meta = params->meta;
	if (params->subfolder != nullptr) {
		init_params.subfolder = params->subfolder;
	}
	if (params->password != nullptr) {
		md5ctx md5_ctx;
		init_params.password_digest.resize(kDefaultMd5DigestSize);
		md5_init(&md5_ctx);
		md5_update(&md5_ctx,(uint8_t *)(params->password),
		           strlen(params->password));
		md5_final(init_params.password_digest.data(), &md5_ctx);
	} else if (params->md5_pass) {
		int ret = md5_parse(init_params.password_digest,
		                    params->md5_pass);
		if (ret < 0) {
			gLastErrorCode = SAUNAFS_ERROR_EINVAL;
			return nullptr;
		}
	}

#define COPY_PARAM(PARAM)                                              \
	do {                                                               \
		static_assert(                                                 \
		    std::is_same<decltype(init_params.PARAM),                  \
		                 decltype(params->PARAM)>::value,              \
		    "sau_init_params member incompatible with FsInitParams");  \
		init_params.PARAM = params->PARAM;                             \
	} while (0)
	COPY_PARAM(do_not_remember_password);
	COPY_PARAM(delayed_init);
	COPY_PARAM(report_reserved_period);
	COPY_PARAM(io_retries);
	COPY_PARAM(chunkserver_latency_sort);
	COPY_PARAM(chunkserver_round_time_ms);
	COPY_PARAM(chunkserver_connect_timeout_ms);
	COPY_PARAM(chunkserver_wave_read_timeout_ms);
	COPY_PARAM(total_read_timeout_ms);
	COPY_PARAM(cache_expiration_time_ms);
	COPY_PARAM(readahead_max_window_size_kB);
	COPY_PARAM(prefetch_xor_stripes);
	COPY_PARAM(bandwidth_overuse);
	COPY_PARAM(write_cache_size);
	COPY_PARAM(write_workers);
	COPY_PARAM(write_window_size);
	COPY_PARAM(chunkserver_write_timeout_ms);
	COPY_PARAM(cache_per_inode_percentage);
	COPY_PARAM(symlink_cache_timeout_s);
	COPY_PARAM(debug_mode);
	COPY_PARAM(keep_cache);
	COPY_PARAM(direntry_cache_timeout);
	COPY_PARAM(direntry_cache_size);
	COPY_PARAM(entry_cache_timeout);
	COPY_PARAM(attr_cache_timeout);
	COPY_PARAM(mkdir_copy_sgid);
	init_params.sugid_clear_mode = (SugidClearMode)params->sugid_clear_mode;
	COPY_PARAM(use_rw_lock);
	COPY_PARAM(acl_cache_timeout);
	COPY_PARAM(acl_cache_size);
	COPY_PARAM(verbose);
#undef COPY_PARAM

	if (params->io_limits_config_file != nullptr) {
		init_params.io_limits_config_file = params->io_limits_config_file;
	}

	try {
		Client *ret = new Client(init_params);
		gLastErrorCode = SAUNAFS_STATUS_OK;
		return (sau_t *)ret;
	} catch (...) {
		gLastErrorCode = SAUNAFS_ERROR_CANTCONNECT;
		return nullptr;
	}
}

int sau_update_groups(sau_t *instance, sau_context_t *ctx, gid_t *gids,
                      int gid_num) {
	Client &client = *(Client *)instance;
	Client::Context &client_ctx = *(Client::Context *)ctx;
	Client::Context::GroupsContainer gids_backup(std::move(client_ctx.gids));
	try {
		client_ctx.gids.assign(gids, gids + gid_num);
		std::error_code ec;
		client.updateGroups(client_ctx, ec);
		gLastErrorCode = ec.value();
		if (ec) {
			client_ctx.gids = std::move(gids_backup);
			return -1;
		}
	} catch (...) {
		client_ctx.gids = std::move(gids_backup);
		gLastErrorCode = SAUNAFS_ERROR_GROUPNOTREGISTERED;
		return -1;
	}
	return 0;
}

int sau_lookup(sau_t *instance, sau_context_t *ctx, sau_inode_t parent,
               const char *path, sau_entry *entry) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	Client::EntryParam entry_param;
	std::error_code ec;
	client.lookup(context, parent, path, entry_param, ec);
	gLastErrorCode = ec.value();
	if (ec) {
		return -1;
	} else {
		to_entry(entry_param, entry);
	}
	return 0;
}

int sau_mknod(sau_t *instance, sau_context_t *ctx, sau_inode_t parent,
              const char *path, mode_t mode, dev_t rdev, sau_entry *entry) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	Client::EntryParam entry_param;
	std::error_code ec;
	client.mknod(context, parent, path, mode, rdev, entry_param, ec);
	gLastErrorCode = ec.value();
	if (ec) {
		return -1;
	} else {
		to_entry(entry_param, entry);
	}
	return 0;
}

int sau_link(sau_t *instance, sau_context_t *ctx, sau_inode_t inode,
             sau_inode_t parent, const char *name, struct sau_entry *entry) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	Client::EntryParam entry_param;
	std::error_code ec;
	client.link(context, inode, parent, name, entry_param, ec);
	gLastErrorCode = ec.value();
	if (ec) {
		return -1;
	}
	to_entry(entry_param, entry);
	return 0;
}

int sau_symlink(sau_t *instance, sau_context_t *ctx, const char *link,
                sau_inode_t parent, const char *name, struct sau_entry *entry) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	Client::EntryParam entry_param;
	std::error_code ec;
	client.symlink(context, link, parent, name, entry_param, ec);
	gLastErrorCode = ec.value();
	if (ec) {
		return -1;
	}
	to_entry(entry_param, entry);
	return 0;
}

sau_fileinfo *sau_open(sau_t *instance, sau_context_t *ctx, sau_inode_t inode,
                       int flags) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	std::error_code ec;
	sau_fileinfo *fi = (sau_fileinfo *)client.open(context, inode, flags, ec);
	gLastErrorCode = ec.value();
	return fi;
}

ssize_t sau_read(sau_t *instance, sau_context_t *ctx, sau_fileinfo *fileinfo,
                 off_t offset, size_t size, char *buffer) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	std::error_code ec;
	auto ret = client.read(context, (Client::FileInfo *)fileinfo, offset,
	                       size, ec);
	if (ec) {
		gLastErrorCode = ec.value();
		return -1;
	}
	return ret.copyToBuffer((uint8_t *)buffer, offset, size);
}

ssize_t sau_readv(sau_t *instance, sau_context_t *ctx, sau_fileinfo *fileinfo,
                  off_t offset, size_t size, const struct iovec *iov,
                  int iovcnt) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	std::error_code ec;
	auto ret = client.read(context, (Client::FileInfo *)fileinfo, offset,
	                       size, ec);
	if (ec) {
		gLastErrorCode = ec.value();
		return -1;
	}
	small_vector<struct iovec, 8> reply;
	ret.toIoVec(reply, offset, size);
	return copyIoVec(iov, iovcnt, reply.data(), reply.size());
}

ssize_t sau_write(sau_t *instance, sau_context_t *ctx, sau_fileinfo *fileinfo,
                  off_t offset, size_t size, const char *buffer) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	std::error_code ec;
	std::size_t write_ret = client.write(context,
	                                     (Client::FileInfo *)fileinfo,
	                                     offset, size, buffer, ec);
	gLastErrorCode = ec.value();
	return ec ? -1 : write_ret;
}

int sau_release(sau_t *instance, sau_fileinfo *fileinfo) {
	if (fileinfo == nullptr) {
		gLastErrorCode = SAUNAFS_ERROR_EBADF;
		return -1;
	}
	Client &client = *(Client *)instance;
	std::error_code ec;
	client.release((Client::FileInfo *)fileinfo, ec);
	gLastErrorCode = ec.value();
	return ec ? -1 : 0;
}

int sau_flush(sau_t *instance, sau_context_t *ctx, sau_fileinfo *fileinfo) {
	if (fileinfo == nullptr) {
		gLastErrorCode = SAUNAFS_ERROR_EBADF;
		return -1;
	}
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	std::error_code ec;
	client.flush(context, (Client::FileInfo *)fileinfo, ec);
	gLastErrorCode = ec.value();
	return ec ? -1 : 0;
}

int sau_getattr(sau_t *instance, sau_context_t *ctx, sau_inode_t inode,
                sau_attr_reply *reply) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	Client::AttrReply r;
	std::error_code ec;
	client.getattr(context, inode, r, ec);
	gLastErrorCode = ec.value();
	if (ec) {
		return -1;
	} else if (reply) {
		to_attr_reply(r, reply);
	}
	return 0;
}

void sau_destroy(sau_t *instance) {
	Client *client = (Client *)instance;
	delete client;
}

struct sau_fileinfo *sau_opendir(sau_t *instance, sau_context_t *ctx,
                                 sau_inode_t inode) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	std::error_code ec;
	sau_fileinfo *fi = (sau_fileinfo *)client.opendir(context, inode, ec);
	gLastErrorCode = ec.value();
	return fi;
}

int sau_readdir(sau_t *instance, sau_context_t *ctx,
                struct sau_fileinfo *fileinfo, off_t offset, size_t max_entries,
                struct sau_direntry *buf, size_t *num_entries) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	std::error_code ec;

	if (max_entries > 0) {
		buf->name = nullptr;
	} else {
		gLastErrorCode = SAUNAFS_ERROR_EINVAL;
		return -1;
	}

	Client::ReadDirReply reply;
	reply = client.readdir(context, (Client::FileInfo *)fileinfo, offset,
	                       max_entries, ec);
	*num_entries = 0;
	gLastErrorCode = ec.value();
	if (ec) {
		return -1;
	}
	if (reply.empty()) {
		return 0;
	}

	size_t total_name_size = 0;
	for (const auto &dir_entry : reply) {
		total_name_size += dir_entry.name.size() + 1;
	}

	char *p_namebuf;
	try {
		p_namebuf = new char[total_name_size];
	} catch (...) {
		gLastErrorCode = SAUNAFS_ERROR_OUTOFMEMORY;
		return -1;
	}

	for (const auto &dir_entry : reply) {
		buf->name = p_namebuf;
		buf->attr = dir_entry.attr;
		buf->next_entry_offset = dir_entry.nextEntryOffset;
		buf++;

		auto s = dir_entry.name.size();
		dir_entry.name.copy(p_namebuf, s);
		p_namebuf[s] = '\0';
		p_namebuf += s + 1;
	}

	*num_entries = reply.size();
	return 0;
}

void sau_destroy_direntry(struct sau_direntry *buf, size_t /*num_entries*/) {
	if (buf->name) {
		delete[] buf->name;
	}
}

int sau_releasedir(sau_t *instance, struct sau_fileinfo *fileinfo) {
	Client &client = *(Client *)instance;
	std::error_code ec;
	client.releasedir((Client::FileInfo *)fileinfo, ec);
	gLastErrorCode = ec.value();
	return ec ? -1 : 0;
}

static int convert_named_inodes(sau_namedinode_entry *out_entries,
                                uint32_t *num_entries,
                                const std::vector<NamedInodeEntry> &input) {
	*num_entries = 0;
	if (input.empty()) {
		return 0;
	}

	size_t total_name_size = 0;
	for (const auto &ni : input) {
		total_name_size += ni.name.size() + 1;
	}

	char *p_namebuf;
	try {
		p_namebuf = new char[total_name_size];
	} catch (...) {
		gLastErrorCode = SAUNAFS_ERROR_OUTOFMEMORY;
		return -1;
	}

	for (const auto &ni : input) {
		out_entries->ino = ni.inode;
		out_entries->name = p_namebuf;
		out_entries++;

		auto s = ni.name.size();
		ni.name.copy(p_namebuf, s);
		p_namebuf[s] = '\0';
		p_namebuf += s + 1;
	}

	*num_entries = input.size();
	return 0;
}

int sau_readreserved(sau_t *instance, sau_context_t *ctx, uint32_t offset,
                     uint32_t max_entries,
                     struct sau_namedinode_entry *out_entries,
                     uint32_t *num_entries) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	std::error_code ec;
	auto named_inodes = client.readreserved(context, offset, max_entries, ec);
	gLastErrorCode = ec.value();
	if (ec) {
		return -1;
	}
	return convert_named_inodes(out_entries, num_entries, named_inodes);
}

int sau_readtrash(sau_t *instance, sau_context_t *ctx, uint32_t offset,
                  uint32_t max_entries, struct sau_namedinode_entry *out_entries,
                  uint32_t *num_entries) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	std::error_code ec;
	auto named_inodes = client.readtrash(context, offset, max_entries, ec);
	gLastErrorCode = ec.value();
	if (ec) {
		return -1;
	}
	return convert_named_inodes(out_entries, num_entries, named_inodes);
}

void sau_free_namedinode_entries(struct sau_namedinode_entry *entries,
                                 uint32_t num_entries) {
	assert(num_entries > 0);
	(void)num_entries;
	delete[] entries->name;
}

int sau_mkdir(sau_t *instance, sau_context_t *ctx, sau_inode_t parent,
              const char *path, mode_t mode, struct sau_entry *entry) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	Client::EntryParam entry_param;
	std::error_code ec;
	client.mkdir(context, parent, path, mode, entry_param, ec);
	if (!ec) {
		to_entry(entry_param, entry);
	}
	gLastErrorCode = ec.value();
	return ec ? -1 : 0;
}

int sau_rmdir(sau_t *instance, sau_context_t *ctx, sau_inode_t parent,
              const char *path) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	std::error_code ec;
	client.rmdir(context, parent, path, ec);
	gLastErrorCode = ec.value();
	return ec ? -1 : 0;
}

int sau_makesnapshot(sau_t *instance, sau_context_t *ctx, sau_inode_t inode,
                     sau_inode_t dst_parent, const char *dst_name,
                     int can_overwrite, uint32_t *job_id) {
	static_assert(sizeof(SaunaClient::JobId) <= sizeof(uint32_t),
	              "JobId type too large");
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	std::error_code ec;
	SaunaClient::JobId ret = client.makesnapshot(context, inode, dst_parent,
	                                              dst_name, can_overwrite, ec);
	if (job_id) {
		*job_id = ret;
	}
	gLastErrorCode = ec.value();
	return ec ? -1 : 0;
}

int sau_getgoal(sau_t *instance, sau_context_t *ctx, sau_inode_t inode,
                char *goal_name) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	std::error_code ec;
	std::string goal = client.getgoal(context, inode, ec);
	gLastErrorCode = ec.value();
	if (ec) {
		return -1;
	}
	size_t copied = goal.copy(goal_name, SAUNAFS_MAX_GOAL_NAME - 1);
	goal_name[copied] = '\0';

	return 0;
}

int sau_setgoal(sau_t *instance, sau_context_t *ctx, sau_inode_t inode,
                const char *goal_name, int is_recursive) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	std::error_code ec;
	client.setgoal(context, inode, goal_name,
	               is_recursive ? SMODE_RMASK : 0, ec);
	gLastErrorCode = ec.value();
	return ec ? -1 : 0;
}

int sau_readlink(sau_t *instance, sau_context_t *ctx, sau_inode_t inode,
                 char *buf, size_t size) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	std::error_code ec;
	std::string link = client.readlink(context, inode, ec);
	gLastErrorCode = ec.value();
	if (ec) {
		return -1;
	}
	link.copy(buf, size);
	return link.size();
}

int sau_unlink(sau_t *instance, sau_context_t *ctx, sau_inode_t parent,
               const char *path) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	std::error_code ec;
	client.unlink(context, parent, path, ec);
	gLastErrorCode = ec.value();
	return ec ? -1 : 0;
}

int sau_undel(sau_t *instance, sau_context_t *ctx, sau_inode_t inode) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	std::error_code ec;
	client.undel(context, inode, ec);
	gLastErrorCode = ec.value();
	return ec ? -1 : 0;
}

int sau_setattr(sau_t *instance, sau_context_t *ctx, sau_inode_t inode,
                struct stat *stbuf, int to_set, struct sau_attr_reply *reply) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	Client::AttrReply r;
	std::error_code ec;
	client.setattr(context, inode, stbuf, to_set, r, ec);
	if (!ec) {
		to_attr_reply(r, reply);
	}
	gLastErrorCode = ec.value();
	return ec ? -1 : 0;
}

int sau_fsync(sau_t *instance, sau_context_t *ctx, struct sau_fileinfo *fileinfo) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	std::error_code ec;
	client.fsync(context, (Client::FileInfo *)fileinfo, ec);
	gLastErrorCode = ec.value();
	return ec ? -1 : 0;
}

int sau_rename(sau_t *instance, sau_context_t *ctx, sau_inode_t parent,
               const char *path, sau_inode_t new_parent, const char *new_path) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	std::error_code ec;
	client.rename(context, parent, path, new_parent, new_path, ec);
	gLastErrorCode = ec.value();
	return ec ? -1 : 0;
}

int sau_statfs(sau_t *instance, sau_stat_t *buf) {
	Client &client = *(Client *)instance;
	Client::Stats stats;
	std::error_code ec;
	client.statfs(stats, ec);
	gLastErrorCode = ec.value();
	if (ec) {
		return -1;
	}
	to_stat(stats, buf);
	return 0;
}

int sau_setxattr(sau_t *instance, sau_context_t *ctx, sau_inode_t ino,
                 const char *name, const uint8_t *value, size_t size,
                 enum sau_setxattr_mode mode) {
	Client &client = *(Client *) instance;
	Client::Context &context = *(Client::Context *)ctx;
	std::error_code ec;
	client.setxattr(context, ino, name,
	                std::vector<uint8_t>(value, value + size), mode, ec);
	gLastErrorCode = ec.value();
	return ec ? -1 : 0;
}

int sau_getxattr(sau_t *instance, sau_context_t *ctx, sau_inode_t ino,
                 const char *name, size_t size, size_t *out_size, uint8_t *buf) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	std::error_code ec;
	auto ret = client.getxattr(context, ino, name, ec);
	gLastErrorCode = ec.value();
	if (ec) {
		return -1;
	}
	std::memcpy(buf, ret.data(), std::min(size, ret.size()));
	if (out_size) {
		*out_size = ret.size();
	}
	return 0;
}

int sau_listxattr(sau_t *instance, sau_context_t *ctx, sau_inode_t ino,
                  size_t size, size_t *out_size, char *buf) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	std::error_code ec;
	auto ret = client.listxattr(context, ino, ec);
	gLastErrorCode = ec.value();
	if (ec) {
		return -1;
	}
	std::memcpy(buf, ret.data(), std::min(size, ret.size()));
	if (out_size) {
		*out_size = ret.size();
	}
	return 0;
}

int sau_removexattr(sau_t *instance, sau_context_t *ctx, sau_inode_t ino,
                    const char *name) {
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	std::error_code ec;
	client.removexattr(context, ino, name, ec);
	gLastErrorCode = ec.value();
	return ec ? -1 : 0;
}

sau_acl_t *sau_create_acl() {
	try {
		return (sau_acl_t *)new RichACL;
	} catch (...) {
		return nullptr;
	}
}

sau_acl_t *sau_create_acl_from_mode(unsigned int mode) {
	try {
		RichACL richacl = RichACL::createFromMode(mode, S_ISDIR(mode));
		RichACL *acl = new RichACL;
		// Updating the masks
		acl->setFlags(richacl.getFlags());
		acl->setOwnerMask(richacl.getOwnerMask());
		acl->setGroupMask(richacl.getGroupMask());
		acl->setOtherMask(richacl.getOtherMask());
		return (sau_acl_t *)acl;
	} catch (...) {
		return nullptr;
	}
}

void sau_destroy_acl(sau_acl_t *acl) {
	delete (RichACL *)acl;
}

int sau_print_acl(sau_acl_t *acl, char *buf, size_t size, size_t *reply_size) {
	assert(acl);
	assert(buf || size == 0);
	RichACL &richacl = *(RichACL *)acl;
	std::string repr = richacl.toString();
	assert(reply_size);
	*reply_size = repr.size();
	if (size < repr.size()) {
		gLastErrorCode = SAUNAFS_ERROR_WRONGSIZE;
		return -1;
	}
	repr.copy(buf, size);

	return 0;
}

void sau_add_acl_entry(sau_acl_t *acl, const sau_acl_ace_t *ace) {
	assert(acl);
	assert(ace);
	RichACL &richacl = *(RichACL *)acl;
	richacl.insert(RichACL::Ace(ace->type, ace->flags, ace->mask, ace->id));
}

int sau_get_acl_entry(const sau_acl_t *acl, int n, sau_acl_ace_t *ace) {
	assert(acl);
	assert(ace);
	const RichACL &richacl = *(RichACL *)acl;
	if ((size_t)n > richacl.size()) {
		gLastErrorCode = SAUNAFS_ERROR_WRONGSIZE;
		return -1;
	}
	auto it = std::next(richacl.begin(), n);
	ace->type = it->type;
	ace->flags = it->flags;
	ace->mask = it->mask;
	ace->id = it->id;
	return 0;
}

size_t sau_get_acl_size(const sau_acl_t *acl) {
	assert(acl);
	const RichACL &richacl = *(RichACL *)acl;
	return richacl.size();
}

int sau_setacl(sau_t *instance, sau_context_t *ctx, sau_inode_t ino,
               sau_acl_t *acl) {
	assert(instance);
	assert(ctx);
	assert(acl);
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	std::error_code ec;
	try {
		RichACL &richacl = *(RichACL *)acl;
		client.setacl(context, ino, richacl, ec);
		gLastErrorCode = ec.value();
		return ec ? -1 : 0;
	} catch (...) {
		gLastErrorCode = SAUNAFS_ERROR_ENOATTR;
		return -1;
	}
}

int sau_getacl(sau_t *instance, sau_context_t *ctx, sau_inode_t ino,
               sau_acl_t **acl) {
	assert(instance);
	assert(ctx);
	assert(acl);
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	std::error_code ec;
	*acl = nullptr;
	try {
		RichACL richacl = client.getacl(context, ino, ec);
		gLastErrorCode = ec.value();
		if (ec) {
			return -1;
		}
		*acl = (sau_acl_t *)new RichACL(std::move(richacl));
	} catch (...) {
		gLastErrorCode = SAUNAFS_ERROR_ENOATTR;
		return -1;
	}
	return 0;
}

int sau_acl_apply_masks(sau_acl_t *acl, uint32_t owner) {
	if (acl) {
		try {
			((RichACL *)acl)->applyMasks(owner);
		} catch (...) {
			gLastErrorCode = SAUNAFS_ERROR_EINVAL;
			return -1;
		}
	}

	return 0;
}

int sau_get_chunks_info(sau_t *instance, sau_context_t *ctx, sau_inode_t inode,
                        uint32_t chunk_index, sau_chunk_info_t *buffer,
                        uint32_t buffer_size, uint32_t *reply_size) {
	assert(instance && ctx && buffer && reply_size);

	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;

	if (buffer_size > 0) {
		buffer->parts = nullptr;
	} else {
		gLastErrorCode = SAUNAFS_ERROR_EINVAL;
		return -1;
	}

	std::error_code ec;
	auto chunks = client.getchunksinfo(context, inode, chunk_index,
	                                   buffer_size, ec);
	gLastErrorCode = ec.value();
	if (ec) {
		return -1;
	}

	*reply_size = chunks.size();
	if (chunks.size() > buffer_size) {
		gLastErrorCode = SAUNAFS_ERROR_WRONGSIZE;
		return -1;
	}

	std::size_t strings_size = 0;
	std::size_t parts_table_size = 0;
	for(const auto &chunk : chunks) {
		auto chunkPartSize = sizeof(sau_chunk_part_info_t);
		parts_table_size += chunk.chunk_parts.size() * chunkPartSize;
		for(const auto &part : chunk.chunk_parts) {
			strings_size += part.label.size() + 1;
		}
	}

	auto totalSize = parts_table_size + strings_size;
	uint8_t *data_buffer = (uint8_t *)std::malloc(totalSize);
	if (data_buffer == nullptr) {
		gLastErrorCode = SAUNAFS_ERROR_OUTOFMEMORY;
		return -1;
	}

	auto buf_part = (sau_chunk_part_info_t *)data_buffer;
	auto buf_string = (char *)(data_buffer + parts_table_size);
	auto buf_chunk = buffer;
	for(const auto &chunk : chunks) {
		buf_chunk->chunk_id = chunk.chunk_id;
		buf_chunk->chunk_version = chunk.chunk_version;
		buf_chunk->parts = buf_part;
		buf_chunk->parts_size = chunk.chunk_parts.size();

		for(const auto &part : chunk.chunk_parts) {
			buf_part->addr = part.address.ip;
			buf_part->port = part.address.port;
			buf_part->part_type_id = part.chunkType.getId();
			buf_part->label = buf_string;
			std::strcpy(buf_string, part.label.c_str());

			buf_string += part.label.size() + 1;
			++buf_part;
		}
		++buf_chunk;
	}

	gLastErrorCode = SAUNAFS_STATUS_OK;
	return 0;
}

int sau_get_chunkservers_info(sau_t *instance, sau_chunkserver_info_t *servers,
                              uint32_t size, uint32_t *reply_size) {
	Client &client = *(Client *)instance;
	std::error_code ec;

	if (size > 0) {
		servers->label = nullptr;
	} else {
		gLastErrorCode = SAUNAFS_ERROR_EINVAL;
		return -1;
	}

	std::vector<ChunkserverListEntry> entries = client.getchunkservers(ec);
	gLastErrorCode = ec.value();
	if (ec) {
		return -1;
	}
	*reply_size = entries.size();
	if (entries.size() > size) {
		gLastErrorCode = SAUNAFS_ERROR_WRONGSIZE;
		return -1;
	}
	size_t total_str_size = 0;
	for (const ChunkserverListEntry &entry : entries) {
		total_str_size += entry.label.size() + 1;
	}

	char *str_buf = (char *)malloc(total_str_size);
	if (str_buf == nullptr) {
		gLastErrorCode = SAUNAFS_ERROR_OUTOFMEMORY;
		return -1;
	}
	for (const ChunkserverListEntry &entry : entries) {
		servers->version = entry.version;
		servers->ip = entry.servip;
		servers->port = entry.servport;
		servers->used_space = entry.usedspace;
		servers->total_space = entry.totalspace;
		servers->error_counter = entry.errorcounter;
		servers->label = strcpy(str_buf, entry.label.c_str());
		str_buf += entry.label.size() + 1;
		servers++;
	}
	gLastErrorCode = SAUNAFS_STATUS_OK;
	return 0;
}

void sau_destroy_chunkservers_info(sau_chunkserver_info_t *servers) {
	if (servers) {
		free(servers->label);
	}
}

void sau_destroy_chunks_info(sau_chunk_info_t *buffer) {
	if (buffer && buffer->parts) {
		std::free(buffer->parts);
	}
}

int sau_setlk(sau_t *instance, sau_context_t *ctx, sau_fileinfo_t *fileinfo,
              const sau_lock_info *lock, sau_lock_register_interrupt_t handler,
              void *priv) {
	if (fileinfo == nullptr) {
		gLastErrorCode = SAUNAFS_ERROR_EBADF;
		return -1;
	}
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	Client::FileInfo *fi = (Client::FileInfo *)fileinfo;
	gLastErrorCode = 0;

	safs_locks::FlockWrapper flock_wrapper;
	// Convert the POSIX lock type to the internal safs_locks equivalent
	flock_wrapper.l_type = safs_locks::posixOpConv(lock->l_type, true);
	flock_wrapper.l_start = lock->l_start;
	flock_wrapper.l_len = lock->l_len;
	flock_wrapper.l_pid = lock->l_pid;
	std::error_code ec;
	sau_lock_interrupt_info_t interrupt_info;
	std::function<int(const safs_locks::InterruptData &)> lambda;
	if (handler) {
		lambda = [&handler, &interrupt_info, priv](
		             const safs_locks::InterruptData &data) {
			interrupt_info.owner = data.owner;
			interrupt_info.ino = data.ino;
			interrupt_info.reqid = data.reqid;
			return handler(&interrupt_info, priv);
		};
	}
	client.setlk(context, fi->inode, fi, flock_wrapper, lambda, ec);
	gLastErrorCode = ec.value();
	return ec ? -1 : 0;
}

int sau_getlk(sau_t *instance, sau_context_t *ctx, sau_fileinfo_t *fileinfo,
              sau_lock_info *lock) {
	if (fileinfo == nullptr) {
		gLastErrorCode = SAUNAFS_ERROR_EBADF;
		return -1;
	}
	Client &client = *(Client *)instance;
	Client::Context &context = *(Client::Context *)ctx;
	Client::FileInfo *fi = (Client::FileInfo *)fileinfo;
	gLastErrorCode = 0;

	safs_locks::FlockWrapper flock_wrapper;
	flock_wrapper.l_type = lock->l_type;
	flock_wrapper.l_start = lock->l_start;
	flock_wrapper.l_len = lock->l_len;
	flock_wrapper.l_pid = lock->l_pid;
	std::error_code ec;
	client.getlk(context, fi->inode, fi, flock_wrapper, ec);
	if (ec) {
		gLastErrorCode = ec.value();
		return -1;
	}
	lock->l_type = flock_wrapper.l_type;
	lock->l_start = flock_wrapper.l_start;
	lock->l_len = flock_wrapper.l_len;
	lock->l_pid = flock_wrapper.l_pid;
	return 0;
}

int sau_setlk_interrupt(sau_t *instance,
                        const sau_lock_interrupt_info_t *interrupt_info) {
	if (interrupt_info == nullptr) {
		return 0;
	}
	Client &client = *(Client *)instance;
	safs_locks::InterruptData interrupt_data;
	interrupt_data.owner = interrupt_info->owner;
	interrupt_data.ino = interrupt_info->ino;
	interrupt_data.reqid = interrupt_info->reqid;
	std::error_code ec;
	client.setlk_interrupt(interrupt_data, ec);
	if (ec) {
		gLastErrorCode = ec.value();
		return -1;
	}
	return 0;
}
