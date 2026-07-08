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

static void get_eattr_usage() {
	fprintf(stderr, "get objects extra attributes\n\nusage:\n leil geteattr [-nhHr] name [name ...]\n");
	print_numberformat_options();
	print_recursive_option();
}

static int get_eattr(const char *fname, uint8_t mode) {
	uint32_t msgid{0};
	inode_t inode;
	uint8_t eattr;
	uint32_t cnt;
	uint8_t status;
	uint8_t fileNodes, directoryNodes;

	MessageBuffer request, response;

	int fd = open_master_conn(fname, &inode, nullptr, false);
	if (fd < 0) { return -1; }

	try {
		serializeLegacyPacket(request, CLTOMA_FUSE_GETEATTR, msgid, inode, mode);

		response = ServerConnection::sendAndReceive(
		    fd, request, MATOCL_FUSE_GETEATTR,
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

		if (bodySize < sizeof(fileNodes) + sizeof(directoryNodes)) {
			printf("%s: master query: wrong answer (leng)\n", fname);
			close_master_conn(1);
			return -1;
		}

		fileNodes = get8bit(&rptr);
		directoryNodes = get8bit(&rptr);

		bodySize -= (sizeof(fileNodes) + sizeof(directoryNodes));

		// NORMAL MODE
		if (mode == GMODE_NORMAL) {
			const uint32_t expectedNormalBody = sizeof(eattr) + sizeof(cnt);

			if (bodySize != expectedNormalBody) {
				printf("%s: master query: wrong answer (leng)\n", fname);
				close_master_conn(1);
				return -1;
			}

			eattr = get8bit(&rptr);
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

			printf("%s: ", fname);

			if (eattr == 0) {
				printf("-\n");
				return 0;
			}

			bool first = true;
			for (uint8_t j = 0; j < EATTR_BITS; ++j) {
				if (eattr & (1u << j)) {
					printf("%s%s", first ? "" : ",", eattrtab[j]);
					first = false;
				}
			}
			printf("\n");

			return 0;
		}

		// AGGREGATED MODE
		const uint32_t entrySize = sizeof(eattr) + sizeof(cnt);
		const uint32_t expectedAggregatedBody = (fileNodes + directoryNodes) * entrySize;

		if (bodySize != expectedAggregatedBody) {
			printf("%s: master query: wrong answer (leng)\n", fname);
			close_master_conn(1);
			return -1;
		}

		uint32_t fcnt[EATTR_BITS] = {};
		uint32_t dcnt[EATTR_BITS] = {};

		for (uint8_t i = 0; i < fileNodes; ++i) {
			eattr = get8bit(&rptr);
			get32bit(&rptr, cnt);

			for (uint8_t j = 0; j < EATTR_BITS; ++j) {
				if (eattr & (1u << j)) { fcnt[j] += cnt; }
			}
		}

		for (uint8_t i = 0; i < directoryNodes; ++i) {
			eattr = get8bit(&rptr);
			get32bit(&rptr, cnt);

			for (uint8_t j = 0; j < EATTR_BITS; ++j) {
				if (eattr & (1u << j)) { dcnt[j] += cnt; }
			}
		}

		close_master_conn(0);

		printf("%s:\n", fname);

		for (uint8_t j = 0; j < EATTR_BITS; ++j) {
			if (eattrtab[j][0]) {
				printf(" not directory nodes with attribute %16s :", eattrtab[j]);
				print_number(" ", "\n", fcnt[j], 1, 0, 1);

				printf(" directories with attribute         %16s :", eattrtab[j]);
				print_number(" ", "\n", dcnt[j], 1, 0, 1);
			} else {
				if (fcnt[j] > 0) {
					printf(" not directory nodes with attribute      'unknown-%u' :", j);
					print_number(" ", "\n", fcnt[j], 1, 0, 1);
				}
				if (dcnt[j] > 0) {
					printf(" directories with attribute              'unknown-%u' :", j);
					print_number(" ", "\n", dcnt[j], 1, 0, 1);
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

int get_eattr_run(int argc, char **argv) {
	int rflag = 0;
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
			get_eattr_usage();
			return 1;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		get_eattr_usage();
		return 1;
	}

	status = 0;
	while (argc > 0) {
		if (get_eattr(*argv, (rflag) ? GMODE_RECURSIVE : GMODE_NORMAL) < 0) {
			status = 1;
		}
		argc--;
		argv++;
	}
	return status;
}
