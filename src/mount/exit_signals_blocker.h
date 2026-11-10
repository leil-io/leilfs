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

#pragma once

#include "common/platform.h"

#ifndef _WIN32

#include <pthread.h>
#include <signal.h>

/// Blocks SIGHUP, SIGINT, SIGQUIT and SIGTERM in the calling thread while in scope.
///
/// libfuse handles exit signals by ending the session, on whichever thread
/// receives them, and blocks this set in its own worker threads. Threads
/// started in scope inherit the mask, so the signals reach the thread running
/// the session loop instead of interrupting helper threads, where the handler
/// would race with that loop.
class ExitSignalsBlocker {
public:
	ExitSignalsBlocker() {
		sigset_t exitSignals;
		sigemptyset(&exitSignals);
		sigaddset(&exitSignals, SIGHUP);
		sigaddset(&exitSignals, SIGINT);
		sigaddset(&exitSignals, SIGQUIT);
		sigaddset(&exitSignals, SIGTERM);
		pthread_sigmask(SIG_BLOCK, &exitSignals, &previousMask_);
	}

	~ExitSignalsBlocker() { pthread_sigmask(SIG_SETMASK, &previousMask_, nullptr); }

	ExitSignalsBlocker(const ExitSignalsBlocker &) = delete;
	ExitSignalsBlocker &operator=(const ExitSignalsBlocker &) = delete;

private:
	sigset_t previousMask_;
};

#endif
