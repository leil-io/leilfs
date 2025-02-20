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

#include <string.h>
#include "common/stat32.h"
#include <sys/types.h>
#include <unistd.h>

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/chunk_with_address_and_label.h"
#include "common/exception.h"
#include "mount/group_cache.h"
#include "mount/sauna_client_context.h"
#include "mount/readdata_cache.h"
#include "mount/stat_defs.h"
#include "protocol/chunkserver_list_entry.h"
#include "protocol/lock_info.h"
#include "protocol/named_inode_entry.h"

#ifdef _WIN32
#define USE_LOCAL_ID -1
#endif

namespace SaunaClient {

typedef uint32_t Inode;
typedef uint32_t JobId;
typedef uint32_t NamedInodeOffset;
#ifdef _WIN32
inline std::atomic<bool> gIgnoreUtimensUpdate = false;
#endif

void update_readdir_session(uint64_t sessId, uint64_t entryIno);
void drop_readdir_session(uint64_t opendirSessionID);

struct FsInitParams {
	static constexpr const char *kDefaultSubfolder = DEFAULT_MOUNTED_SUBFOLDER;
	static constexpr bool     kDefaultDoNotRememberPassword = false;
	static constexpr bool     kDefaultDelayedInit = false;
	static constexpr unsigned kDefaultIoRetries = 30;
	static constexpr unsigned kDefaultRoundTime = 200;
	static constexpr unsigned kDefaultChunkserverConnectTo = 2000;
	static constexpr unsigned kDefaultChunkserverReadTo = 2000;
	static constexpr unsigned kDefaultChunkserverWaveReadTo = 500;
	static constexpr unsigned kDefaultChunkserverTotalReadTo = 2000;
	static constexpr unsigned kDefaultCacheExpirationTime = 1000;
	static constexpr unsigned kDefaultReadaheadMaxWindowSize = 65536;
	static constexpr unsigned kDefaultReadCacheMaxSizePercentage = 60;
	static constexpr unsigned kDefaultReadWorkers = 30;
	static constexpr unsigned kDefaultMaxReadaheadRequests = 5;
	static constexpr bool     kDefaultPrefetchXorStripes = false;

	static constexpr float    kDefaultBandwidthOveruse = 1.0;
	static constexpr unsigned kDefaultChunkserverWriteTo = 5000;
	static constexpr bool     kDefaultIgnoreFlush = false;
	static constexpr int      kDefaultLogNotificationArea = 0;
	static constexpr unsigned kDefaultMessageSuppressionPeriod = 10;
	static constexpr unsigned kDefaultStatfsCacheTo = 0;
	static constexpr bool     kDefaultUseQuotaInVolumeSize = false;
#ifdef _WIN32
	static constexpr unsigned kDefaultReportReservedPeriod = 60;
	static constexpr const char *kDefaultUmaskDir = "002"; // means rwxrwxr-x permissions mask, 775 default permissions
	static constexpr const char *kDefaultUmaskFile = "002"; // means rwxrwxr-x permissions mask, 775 default permissions
	static constexpr const char *kDefaultLogLevel = "warn";
	static constexpr const char *kDefaultLogFlushLevel = "err";
	static constexpr unsigned kDefaultWriteCacheSize = 50;
	static constexpr unsigned kDefaultCleanAcquiredFilesPeriod = 0;
	static constexpr unsigned kDefaultCleanAcquiredFilesTimeout = 0;
	static constexpr int      kDefaultEnableStatusUpdaterThread = 0;
	static constexpr bool     kDefaultIgnoreUtimensUpdate = false;
	static constexpr bool     kDefaultMkdirCopySgid = false;
#else
	static constexpr unsigned kDefaultReportReservedPeriod = 30;
	static constexpr unsigned kDefaultWriteCacheSize = 0;
	static constexpr unsigned kDefaultLimitGlibcMallocArenas = 0;
	static constexpr bool     kDefaultMkdirCopySgid = true;
#endif
	static constexpr unsigned kDefaultCachePerInodePercentage = 25;
	static constexpr unsigned kDefaultWriteWorkers = 10;
	static constexpr unsigned kDefaultWriteWindowSize = 15;
	static constexpr unsigned kDefaultSymlinkCacheTimeout = 3600;
	static constexpr int      kDefaultNonEmptyMounts = 0;

	static constexpr bool     kDefaultDebugMode = false;
	static constexpr int      kDefaultKeepCache = 0;
	static constexpr double   kDefaultDirentryCacheTimeout = 0.25;
	static constexpr unsigned kDefaultDirentryCacheSize = 100000;
	static constexpr double   kDefaultEntryCacheTimeout = 0.0;
	static constexpr double   kDefaultAttrCacheTimeout = 1.0;
#if defined(DEFAULT_SUGID_CLEAR_MODE_EXT)
	static constexpr SugidClearMode kDefaultSugidClearMode = SugidClearMode::kExt;
#elif defined(DEFAULT_SUGID_CLEAR_MODE_BSD)
	static constexpr SugidClearMode kDefaultSugidClearMode = SugidClearMode::kBsd;
#elif defined(DEFAULT_SUGID_CLEAR_MODE_OSX)
	static constexpr SugidClearMode kDefaultSugidClearMode = SugidClearMode::kOsx;
#else
	static constexpr SugidClearMode kDefaultSugidClearMode = SugidClearMode::kNever;
#endif
	static constexpr bool     kDefaultUseRwLock = true;
	static constexpr double   kDefaultAclCacheTimeout = 1.0;
	static constexpr unsigned kDefaultAclCacheSize = 1000;
	static constexpr bool     kDefaultVerbose = false;
	static constexpr bool     kDirectIO = false;
	// Thank you, GCC 4.6, for no delegating constructors
	FsInitParams()
	             : bind_host(), host(), port(), meta(false), mountpoint(), subfolder(kDefaultSubfolder),
	             do_not_remember_password(kDefaultDoNotRememberPassword), delayed_init(kDefaultDelayedInit),
	             report_reserved_period(kDefaultReportReservedPeriod),
	             io_retries(kDefaultIoRetries),
	             chunkserver_round_time_ms(kDefaultRoundTime),
	             chunkserver_connect_timeout_ms(kDefaultChunkserverConnectTo),
	             chunkserver_wave_read_timeout_ms(kDefaultChunkserverWaveReadTo),
	             total_read_timeout_ms(kDefaultChunkserverTotalReadTo),
	             cache_expiration_time_ms(kDefaultCacheExpirationTime),
	             readahead_max_window_size_kB(kDefaultReadaheadMaxWindowSize),
				 read_cache_max_size_percentage(kDefaultReadCacheMaxSizePercentage),
	             read_workers(kDefaultReadWorkers),
	             max_readahead_requests(kDefaultMaxReadaheadRequests),
	             prefetch_xor_stripes(kDefaultPrefetchXorStripes),
	             bandwidth_overuse(kDefaultBandwidthOveruse),
	             write_cache_size(kDefaultWriteCacheSize),
	             write_workers(kDefaultWriteWorkers), write_window_size(kDefaultWriteWindowSize),
	             chunkserver_write_timeout_ms(kDefaultChunkserverWriteTo),
	             cache_per_inode_percentage(kDefaultCachePerInodePercentage),
	             symlink_cache_timeout_s(kDefaultSymlinkCacheTimeout),
	             debug_mode(kDefaultDebugMode), keep_cache(kDefaultKeepCache),
	             direntry_cache_timeout(kDefaultDirentryCacheTimeout), direntry_cache_size(kDefaultDirentryCacheSize),
	             entry_cache_timeout(kDefaultEntryCacheTimeout), attr_cache_timeout(kDefaultAttrCacheTimeout),
	             mkdir_copy_sgid(kDefaultMkdirCopySgid), sugid_clear_mode(kDefaultSugidClearMode),
	             use_rw_lock(kDefaultUseRwLock),
	             acl_cache_timeout(kDefaultAclCacheTimeout), acl_cache_size(kDefaultAclCacheSize),
#ifdef _WIN32
	             mounting_uid(USE_LOCAL_ID), mounting_gid(USE_LOCAL_ID),
	             clean_acquired_files_period(kDefaultCleanAcquiredFilesPeriod), 
	             clean_acquired_files_timeout(kDefaultCleanAcquiredFilesTimeout),
	             enable_status_updater_thread(kDefaultEnableStatusUpdaterThread),
	             ignore_utimens_update(kDefaultIgnoreUtimensUpdate),
#else
	             limit_glibc_malloc_arenas(kDefaultLimitGlibcMallocArenas),
#endif
	             ignore_flush(kDefaultIgnoreFlush), statfs_cache_timeout(kDefaultStatfsCacheTo),
				 use_quota_in_volume_size(kDefaultUseQuotaInVolumeSize),
	             verbose(kDefaultVerbose), direct_io(kDirectIO),
	             log_notifications_area(kDefaultLogNotificationArea), 
	             message_suppression_period(kDefaultMessageSuppressionPeriod) {
	}

	FsInitParams(const std::string &bind_host, const std::string &host, const std::string &port, const std::string &mountpoint)
	             : bind_host(bind_host), host(host), port(port), meta(false), mountpoint(mountpoint), subfolder(kDefaultSubfolder),
	             do_not_remember_password(kDefaultDoNotRememberPassword), delayed_init(kDefaultDelayedInit),
	             report_reserved_period(kDefaultReportReservedPeriod),
	             io_retries(kDefaultIoRetries),
	             chunkserver_round_time_ms(kDefaultRoundTime),
	             chunkserver_connect_timeout_ms(kDefaultChunkserverConnectTo),
	             chunkserver_wave_read_timeout_ms(kDefaultChunkserverWaveReadTo),
	             total_read_timeout_ms(kDefaultChunkserverTotalReadTo),
	             cache_expiration_time_ms(kDefaultCacheExpirationTime),
	             readahead_max_window_size_kB(kDefaultReadaheadMaxWindowSize),
				 read_cache_max_size_percentage(kDefaultReadCacheMaxSizePercentage),
	             read_workers(kDefaultReadWorkers),
	             max_readahead_requests(kDefaultMaxReadaheadRequests),
	             prefetch_xor_stripes(kDefaultPrefetchXorStripes),
	             bandwidth_overuse(kDefaultBandwidthOveruse),
	             write_cache_size(kDefaultWriteCacheSize),
	             write_workers(kDefaultWriteWorkers), write_window_size(kDefaultWriteWindowSize),
	             chunkserver_write_timeout_ms(kDefaultChunkserverWriteTo),
	             cache_per_inode_percentage(kDefaultCachePerInodePercentage),
	             symlink_cache_timeout_s(kDefaultSymlinkCacheTimeout),
	             debug_mode(kDefaultDebugMode), keep_cache(kDefaultKeepCache),
	             direntry_cache_timeout(kDefaultDirentryCacheTimeout), direntry_cache_size(kDefaultDirentryCacheSize),
	             entry_cache_timeout(kDefaultEntryCacheTimeout), attr_cache_timeout(kDefaultAttrCacheTimeout),
	             mkdir_copy_sgid(kDefaultMkdirCopySgid), sugid_clear_mode(kDefaultSugidClearMode),
	             use_rw_lock(kDefaultUseRwLock),
	             acl_cache_timeout(kDefaultAclCacheTimeout), acl_cache_size(kDefaultAclCacheSize),
#ifdef _WIN32
	             mounting_uid(USE_LOCAL_ID), mounting_gid(USE_LOCAL_ID),
	             clean_acquired_files_period(kDefaultCleanAcquiredFilesPeriod), 
	             clean_acquired_files_timeout(kDefaultCleanAcquiredFilesTimeout),
	             enable_status_updater_thread(kDefaultEnableStatusUpdaterThread),
	             ignore_utimens_update(kDefaultIgnoreUtimensUpdate),
#else
	             limit_glibc_malloc_arenas(kDefaultLimitGlibcMallocArenas),
#endif 
	             ignore_flush(kDefaultIgnoreFlush), statfs_cache_timeout(kDefaultStatfsCacheTo),
				 use_quota_in_volume_size(kDefaultUseQuotaInVolumeSize),
	             verbose(kDefaultVerbose), direct_io(kDirectIO),
	             log_notifications_area(kDefaultLogNotificationArea),
	             message_suppression_period(kDefaultMessageSuppressionPeriod) {
	}

	std::string bind_host;
	std::string host;
	std::string port;
	bool meta;
	std::string mountpoint;
	std::string subfolder;
	std::vector<uint8_t> password_digest;
	bool do_not_remember_password;
	bool delayed_init;
	unsigned report_reserved_period;

	unsigned io_retries;
	unsigned chunkserver_round_time_ms;
	unsigned chunkserver_connect_timeout_ms;
	unsigned chunkserver_wave_read_timeout_ms;
	unsigned total_read_timeout_ms;
	unsigned cache_expiration_time_ms;
	unsigned readahead_max_window_size_kB;
	unsigned read_cache_max_size_percentage;
	unsigned read_workers;
	unsigned max_readahead_requests;
	bool prefetch_xor_stripes;
	double bandwidth_overuse;

	unsigned write_cache_size;
	unsigned write_workers;
	unsigned write_window_size;
	unsigned chunkserver_write_timeout_ms;
	unsigned cache_per_inode_percentage;
	unsigned symlink_cache_timeout_s;

	bool debug_mode;
	// NOTICE(sarna): This variable can hold more values than 0-1, don't change it to bool ever.
	int keep_cache;
	double direntry_cache_timeout;
	unsigned direntry_cache_size;
	double entry_cache_timeout;
	double attr_cache_timeout;
	bool mkdir_copy_sgid;
	SugidClearMode sugid_clear_mode;
	bool use_rw_lock;
	double acl_cache_timeout;
	unsigned acl_cache_size;
#ifdef _WIN32
	int mounting_uid;
	int mounting_gid;
	std::unordered_set<uint32_t> allowed_users;
	unsigned clean_acquired_files_period;
	unsigned clean_acquired_files_timeout;
	unsigned enable_status_updater_thread;
	unsigned ignore_utimens_update;
#else
	unsigned limit_glibc_malloc_arenas;
#endif

	bool ignore_flush;
	unsigned statfs_cache_timeout;
	bool use_quota_in_volume_size;
	bool verbose;
	bool direct_io;
	int log_notifications_area;
	unsigned message_suppression_period;

	std::string io_limits_config_file;
};

/**
 * A class that is used for passing information between subsequent calls to the filesystem.
 * It is created when a file is opened, updated with every use of the file descriptor and
 * removed when a file is closed.
 */
struct FileInfo {
	FileInfo() : flags(), direct_io(), keep_cache(), fh(), lock_owner() {}

	FileInfo(int flags, unsigned int direct_io, unsigned int keep_cache, uint64_t fh,
		uint64_t lock_owner)
			: flags(flags),
			direct_io(direct_io),
			keep_cache(keep_cache),
			fh(fh),
			lock_owner(lock_owner) {
	}

	FileInfo(const FileInfo &other) = default;
	FileInfo(FileInfo &&other) = default;

	FileInfo &operator=(const FileInfo &other) = default;
	FileInfo &operator=(FileInfo &&other) = default;

	bool isValid() const {
		return fh;
	}

	void reset() {
		*this = FileInfo();
	}

	int flags;
	unsigned int direct_io : 1;
	unsigned int keep_cache : 1;
	uint64_t fh;
	uint64_t lock_owner;
};

/**
 * Directory entry parameters, a result of some filesystem operations (lookup, mkdir,
 * link etc.).
 */
struct EntryParam {
	EntryParam() : ino(0), generation(0), attr_timeout(0), entry_timeout(0) {
		memset(&attr, 0, sizeof(struct stat));
	}

	Inode ino;
#if FUSE_USE_VERSION >= 30
	uint64_t generation;
#else
	unsigned long generation;
#endif
	struct stat attr;
	double attr_timeout;
	double entry_timeout;
};

/**
 * A result of setattr and getattr operations
 */
struct AttrReply {
	struct stat attr;
	double attrTimeout;
};

/**
 * A result of readdir operation
 */
struct DirEntry {
	std::string name;
	struct stat attr;
	off_t nextEntryOffset;

	DirEntry(const std::string n, const struct stat &s, off_t o) : name(n), attr(s), nextEntryOffset(o) {}
};

/**
 * A result of getxattr, setxattr and listattr operations
 */
struct XattrReply {
	uint32_t valueLength;
	std::vector<uint8_t> valueBuffer;
};

/**
 * An exception that is thrown when a request can't be executed successfully
 */
struct RequestException : public std::exception {
	explicit RequestException(int error_code);

	int system_error_code;
	int saunafs_error_code;
};

#ifdef _WIN32

uint8_t get_session_flags();

bool isMasterDisconnected();

void update_last_winfsp_context(const unsigned int uid, const unsigned int gid);

void convert_winfsp_context_to_master_context(unsigned int& uid, unsigned int& gid);
#endif
void updateGroups(Context &ctx);

void masterDisconnectedCallback();

// TODO what about this one? Will decide when writing non-fuse client
// void fsinit(void *userdata, struct fuse_conn_info *conn);
bool isSpecialInode(SaunaClient::Inode ino);

EntryParam lookup(Context &ctx, Inode parent, const char *name);

AttrReply getattr(Context &ctx, Inode ino);

#define SAUNAFS_SET_ATTR_MODE      (1 << 0)
#define SAUNAFS_SET_ATTR_UID       (1 << 1)
#define SAUNAFS_SET_ATTR_GID       (1 << 2)
#define SAUNAFS_SET_ATTR_SIZE      (1 << 3)
#define SAUNAFS_SET_ATTR_ATIME     (1 << 4)
#define SAUNAFS_SET_ATTR_MTIME     (1 << 5)
#define SAUNAFS_SET_ATTR_ATIME_NOW (1 << 7)
#define SAUNAFS_SET_ATTR_MTIME_NOW (1 << 8)
AttrReply setattr(Context &ctx, Inode ino, struct stat *stbuf, int to_set);

std::string readlink(Context &ctx, Inode ino);

EntryParam mknod(Context &ctx, Inode parent, const char *name, mode_t mode, dev_t rdev);

EntryParam mkdir(Context &ctx, Inode parent, const char *name, mode_t mode);

void unlink(Context &ctx, Inode parent, const char *name);

void undel(Context &ctx, Inode ino);

void rmdir(Context &ctx, Inode parent, const char *name);

EntryParam symlink(Context &ctx, const char *link, Inode parent, const char *name);

void rename(Context &ctx, Inode parent, const char *name, Inode newparent, const char *newname);

EntryParam link(Context &ctx, Inode ino, Inode newparent, const char *newname);

void open(Context &ctx, Inode ino, FileInfo* fi);

std::vector<uint8_t> read_special_inode(Context &ctx, Inode ino, size_t size, off_t off,
				        FileInfo* fi);

ReadCache::Result read(Context &ctx, Inode ino, size_t size, off_t off, FileInfo* fi);

typedef size_t BytesWritten;
BytesWritten write(Context &ctx, Inode ino, const char *buf, size_t size, off_t off,
		FileInfo* fi);

void flush(Context &ctx, Inode ino, FileInfo* fi);

void release(Inode ino, FileInfo* fi);

void fsync(Context &ctx, Inode ino, int datasync, FileInfo* fi);

void opendir(Context &ctx, Inode ino);

std::vector<DirEntry> readdir(Context &ctx, uint64_t fh, Inode ino, off_t off, size_t max_entries);

std::vector<NamedInodeEntry> readreserved(Context &ctx, NamedInodeOffset offset, NamedInodeOffset max_entries);

std::vector<NamedInodeEntry> readtrash(Context &ctx, NamedInodeOffset offset, NamedInodeOffset max_entries);

void releasedir(Inode ino);

struct statvfs statfs(Context &ctx, Inode ino);

void setxattr(Context &ctx, Inode ino, const char *name, const char *value,
		size_t size, int flags, uint32_t position);

XattrReply getxattr(Context &ctx, Inode ino, const char *name, size_t size, uint32_t position);

XattrReply listxattr(Context &ctx, Inode ino, size_t size);

void removexattr(Context &ctx, Inode ino, const char *name);

void access(Context &ctx, Inode ino, int mask);

EntryParam create(Context &ctx, Inode parent, const char *name,
		mode_t mode, FileInfo* fi);

void getlk(Context &ctx, Inode ino, FileInfo* fi, struct safs_locks::FlockWrapper &lock);
uint32_t setlk_send(Context &ctx, Inode ino, FileInfo* fi, struct safs_locks::FlockWrapper &lock);
void setlk_recv();
uint32_t flock_send(Context &ctx, Inode ino, FileInfo* fi, int op);
void flock_recv();

void flock_interrupt(const safs_locks::InterruptData &data);
void setlk_interrupt(const safs_locks::InterruptData &data);

void remove_file_info(FileInfo *f);
void remove_dir_info(FileInfo *f);

JobId makesnapshot(Context &ctx, Inode ino, Inode dst_parent, const std::string &dst_name,
	          bool can_overwrite);
std::string getgoal(Context &ctx, Inode ino);
void setgoal(Context &ctx, Inode ino, const std::string &goal_name, uint8_t smode);

void statfs(uint64_t *totalspace, uint64_t *availspace, uint64_t *trashspace, uint64_t *reservedspace, uint32_t *inodes);

std::vector<ChunkWithAddressAndLabel> getchunksinfo(Context &ctx, Inode ino,
	                                                uint32_t chunk_index, uint32_t chunk_count);

std::vector<ChunkserverListEntry> getchunkservers();

void fs_init(FsInitParams &params);
void fs_term();

}
