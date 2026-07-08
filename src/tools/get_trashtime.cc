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

#include <algorithm>
#include <cstdint>
#include <stdio.h>
#include <stdlib.h>

#include "common/datapack.h"
#include "common/server_connection.h"
#include "errors/saunafs_error_codes.h"
#include "errors/sfserr.h"
#include "tools/tools_commands.h"
#include "tools/tools_common_functions.h"

static void get_trashtime_usage() {
	fprintf(stderr,
	        "get objects trashtime (how many seconds file should be left in trash)\n\nusage: "
	        "\n leil gettrashtime [-nhHr] name [name ...]\n");
	print_numberformat_options();
	print_recursive_option();
}

static int get_trashtime(const char *fname, uint8_t mode) {
	uint32_t msgid = 0;
	inode_t inode;
	uint32_t fileNodes, directoryNodes, i;
	uint32_t trashtime;
	uint32_t cnt;
	uint8_t status;

	MessageBuffer request, response;

	int fd = open_master_conn(fname, &inode, nullptr, false);
	if (fd < 0) { return -1; }

	try {
		serializeLegacyPacket(request, CLTOMA_FUSE_GETTRASHTIME, msgid, inode, mode);

		response = ServerConnection::sendAndReceive(
		    fd, request, MATOCL_FUSE_GETTRASHTIME,
		    ServerConnection::ReceiveMode::kReceiveFirstNonNopMessage, kDefaultTimeoutMs);

		const uint8_t *rptr = response.data();

		get32bit(&rptr, msgid);
		if (msgid != 0) {
			printf("%s: master query: wrong answer (queryid)\n", fname);
			close_master_conn(1);
			return -1;
		}

		uint32_t bodySize = static_cast<uint32_t>(response.size() - sizeof(msgid));

		if (bodySize == sizeof(status)) {
			status = *rptr;
			printf("%s: %s\n", fname, saunafs_error_string(status));
			close_master_conn(0);
			return (status == SAUNAFS_STATUS_OK) ? 0 : -1;
		}

		const uint32_t headerFieldsSize = sizeof(fileNodes) + sizeof(directoryNodes);

		if (bodySize < headerFieldsSize) {
			printf("%s: master query: wrong answer (leng)\n", fname);
			close_master_conn(1);
			return -1;
		}

		get32bit(&rptr, fileNodes);
		get32bit(&rptr, directoryNodes);

		bodySize -= headerFieldsSize;

		if (mode == GMODE_NORMAL) {
			const uint32_t expectedNormalBody = sizeof(trashtime) + sizeof(cnt);
			if (bodySize != expectedNormalBody) {
				printf("%s: master query: wrong answer (leng)\n", fname);
				close_master_conn(1);
				return -1;
			}

			get32bit(&rptr, trashtime);
			get32bit(&rptr, cnt);

			if ((fileNodes != 0 || directoryNodes != 1) &&
			    (fileNodes != 1 || directoryNodes != 0)) {
				printf("%s: master query: wrong answer (fn,dn)\n", fname);
				close_master_conn(1);
				return -1;
			}

			if (cnt != 1) {
				printf("%s: master query: wrong answer (cnt)\n", fname);
				close_master_conn(1);
				return -1;
			}

			close_master_conn(0);
			printf("%s: %" PRIu32 "\n", fname, trashtime);
			return 0;
		}

		const uint32_t entrySize = sizeof(trashtime) + sizeof(cnt);
		const uint32_t expectedAggregatedBody = (fileNodes + directoryNodes) * entrySize;

		if (bodySize != expectedAggregatedBody) {
			printf("%s: master query: wrong answer (leng)\n", fname);
			close_master_conn(1);
			return -1;
		}

		std::vector<std::pair<uint32_t, uint32_t>> files;
		std::vector<std::pair<uint32_t, uint32_t>> dirs;

		files.reserve(fileNodes);
		dirs.reserve(directoryNodes);

		for (i = 0; i < fileNodes; ++i) {
			get32bit(&rptr, trashtime);
			get32bit(&rptr, cnt);
			files.emplace_back(trashtime, cnt);
		}

		for (i = 0; i < directoryNodes; ++i) {
			get32bit(&rptr, trashtime);
			get32bit(&rptr, cnt);
			dirs.emplace_back(trashtime, cnt);
		}

		close_master_conn(0);

		std::sort(files.begin(), files.end());
		std::sort(dirs.begin(), dirs.end());

		printf("%s:\n", fname);

		for (const auto &entry : files) {
			printf(" files with trashtime        %10" PRIu32 " :", entry.first);
			print_number(" ", "\n", entry.second, 1, 0, 1);
		}

		for (const auto &entry : dirs) {
			printf(" directories with trashtime  %10" PRIu32 " :", entry.first);
			print_number(" ", "\n", entry.second, 1, 0, 1);
		}

	} catch (const Exception &e) {
		fprintf(stderr, "%s\n", e.what());
		close_master_conn(1);
		return -1;
	}

	return 0;
}

static int gene_get_trashtime_run(int argc, char **argv, int rflag) {
	int ch, status;
	while ((ch = getopt(argc, argv, "rnhH")) != -1) {
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
		case 'r':
			rflag = 1;
			break;
		case '?':
			get_trashtime_usage();
			return 1;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		get_trashtime_usage();
		return 1;
	}

	status = 0;
	while (argc > 0) {
		if (get_trashtime(*argv, (rflag) ? GMODE_RECURSIVE : GMODE_NORMAL) < 0) {
			status = 1;
		}
		argc--;
		argv++;
	}
	return status;
}

int get_trashtime_run(int argc, char **argv) {
	return gene_get_trashtime_run(argc, argv, 0);
}
