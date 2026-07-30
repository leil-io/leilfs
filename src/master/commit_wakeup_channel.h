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

#pragma once

#include "common/platform.h"

#include <mutex>
#include <vector>

#include <poll.h>

/// Guards an eventfd shared between the master's event-loop thread and the FDB
/// network thread (which is never joined in production, see fdb::Runtime).
/// wakeup() is the one call that can arrive from that other thread; every other
/// method runs on the event-loop thread. The mutex's job is only to serialize
/// wakeup() against retire(): whichever acquires it first either completes its
/// write while the fd is still valid, or observes it already cleared and skips.
/// arm()/pollDesc()/pollServe() take the same lock purely so every touch of the
/// fd goes through one consistent accessor, not because they need protection
/// from each other (the event-loop thread already serializes them).
class CommitWakeupChannel {
public:
	/// Registers the fd to signal. Called once from module init.
	void arm(int fd);

	/// Writes one to the fd if still armed. Called from the FDB network thread's
	/// future ready-callback; kept minimal, touches only the fd.
	void wakeup();

	/// pollregister desc: adds the fd to the watch set if still armed.
	void pollDesc(std::vector<pollfd> &pdesc);

	/// pollregister serve: drains the fd if it was signaled.
	void pollServe(const std::vector<pollfd> &pdesc);

	/// Invalidates and closes the fd. Called once from matoclserv_term, holds the
	/// lock through both the invalidation and the close() so no interleaving with
	/// a concurrent wakeup() is possible.
	void retire();

private:
	std::mutex mutex_;
	int fd_ = -1;  ///< Guarded by mutex_. -1 means unarmed or retired.
};
