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

#include "common/server_connection.h"
#include "common/type_defs.h"
#include "protocol/cltoma.h"
#include "protocol/matocl.h"
#include "tools/tools_commands.h"
#include "tools/tools_common_functions.h"

static void get_goal_usage() {
	fprintf(stderr,
	        "get objects goal (desired number of copies)\n\nusage:\n leil getgoal [-nhHr] name [name "
	        "...]\n");
	print_numberformat_options();
	print_recursive_option();
}

static int get_goal(const char *fname, uint8_t mode) {
	inode_t inode;
	int fd = open_master_conn(fname, &inode, NULL, false);
	if (fd < 0) {
		return -1;
	}
	try {
		uint32_t messageId = 0;
		MessageBuffer request;
		cltoma::fuseGetGoal::serialize(request, messageId, inode, mode);
		MessageBuffer response =
		    ServerConnection::sendAndReceive(fd, request, SAU_MATOCL_FUSE_GETGOAL);
		PacketVersion version;
		std::vector<FuseGetGoalStats> goalsStats;
		deserializePacketVersionNoHeader(response, version);
		if (version == matocl::fuseGetGoal::kStatusPacketVersion) {
			uint8_t status;
			matocl::fuseGetGoal::deserialize(response, messageId, status);
			throw Exception(std::string(fname) + ": failed", status);
		}
		matocl::fuseGetGoal::deserialize(response, messageId, goalsStats);

		if (mode == GMODE_NORMAL) {
			if (goalsStats.size() != 1) {
				throw Exception(std::string(fname) +
				                ": master query: wrong answer (goalsStats.size != 1)");
			}
			printf("%s: %s\n", fname, goalsStats[0].goalName.c_str());
		} else {
			printf("%s:\n", fname);
			for (FuseGetGoalStats goalStats : goalsStats) {
				if (goalStats.files > 0) {
					printf(" files with goal        %s :", goalStats.goalName.c_str());
					print_number(" ", "\n", goalStats.files, kMode32, 0, 1);
				}
			}
			for (FuseGetGoalStats goalStats : goalsStats) {
				if (goalStats.directories > 0) {
					printf(" directories with goal  %s :", goalStats.goalName.c_str());
					print_number(" ", "\n", goalStats.directories, kMode32, 0, 1);
				}
			}
		}
	} catch (Exception &e) {
		fprintf(stderr, "%s\n", e.what());
		close_master_conn(1);
		return -1;
	}
	close_master_conn(0);
	return 0;
}

static int gene_get_goal_run(int argc, char **argv, int rflag) {
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
			get_goal_usage();
			return 1;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		get_goal_usage();
		return 1;
	}

	status = 0;
	while (argc > 0) {
		if (get_goal(*argv, (rflag) ? GMODE_RECURSIVE : GMODE_NORMAL) < 0) {
			status = 1;
		}
		argc--;
		argv++;
	}
	return status;
}

int get_goal_run(int argc, char **argv) {
	return gene_get_goal_run(argc, argv, 0);
}
