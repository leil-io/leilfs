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
#include "common/server_connection.h"
#include "errors/saunafs_error_codes.h"
#include "errors/sfserr.h"
#include "tools/tools_commands.h"
#include "tools/tools_common_functions.h"

static void append_file_usage() {
	fprintf(
	    stderr,
	    "append file chunks to another file. If destination file doesn't exist then it's created"
	    " as empty file and then chunks are appended\n\nusage:\n leil appendchunks dstfile name [name "
	    "...]\n");
}

static int append_file(const char *fname, const char *afname) {
	uint32_t uid, gid;
	uint32_t msgid{0};
	inode_t inode, ainode;
	uint8_t status;

	MessageBuffer request, response;
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

	try {
		serializeLegacyPacket(request, CLTOMA_FUSE_APPEND, msgid, inode, ainode, uid, gid);
		response = ServerConnection::sendAndReceive(
		    fd, request, MATOCL_FUSE_APPEND,
		    ServerConnection::ReceiveMode::kReceiveFirstNonNopMessage, kDefaultTimeoutMs);
		deserializeAllLegacyPacketDataNoHeader(response, msgid, status);

		close_master_conn(0);

		if (msgid != 0) {
			printf("%s: master query: wrong answer (msgid)\n", fname);
			return -1;
		}

		if (status != SAUNAFS_STATUS_OK) {
			printf("%s: %s\n", fname, saunafs_error_string(status));
			return -1;
		}
	} catch (const Exception &e) {
		fprintf(stderr, "%s\n", e.what());
		close_master_conn(1);
		return -1;
	}

	return 0;
}

int append_file_run(int argc, char **argv) {
	char *appendfname = nullptr;
	int i, status;

	while (int ch = getopt(argc, argv, "") != -1) {
		if (ch == '?') {
			append_file_usage();
			return 1;
		}
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
