/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2016 Skytechnology sp. z o.o.
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

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <cstdint>

#include "common/datapack.h"
#include "errors/saunafs_error_codes.h"
#include "errors/sfserr.h"
#include "tools/tools_commands.h"
#include "tools/tools_common_functions.h"

static void append_file_usage() {
	fprintf(
	    stderr,
	    "append file chunks to another file. If destination file doesn't exist then it's created"
	    " as empty file and then chunks are appended\n\nusage:\n saunafs appendchunks dstfile name [name "
	    "...]\n");
}

static int append_file(const char *fname, const char *afname) {
	uint32_t cmd, leng, uid, gid;
	uint32_t msgid{0};
	inode_t inode, ainode;

	constexpr uint32_t kAppendFilePayload =
	    sizeof(msgid) + sizeof(inode) + sizeof(ainode) + sizeof(uid) + sizeof(gid);
	constexpr uint32_t kReqBuffSize = sizeof(cmd) + sizeof(kAppendFilePayload) + kAppendFilePayload;
	uint8_t reqbuff[kReqBuffSize], *wptr, *buff;
	const uint8_t *rptr;
	mode_t dmode, smode;

	int fd;
	fd = open_master_conn(fname, &inode, &dmode, true);
	if (fd < 0) {
		return -1;
	}

	if (open_master_conn(afname, &ainode, &smode, true) < 0) {
		return -1;
	}

	if ((smode & S_IFMT) != S_IFREG) {
		printf("%s: not a file\n", afname);
		return -1;
	}
	if ((dmode & S_IFMT) != S_IFREG) {
		printf("%s: not a file\n", fname);
		return -1;
	}

	uid = getUId();
	gid = getGId();
	
	wptr = reqbuff;
	put32bit(&wptr, CLTOMA_FUSE_APPEND);
	put32bit(&wptr, kAppendFilePayload);
	put32bit(&wptr, msgid);
	putINode(&wptr, inode);
	putINode(&wptr, ainode);
	put32bit(&wptr, uid);
	put32bit(&wptr, gid);

	// send the request
	if (tcpwrite(fd, reqbuff, kReqBuffSize) != kReqBuffSize) {
		printf("%s: master query: send error\n", fname);
		close_master_conn(1);
		return -1;
	}

	// read the first part of the answer
	if (tcpread(fd, reqbuff, sizeof(cmd) + sizeof(leng)) != sizeof(cmd) + sizeof(leng)) {
		printf("%s: master query: receive error\n", fname);
		close_master_conn(1);
		return -1;
	}

	rptr = reqbuff;
	get32bit(&rptr, cmd);
	get32bit(&rptr, leng);

	if (cmd != MATOCL_FUSE_APPEND) {
		printf("%s: master query: wrong answer (type)\n", fname);
		close_master_conn(1);
		return -1;
	}

	buff = (uint8_t *)malloc(leng);

	if (tcpread(fd, buff, leng) != (int32_t)leng) {
		printf("%s: master query: receive error\n", fname);
		free(buff);
		close_master_conn(1);
		return -1;
	}

	close_master_conn(0);  // not needed anymore

	// check the msgid
	rptr = buff;
	get32bit(&rptr, msgid);  // queryid
	if (msgid != 0) {
		printf("%s: master query: wrong answer (queryid)\n", fname);
		free(buff);
		return -1;
	}

	if (leng - sizeof(msgid) != 1) {
		printf("%s: master query: wrong answer (leng)\n", fname);
		free(buff);
		return -1;
	}

	if (*rptr != SAUNAFS_STATUS_OK) {
		printf("%s: %s\n", fname, saunafs_error_string(*rptr));
		free(buff);
		return -1;
	}

	free(buff);

	return 0;
}

int append_file_run(int argc, char **argv) {
	char *appendfname = nullptr;
	int i, status;

	while (getopt(argc, argv, "") != -1) {
	}
	argc -= optind;
	argv += optind;

	if (argc <= 1) {
		append_file_usage();
		return 1;
	}
	appendfname = argv[0];
	i = open(appendfname, O_RDWR | O_CREAT, 0666);
	if (i < 0) {
		fprintf(stderr, "can't create/open file: %s\n", appendfname);
		return 1;
	}
	close(i);
	argc--;
	argv++;

	if (argc < 1) {
		append_file_usage();
		return 1;
	}

	status = 0;
	while (argc > 0) {
		if (append_file(appendfname, *argv) < 0) {
			status = 1;
		}
		argc--;
		argv++;
	}
	return status;
}
