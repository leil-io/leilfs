/*
   Copyright 2013-2017 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

   This file is part of SaunaFS.

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

#include <ostream>
#include <stdlib.h>
#include <unistd.h>

#include "common/version.h"
#include "tools/tools_commands.h"

int printUsage(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage:\n");
		fprintf(stderr, "\tleil <tool name> [options]\n");
		printTools();
		printf("\n");
		printOptions();
	}
	else {
		std::function<int(int, char **)> func = getCommand(argv[1]);
		if (func == nullptr) {
			fprintf(stderr, "unknown command: %s\n", argv[1]);
		} else {
			func(1, &argv[1]);
		}
	}
	return 0;
}

void printTools() {
	fprintf(stderr, "tools:\n");
	fprintf(stderr, "\tgetgoal\n\tsetgoal\n");
	fprintf(stderr, "\tgettrashtime\n\tsettrashtime\n");
	fprintf(stderr, "\tgeteattr\n\tseteattr\n\tdeleattr\n");
	fprintf(stderr, "\tcheckfile\n\tfileinfo\n");
	fprintf(stderr, "\tappendchunks\n\tdirinfo\n");
	fprintf(stderr, "\tfilerepair\n\tmakesnapshot\n");
	fprintf(stderr, "\trepquota\n\tsetquota\n");
	fprintf(stderr, "\trremove\n");
	fprintf(stderr, "\thelp [tool name]\n");
	fprintf(stderr, "deprecated tools:\n");
}

void printOptions() {
	printf("options: \n");
	printf("\t-v/--version: print version\n");
}

static int cd_func(int argc, char **argv) {
	if (argc == 2) {
		int status = chdir(argv[1]);
		if (status == -1) {
			fprintf(stderr, "sau: %s: No such file or directory\n", argv[1]);
		}
		return status;
	}
	return -1;
}

static int ls_func(int argc, char **argv) {
#ifdef _WIN32
	std::string command("dir");
#else
	std::string command("ls");
#endif

	for (int i = 1; i < argc; ++i) {
		command += " ";
		command.append(argv[i]);
	}
	int status = system(command.c_str());
	if (status == -1) {
		fprintf(stderr, "sau: ls operation failed to execute successfully\n");
	}
	return status;
}

static int exit_func(int /*argc*/, char **/*argv*/) {
	exit(0);
	return 0;
}

static std::unordered_map<std::string, std::function<int(int, char **)>> sauna_commands({
	{"getgoal", get_goal_run},
	{"setgoal", set_goal_run},
	{"gettrashtime", get_trashtime_run},
	{"settrashtime", set_trashtime_run},
	{"checkfile", check_file_run},
	{"fileinfo", file_info_run},
	{"appendchunks", append_file_run},
	{"dirinfo", dir_info_run},
	{"geteattr", get_eattr_run},
	{"seteattr", set_eattr_run},
	{"deleattr", del_eattr_run},
	{"filerepair", file_repair_run},
	{"makesnapshot", snapshot_run},
	{"repquota", quota_rep_run},
	{"setquota", quota_set_run},
	{"rremove", recursive_remove_run},
	{"help", printUsage},
	{"cd", cd_func},
	{"ls", ls_func},
	{"exit", exit_func},
	{"quit", exit_func},
});

std::function<int(int, char **)> getCommand(const std::string &func_name) {
	auto func = sauna_commands.find(func_name);
	if (func == sauna_commands.end()) {
		return nullptr;
	} else {
		return func->second;
	}
}

void printArgs(int argc, char **argv) {
	fprintf(stdout, "Args:\n");
	for (int i = 0; i < argc; ++i) {
		fprintf(stdout, "   <%s>\n", argv[i]);
	}
}
