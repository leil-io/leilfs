/*
   Copyright 2013-2018 Skytechnology sp. z o.o.
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

#include <stddef.h>
#include <string.h>
#include <fuse.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

#include "protocol/SFSCommunication.h"
#include "mount/sauna_client.h"

#if defined(SAUNAFS_HAVE_MLOCKALL) && defined(RLIMIT_MEMLOCK)
#  include <sys/mman.h>
#endif

#if defined(MCL_CURRENT) && defined(MCL_FUTURE)
#  define SFS_USE_MEMLOCK
#endif

#if defined(__APPLE__)
#  define DEFAULT_OPTIONS "allow_other,default_permissions,daemon_timeout=600,iosize=65536"
#else
#  define DEFAULT_OPTIONS "allow_other,default_permissions"
#endif

enum {
	KEY_CFGFILE,
	KEY_META,
	KEY_HOST,
	KEY_PORT,
	KEY_BIND,
	KEY_PATH,
	KEY_PASSWORDASK,
	KEY_NOSTDMOUNTOPTIONS,
	KEY_NONEMPTY,
	KEY_HELP,
	KEY_VERSION
};

struct sfsopts_ {
	char *masterhost;
	char *masterport;
	char *bindhost;
	char *subfolder;
	char *password;
	char *md5pass;
	unsigned nofile;
	signed nice;
#ifdef SFS_USE_MEMLOCK
	int memlock;
#endif
	int filelocks;
	int nostdmountoptions;
	int meta;
	int debug;
	int delayedinit;
	int acl;
	double aclcacheto;
	unsigned aclcachesize;
	int rwlock;
	int mkdircopysgid;
	char *sugidclearmodestr;
	SugidClearMode sugidclearmode;
	char *cachemode;
	int cachefiles;
	int keepcache;
	int passwordask;
	int donotrememberpassword;
	unsigned writecachesize;
	unsigned cachePerInodePercentage;
	unsigned writeworkers;
	unsigned ioretries;
	unsigned writewindowsize;
	double attrcacheto;
	double entrycacheto;
	double direntrycacheto;
	unsigned direntrycachesize;
	unsigned reportreservedperiod;
	char *iolimits;
	int chunkserverrtt;
	int chunkserverconnectreadto;
	int chunkserverwavereadto;
	int chunkservertotalreadto;
	int chunkserverwriteto;
	int cacheexpirationtime;
	int readaheadmaxwindowsize;
	int readcachemaxsizepercentage;
	unsigned readworkers;
	unsigned maxreadaheadrequests;
	int prefetchxorstripes;
	unsigned symlinkcachetimeout;
	double bandwidthoveruse;
	int nonemptymount;
	bool directio;
	int ignoreflush;
	unsigned limitglibcmallocarenas;
	int lognotificationarea;
	unsigned messagesuppressionperiod;
	int statfscachetimeout;
	int usequotainvolumesize;

	sfsopts_()
		: masterhost(NULL),
		masterport(NULL),
		bindhost(NULL),
		subfolder(NULL),
		password(NULL),
		md5pass(NULL),
		nofile(0),
		nice(-19),
#ifdef SFS_USE_MEMLOCK
		memlock(0),
#endif
		filelocks(0),
		nostdmountoptions(0),
		meta(0),
		debug(SaunaClient::FsInitParams::kDefaultDebugMode),
		delayedinit(SaunaClient::FsInitParams::kDefaultDelayedInit),
		acl(), // deprecated
		aclcacheto(SaunaClient::FsInitParams::kDefaultAclCacheTimeout),
		aclcachesize(SaunaClient::FsInitParams::kDefaultAclCacheSize),
		rwlock(SaunaClient::FsInitParams::kDefaultUseRwLock),
		mkdircopysgid(SaunaClient::FsInitParams::kDefaultMkdirCopySgid),
		sugidclearmodestr(NULL),
		sugidclearmode(SaunaClient::FsInitParams::kDefaultSugidClearMode),
		cachemode(NULL),
		cachefiles(0),
		keepcache(SaunaClient::FsInitParams::kDefaultKeepCache),
		passwordask(0),
		donotrememberpassword(SaunaClient::FsInitParams::kDefaultDoNotRememberPassword),
		writecachesize(SaunaClient::FsInitParams::kDefaultWriteCacheSize),
		cachePerInodePercentage(SaunaClient::FsInitParams::kDefaultCachePerInodePercentage),
		writeworkers(SaunaClient::FsInitParams::kDefaultWriteWorkers),
		ioretries(SaunaClient::FsInitParams::kDefaultIoRetries),
		writewindowsize(SaunaClient::FsInitParams::kDefaultWriteWindowSize),
		attrcacheto(SaunaClient::FsInitParams::kDefaultAttrCacheTimeout),
		entrycacheto(SaunaClient::FsInitParams::kDefaultEntryCacheTimeout),
		direntrycacheto(SaunaClient::FsInitParams::kDefaultDirentryCacheTimeout),
		direntrycachesize(SaunaClient::FsInitParams::kDefaultDirentryCacheSize),
		reportreservedperiod(SaunaClient::FsInitParams::kDefaultReportReservedPeriod),
		iolimits(NULL),
		chunkserverrtt(SaunaClient::FsInitParams::kDefaultRoundTime),
		chunkserverconnectreadto(SaunaClient::FsInitParams::kDefaultChunkserverConnectTo),
		chunkserverwavereadto(SaunaClient::FsInitParams::kDefaultChunkserverWaveReadTo),
		chunkservertotalreadto(SaunaClient::FsInitParams::kDefaultChunkserverTotalReadTo),
		chunkserverwriteto(SaunaClient::FsInitParams::kDefaultChunkserverWriteTo),
		cacheexpirationtime(SaunaClient::FsInitParams::kDefaultCacheExpirationTime),
		readaheadmaxwindowsize(SaunaClient::FsInitParams::kDefaultReadaheadMaxWindowSize),
		readcachemaxsizepercentage(SaunaClient::FsInitParams::kDefaultReadCacheMaxSizePercentage),
		readworkers(SaunaClient::FsInitParams::kDefaultReadWorkers),
		maxreadaheadrequests(SaunaClient::FsInitParams::kDefaultMaxReadaheadRequests),
		prefetchxorstripes(SaunaClient::FsInitParams::kDefaultPrefetchXorStripes),
		symlinkcachetimeout(SaunaClient::FsInitParams::kDefaultSymlinkCacheTimeout),
		bandwidthoveruse(SaunaClient::FsInitParams::kDefaultBandwidthOveruse),
		nonemptymount(SaunaClient::FsInitParams::kDefaultNonEmptyMounts),
		directio(SaunaClient::FsInitParams::kDirectIO),
		ignoreflush(SaunaClient::FsInitParams::kDefaultIgnoreFlush),
		limitglibcmallocarenas(SaunaClient::FsInitParams::kDefaultLimitGlibcMallocArenas),
		statfscachetimeout(SaunaClient::FsInitParams::kDefaultStatfsCacheTo),
		usequotainvolumesize(SaunaClient::FsInitParams::kDefaultUseQuotaInVolumeSize)
	{ }
};

extern sfsopts_ gMountOptions;
extern int gCustomCfg;
extern char *gDefaultMountpoint;
extern fuse_opt gSfsOptsStage1[];
extern fuse_opt gSfsOptsStage2[];

void usage(const char *progname);
void sfs_opt_parse_cfg_file(const char *filename,int optional,struct fuse_args *outargs);
int sfs_opt_proc_stage1(void *data, const char *arg, int key, struct fuse_args *outargs);
int sfs_opt_proc_stage2(void *data, const char *arg, int key, struct fuse_args *outargs);
