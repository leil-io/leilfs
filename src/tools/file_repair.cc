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
#include "common/server_connection.h"
#include "errors/saunafs_error_codes.h"
#include "errors/sfserr.h"
#include "tools/tools_commands.h"
#include "tools/tools_common_functions.h"

static void file_repair_usage() {
	fprintf(
	    stderr,
	    "repair given file. Use it with caution. It forces file to be readable, so it could erase "
	    "(fill with zeros) file when chunkservers are not currently connected.\n\n"
	    "usage:\n leil filerepair [-nhHc] name [name ...]\n");
	print_numberformat_options();
	fprintf(stderr, " -c - restore to previous version if applicable, never erase\n");
}

static int file_repair(const char *fname, uint8_t correct_only_flag) {
	uint32_t msgid{0};
	inode_t inode;
	uint32_t notchanged, erased, repaired;

	int fd;
	fd = open_master_conn(fname, &inode, nullptr, true);
	if (fd < 0) {
		return -1;
	}

	uint32_t uid = getUId();
	uint32_t gid = getGId();

	try {
		MessageBuffer request, response;
		serializeLegacyPacket(request, CLTOMA_FUSE_REPAIR, msgid, inode, uid, gid,
		                      correct_only_flag);

		response = ServerConnection::sendAndReceive(
		    fd, request, MATOCL_FUSE_REPAIR,
		    ServerConnection::ReceiveMode::kReceiveFirstNonNopMessage, kDefaultTimeoutMs);

		const uint8_t *rptr = response.data();
		get32bit(&rptr, msgid);

		if (msgid != 0) {
			printf("%s: master query: wrong answer (queryid)\n", fname);
			close_master_conn(1);
			return -1;
		}

		uint32_t remaining = static_cast<uint32_t>(response.size() - sizeof(msgid));

		if (remaining == sizeof(uint8_t)) {
			uint8_t status = *rptr;
			printf("%s: %s\n", fname, saunafs_error_string(status));
			close_master_conn(0);
			return (status == SAUNAFS_STATUS_OK) ? 0 : -1;
		}

		constexpr uint32_t kFileRepairPayload =
		    sizeof(notchanged) + sizeof(erased) + sizeof(repaired);
		if (remaining != kFileRepairPayload) {
			printf("%s: master query: wrong answer (leng)\n", fname);
			close_master_conn(1);
			return -1;
		}

		get32bit(&rptr, notchanged);
		get32bit(&rptr, erased);
		get32bit(&rptr, repaired);

		close_master_conn(0);

		printf("%s:\n", fname);
		print_number(" chunks not changed: ", "\n", notchanged, 1, 0, 1);
		print_number(" chunks erased:      ", "\n", erased, 1, 0, 1);
		print_number(" chunks repaired:    ", "\n", repaired, 1, 0, 1);

		return 0;
	} catch (const Exception &e) {
		fprintf(stderr, "%s\n", e.what());
		close_master_conn(1);
		return -1;
	}
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
		case '?':
			file_repair_usage();
			return 1;
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
