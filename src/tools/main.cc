/*
   Copyright 2013-2016 Skytechnology sp. z o.o.
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

#include <cassert>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <ostream>
#include <unistd.h>

#include "common/version.h"
#include "tools/tools_commands.h"
#include "tools/tools_common_functions.h"

static char path_buf[PATH_MAX];

void split(std::vector<char*> &argv_new, std::vector<char> &line) {
	size_t pos = 0;
	size_t endpos = 0;
	for (endpos = 0; endpos < line.size(); ++endpos) {
		if (std::isspace(line[endpos])) {
			line[endpos] = '\0';
			if (endpos - pos > 0) {
				argv_new.push_back(&line[pos]);
			}
			pos = endpos + 1;
		}
	}
	if (endpos - pos > 1) {
		argv_new.push_back(&line[pos]);
	}
}

static void print_prefix() {
	char *path = getcwd(path_buf, PATH_MAX);
	fprintf(stdout, "sau:%s$ ", path);
}

int main(int argc, char **argv) {
	int status = 0;
	set_humode();

#ifdef _WIN32
	socketinit();
#endif

	if (argc > 1) {
		std::string func_name(argv[1]);
		if (func_name == "--version" || func_name == "-v") {
			std::cout << common::version() << '\n';
			exit(EXIT_SUCCESS);
		}
		auto func = getCommand(func_name);
		if (func == nullptr) {
			fprintf(stderr, "unknown command: %s\n", argv[1]);
			printUsage();
			status = 1;
		} else {
			status = func(argc - 1, &argv[1]);
		}
	} else if (argc == 1) {
		std::string command;
		std::vector<char*> argv_new;
		print_prefix();
		while (std::getline(std::cin, command)) {
			optind = 0;
			std::vector<char> line(command.begin(), command.end());
			line.push_back('\0');
			if (command.size() != 0) {
				split(argv_new, line);
				assert(!argv_new.empty());
				auto func = getCommand(argv_new[0]);
				if (func == nullptr) {
					fprintf(stderr, "unknown command: %s\n", argv_new[0]);
					printTools();
					status = 1;
				} else {
					status = func(argv_new.size(), argv_new.data());
					force_master_conn_close();
				}
			}
			argv_new.clear();
			set_humode();
			print_prefix();
		}
	}
	return status;
}
