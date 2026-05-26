/*
   Copyright 2005-2017 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2017 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


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

#include <errno.h>
#include <fmt/base.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "common/lambda_guard.h"
#include "common/server_connection.h"
#include "common/signal_handling.h"
#include "common/stat_defs.h"
#include "protocol/cltoma.h"
#include "protocol/matocl.h"
#include "tools/tools_commands.h"
#include "tools/tools_common_functions.h"

// This import should be placed after other includes to avoid Windows dependency issues
// with winsock2.h and windows.h included in tools/tools_common_functions.h
#include "common/args_stat_encoding.h"

static int kInfiniteTimeout = 10 * 24 * 3600 * 1000; // simulate infinite timeout (10 days)
#ifdef _WIN32
static saunafs_stat_t kDefaultEmptyStat = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
#endif

static void snapshot_usage() {
	fprintf(stderr,
	        "make snapshot (lazy copy)\n\nusage:\n leil makesnapshot [-of] src [src ...] dst\n");
	fprintf(stderr, " -o,-f - allow to overwrite existing objects\n");
}

static int make_snapshot(const char *dstdir, const char *dstbase, const char *srcname,
                         inode_t srcinode, uint8_t canoverwrite, uint8_t ignore_missing_src,
                         int initial_batch_size) {
	inode_t dstinode;
	uint32_t nleng, uid, gid;
	uint8_t status;
	uint32_t msgid = 0, job_id;
	int fd;

	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGHUP);
	sigaddset(&set, SIGUSR1);
	sigprocmask(SIG_BLOCK, &set, NULL);

	nleng = strlen(dstbase);
	if (nleng > 255) {
		printf("%s: name too long\n", dstbase);
		return -1;
	}

	fd = open_master_conn(dstdir, &dstinode, NULL, true);
	if (fd < 0) {
		return -1;
	}

	uid = getUId();
	gid = getGId();

	printf("Creating snapshot: %s -> %s/%s ...\n", srcname, dstdir, dstbase);
	try {
		auto request = cltoma::requestTaskId::build(msgid);
		auto response = ServerConnection::sendAndReceive(fd, request,
				SAU_MATOCL_REQUEST_TASK_ID,
				ServerConnection::ReceiveMode::kReceiveFirstNonNopMessage,
				kInfiniteTimeout);
		matocl::requestTaskId::deserialize(response, msgid, job_id);

		std::thread signal_thread(std::bind(signalHandler, job_id));

		/* destructor of LambdaGuard will send SIGUSR1 signal in order to
		 * return from signalHandler function and join thread */
		auto join_guard = makeLambdaGuard([&signal_thread]() {
			fmt::println("sending SIGUSR1 to signal_thread");
			kill(getpid(), SIGUSR1);
			signal_thread.join();
		});
		request = cltoma::snapshot::build(msgid, job_id, srcinode, dstinode, dstbase,
		                                  uid, gid, canoverwrite, ignore_missing_src, initial_batch_size);
		response = ServerConnection::sendAndReceive(fd, request, SAU_MATOCL_FUSE_SNAPSHOT,
				ServerConnection::ReceiveMode::kReceiveFirstNonNopMessage,
				kInfiniteTimeout);
		matocl::snapshot::deserialize(response, msgid, status);

		close_master_conn(0);

		if (status == SAUNAFS_STATUS_OK) {
			printf("Snapshot %s -> %s/%s completed\n", srcname, dstdir, dstbase);
			return 0;
		} else {
			printf("Snapshot %s -> %s/%s:\n returned error status %d: %s\n",
			       srcname, dstdir, dstbase, status, saunafs_error_string(status));
			return -1;
		}

	} catch (Exception &e) {
		fprintf(stderr, "%s\n", e.what());
		close_master_conn(1);
		return -1;
	}
	return 0;
}

static int snapshot(const char *dstname, char *const *srcnames, uint32_t srcelements,
					uint8_t canowerwrite, uint8_t ignore_missing_src, int initial_batch_size) {
	char to[PATH_MAX + 1], base[PATH_MAX + 1], dir[PATH_MAX + 1];
	char src[PATH_MAX + 1];
	std::string lookup_dstname = std::string(dstname);
#ifdef _WIN32
	std::vector<std::string> lookup_srcnames;
	for (uint32_t i = 0; i < srcelements; i++) {
		lookup_srcnames.push_back(std::string(srcnames[i]));
	}
	saunafs_stat_t sst = kDefaultEmptyStat;
	saunafs_stat_t dst = kDefaultEmptyStat;
#else
	saunafs_stat_t sst, dst;
#endif
	int status;
	uint32_t i, l;

#ifdef _WIN32
	for (size_t i = 0; i < lookup_srcnames.size(); i++) {
		if (lookup_srcnames[i].back() == '\\' ||
		    lookup_srcnames[i].back() == '/') {
			lookup_srcnames[i].pop_back();
		}
	}
#endif

	if (stat_portable(lookup_dstname.c_str(), &dst) < 0) {  // dst does not exist
		if (errno != ENOENT) {
			printf("%s: stat error: %s\n", dstname, strerr(errno));
			return -1;
		}
		if (srcelements > 1) {
			printf("can snapshot multiple elements only into existing directory\n");
			return -1;
		}
#ifdef _WIN32
		if (stat_portable(lookup_srcnames[0].c_str(), &sst) < 0) {
			printf("%s: stat error: %s\n", srcnames[0], strerr(errno));
			return -1;
		}
#else
		if (lstat(srcnames[0], &sst) < 0) {
			printf("%s: lstat error: %s\n", srcnames[0], strerr(errno));
			return -1;
		}
#endif
		if (bsd_dirname(dstname, dir) < 0) {
			printf("%s: dirname error\n", dstname);
			return -1;
		}
		if (stat_portable(dir, &dst) < 0) {
			printf("%s: stat error: %s\n", dir, strerr(errno));
			return -1;
		}
		if (sst.st_dev != dst.st_dev) {
			printf("(%s,%s): both elements must be on the same device\n", dstname, srcnames[0]);
			return -1;
		}
		if (!get_full_path(dir, to)) {
			printf("%s: get_full_path error\n", dir);
			return -1;
		}
		if (bsd_basename(dstname, base) < 0) {
			printf("%s: basename error\n", dstname);
			return -1;
		}
		if (strlen(dstname) > 0 &&
		    (dstname[strlen(dstname) - 1] == '/' ||
		     dstname[strlen(dstname) - 1] == '\\') &&
		    !S_ISDIR(sst.st_mode)) {
			printf("directory %s does not exist\n", dstname);
			return -1;
		}

#ifdef _WIN32
		inode_t srcinode;
		int fd = open_master_conn(srcnames[0], &srcinode, NULL, true);
		if (fd < 0) { return -1; }
		return make_snapshot(to, base, srcnames[0], srcinode, canowerwrite, ignore_missing_src, initial_batch_size);
#else
		return make_snapshot(to, base, srcnames[0], sst.st_ino, canowerwrite, ignore_missing_src, initial_batch_size);
#endif
	} else {  // dst exists
		if (!get_full_path(dstname, to)) {
			printf("%s: get_full_path error\n", dstname);
			return -1;
		}
		if (!S_ISDIR(dst.st_mode)) {  // dst id not a directory
			if (srcelements > 1) {
				printf("can snapshot multiple elements only into existing directory\n");
				return -1;
			}
#ifdef _WIN32
			if (stat_portable(lookup_srcnames[0].c_str(), &sst) < 0) {
				printf("%s: stat error: %s\n", srcnames[0], strerr(errno));
				return -1;
			}
#else
			if (lstat(srcnames[0], &sst) < 0) {
				printf("%s: lstat error: %s\n", srcnames[0], strerr(errno));
				return -1;
			}
#endif
			if (sst.st_dev != dst.st_dev) {
				printf("(%s,%s): both elements must be on the same device\n", dstname, srcnames[0]);
				return -1;
			}
			memcpy(dir, to, PATH_MAX + 1);
			dirname_inplace(dir);
			if (bsd_basename(to, base) < 0) {
				printf("%s: basename error\n", to);
				return -1;
			}
#ifdef _WIN32
			inode_t srcinode;
			int fd = open_master_conn(srcnames[0], &srcinode, NULL, true);
			if (fd < 0) { return -1; }
			return make_snapshot(dir, base, srcnames[0], srcinode, canowerwrite, ignore_missing_src, initial_batch_size);
#else
			return make_snapshot(dir, base, srcnames[0], sst.st_ino, canowerwrite, ignore_missing_src, initial_batch_size);
#endif
		} else {  // dst is a directory
			status = 0;
			for (i = 0; i < srcelements; i++) {
#ifdef _WIN32
				if (stat_portable(lookup_srcnames[i].c_str(), &sst) < 0) {
					printf("%s: stat error: %lu\n", srcnames[i], GetLastError());
					status = -1;
					continue;
				}
#else
				if (lstat(srcnames[i], &sst) < 0) {
					printf("%s: lstat error: %s\n", srcnames[i], strerr(errno));
					status = -1;
					continue;
				}
#endif
				if (sst.st_dev != dst.st_dev) {
					printf("(%s,%s): both elements must be on the same device\n", dstname,
					       srcnames[i]);
					status = -1;
					continue;
				}
				if (!S_ISDIR(sst.st_mode)) {      // src is not a directory
					if (!S_ISLNK(sst.st_mode)) {  // src is not a symbolic link
						if (!get_full_path(srcnames[i], src)) {
							printf("%s: get_full_path error\n", src);
							status = -1;
							continue;
						}
						if (bsd_basename(src, base) < 0) {
							printf("%s: basename error\n", src);
							status = -1;
							continue;
						}
					} else {  // src is a symbolic link
						if (bsd_basename(srcnames[i], base) < 0) {
							printf("%s: basename error\n", srcnames[i]);
							status = -1;
							continue;
						}
					}
#ifdef _WIN32
					inode_t srcinode;
					int fd =
					    open_master_conn(srcnames[i], &srcinode, NULL, true);
					if (fd < 0) { return -1; }
					if (make_snapshot(to, base, srcnames[i], srcinode, canowerwrite,
					                  ignore_missing_src, initial_batch_size) < 0) {
#else
					if (make_snapshot(to, base, srcnames[i], sst.st_ino, canowerwrite,
					                  ignore_missing_src, initial_batch_size) < 0) {
#endif
						status = -1;
					}
				} else {  // src is a directory
					l = strlen(srcnames[i]);
					if (l > 0 &&
					    (srcnames[i][l - 1] !=
					        '/' && srcnames[i][l - 1] != '\\')) {  // src is a directory and name has trailing slash
						if (!get_full_path(srcnames[i], src)) {
							printf("%s: get_full_path error\n", src);
							status = -1;
							continue;
						}
						if (bsd_basename(src, base) < 0) {
							printf("%s: basename error\n", src);
							status = -1;
							continue;
						}
#ifdef _WIN32
						inode_t srcinode;
						int fd = open_master_conn(srcnames[i], &srcinode, NULL,
						                          true);
						if (fd < 0) { return -1; }
						if (make_snapshot(to, base, srcnames[i], srcinode, canowerwrite,
						                  ignore_missing_src, initial_batch_size) < 0) {
#else
						if (make_snapshot(to, base, srcnames[i], sst.st_ino, canowerwrite,
						                  ignore_missing_src, initial_batch_size) < 0) {
#endif
							status = -1;
						}
					} else {  // src is a directory and name has not trailing slash
						memcpy(dir, to, PATH_MAX + 1);
						dirname_inplace(dir);
						if (bsd_basename(to, base) < 0) {
							printf("%s: basename error\n", to);
							status = -1;
							continue;
						}
#ifdef _WIN32
						inode_t srcinode;
						int fd = open_master_conn(srcnames[i], &srcinode, NULL,
						                          true);
						if (fd < 0) { return -1; }
						if (make_snapshot(dir, base, srcnames[i], srcinode, canowerwrite,
						                  ignore_missing_src, initial_batch_size) < 0) {
#else
						if (make_snapshot(dir, base, srcnames[i], sst.st_ino, canowerwrite,
						                  ignore_missing_src, initial_batch_size) < 0) {
#endif
							status = -1;
						}
					}
				}
			}
			return status;
		}
	}
}

int snapshot_run(int argc, char **argv) {
	int ch;
	int oflag = 0;
	uint8_t ignore_missing_src = 0;
	int initial_batch_size = 0;

	while ((ch = getopt(argc, argv, "fois:")) != -1) {
		switch (ch) {
		case 'f':
		case 'o':
			oflag = 1;
			break;
		case 'i':
			ignore_missing_src = 1;
			break;
		case 's':
			initial_batch_size = std::stoi(optarg);
			break;
		case '?':
			snapshot_usage();
			return 1;
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 2) {
		snapshot_usage();
		return 1;
	}
	return snapshot(argv[argc - 1], argv, argc - 1, oflag, ignore_missing_src, initial_batch_size);
}
