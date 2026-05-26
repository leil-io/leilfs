/*
   Copyright 2016-2017 Skytechnology sp. z o.o.
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

#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string>
#include <thread>

#include "common/lambda_guard.h"
#include "common/server_connection.h"
#include "common/signal_handling.h"
#include "protocol/cltoma.h"
#include "protocol/matocl.h"
#include "tools/tools_commands.h"
#include "tools/tools_common_functions.h"

static int kInfiniteTimeout = 10 * 24 * 3600 * 1000; // simulate infinite timeout (10 days)

static void recursive_remove_usage() {
	fprintf(stderr,
	        "recursive remove\n\nusage:\n leil rremove name [name ...]\n");
}

static int recursive_remove(const char *file_name) {
	char path_buf[PATH_MAX];
	inode_t parent;
	uint32_t uid, gid;
	int fd;
	uint8_t status;
	uint32_t msgid = 0, job_id;

	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGHUP);
	sigaddset(&set, SIGUSR1);
	sigprocmask(SIG_BLOCK, &set, NULL);

	auto find_last_delimiter_pos = [](const std::string &parent_path) {
		std::size_t last_pos_delimiter_unix = parent_path.find_last_of("/");
		std::size_t last_pos_delimiter_win = parent_path.find_last_of("\\");
		std::size_t last_pos_delimiter = std::string::npos;

		if (last_pos_delimiter_unix != std::string::npos &&
		    last_pos_delimiter_win != std::string::npos) {
			last_pos_delimiter =
			    std::max(last_pos_delimiter_unix, last_pos_delimiter_win);
		} else {
			last_pos_delimiter = (last_pos_delimiter_unix == std::string::npos)
			                         ? last_pos_delimiter_win
			                         : last_pos_delimiter_unix;
		}

		return last_pos_delimiter;
	};

	std::string name_to_use = std::string(file_name);
#ifdef _WIN32
	if (name_to_use.back() == '\\' || name_to_use.back() == '/') {
		name_to_use.pop_back();
	}
#endif
	if (!get_full_path(name_to_use.c_str(), path_buf)) {
		printf("%s: get_full_path error\n", file_name);
		return -1;
	}
	std::string parent_path(path_buf);
	parent_path = parent_path.substr(0, find_last_delimiter_pos(parent_path));

	fd = open_master_conn(parent_path.c_str(), &parent, nullptr, false);
	if (fd < 0) {
		return -1;
	}

	uid = getUId();
	gid = getGId();

	std::string fname(path_buf);
	std::size_t pos = find_last_delimiter_pos(fname);
	if (pos != std::string::npos) {
		fname = fname.substr(pos + 1);
	}

	printf("Executing recursive remove (%s) ...\n", path_buf);
	try {
		auto request = cltoma::requestTaskId::build(msgid);
		auto response = ServerConnection::sendAndReceive(fd,
				request, SAU_MATOCL_REQUEST_TASK_ID,
				ServerConnection::ReceiveMode::kReceiveFirstNonNopMessage,
				kInfiniteTimeout);
		matocl::requestTaskId::deserialize(response, msgid, job_id);

		std::thread signal_thread(std::bind(signalHandler, job_id));

		/* destructor of LambdaGuard will send SIGUSR1 signal in order to
		 * return from signalHandler function and join thread */
		auto join_guard = makeLambdaGuard([&signal_thread]() {
			kill(getpid(), SIGUSR1);
			signal_thread.join();
		});
		request = cltoma::recursiveRemove::build(msgid, job_id, parent, fname, uid, gid);
		response = ServerConnection::sendAndReceive(fd, request, SAU_MATOCL_RECURSIVE_REMOVE,
					ServerConnection::ReceiveMode::kReceiveFirstNonNopMessage,
					kInfiniteTimeout);

		matocl::recursiveRemove::deserialize(response, msgid, status);

		close_master_conn(0);

		if (status == SAUNAFS_STATUS_OK) {
			printf("Recursive remove (%s) completed\n", path_buf);
			return 0;
		} else {
			printf("Recursive remove (%s):\n returned error status %d: %s\n", path_buf, status, saunafs_error_string(status));
			return -1;
		}

	} catch (Exception &e) {
		fprintf(stderr, "%s\n", e.what());
		close_master_conn(1);
		return -1;
	}
	return 0;
}

int recursive_remove_run(int argc, char **argv) {
	int status = 0;
	// Go past initial command
	argc--;
	argv++;

	if (argc < 1) {
		recursive_remove_usage();
		return 1;
	}

	while (argc > 0) {
		if (recursive_remove(*argv) < 0) {
			status = 1;
		}
		argc--;
		argv++;
	}
	return status;
}
