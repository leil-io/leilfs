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

#include <stdio.h>
#include <stdlib.h>
#include <cstdint>

#include "common/datapack.h"
#include "errors/saunafs_error_codes.h"
#include "errors/sfserr.h"
#include "tools/tools_commands.h"
#include "tools/tools_common_functions.h"

static void file_repair_usage() {
	fprintf(
	    stderr,
	    "repair given file. Use it with caution. It forces file to be readable, so it could erase "
	    "(fill with zeros) file when chunkservers are not currently connected.\n\n"
	    "usage:\n saunafs filerepair [-nhHc] name [name ...]\n");
	print_numberformat_options();
	fprintf(stderr, " -c - restore to previous version if applicable, never erase\n");
}

static int file_repair(const char *fname, uint8_t correct_only_flag) {
	uint32_t cmd, leng;
	inode_t inode;
	uint32_t msgid{0};
	uint32_t notchanged, erased, repaired;

	int fd;
	fd = open_master_conn(fname, &inode, nullptr, true);
	if (fd < 0) {
		return -1;
	}

	uint32_t uid = getUId();
	uint32_t gid = getGId();

	constexpr uint32_t kRepairPayload =
	    sizeof(msgid) + sizeof(inode) + sizeof(uid) + sizeof(gid) + sizeof(correct_only_flag);
	constexpr uint32_t kReqBuffSize = sizeof(cmd) + sizeof(kRepairPayload) + kRepairPayload;
	uint8_t reqbuff[kReqBuffSize], *wptr, *buff;
	const uint8_t *rptr;

	wptr = reqbuff;
	put32bit(&wptr, CLTOMA_FUSE_REPAIR);
	put32bit(&wptr, kRepairPayload);
	put32bit(&wptr, msgid);
	putINode(&wptr, inode);
	put32bit(&wptr, uid);
	put32bit(&wptr, gid);
	put8bit(&wptr, correct_only_flag);

	// send request to master
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

	if (cmd != MATOCL_FUSE_REPAIR) {
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

	rptr = buff;
	get32bit(&rptr, msgid);  // queryid

	if (msgid != 0) {
		printf("%s: master query: wrong answer (queryid)\n", fname);
		free(buff);
		close_master_conn(1);
		return -1;
	}

	if (leng - sizeof(msgid) == 1) {  // an error code was returned
		printf("%s: %s\n", fname, saunafs_error_string(*rptr));
		free(buff);
		close_master_conn(1);
		return -1;
	}

	if (leng != sizeof(msgid) + 3 * sizeof(uint32_t)) {
		printf("%s: master query: wrong answer (leng)\n", fname);
		free(buff);
		close_master_conn(1);
		return -1;
	}

	close_master_conn(0);  // not needed anymore

	get32bit(&rptr, notchanged);
	get32bit(&rptr, erased);
	get32bit(&rptr, repaired);

	free(buff);

	printf("%s:\n", fname);
	print_number(" chunks not changed: ", "\n", notchanged, 1, 0, 1);
	print_number(" chunks erased:      ", "\n", erased, 1, 0, 1);
	print_number(" chunks repaired:    ", "\n", repaired, 1, 0, 1);

	return 0;
}

int file_repair_run(int argc, char **argv) {
	int ch, status;
	uint8_t correct_only = 0;
	while ((ch = getopt(argc, argv, "nhHc")) != -1) {
		switch (ch) {
		case 'n':
			humode = 0;
			break;
		case 'h':
			humode = 1;
			break;
		case 'H':
			humode = 2;
			break;
		case 'c':
			correct_only = 1;
			break;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		file_repair_usage();
		return 1;
	}
	status = 0;
	while (argc > 0) {
		if (file_repair(*argv, correct_only) < 0) {
			status = 1;
		}
		argc--;
		argv++;
	}
	return status;
}
