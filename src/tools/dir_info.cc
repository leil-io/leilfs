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
#include "common/type_defs.h"
#include "errors/saunafs_error_codes.h"
#include "errors/sfserr.h"
#include "tools/tools_commands.h"
#include "tools/tools_common_functions.h"

static void dir_info_usage() {
	fprintf(stderr, "show directories stats\n\nusage:\n saunafs dirinfo [-nhH] name [name ...]\n");
	print_numberformat_options();
	fprintf(stderr,
	        "\nMeaning of some not obvious output data:\n 'length' is just sum of files lengths\n"
	        " 'size' is sum of chunks lengths\n 'realsize' is estimated hdd usage (usually size "
	        "multiplied by current goal)\n");
}

static int dir_info(const char *fname) {
	uint32_t cmd, leng;
	uint32_t msgid = 0;
	inode_t inode, inodes, dirs, files, links;
	uint32_t chunks;
	uint64_t length, size, realsize;

	constexpr uint8_t kDirStatsPayload =
	    sizeof(cmd) + sizeof(leng) + sizeof(msgid) + sizeof(inodes) + sizeof(dirs) + sizeof(files) +
	    sizeof(links) + sizeof(chunks) + sizeof(length) + sizeof(size) + sizeof(realsize);
	constexpr uint8_t kDirStatsLegacyPayload =
	    sizeof(msgid) + sizeof(inodes) + sizeof(dirs) + sizeof(files) + sizeof(links) +
	    (4 * sizeof(uint32_t)) + sizeof(chunks) + sizeof(length) + sizeof(size) + sizeof(realsize);

	int fd;
	fd = open_master_conn(fname, &inode, nullptr, false);
	if (fd < 0) {
		return -1;
	}

	constexpr uint32_t kDirInfoPayload = sizeof(msgid) + sizeof(inode);
	constexpr uint32_t kReqBuffSize = sizeof(cmd) + sizeof(kDirInfoPayload) + kDirInfoPayload;

	uint8_t reqbuff[kReqBuffSize], *wptr, *buff;
	const uint8_t *rptr;

	wptr = reqbuff;
	put32bit(&wptr, CLTOMA_FUSE_GETDIRSTATS);
	put32bit(&wptr, kDirInfoPayload);
	put32bit(&wptr, msgid);
	putINode(&wptr, inode);

	// send the request
	if (tcpwrite(fd, reqbuff, kReqBuffSize) != kReqBuffSize) {
		printf("%s: master query: send error\n", fname);
		close_master_conn(1);
		return -1;
	}

	// read the first part of the answer (cmd and length)
	if (tcpread(fd, reqbuff, sizeof(cmd) + sizeof(leng)) != sizeof(cmd) + sizeof(leng)) {
		printf("%s: master query: receive error\n", fname);
		close_master_conn(1);
		return -1;
	}

	rptr = reqbuff;
	get32bit(&rptr, cmd);
	get32bit(&rptr, leng);

	if (cmd != MATOCL_FUSE_GETDIRSTATS) {
		printf("%s: master query: wrong answer (type)\n", fname);
		close_master_conn(1);
		return -1;
	}

	buff = (uint8_t *)malloc(leng);

	// read the rest of the answer into the buffer
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

	if (leng == sizeof(msgid) + sizeof(uint8_t)) {  // status code
		printf("%s: %s\n", fname, saunafs_error_string(*rptr));
		free(buff);
		close_master_conn(1);
		return -1;
	}

	if (leng != kDirStatsLegacyPayload && leng != kDirStatsPayload) {
		printf("%s: master query: wrong answer (leng) %u/(%u|%u)\n", fname, leng, kDirStatsPayload,
		       kDirStatsLegacyPayload);
		free(buff);
		close_master_conn(1);
		return -1;
	}

	close_master_conn(0);

	getINode(&rptr, inodes);
	getINode(&rptr, dirs);
	getINode(&rptr, files);
	getINode(&rptr, links);
	if (leng == kDirStatsLegacyPayload) {
		rptr += 2 * sizeof(uint32_t);  // skip empty data (8 bytes) from legacy format
	}
	get32bit(&rptr, chunks);
	if (leng == kDirStatsLegacyPayload) {
		rptr += 2 * sizeof(uint32_t);  // skip empty data (8 bytes) from legacy format
	}
	length = get64bit(&rptr);
	size = get64bit(&rptr);
	realsize = get64bit(&rptr);

	free(buff);

	printf("%s:\n", fname);
	print_number(" inodes:       ", "\n", inodes, 0, 0, 1);
	print_number("  directories: ", "\n", dirs, 0, 0, 1);
	print_number("  files:       ", "\n", files, 0, 0, 1);
	print_number("  links:       ", "\n", links, 0, 0, 1);
	print_number(" chunks:       ", "\n", chunks, 0, 0, 1);
	print_number(" length:       ", "\n", length, 0, 1, 1);
	print_number(" size:         ", "\n", size, 0, 1, 1);
	print_number(" realsize:     ", "\n", realsize, 0, 1, 1);

	return 0;
}

int dir_info_run(int argc, char **argv) {
	int ch, status;

	while ((ch = getopt(argc, argv, "nhH")) != -1) {
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
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		dir_info_usage();
		return 1;
	}

	status = 0;
	while (argc > 0) {
		if (dir_info(*argv) < 0) {
			status = 1;
		}
		argc--;
		argv++;
	}
	return status;
}
