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

#include <dirent.h>
#include <errno.h>
#include <fstream>
#include <malloc.h>
#include <ostream>
#include <fuse.h>
#include <fuse_lowlevel.h>
#include <sys/types.h>

#include "common/crc.h"
#include "common/md5.h"
#include "common/version.h"
#include "errors/sfserr.h"
#include "common/sockets.h"
#include "mount/fuse/daemonize.h"
#include "mount/fuse/sfs_fuse.h"
#include "mount/fuse/sfs_meta_fuse.h"
#include "mount/fuse/mount_config.h"
#include "mount/g_io_limiters.h"
#include "mount/mastercomm.h"
#include "mount/masterproxy.h"
#include "mount/option_casing_normalization.h"
#include "mount/readdata.h"
#include "mount/stats.h"
#include "mount/symlinkcache.h"
#include "mount/writedata.h"
#include "protocol/SFSCommunication.h"
#include "slogger/slogger.h"

#define STR_AUX(x) #x
#define STR(x) STR_AUX(x)

constexpr const char *kEnvironmentVariableNotDefined = nullptr;
static uint8_t gLimitGlibcArenas =
    SaunaClient::FsInitParams::kDefaultLimitGlibcMallocArenas;

static void sfs_fsinit(void *userdata, struct fuse_conn_info *conn);

static struct fuse_lowlevel_ops sfs_meta_oper;

static struct fuse_lowlevel_ops sfs_oper;

static void init_fuse_lowlevel_ops() {
	sfs_meta_oper.init = sfs_fsinit;
	sfs_meta_oper.statfs = sfs_meta_statfs;
	sfs_meta_oper.lookup = sfs_meta_lookup;
	sfs_meta_oper.getattr = sfs_meta_getattr;
	sfs_meta_oper.setattr = sfs_meta_setattr;
	sfs_meta_oper.unlink = sfs_meta_unlink;
	sfs_meta_oper.rename = sfs_meta_rename;
	sfs_meta_oper.opendir = sfs_meta_opendir;
	sfs_meta_oper.readdir = sfs_meta_readdir;
	sfs_meta_oper.releasedir = sfs_meta_releasedir;
	sfs_meta_oper.open = sfs_meta_open;
	sfs_meta_oper.release = sfs_meta_release;
	sfs_meta_oper.read = sfs_meta_read;
	sfs_meta_oper.write = sfs_meta_write;

	sfs_oper.init = sfs_fsinit;
	sfs_oper.statfs = sfs_statfs;
	sfs_oper.lookup = sfs_lookup;
	sfs_oper.getattr = sfs_getattr;
	sfs_oper.setattr = sfs_setattr;
	sfs_oper.mknod = sfs_mknod;
	sfs_oper.unlink = sfs_unlink;
	sfs_oper.mkdir = sfs_mkdir;
	sfs_oper.rmdir = sfs_rmdir;
	sfs_oper.symlink = sfs_symlink;
	sfs_oper.readlink = sfs_readlink;
	sfs_oper.rename = sfs_rename;
	sfs_oper.link = sfs_link;
	sfs_oper.opendir = sfs_opendir;
	sfs_oper.readdir = sfs_readdir;
	sfs_oper.releasedir = sfs_releasedir;
	sfs_oper.create = sfs_create;
	sfs_oper.open = sfs_open;
	sfs_oper.release = sfs_release;
	sfs_oper.flush = sfs_flush;
	sfs_oper.fsync = sfs_fsync;
	sfs_oper.read = sfs_read;
	sfs_oper.write = sfs_write;
	sfs_oper.access = sfs_access;
	sfs_oper.getxattr = sfs_getxattr;
	sfs_oper.setxattr = sfs_setxattr;
	sfs_oper.listxattr = sfs_listxattr;
	sfs_oper.removexattr = sfs_removexattr;
	if (gMountOptions.filelocks) {
		sfs_oper.getlk = safs_getlk;
		sfs_oper.setlk = safs_setlk;
		sfs_oper.flock = safs_flock;
	}
}

static void sfs_fsinit(void *userdata, struct fuse_conn_info *conn) {
	(void)userdata;
	(void)conn;

	conn->want |= FUSE_CAP_DONT_MASK;

	fuse_conn_info_opts *conn_opts = (fuse_conn_info_opts *)userdata;
	fuse_apply_conn_info_opts(conn_opts, conn);
	conn->want |= FUSE_CAP_POSIX_ACL;
	conn->want &= ~FUSE_CAP_ATOMIC_O_TRUNC;

	daemonize_return_status(0);
}

static bool setup_password(std::vector<uint8_t> &md5pass) {
	md5ctx ctx;

	if (gMountOptions.password) {
		md5pass.resize(16);
		md5_init(&ctx);
		md5_update(&ctx, (uint8_t *)(gMountOptions.password), strlen(gMountOptions.password));
		md5_final(md5pass.data(), &ctx);
		memset(gMountOptions.password, 0, strlen(gMountOptions.password));
	} else if (gMountOptions.md5pass) {
		int ret = md5_parse(md5pass, gMountOptions.md5pass);
		if (ret) {
			fprintf(stderr, "bad md5 definition (md5 should be given as 32 hex digits)\n");
			return false;
		}
		memset(gMountOptions.md5pass, 0, strlen(gMountOptions.md5pass));
	}

	return true;
}

int fuse_mnt_check_empty(const char *mnt, mode_t rootmode, off_t rootsize) {
	int isempty = 1;

	if (S_ISDIR(rootmode)) {
		struct dirent *ent;
		DIR *dp = opendir(mnt);
		if (!dp) {
			return -1;
		}
		while ((ent = readdir(dp))) {
			if (strncmp(ent->d_name, ".", 1) &&
			    strncmp(ent->d_name, "..", 2)) {
				isempty = 0;
				break;
			}
		}
		closedir(dp);
	} else if (rootsize) {
		isempty = 0;
	}

	if (!isempty)
		return -1;

	return 0;
}

static int mainloop(struct fuse_args *args, struct fuse_cmdline_opts *fuse_opts,
			struct fuse_conn_info_opts *conn_opts) try {
	const char *mountpoint = fuse_opts->mountpoint;
	bool multithread = !fuse_opts->singlethread;
	bool foreground = fuse_opts->foreground;

	if (!foreground) {
		openlog(STR(APPNAME), LOG_PID | LOG_NDELAY, LOG_DAEMON);
	} else {
#if defined(LOG_PERROR)
		openlog(STR(APPNAME), LOG_PID | LOG_NDELAY | LOG_PERROR, LOG_USER);
#else
		openlog(STR(APPNAME), LOG_PID | LOG_NDELAY, LOG_USER);
#endif
	}
	safs::add_log_syslog(safs::log_level::info);
	if (!foreground)
		safs::add_log_stderr(safs::log_level::debug);

	struct rlimit rls;
	rls.rlim_cur = gMountOptions.nofile;
	rls.rlim_max = gMountOptions.nofile;
	setrlimit(RLIMIT_NOFILE, &rls);

	setpriority(PRIO_PROCESS, getpid(), gMountOptions.nice);
#ifdef SFS_USE_MEMLOCK
	if (gMountOptions.memlock) {
		rls.rlim_cur = RLIM_INFINITY;
		rls.rlim_max = RLIM_INFINITY;
		if (setrlimit(RLIMIT_MEMLOCK, &rls))
			gMountOptions.memlock = 0;
	}
#endif

#ifdef SFS_USE_MEMLOCK
	if (gMountOptions.memlock &&  !mlockall(MCL_CURRENT | MCL_FUTURE))
		safs_pretty_syslog(LOG_NOTICE, "process memory was "
				"successfully locked in RAM");
#endif

	std::vector<uint8_t> md5pass;
	if (!setup_password(md5pass)) {
		return 1;
	}
	SaunaClient::FsInitParams params(gMountOptions.bindhost ?
			gMountOptions.bindhost : "", gMountOptions.masterhost,
			gMountOptions.masterport, mountpoint);
	params.verbose = true;
	params.meta = gMountOptions.meta;
	params.subfolder = gMountOptions.subfolder;
	params.password_digest = std::move(md5pass);
	params.do_not_remember_password = gMountOptions.donotrememberpassword;
	params.delayed_init = gMountOptions.delayedinit;
	params.report_reserved_period = gMountOptions.reportreservedperiod;
	params.io_retries = gMountOptions.ioretries;
	params.io_limits_config_file = gMountOptions.iolimits ? gMountOptions.iolimits : "";
	params.bandwidth_overuse = gMountOptions.bandwidthoveruse;
	params.chunkserver_round_time_ms = gMountOptions.chunkserverrtt;
	params.chunkserver_connect_timeout_ms = gMountOptions.chunkserverconnectreadto;
	params.chunkserver_wave_read_timeout_ms = gMountOptions.chunkserverwavereadto;
	params.total_read_timeout_ms = gMountOptions.chunkservertotalreadto;
	params.cache_expiration_time_ms = gMountOptions.cacheexpirationtime;
	params.readahead_max_window_size_kB = gMountOptions.readaheadmaxwindowsize;
	params.read_cache_max_size_percentage = gMountOptions.readcachemaxsizepercentage,
	params.read_workers = gMountOptions.readworkers;
	params.max_readahead_requests = gMountOptions.maxreadaheadrequests;
	params.prefetch_xor_stripes = gMountOptions.prefetchxorstripes;
	params.bandwidth_overuse = gMountOptions.bandwidthoveruse;
	params.write_cache_size = gMountOptions.writecachesize;
	params.write_workers = gMountOptions.writeworkers;
	params.write_window_size = gMountOptions.writewindowsize;
	params.chunkserver_write_timeout_ms = gMountOptions.chunkserverwriteto;
	params.cache_per_inode_percentage = gMountOptions.cachePerInodePercentage;
	params.keep_cache = gMountOptions.keepcache;
	params.direntry_cache_timeout = gMountOptions.direntrycacheto;
	params.direntry_cache_size = gMountOptions.direntrycachesize;
	params.entry_cache_timeout = gMountOptions.entrycacheto;
	params.attr_cache_timeout = gMountOptions.attrcacheto;
	params.mkdir_copy_sgid = gMountOptions.mkdircopysgid;
	params.sugid_clear_mode = gMountOptions.sugidclearmode;
	params.use_rw_lock = gMountOptions.rwlock;
	params.acl_cache_timeout = gMountOptions.aclcacheto;
	params.acl_cache_size = gMountOptions.aclcachesize;
	params.debug_mode = gMountOptions.debug;
	params.direct_io = gMountOptions.directio;
	params.ignore_flush = gMountOptions.ignoreflush;
	params.log_notifications_area = gMountOptions.lognotificationarea;
	params.message_suppression_period = gMountOptions.messagesuppressionperiod;
	params.statfs_cache_timeout = gMountOptions.statfscachetimeout;
	params.use_quota_in_volume_size = gMountOptions.usequotainvolumesize;

	if (!gMountOptions.meta) {
		SaunaClient::fs_init(params);
	} else {
		masterproxy_init();
		symlink_cache_init();
		if (gMountOptions.delayedinit) {
			fs_init_master_connection(params);
		} else {
			if (fs_init_master_connection(params) < 0) {
				return 1;
			}
		}
		fs_init_threads(params.io_retries);
	}

	struct fuse_session *se;
	if (gMountOptions.meta) {
		sfs_meta_init(gMountOptions.debug, gMountOptions.entrycacheto, gMountOptions.attrcacheto);
		se = fuse_session_new(args, &sfs_meta_oper, sizeof(sfs_meta_oper), (void *)conn_opts);
	} else {
		se = fuse_session_new(args, &sfs_oper, sizeof(sfs_oper), (void *)conn_opts);
	}
	if (!se) {
		fprintf(stderr, "error in fuse_session_new\n");
		usleep(100000);  // time for print other error messages by FUSE
		if (!gMountOptions.meta) {
			SaunaClient::fs_term();
		} else {
			masterproxy_term();
			fs_term();
			symlink_cache_term();
		}
		return 1;
	}

	if (fuse_set_signal_handlers(se)) {
		fprintf(stderr, "error in fuse_set_signal_handlers\n");
		fuse_session_destroy(se);
		if (!gMountOptions.meta) {
			SaunaClient::fs_term();
		} else {
			masterproxy_term();
			fs_term();
			symlink_cache_term();
		}
		return 1;
	}

	if (fuse_session_mount(se, mountpoint)) {
		fprintf(stderr, "error in fuse_session_mount\n");
		fuse_remove_signal_handlers(se);
		fuse_session_destroy(se);
		if (!gMountOptions.meta) {
			SaunaClient::fs_term();
		} else {
			masterproxy_term();
			fs_term();
			symlink_cache_term();
		}
		return 1;
	}

	if (!gMountOptions.debug && !foreground) {
		setsid();
		setpgid(0, getpid());
		int nullfd = open("/dev/null", O_RDWR, 0);
		if (nullfd != -1) {
			(void)dup2(nullfd, 0);
			(void)dup2(nullfd, 1);
			(void)dup2(nullfd, 2);
			if (nullfd > 2)
				close(nullfd);
		}
	}

	int err;
	if (multithread) {
		err = fuse_session_loop_mt(se, fuse_opts->clone_fd);
	} else {
		err = fuse_session_loop(se);
	}
	fuse_remove_signal_handlers(se);
	fuse_session_unmount(se);
	fuse_session_destroy(se);
	if (!gMountOptions.meta) {
		SaunaClient::fs_term();
	} else {
		masterproxy_term();
		fs_term();
		symlink_cache_term();
	}
	return err ? 1 : 0;
} catch (...) {
	return 1;
}

static unsigned int strncpy_remove_commas(char *dstbuff, unsigned int dstsize, char *src) {
	char c;
	unsigned int l;
	l = 0;
	while ((c = *src++) && l + 1 < dstsize) {
		if (c != ',') {
			*dstbuff++ = c;
			l++;
		}
	}
	*dstbuff = 0;
	return l;
}

static unsigned int strncpy_escape_commas(char *dstbuff, unsigned int dstsize, char *src) {
	char c;
	unsigned int l;
	l = 0;
	while ((c = *src++) && l + 1 < dstsize) {
		if (c != ',' && c != '\\') {
			*dstbuff++ = c;
			l++;
		} else {
			if (l + 2 < dstsize) {
				*dstbuff++ = '\\';
				*dstbuff++ = c;
				l += 2;
			} else {
				*dstbuff = 0;
				return l;
			}
		}
	}
	*dstbuff = 0;
	return l;
}

static void make_fsname(struct fuse_args *args) {
	unsigned int l;
	char fsnamearg[256];
	int libver = fuse_version();
	unsigned int (*strncpy_commas)(char*, unsigned int, char*) = libver >= 28 ? strncpy_escape_commas : strncpy_remove_commas;
	const char *fmt = libver >= 27 ? "-osubtype=sfs%s,fsname=" : "-ofsname=sfs%s#";
	l = snprintf(fsnamearg, 256, fmt, (gMountOptions.meta) ? "meta" : "");
	l += strncpy_commas(fsnamearg + l, 256 - l, gMountOptions.masterhost);

	if (l < 255)
		fsnamearg[l++] = ':';

	l += strncpy_commas(fsnamearg + l, 256 - l, gMountOptions.masterport);

	if (gMountOptions.subfolder[0] != '/' && l < 255)
		fsnamearg[l++] = '/';

	if (gMountOptions.subfolder[0] != '/' && gMountOptions.subfolder[1] != 0)
		l += strncpy_commas(fsnamearg + l, 256 - l, gMountOptions.subfolder);

	if (l > 255)
		l = 255;

	fsnamearg[l] = 0;
	fuse_opt_insert_arg(args, 1, fsnamearg);
}

static int is_dns_char(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '-' || c == '.';
}

static size_t count_colons_in_str(const char *str, size_t len) {
	size_t colons_count = 0;

	for (size_t i = 0; i < len; i++)
		if (str[i] == ':')
			colons_count++;

	return colons_count;
}

/**
 * Find and parse arg that matches to HOST[:PORT]:[PATH] pattern.
 */
static int read_masterhost_if_present(struct fuse_args *args) {
	if (args->argc < 2)
		return 0;

	char *c;
	int optpos = 1;

	while (optpos < args->argc) {
		c = args->argv[optpos];

		if (!strncmp(c, "-o", 2))
			optpos += strlen(c) > 2 ? 1 : 2;
		else
			break;
	}

	if (optpos >= args->argc)
		return 0;

	size_t colons = count_colons_in_str(c, strlen(c));

	if (!colons)
		return 0;

	uint32_t hostlen = 0;

	while (is_dns_char(*c)) {
		c++;
		hostlen++;
	}

	if (!hostlen)
		return 0;

	uint32_t portlen = 0;
	char *portbegin = NULL;

	if (*c == ':' && colons > 1) {
		c++;
		portbegin = c;
		while (*c >= '0' && *c <= '9') {
			c++;
			portlen++;
		}
	}

	if (*c != ':')
		return 0;

	c++;

	if (*c)
		gMountOptions.subfolder = strdup(c);

	if (!(gMountOptions.masterhost = (char*)malloc(hostlen + 1)))
		return -1;
	strncpy(gMountOptions.masterhost, args->argv[optpos], hostlen);

	if (portbegin && portlen) {
		if (!(gMountOptions.masterport = (char*)malloc(portlen + 1)))
			return -1;
		strncpy(gMountOptions.masterport, portbegin, portlen);
	}

	for (int i = optpos + 1; i < args->argc; i++)
		args->argv[i - 1] = args->argv[i];

	args->argc--;

	return 0;
}

/// (glibc specific) Tune glibc malloc arenas to avoid high virtual memory usage
inline void tuneMalloc() {
#if defined(SAUNAFS_HAVE_MALLOPT) && defined(M_ARENA_MAX) && \
    defined(M_ARENA_TEST)

	if (gLimitGlibcArenas > 0) {
		if (::getenv("MALLOC_ARENA_MAX") == kEnvironmentVariableNotDefined) {
			safs::log_info("Setting glibc malloc arena max to {}",
			               gLimitGlibcArenas);
			::mallopt(M_ARENA_MAX, static_cast<int>(gLimitGlibcArenas));
		}
		if (::getenv("MALLOC_ARENA_TEST") == kEnvironmentVariableNotDefined) {
			safs::log_info("Setting glibc malloc arena test to {}",
			               gLimitGlibcArenas);
			::mallopt(M_ARENA_TEST, static_cast<int>(gLimitGlibcArenas));
		}
	}
#endif
}

int main(int argc, char *argv[]) try {
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct fuse_args defaultargs = FUSE_ARGS_INIT(0, NULL);

	normalize_options_casing(args.argc, args.argv);

	fuse_opt_add_arg(&defaultargs, "fakeappname");

	if (read_masterhost_if_present(&args))
		exit(1);

	if (fuse_opt_parse(&args, &defaultargs, gSfsOptsStage1, sfs_opt_proc_stage1))
		exit(1);

	if (!gCustomCfg)
		sfs_opt_parse_cfg_file(DEFAULT_SFSMOUNT_CONFIG_PATH, 1, &defaultargs);

	normalize_options_casing(defaultargs.argc, defaultargs.argv);

	if (fuse_opt_parse(&defaultargs, &gMountOptions, gSfsOptsStage2, sfs_opt_proc_stage2))
		exit(1);

	if (defaultargs.argc > 1) {
		fprintf(stderr, "Unexpected/wrong option(s) at cfg file:\n");
		for (int i = 1; i < defaultargs.argc; i++) {
			if (strcmp(defaultargs.argv[i], "-o") != 0) {
				fprintf(stderr, "%s\n", defaultargs.argv[i]);
			}
		}
		exit(1);
	}

	if (fuse_opt_parse(&args, &gMountOptions, gSfsOptsStage2, sfs_opt_proc_stage2))
		exit(1);

	struct fuse_conn_info_opts *conn_opts;
	conn_opts = fuse_parse_conn_info_opts(&args);
	if (!conn_opts) {
		exit(1);
	}

	init_fuse_lowlevel_ops();

	if (gMountOptions.cachemode && gMountOptions.cachefiles) {
		fprintf(stderr,
			"sfscachemode and sfscachefiles options are exclusive "
			"- use only " "sfscachemode\nsee: %s -h for help\n",
		        argv[0]);
		return 1;
	}

	if (!gMountOptions.cachemode) {
		gMountOptions.keepcache = (gMountOptions.cachefiles) ? 1 : 0;
	} else if (!strcasecmp(gMountOptions.cachemode, "AUTO")) {
		gMountOptions.keepcache = 0;
	} else if (!strcasecmp(gMountOptions.cachemode, "YES") ||
	           !strcasecmp(gMountOptions.cachemode, "ALWAYS")) {
		gMountOptions.keepcache = 1;
	} else if (!strcasecmp(gMountOptions.cachemode, "NO") ||
	           !strcasecmp(gMountOptions.cachemode, "NONE") ||
	           !strcasecmp(gMountOptions.cachemode, "NEVER")) {
		gMountOptions.keepcache = 2;
	} else {
		fprintf(stderr, "unrecognized cachemode option\nsee: %s -h "
				"for help\n", argv[0]);
		return 1;
	}
	if (!gMountOptions.sugidclearmodestr) {
		gMountOptions.sugidclearmode = SaunaClient::FsInitParams::kDefaultSugidClearMode;
	} else if (!strcasecmp(gMountOptions.sugidclearmodestr, "NEVER")) {
		gMountOptions.sugidclearmode = SugidClearMode::kNever;
	} else if (!strcasecmp(gMountOptions.sugidclearmodestr, "ALWAYS")) {
		gMountOptions.sugidclearmode = SugidClearMode::kAlways;
	} else if (!strcasecmp(gMountOptions.sugidclearmodestr, "OSX")) {
		gMountOptions.sugidclearmode = SugidClearMode::kOsx;
	} else if (!strcasecmp(gMountOptions.sugidclearmodestr, "BSD")) {
		gMountOptions.sugidclearmode = SugidClearMode::kBsd;
	} else if (!strcasecmp(gMountOptions.sugidclearmodestr, "EXT")) {
		gMountOptions.sugidclearmode = SugidClearMode::kExt;
	} else if (!strcasecmp(gMountOptions.sugidclearmodestr, "SFS")) {
		gMountOptions.sugidclearmode = SugidClearMode::kSfs;
	} else {
		fprintf(stderr, "unrecognized sugidclearmode option\nsee: %s "
				"-h for help\n", argv[0]);
		return 1;
	}

	if (!gMountOptions.masterhost)
		gMountOptions.masterhost = strdup(DEFAULT_MASTER_HOSTNAME);

	if (!gMountOptions.masterport)
		gMountOptions.masterport = strdup(DEFAULT_MASTER_PORT);

	if (!gMountOptions.subfolder)
		gMountOptions.subfolder = strdup(DEFAULT_MOUNTED_SUBFOLDER);

	if (!gMountOptions.nofile)
		gMountOptions.nofile = 100000;

	if (!gMountOptions.writecachesize)
		gMountOptions.writecachesize = 128;

	if (gMountOptions.cachePerInodePercentage < 1) {
		fprintf(stderr, "cache per inode percentage too low (%u %%) - "
				"increased to 1%%\n",
		        gMountOptions.cachePerInodePercentage);
		gMountOptions.cachePerInodePercentage = 1;
	}

	if (gMountOptions.cachePerInodePercentage > 100) {
		fprintf(stderr, "cache per inode percentage too big (%u %%) - "
				"decreased to 100%%\n",
		        gMountOptions.cachePerInodePercentage);
		gMountOptions.cachePerInodePercentage = 100;
	}

	if (gMountOptions.writecachesize < 16) {
		fprintf(stderr, "write cache size too low (%u MiB) - "
				"increased to 16 MiB\n",
		        gMountOptions.writecachesize);
		gMountOptions.writecachesize = 16;
	}

	if (gMountOptions.writecachesize > 1024 * 1024) {
		fprintf(stderr, "write cache size too big (%u MiB) - "
				"decreased to 1 TiB\n",
		        gMountOptions.writecachesize);
		gMountOptions.writecachesize = 1024 * 1024;
	}

	if (gMountOptions.writeworkers < 1) {
		fprintf(stderr, "no write workers - increasing number of "
				"workers to 1\n");
		gMountOptions.writeworkers = 1;
	}

	if (gMountOptions.writewindowsize < 1) {
		fprintf(stderr, "write window size is 0 - increasing to 1\n");
		gMountOptions.writewindowsize = 1;
	}

	if (gMountOptions.readworkers < 1) {
		fprintf(stderr, "no read workers - increasing number of "
				"workers to 1\n");
		gMountOptions.readworkers = 1;
	}

	if (!gMountOptions.nostdmountoptions)
		fuse_opt_add_arg(&args, "-o" DEFAULT_OPTIONS);

	if (gMountOptions.aclcachesize > 1000 * 1000) {
		fprintf(stderr, "acl cache size too big (%u) - decreased to "
				"1000000\n", gMountOptions.aclcachesize);
		gMountOptions.aclcachesize = 1000 * 1000;
	}

	if (gMountOptions.direntrycachesize > 10000000) {
		fprintf(stderr, "directory entry cache size too big (%u) - "
				"decreased to 10000000\n",
		        gMountOptions.direntrycachesize);
		gMountOptions.direntrycachesize = 10000000;
	}

	gLimitGlibcArenas = gMountOptions.limitglibcmallocarenas;
	tuneMalloc();

	make_fsname(&args);

	struct fuse_cmdline_opts fuse_opts;
	if (fuse_parse_cmdline(&args, &fuse_opts)) {
		fprintf(stderr, "see: %s -h for help\n", argv[0]);
		return 1;
	}

	if (fuse_opts.show_help) {
		usage(argv[0]);
		return 0;
	}

	if (fuse_opts.show_version) {
		std::cout << common::version() << '\n';
		printf("FUSE library version: %s\n", fuse_pkgversion());
		fuse_lowlevel_version();
		return 0;
	}

	if (gMountOptions.passwordask && !gMountOptions.password && !gMountOptions.md5pass)
		gMountOptions.password = getpass("SaunaFS Password:");

	if (!fuse_opts.mountpoint) {
		if (gDefaultMountpoint) {
			fuse_opts.mountpoint = gDefaultMountpoint;
		} else {
			fprintf(stderr, "no mount point\nsee: %s -h for help\n", argv[0]);
			return 1;
		}
	}

	int res;
	struct stat stbuf;
	res = stat(fuse_opts.mountpoint, &stbuf);
	if (res) {
		fprintf(stderr, "failed to access mountpoint %s: %s\n",
			fuse_opts.mountpoint, strerror(errno));
		return 1;
	}
	if (!gMountOptions.nonemptymount) {
		if (fuse_mnt_check_empty(fuse_opts.mountpoint, stbuf.st_mode,
					 stbuf.st_size)) {
			return 1;
		}
	}

	if (!fuse_opts.foreground)
		res = daemonize_and_wait(std::bind(&mainloop, &args, &fuse_opts, conn_opts));
	else
		res = mainloop(&args, &fuse_opts, conn_opts);

	fuse_opt_free_args(&args);
	fuse_opt_free_args(&defaultargs);
	free(gMountOptions.masterhost);
	free(gMountOptions.masterport);
	if (gMountOptions.bindhost)
		free(gMountOptions.bindhost);
	free(gMountOptions.subfolder);
	if (gMountOptions.iolimits)
		free(gMountOptions.iolimits);
	if (gDefaultMountpoint && gDefaultMountpoint != fuse_opts.mountpoint)
		free(gDefaultMountpoint);
	free(fuse_opts.mountpoint);
	free(conn_opts);
	stats_term();
	return res;
} catch (std::bad_alloc& ex) {
	mabort("run out of memory");
}
