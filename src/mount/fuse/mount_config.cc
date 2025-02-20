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

#include "common/platform.h"
#include "mount/fuse/mount_config.h"
#include "mount/sugid_clear_mode_string.h"

#include <stdio.h>
#include <stdlib.h>
#include <fuse.h>
#include <fuse_lowlevel.h>
#include <regex>

#define SFS_OPT(t, p, v) { t, offsetof(struct sfsopts_, p), v }

sfsopts_ gMountOptions;

int gCustomCfg = 0;
char *gDefaultMountpoint = NULL;
extern std::string gCfgString;

struct fuse_opt gSfsOptsStage1[] = {
	FUSE_OPT_KEY("sfscfgfile=",    KEY_CFGFILE),
	FUSE_OPT_KEY("-c ",            KEY_CFGFILE),
	FUSE_OPT_END
};

struct fuse_opt gSfsOptsStage2[] = {
	SFS_OPT("sfsmaster=%s", masterhost, 0),
	SFS_OPT("sfsport=%s", masterport, 0),
	SFS_OPT("sfsbind=%s", bindhost, 0),
	SFS_OPT("sfssubfolder=%s", subfolder, 0),
	SFS_OPT("sfspassword=%s", password, 0),
	SFS_OPT("askpassword", passwordask, 1),
	SFS_OPT("sfsmd5pass=%s", md5pass, 0),
	SFS_OPT("sfsrlimitnofile=%u", nofile, 0),
	SFS_OPT("sfsnice=%d", nice, 0),
#ifdef SFS_USE_MEMLOCK
	SFS_OPT("sfsmemlock", memlock, 1),
#endif
	SFS_OPT("sfswritecachesize=%u", writecachesize, 0),
	SFS_OPT("sfsaclcachesize=%u", aclcachesize, 0),
	SFS_OPT("sfscacheperinodepercentage=%u", cachePerInodePercentage, 0),
	SFS_OPT("sfswriteworkers=%u", writeworkers, 0),
	SFS_OPT("sfsioretries=%u", ioretries, 0),
	SFS_OPT("sfswritewindowsize=%u", writewindowsize, 0),
	SFS_OPT("sfsdebug", debug, 1),
	SFS_OPT("sfsmeta", meta, 1),
	SFS_OPT("sfsdelayedinit", delayedinit, 1),
	SFS_OPT("sfsacl", acl, 1),
	SFS_OPT("sfsrwlock=%d", rwlock, 0),
	SFS_OPT("sfsdonotrememberpassword", donotrememberpassword, 1),
	SFS_OPT("sfscachefiles", cachefiles, 1),
	SFS_OPT("sfscachemode=%s", cachemode, 0),
	SFS_OPT("sfsmkdircopysgid=%u", mkdircopysgid, 0),
	SFS_OPT("sfssugidclearmode=%s", sugidclearmodestr, 0),
	SFS_OPT("sfsattrcacheto=%lf", attrcacheto, 0),
	SFS_OPT("sfsentrycacheto=%lf", entrycacheto, 0),
	SFS_OPT("sfsdirectio=%d", directio, 0),
	SFS_OPT("sfsdirentrycacheto=%lf", direntrycacheto, 0),
	SFS_OPT("sfsaclcacheto=%lf", aclcacheto, 0),
	SFS_OPT("sfsreportreservedperiod=%u", reportreservedperiod, 0),
	SFS_OPT("sfsiolimits=%s", iolimits, 0),
	SFS_OPT("sfschunkserverrtt=%d", chunkserverrtt, 0),
	SFS_OPT("sfschunkserverconnectreadto=%d", chunkserverconnectreadto, 0),
	SFS_OPT("sfschunkserverwavereadto=%d", chunkserverwavereadto, 0),
	SFS_OPT("sfschunkservertotalreadto=%d", chunkservertotalreadto, 0),
	SFS_OPT("cacheexpirationtime=%d", cacheexpirationtime, 0),
	SFS_OPT("readaheadmaxwindowsize=%d", readaheadmaxwindowsize, 4096),
	SFS_OPT("readcachemaxsizepercentage=%d", readcachemaxsizepercentage, 60),
	SFS_OPT("readworkers=%d", readworkers, 1),
	SFS_OPT("maxreadaheadrequests=%d", maxreadaheadrequests, 0),
	SFS_OPT("sfsprefetchxorstripes", prefetchxorstripes, 1),
	SFS_OPT("sfschunkserverwriteto=%d", chunkserverwriteto, 0),
	SFS_OPT("symlinkcachetimeout=%d", symlinkcachetimeout, 3600),
	SFS_OPT("bandwidthoveruse=%lf", bandwidthoveruse, 1),
	SFS_OPT("sfsdirentrycachesize=%u", direntrycachesize, 0),
	SFS_OPT("nostdmountoptions", nostdmountoptions, 1),
	SFS_OPT("sfsignoreflush=%d", ignoreflush, 0),
	SFS_OPT("limitglibcmallocarenas=%d", limitglibcmallocarenas, 0),
	SFS_OPT("sfslognotificationarea=%d", lognotificationarea, 0),
	SFS_OPT("sfsmessagesuppressionperiod=%u", messagesuppressionperiod, 0),
	SFS_OPT("statfscachetimeout=%d", statfscachetimeout, 0),
	SFS_OPT("usequotainvolumesize=%d", usequotainvolumesize, 0),

	SFS_OPT("enablefilelocks=%u", filelocks, 0),
	SFS_OPT("nonempty", nonemptymount, 1),

	FUSE_OPT_KEY("-m",             KEY_META),
	FUSE_OPT_KEY("--meta",         KEY_META),
	FUSE_OPT_KEY("-H ",            KEY_HOST),
	FUSE_OPT_KEY("-P ",            KEY_PORT),
	FUSE_OPT_KEY("-B ",            KEY_BIND),
	FUSE_OPT_KEY("-S ",            KEY_PATH),
	FUSE_OPT_KEY("-p",             KEY_PASSWORDASK),
	FUSE_OPT_KEY("--password",     KEY_PASSWORDASK),
	FUSE_OPT_KEY("-n",             KEY_NOSTDMOUNTOPTIONS),
	FUSE_OPT_KEY("--nostdopts",    KEY_NOSTDMOUNTOPTIONS),
	FUSE_OPT_KEY("--nonempty",     KEY_NONEMPTY),
	FUSE_OPT_END
};

void usage(const char *progname) {
	printf(
"usage: %s  [HOST[/PORT]:[PATH]] [options] mountpoint\n"
"\n", progname);
	fuse_cmdline_help();

	printf(
"\nGeneral options:\n"
"    -c CFGFILE                  equivalent to '-o sfscfgfile=CFGFILE'\n"
"    -H HOST                     equivalent to '-o sfsmaster=HOST'\n"
"    -P PORT                     equivalent to '-o sfsport=PORT'\n"
"    -B IP                       equivalent to '-o sfsbind=IP'\n"
"    -S PATH                     equivalent to '-o sfssubfolder=PATH'\n"
"\n"
"Security/permissions related options:\n"
"    -o sfspassword=PASSWORD     authenticate to sfsmaster with password\n"
"    -o sfsmd5pass=MD5           authenticate to sfsmaster using directly "
				"given md5 (only if sfspassword is not defined)\n"
"    -p   --password             similar to '-o sfspassword=PASSWORD', but "
				"show prompt and ask user for password\n"
"    -o askpassword              show prompt and ask user for password\n"
"    -o sfsdonotrememberpassword  do not remember password in memory - more "
				"secure, but when session is lost then new"
				"session is created without password\n"
"\n"
"Read related options:\n"
"    -o cacheexpirationtime=MSEC  set timeout for read cache entries to be "
				"considered valid in milliseconds (0 disables "
				"cache) (default: %u)\n"
"    -o readaheadmaxwindowsize=KB  set max value of readahead window per single "
				"descriptor in kibibytes (default: %u)\n"
"    -o readworkers=N            define number of read workers (default: %u)\n"
"    -o maxreadaheadrequests=N   define number of readahead requests per inode "
				"(default: %u)\n"
"    -o sfschunkserverconnectreadto=MSEC  set timeout for connecting with "
				"chunkservers during read operation in "
				"milliseconds (default: %u)\n"
"    -o sfschunkservertotalreadto=MSEC  set timeout for the whole "
				"communication with chunkservers during a "
				"read operation in milliseconds (default: %u)\n"
"    -o sfsprefetchxorstripes    prefetch full xor stripe on every first read "
				"of a xor chunk\n"
"    -o bandwidthoveruse=N       define ratio of allowed bandwidth overuse "
				"when fetching data (default: %.2f)\n"
"    -o readcachemaxsizepercentage=P  specifies the maximum percentage of "
				"system memory for the read cache (default: %u%%)\n"
"\n"
"Write related options:\n"
"    -o sfschunkserverwriteto=MSEC  set chunkserver response timeout during "
				"write operation in milliseconds (default: %u)\n"
"    -o sfswritecachesize=N      define size of write cache in MiB (default: %u)\n"
"    -o sfscacheperinodepercentage=P  define what part of the write cache non "
				"occupied by other inodes can a single inode "
				"occupy (in %%, default: %u)\n"
"    -o sfswriteworkers=N        define number of write workers (default: %u)\n"
"    -o sfswritewindowsize=N     define write window size (in blocks) for "
				"each chunk (default: %u)\n"
"    -o sfsignoreflush=0|1       Advanced: use with caution. Ignore flush usual "
				"behavior by replying SUCCESS to it immediately. Targets fast "
				"creation of small files, but may cause data loss during crashes "
				"(default: %d)\n"
"\n"
"Other options:\n"
"    -m   --meta                 equivalent to '-o sfsmeta'\n"
"    -n   --nostdopts            do not add standard SaunaFS mount options: "
"'-o " DEFAULT_OPTIONS ",fsname=SFS'\n"
"    --nonempty                  allow mounts over non-empty file/dir\n"
"    -o nostdmountoptions        equivalent of --nostdopts for /etc/fstab\n"
"    -o sfscfgfile=CFGFILE       load some mount options from external file "
				"(if not specified then use default file: "
				ETC_PATH "/sfsmount.cfg)\n"
"    -o sfsmeta                  mount meta filesystem (trash etc.)\n"
"    -o sfsdelayedinit           connection with master is done in background "
				"- with this option mount can be run without "
				"network (good for being run from fstab/init "
				"scripts etc.)\n"
"    -o sfsacl                   DEPRECATED, used to enable/disable ACL "
				"support, ignored now\n"
"    -o sfsrwlock=0|1            when set to 1, parallel reads from the same "
				"descriptor are performed (default: %d)\n"
"    -o sfsmkdircopysgid=N       sgid bit should be copied during mkdir "
				"operation (default: %d)\n"
"    -o sfssugidclearmode=SMODE  set sugid clear mode (see below ; default: %s)\n"
"    -o sfscachemode=CMODE       set cache mode (see below ; default: AUTO)\n"
"    -o sfscachefiles            (deprecated) equivalent to '-o sfscachemode=YES'\n"
"    -o sfsattrcacheto=SEC       set attributes cache timeout in seconds "
				"(default: %.2f)\n"
"    -o sfsentrycacheto=SEC      set file entry cache timeout in seconds "
				"(default: %.2f)\n"
"    -o sfsdirectio=0|1          set DirectIO mode (default: 0)\n"
"    -o sfsdirentrycacheto=SEC   set directory entry cache timeout in seconds "
				"(default: %.2f)\n"
"    -o sfsdirentrycachesize=N   define directory entry cache size in number "
				"of entries (default: %u)\n"
"    -o sfsaclcacheto=SEC        set ACL cache timeout in seconds (default: %.2f)\n"
"    -o sfsreportreservedperiod=SEC  set reporting reserved inodes interval in "
				"seconds (default: %u)\n"
"    -o sfschunkserverrtt=MSEC   set timeout after which SYN packet is "
				"considered lost during the first retry of "
				"connecting a chunkserver (default: %u)\n"
"    -o sfschunkserverwavereadto=MSEC  set timeout for executing each wave "
				"of a read operation in milliseconds (default: %u)\n"
"    -o sfsnice=N                on startup sfsmount tries to change his "
				"'nice' value (default: -19)\n"
#ifdef SFS_USE_MEMLOCK
"    -o sfsmemlock               try to lock memory\n"
#endif
"    -o sfsaclcachesize=N        define ACL cache size in number of entries "
				"(0: no cache; default: %u)\n"
"    -o sfsioretries=N           define number of retries before I/O error is "
				"returned (default: %u)\n"
"    -o sfsmaster=HOST           define sfsmaster location (default: sfsmaster)\n"
"    -o sfsport=PORT             define sfsmaster port number (default: 9421)\n"
"    -o sfsbind=IP               define source ip address for connections "
				"(default: NOT DEFINED - chosen automatically "
				"by OS)\n"
"    -o sfsiolimits=FILE         define I/O limits configuration file\n"
"    -o symlinkcachetimeout=N    define timeout of symlink cache in seconds "
				"(default: %u)\n"
"    -o enablefilelocks=0|1      enables/disables global file locking "
				"(disabled by default)\n"
"    -o nonempty                 allow mounts over non-empty file/dir\n"
"    -o sfsdebug                 print some debugging information\n"
"    -o sfssubfolder=PATH        define subfolder to mount as root (default: %s)\n"
"    -o limitglibcmallocarenas=N  limit glibc malloc arenas to given value - prevents "
				"from using huge amount of virtual memory. Use it in constrained memory "
				"environments (default: %u)\n"
"    -o sfslognotificationarea=0|1  enable/disable logging to Linux notification area (default: %d)\n"
"    -o sfsmessagesuppressionperiod=N  set period of message suppression in seconds for logging on "
				"notification area (default: %u)\n"
"    -o statfscachetimeout=MSEC  set statfs cache timeout in milliseconds. When equal to 0 "
				"the cache is disabled (default: %u)\n"
"    -o usequotainvolumesize=0|1  when set to 1, use the user and group specific quota hard "
				"limit to calculate the volume size (default: %u)\n"
"\n",
		SaunaClient::FsInitParams::kDefaultCacheExpirationTime,
		SaunaClient::FsInitParams::kDefaultReadaheadMaxWindowSize,
		SaunaClient::FsInitParams::kDefaultReadWorkers,
		SaunaClient::FsInitParams::kDefaultMaxReadaheadRequests,
		SaunaClient::FsInitParams::kDefaultChunkserverReadTo,
		SaunaClient::FsInitParams::kDefaultChunkserverTotalReadTo,
		SaunaClient::FsInitParams::kDefaultBandwidthOveruse,
		SaunaClient::FsInitParams::kDefaultReadCacheMaxSizePercentage,
		SaunaClient::FsInitParams::kDefaultChunkserverWriteTo,
		SaunaClient::FsInitParams::kDefaultWriteCacheSize,
		SaunaClient::FsInitParams::kDefaultCachePerInodePercentage,
		SaunaClient::FsInitParams::kDefaultWriteWorkers,
		SaunaClient::FsInitParams::kDefaultWriteWindowSize,
		SaunaClient::FsInitParams::kDefaultIgnoreFlush,
		SaunaClient::FsInitParams::kDefaultUseRwLock,
		SaunaClient::FsInitParams::kDefaultMkdirCopySgid,
		sugidClearModeString(SaunaClient::FsInitParams::kDefaultSugidClearMode),
		SaunaClient::FsInitParams::kDefaultAttrCacheTimeout,
		SaunaClient::FsInitParams::kDefaultEntryCacheTimeout,
		SaunaClient::FsInitParams::kDefaultDirentryCacheTimeout,
		SaunaClient::FsInitParams::kDefaultDirentryCacheSize,
		SaunaClient::FsInitParams::kDefaultAclCacheTimeout,
		SaunaClient::FsInitParams::kDefaultReportReservedPeriod,
		SaunaClient::FsInitParams::kDefaultRoundTime,
		SaunaClient::FsInitParams::kDefaultChunkserverWaveReadTo,
		SaunaClient::FsInitParams::kDefaultAclCacheSize,
		SaunaClient::FsInitParams::kDefaultIoRetries,
		SaunaClient::FsInitParams::kDefaultSymlinkCacheTimeout,
		SaunaClient::FsInitParams::kDefaultSubfolder,
		SaunaClient::FsInitParams::kDefaultLimitGlibcMallocArenas,
		SaunaClient::FsInitParams::kDefaultLogNotificationArea,
		SaunaClient::FsInitParams::kDefaultMessageSuppressionPeriod,
		SaunaClient::FsInitParams::kDefaultStatfsCacheTo,
		SaunaClient::FsInitParams::kDefaultUseQuotaInVolumeSize
	);
	printf(
"CMODE can be set to:\n"
"    NO,NONE or NEVER            never allow files data to be kept in cache "
				"(safest but can reduce efficiency)\n"
"    YES or ALWAYS               always allow files data to be kept in cache "
				"(dangerous)\n"
"    AUTO                        file cache is managed by sfsmaster "
				"automatically (should be very safe and "
				"efficient)\n"
"\n");
	printf(
"SMODE can be set to:\n"
"    NEVER                       SaunaFS will not change suid and sgid bit "
				"on chown\n"
"    ALWAYS                      clear suid and sgid on every chown - safest "
				"operation\n"
"    OSX                         standard behavior in OS X and Solaris (chown "
				"made by unprivileged user clear suid and sgid)\n"
"    BSD                         standard behavior in *BSD systems (like in "
				"OSX, but only when something is really changed)\n"
"    EXT                         standard behavior in most filesystems on "
				"Linux (directories not changed, others: suid "
				"cleared always, sgid only when group exec "
				"bit is set)\n"
"    SFS                         standard behavior in SFS on Linux (like EXT "
				"but directories are changed by unprivileged "
				"users)\n\n"
"SMODE extra info:\n"
"    btrfs,ext2,ext3,ext4,hfs[+],jfs,ntfs and reiserfs on Linux work as 'EXT'.\n"
"    Only sfs on Linux works a little different. Beware that there is a strange\n"
"    operation - chown(-1,-1) which is usually converted by a kernel into something\n"
"    like 'chmod ug-s', and therefore can't be controlled by SFS as 'chown'\n"
"\n");

	printf("\nFUSE options:\n");
	fuse_lowlevel_help();
	printf(
"    -o rw                  mount the filesystem read-write\n"
"    -o ro                  mount the filesystem read-only.\n"
"    -o suid                allow set-user-identifier or set-group-identifier "
				"bits to take effect\n"
"    -o nosuid              do not allow set-user-identifier or set-group-identifier "
				"bits to take effect\n"
"    -o dev                 interpret character or block special devices on the "
				"filesystem\n"
"    -o nodev               do not interpret character or block special devices on "
				"the filesystem\n"
"    -o exec                permit execution of binaries\n"
"    -o noexec              do not permit direct execution of any binaries on the "
				"mounted filesystem\n"
"    -o max_write=N         set the maximum number of bytes in a single write "
				"operation. The default is 128kB. Note, that due to various "
				"limitations, the size of write requests can be much smaller (4kB)\n"
"    -o max_readahead=N     set the maximum number of bytes to read-ahead. "
				"The default is determined by the kernel. On linux-2.6.22 or earlier "
				"it's 131072 (128kB)\n"
"    -o max_background=N    the maximum number of outstanding background requests that "
				"the FUSE kernel driver is allowed to submit\n"
"    -o congestion_threshold=N  kernel congestion threshold parameter. If the number of "
				"pending background requests exceeds this number, the FUSE kernel module "
				"will mark the filesystem as \"congested\". This instructs the kernel to "
				"expect that queued requests will take some time to complete, and to adjust "
				"its algorithms accordingly (e.g. by putting a waiting thread to sleep "
				"instead of using a busy-loop)\n"
"    -o sync_read           perform all reads (even read-ahead) synchronously\n"
"    -o async_read          perform reads asynchronously. This is the default\n"
"    -o atomic_o_trunc      enable atomic open+truncate support\n"
"    -o no_remote_lock      disable remote file locking\n"
"    -o no_remote_flock     disable remote file locking (BSD)\n"
"    -o no_remote_posix_lock  disable remove file locking (POSIX)\n"
"    -o [no_]splice_write   use splice to write to the fuse device\n"
"    -o [no_]splice_move    move data while splicing to the fuse device\n"
"    -o [no_]splice_read    use splice to read from the fuse device\n"
"    -o [no_]auto_inval_data  use automatic kernel cache invalidation logic\n"
"    -o readdirplus=(yes,no,auto)  extended read from directory\n"
"    -o [no_]async_dio      set asynchronous direct I/O\n"
"    -o [no_]writeback_cache  set asynchronous, buffered writes\n"
"    -o time_gran=N         sets the time granularity (in nanoseconds) for "
				"file timestamps\n"
	);
}

void sfs_opt_parse_cfg_file(const char *filename,int optional,struct fuse_args *outargs) {
	FILE *fd;
	constexpr size_t N = 1000;
	char lbuff[N],*p;
	gCfgString.reserve(N);

	fd = fopen(filename, "r");
	if (!fd) {
		if (!optional) {
			fprintf(stderr,"can't open cfg file: %s\n",filename);
			abort();
		}
		return;
	}
	gCustomCfg = 1;
	while (fgets(lbuff, N - 1, fd)) {
		// Skip a comment line
		if (lbuff[0] == '#' || lbuff[0] == ';')
			continue;

		lbuff[N - 1] = 0;
		gCfgString += lbuff;

		// Go to the end of the cfg file line
		for (p = lbuff; *p; p++) {
			if (*p == '\r' || *p == '\n') {
				*p = 0;
				break;
			}
		}

		p--;

		// Skip last blank spaces in the line
		while (p >= lbuff && (*p == ' ' || *p == '\t')) {
			*p = 0;
			p--;
		}

		p = lbuff;

		// Skip first blank spaces in the line
		while (*p == ' ' || *p == '\t') {
			p++;
		}

		if (*p) {
			if (*p == '-') {
				// "-o opt1=val1,opt2=val2,..." or "-D S"-like formats
				if (p[1] != ' ' && p[1] != 0 && p[2] == ' ') {
					fuse_opt_add_arg(outargs, std::string(p, p + 2).c_str());
					fuse_opt_add_arg(outargs, p + 3);
				} else { // "-oopt1=val1,opt2=val2,..." format
					fuse_opt_add_arg(outargs, p);
				}
			} else if (*p == '/') { // default mountpoint
				if (gDefaultMountpoint)
					free(gDefaultMountpoint);
				gDefaultMountpoint = strdup(p);
			} else { // "opt1=val1,opt2=val2,..." format
				fuse_opt_add_arg(outargs,"-o");
				fuse_opt_add_arg(outargs,p);
			}
		}
	}
	fclose(fd);

	// Replace the equal signs with colons for YAML
	gCfgString = std::regex_replace(gCfgString, std::regex("="), ": ");
}

// Function for FUSE: has to have these arguments
int sfs_opt_proc_stage1(struct fuse_args *defargs, const char *arg, int key) {
	const char *sfscfgfile_opt = "sfscfgfile=";
	const int n = strlen(sfscfgfile_opt);

	if (key == KEY_CFGFILE) {
		if (!strncmp(arg, sfscfgfile_opt, n))
			sfs_opt_parse_cfg_file(arg + n, 0, defargs);
		else if (!strncmp(arg, "-c", 2))
			sfs_opt_parse_cfg_file(arg + 2, 0, defargs);

		return 0;
	}
	return 1;
}

int sfs_opt_proc_stage1(void *data, const char *arg, int key, struct fuse_args *outargs) {
	(void)outargs; // remove unused argument warning
	return sfs_opt_proc_stage1((struct fuse_args*)data, arg, key);
}

// Function for FUSE: has to have these arguments
// return value:
//   0 - discard this arg
//   1 - keep this arg for future processing
int sfs_opt_proc_stage2(void *data, const char *arg, int key, struct fuse_args *outargs) {
	(void)data; // remove unused argument warning
	(void)outargs;

	switch (key) {
	case FUSE_OPT_KEY_OPT:
		return 1;
	case FUSE_OPT_KEY_NONOPT:
		return 1;
	case KEY_HOST:
		if (gMountOptions.masterhost)
			free(gMountOptions.masterhost);
		gMountOptions.masterhost = strdup(arg + 2);
		return 0;
	case KEY_PORT:
		if (gMountOptions.masterport)
			free(gMountOptions.masterport);
		gMountOptions.masterport = strdup(arg + 2);
		return 0;
	case KEY_BIND:
		if (gMountOptions.bindhost)
			free(gMountOptions.bindhost);
		gMountOptions.bindhost = strdup(arg + 2);
		return 0;
	case KEY_PATH:
		if (gMountOptions.subfolder)
			free(gMountOptions.subfolder);
		gMountOptions.subfolder = strdup(arg + 2);
		return 0;
	case KEY_PASSWORDASK:
		gMountOptions.passwordask = 1;
		return 0;
	case KEY_META:
		gMountOptions.meta = 1;
		return 0;
	case KEY_NOSTDMOUNTOPTIONS:
		gMountOptions.nostdmountoptions = 1;
		return 0;
	case KEY_NONEMPTY:
		gMountOptions.nonemptymount = 1;
		return 0;
	default:
		fprintf(stderr, "internal error\n");
		abort();
	}
}
