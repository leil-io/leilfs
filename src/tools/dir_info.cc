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
#include "common/type_defs.h"
#include "errors/saunafs_error_codes.h"
#include "errors/sfserr.h"
#include "tools/tools_commands.h"
#include "tools/tools_common_functions.h"

static void dir_info_usage() {
	fprintf(stderr, "show directories stats\n\nusage:\n leil dirinfo [-nhH] name [name ...]\n");
	print_numberformat_options();
	fprintf(stderr,
	        "\nMeaning of some not obvious output data:\n 'length' is just sum of files lengths\n"
	        " 'size' is sum of chunks lengths\n 'realsize' is estimated hdd usage (usually size "
	        "multiplied by current goal)\n");
}

static int dir_info(const char *fname) {
	uint32_t msgid = 0;
	inode_t inode;

	int fd = open_master_conn(fname, &inode, nullptr, false);
	if (fd < 0) { return -1; }

	try {
		MessageBuffer request, response;
		serializeLegacyPacket(request, CLTOMA_FUSE_GETDIRSTATS, msgid, inode);
		response = ServerConnection::sendAndReceive(
		    fd, request, MATOCL_FUSE_GETDIRSTATS,
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

		inode_t inodes, dirs, files, links;
		uint32_t chunks = 0;
		uint64_t length = 0, size = 0, realsize = 0;

		// Expected body sizes (after msgid)
		const uint32_t expectedCurrentBody = sizeof(inodes) + sizeof(dirs) + sizeof(files) +
		                                     sizeof(links) + sizeof(chunks) + sizeof(length) +
		                                     sizeof(size) + sizeof(realsize);

		const uint32_t expectedLegacyBody =
		    sizeof(inodes) + sizeof(dirs) + sizeof(files) + sizeof(links) +
		    (4 * sizeof(uint32_t)) +  // legacy fillers
		    sizeof(chunks) + sizeof(length) + sizeof(size) + sizeof(realsize);

		if (remaining != expectedCurrentBody && remaining != expectedLegacyBody) {
			printf("%s: master query: wrong answer (leng) %u/(%u|%u)\n", fname, remaining,
			       expectedCurrentBody, expectedLegacyBody);
			close_master_conn(1);
			return -1;
		}

		getINode(&rptr, inodes);
		getINode(&rptr, dirs);
		getINode(&rptr, files);
		getINode(&rptr, links);

		const bool isLegacy = (remaining == expectedLegacyBody);
		if (isLegacy) {
			rptr += 2 * sizeof(uint32_t);  // skip empty data (8 bytes) from legacy format
		}

		get32bit(&rptr, chunks);

		if (isLegacy) {
			rptr += 2 * sizeof(uint32_t);  // skip empty data (8 bytes) from legacy format
		}

		length = get64bit(&rptr);
		size = get64bit(&rptr);
		realsize = get64bit(&rptr);

		close_master_conn(0);

		printf("%s:\n", fname);
		print_number(" inodes:       ", "\n", inodes, 0, 0, 1);
		print_number("  directories: ", "\n", dirs, 0, 0, 1);
		print_number("  files:       ", "\n", files, 0, 0, 1);
		print_number("  links:       ", "\n", links, 0, 0, 1);
		print_number(" chunks:       ", "\n", chunks, 0, 0, 1);
		print_number(" length:       ", "\n", length, 0, 1, 1);
		print_number(" size:         ", "\n", size, 0, 1, 1);
		print_number(" realsize:     ", "\n", realsize, 0, 1, 1);

	} catch (const Exception &e) {
		fprintf(stderr, "%s\n", e.what());
		close_master_conn(1);
		return -1;
	}

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
		case '?':
			dir_info_usage();
			return 1;
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
