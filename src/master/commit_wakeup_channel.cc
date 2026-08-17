/*
   Copyright 2026      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include <cstdint>

#include <unistd.h>

#include "master/commit_wakeup_channel.h"

void CommitWakeupChannel::arm(int fd) {
	std::lock_guard<std::mutex> lock(mutex_);
	fd_ = fd;
}

void CommitWakeupChannel::wakeup() {
	std::lock_guard<std::mutex> lock(mutex_);
	if (fd_ < 0) { return; }
	uint64_t one = 1;
	ssize_t written = write(fd_, &one, sizeof(one));
	(void)written;
}

void CommitWakeupChannel::pollDesc(std::vector<pollfd> &pdesc) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (fd_ < 0) { return; }
	pdesc.push_back(pollfd{fd_, POLLIN, 0});
}

void CommitWakeupChannel::pollServe(const std::vector<pollfd> &pdesc) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (fd_ < 0) { return; }
	for (const auto &pfd : pdesc) {
		if (pfd.fd == fd_ && (pfd.revents & POLLIN) != 0) {
			uint64_t drain = 0;
			ssize_t got = read(fd_, &drain, sizeof(drain));
			(void)got;
			break;
		}
	}
}

void CommitWakeupChannel::retire() {
	std::lock_guard<std::mutex> lock(mutex_);
	if (fd_ >= 0) {
		close(fd_);
		fd_ = -1;
	}
}
