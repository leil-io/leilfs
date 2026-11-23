/*
   Copyright 2026 Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * Creates files and keeps them all open until it is terminated.
 *
 * Usage:
 *   hold-open-files <directory> <number-of-files> <ready-file>
 *
 * Creates <directory> (if missing) and the files <directory>/file_<i> for i in
 * [0, number-of-files), each kept open. A process may hold only as many files
 * as its open file hard limit allows, so the files are spread over as many
 * child processes as needed. Once every file is open, <ready-file> is created.
 * On SIGTERM or SIGINT the children are stopped, which closes all the files.
 *
 * Exit status: 0 after a clean stop, 1 on any error.
 */

#include "common/platform.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

/// Descriptors a child keeps free for everything other than the held files.
constexpr uint64_t kSpareDescriptors = 64;
constexpr char kChildReady = '1';
constexpr char kChildFailed = '0';

volatile sig_atomic_t gStopRequested = 0;

void requestStop(int) { gStopRequested = 1; }

void usage(const char *program) {
	std::cerr << "Usage: " << program << " <directory> <number-of-files> <ready-file>\n";
}

/// Opens files [first, last) and reports to the parent through reportFd; then waits to be
/// terminated. Never returns.
[[noreturn]] void runChild(const std::string &directory, uint64_t first, uint64_t last,
                           int reportFd) {
#ifdef __linux__
	// The files must not stay open if the parent is gone.
	prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
	std::vector<int> fds;
	fds.reserve(last - first);
	for (uint64_t i = first; i < last; ++i) {
		const std::string path = directory + "/file_" + std::to_string(i);
		const int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
		if (fd < 0) {
			std::cerr << "Failed to open '" << path << "': " << std::strerror(errno) << '\n';
			const char status = kChildFailed;
			(void)!::write(reportFd, &status, 1);
			_exit(1);
		}
		fds.push_back(fd);
	}

	const char status = kChildReady;
	if (::write(reportFd, &status, 1) != 1) { _exit(1); }
	::close(reportFd);

	for (;;) { ::pause(); }
}

void stopChildren(const std::vector<pid_t> &children) {
	for (pid_t child : children) { ::kill(child, SIGTERM); }
	for (pid_t child : children) { ::waitpid(child, nullptr, 0); }
}

}  // namespace

int main(int argc, char **argv) {
	if (argc != 4) {
		usage(argv[0]);
		return 1;
	}
	const std::string directory = argv[1];
	char *end = nullptr;
	const uint64_t fileCount = std::strtoull(argv[2], &end, 10);
	if (end == argv[2] || *end != '\0' || fileCount == 0) {
		usage(argv[0]);
		return 1;
	}
	const std::string readyFile = argv[3];

	rlimit limit{};
	if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) {
		std::cerr << "getrlimit failed: " << std::strerror(errno) << '\n';
		return 1;
	}
	uint64_t filesPerChild = fileCount;
	if (limit.rlim_max != RLIM_INFINITY) {
		if (limit.rlim_max <= kSpareDescriptors) {
			std::cerr << "Open file hard limit " << limit.rlim_max << " is too low\n";
			return 1;
		}
		filesPerChild = std::min<uint64_t>(fileCount, limit.rlim_max - kSpareDescriptors);
	}
	// Children inherit the raised soft limit.
	limit.rlim_cur = limit.rlim_max == RLIM_INFINITY
	                     ? static_cast<rlim_t>(filesPerChild + kSpareDescriptors)
	                     : limit.rlim_max;
	if (::setrlimit(RLIMIT_NOFILE, &limit) != 0) {
		std::cerr << "setrlimit failed: " << std::strerror(errno) << '\n';
		return 1;
	}

	if (::mkdir(directory.c_str(), 0755) != 0 && errno != EEXIST) {
		std::cerr << "Failed to create '" << directory << "': " << std::strerror(errno) << '\n';
		return 1;
	}

	struct sigaction action{};
	action.sa_handler = requestStop;
	sigemptyset(&action.sa_mask);
	::sigaction(SIGTERM, &action, nullptr);
	::sigaction(SIGINT, &action, nullptr);

	int reportPipe[2];
	if (::pipe(reportPipe) != 0) {
		std::cerr << "pipe failed: " << std::strerror(errno) << '\n';
		return 1;
	}

	std::vector<pid_t> children;
	for (uint64_t first = 0; first < fileCount; first += filesPerChild) {
		const uint64_t last = std::min(fileCount, first + filesPerChild);
		const pid_t child = ::fork();
		if (child < 0) {
			std::cerr << "fork failed: " << std::strerror(errno) << '\n';
			stopChildren(children);
			return 1;
		}
		if (child == 0) {
			::close(reportPipe[0]);
			struct sigaction defaultAction{};
			defaultAction.sa_handler = SIG_DFL;
			::sigaction(SIGTERM, &defaultAction, nullptr);
			::sigaction(SIGINT, &defaultAction, nullptr);
			runChild(directory, first, last, reportPipe[1]);
		}
		children.push_back(child);
	}
	::close(reportPipe[1]);

	// A stop request stays pending until sigsuspend below, so it cannot slip in between
	// checking the flag and waiting.
	sigset_t stopSignals, waitMask;
	sigemptyset(&stopSignals);
	sigaddset(&stopSignals, SIGTERM);
	sigaddset(&stopSignals, SIGINT);
	::sigprocmask(SIG_BLOCK, &stopSignals, &waitMask);
	sigdelset(&waitMask, SIGTERM);
	sigdelset(&waitMask, SIGINT);

	for (size_t reported = 0; reported < children.size();) {
		char status;
		const ssize_t bytesRead = ::read(reportPipe[0], &status, 1);
		if (bytesRead < 0 && errno == EINTR) { continue; }
		if (bytesRead != 1 || status != kChildReady) {
			std::cerr << "Not all files could be opened\n";
			stopChildren(children);
			return 1;
		}
		++reported;
	}
	::close(reportPipe[0]);

	const int readyFd = ::open(readyFile.c_str(), O_CREAT | O_WRONLY, 0644);
	if (readyFd < 0) {
		std::cerr << "Failed to create '" << readyFile << "': " << std::strerror(errno) << '\n';
		stopChildren(children);
		return 1;
	}
	::close(readyFd);
	std::cout << "Holding " << fileCount << " files open in " << children.size()
	          << " processes" << std::endl;

	while (!gStopRequested) { ::sigsuspend(&waitMask); }

	stopChildren(children);
	return 0;
}
