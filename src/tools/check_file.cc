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
#include "tools/tools_commands.h"
#include "tools/tools_common_functions.h"

static void check_file_usage() {
	fprintf(stderr, "check files\n\nusage:\n leil checkfile [-nhH] name [name ...]\n");
}

static int check_file(const char *fname) {
	uint32_t msgid{0};
	inode_t inode;
	uint8_t status;

	MessageBuffer request, response;

	int fd;
	fd = open_master_conn(fname, &inode, nullptr, false);
	if (fd < 0) {
		return -1;
	}

	try {
		serializeLegacyPacket(request, CLTOMA_FUSE_CHECK, msgid, inode);
		response = ServerConnection::sendAndReceive(
		    fd, request, MATOCL_FUSE_CHECK,
		    ServerConnection::ReceiveMode::kReceiveFirstNonNopMessage, kDefaultTimeoutMs);

		close_master_conn(0);

		// Parse response buffer
		const uint8_t *rptr = response.data();
		get32bit(&rptr, msgid);

		if (msgid != 0) {
			printf("%s: master query: wrong answer (msgid)\n", fname);
			return -1;
		}

		uint32_t remaining = static_cast<uint32_t>(response.size() - sizeof(msgid));

		if (remaining == sizeof(status)) {
			status = *rptr;
			printf("%s: %s\n", fname, saunafs_error_string(status));
			return -1;
		}

		constexpr uint32_t kExpectedSize = CHUNK_MATRIX_SIZE * sizeof(uint32_t);

		if (remaining % 3 != 0 && remaining != kExpectedSize) {
			printf("%s: master query: wrong answer (leng)\n", fname);
			return -1;
		}

		printf("%s:\n", fname);

		// Legacy format: N * [copies:8, chunks:16]
		if (remaining % 3 == 0) {
			for (uint32_t offset = 0; offset < remaining; offset += 3) {
				uint8_t copies = get8bit(&rptr);
				uint16_t chunkCount16 = get16bit(&rptr);
				uint32_t chunkCount = chunkCount16;
				if (copies == 1) {
					printf("1 copy:");
				} else {
					printf("%" PRIu8 " copies:", copies);
				}
				print_number(" ", "\n", chunkCount, 1, 0, 1);
			}
		} else {
			// Modern format: CHUNK_MATRIX_SIZE * [chunks:32]
			for (uint32_t copyIndex = 0; copyIndex < CHUNK_MATRIX_SIZE; ++copyIndex) {
				uint32_t chunkCount = 0;
				get32bit(&rptr, chunkCount);
				if (chunkCount > 0) {
					if (copyIndex == 1) {
						printf(" chunks with 1 copy:    ");
					} else if (copyIndex >= 10) {
						printf(" chunks with 10+ copies:");
					} else {
						printf(" chunks with %u copies:  ", copyIndex);
					}
					print_number(" ", "\n", chunkCount, 1, 0, 1);
				}
			}
		}
	} catch (const Exception &e) {
		fprintf(stderr, "%s\n", e.what());
		close_master_conn(1);
		return -1;
	}

	return 0;
}

int check_file_run(int argc, char **argv) {
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
		case '?':
			check_file_usage();
			return 1;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		check_file_usage();
		return 1;
	}

	status = 0;
	while (argc > 0) {
		if (check_file(*argv) < 0) {
			status = 1;
		}
		argc--;
		argv++;
	}
	return status;
}
