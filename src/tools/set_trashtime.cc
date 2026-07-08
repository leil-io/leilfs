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
#include <cstddef>

#include "common/datapack.h"
#include "common/massert.h"
#include "common/server_connection.h"
#include "common/type_defs.h"
#include "errors/saunafs_error_codes.h"
#include "errors/sfserr.h"
#include "tools/tools_commands.h"
#include "tools/tools_common_functions.h"

static int kInfiniteTimeout = -1;

static void set_trashtime_usage() {
	fprintf(stderr,
	        "set objects trashtime (how many seconds file should be left in trash)\n\nusage: "
	        "\n leil settrashtime [-nhHr] SECONDS[-|+] name [name ...]\n");
	print_numberformat_options();
	print_recursive_option();
	fprintf(stderr, " SECONDS+ - if trashtime smaller then given value, increase trashtime to given value\n");
	fprintf(stderr, " SECONDS- - if trashtime bigger then given value, decrease trashtime to given value\n");
	fprintf(stderr, " SECONDS - just set trashtime to given value\n");
}

static int set_trashtime(const char *fname, uint32_t trashtime, uint8_t mode) {
	uint32_t uid;
	uint32_t msgid{0};
	uint8_t status;
	inode_t inode, changed, notchanged, notpermitted;

	int fd = open_master_conn(fname, &inode, nullptr, true);
	if (fd < 0) {
		return -1;
	}

	uid = getUId();

	try {
		MessageBuffer request, response;

		serializeLegacyPacket(request, CLTOMA_FUSE_SETTRASHTIME, msgid, inode, uid, trashtime,
		                      mode);

		response = ServerConnection::sendAndReceive(
		    fd, request, MATOCL_FUSE_SETTRASHTIME,
		    ServerConnection::ReceiveMode::kReceiveFirstNonNopMessage, kInfiniteTimeout);

		const uint8_t *rptr = response.data();
		get32bit(&rptr, msgid);

		if (msgid != 0) {
			printf("%s: master query: wrong answer (queryid)\n", fname);
			close_master_conn(0);
			return -1;
		}

		uint32_t remaining = static_cast<uint32_t>(response.size() - sizeof(msgid));

		if (remaining == sizeof(status)) {
			status = *rptr;
			printf("%s: %s\n", fname, saunafs_error_string(status));
			close_master_conn(0);
			return (status == SAUNAFS_STATUS_OK) ? 0 : -1;
		}

		const uint32_t kExpectedSize = sizeof(changed) + sizeof(notchanged) + sizeof(notpermitted);
		if (remaining != kExpectedSize) {
			printf("%s: master query: wrong answer (leng)\n", fname);
			close_master_conn(0);
			return -1;
		}

		getINode(&rptr, changed);
		getINode(&rptr, notchanged);
		getINode(&rptr, notpermitted);

		if ((mode & SMODE_RMASK) == 0) {
			if (changed || mode == SMODE_SET) {
				printf("%s: %" PRIu32 "\n", fname, trashtime);
			} else {
				printf("%s: trashtime not changed\n", fname);
			}
		} else {
			printf("%s:\n", fname);
			print_number(" inodes with trashtime changed:     ", "\n", changed, kMode32, 0, 1);
			print_number(" inodes with trashtime not changed: ", "\n", notchanged, kMode32, 0, 1);
			print_number(" inodes with permission denied:     ", "\n", notpermitted, kMode32, 0, 1);
		}

		close_master_conn(0);
		return 0;
	} catch (const Exception &e) {
		fprintf(stderr, "%s\n", e.what());
		close_master_conn(1);
		return -1;
	}
}

static int gene_set_trashtime_run(int argc, char **argv, int rflag) {
	int ch, status;
	uint32_t trashtime = 86400;
	uint8_t smode = SMODE_SET;

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
			set_trashtime_usage();
			return 1;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc == 0) {
		set_trashtime_usage();
		return 1;
	}

	char *p = argv[0];
	trashtime = 0;
	while (p[0] >= '0' && p[0] <= '9') {
		trashtime *= 10;
		trashtime += (p[0] - '0');
		p++;
	}
	if (p[0] == '\0' || ((p[0] == '-' || p[0] == '+') && p[1] == '\0')) {
		if (p[0] == '-') {
			smode = SMODE_DECREASE;
		} else if (p[0] == '+') {
			smode = SMODE_INCREASE;
		}
	} else {
		fprintf(
		    stderr,
		    "trashtime should be given as number of seconds optionally followed by '-' or '+'\n");
		set_trashtime_usage();
		return 1;
	}
	argc--;
	argv++;

	if (argc < 1) {
		set_trashtime_usage();
		return 1;
	}
	status = 0;
	while (argc > 0) {
		if (set_trashtime(*argv, trashtime, (rflag) ? (smode | SMODE_RMASK) : smode) < 0) {
			status = 1;
		}
		argc--;
		argv++;
	}
	return status;
}

int set_trashtime_run(int argc, char **argv) {
	return gene_set_trashtime_run(argc, argv, 0);
}
