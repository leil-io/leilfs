/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2017 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include "event_loop.h"

#include <atomic>
#include <list>
#include <sys/time.h>
#include <unistd.h>

#include "config/cfg.h"
#include "common/exception.h"
#include "common/massert.h"
#include "errors/sfserr.h"
#include "kv/ifuture.h"

#if defined(_WIN32)
  #include "common/sockets.h"
#endif

ExitingStatus gExitingStatus = ExitingStatus::kRunning;
bool gReloadRequested = false;
static bool nextPollNonblocking = false;

/// Runs one event-loop handler, containing KV backend errors.
///
/// Op bodies replay these on a fresh transaction at the op boundary, but periodic
/// and connection handlers (session sweeps, disconnect bookkeeping, ...) have no
/// such boundary. An escaped backend error must not reach std::terminate and abort
/// the whole server: retryable failures are warnings, non-retryable failures are
/// errors, and the interrupted handler runs again on a later loop iteration. Exceptions
/// outside the backend error family still propagate and retain fail-stop behavior.
template <typename Handler, typename... Args>
static void eventloop_run_handler(const char *kind, Handler &&handler, Args &&...args) {
	try {
		handler(std::forward<Args>(args)...);
	} catch (const kv::RetryableTransactionError &e) {
		safs::log_warn(
		    "event loop: retryable backend error escaped a {} handler (err {}), skipping this "
		    "run: {}",
		    kind, e.errorCode(), e.what());
	} catch (const kv::TransactionError &e) {
		safs::log_err(
		    "event loop: backend transaction error escaped a {} handler (err {}), skipping this "
		    "run: {}",
		    kind, e.errorCode(), e.what());
	}
}

typedef struct pollentry {
	void (*desc)(std::vector<pollfd>&);
	void (*serve)(const std::vector<pollfd>&);
} pollentry;

namespace {
std::list<pollentry> gPollEntries;
}

struct timeentry {
	typedef void (*fun_t)(void);
	timeentry(uint64_t ne, uint64_t sec, uint64_t off, int mod, fun_t f, bool ms)
		: nextevent(ne), period(sec), offset(off), mode(mod), fun(f), millisecond_precision(ms) {
	}
	uint64_t nextevent;
	uint64_t period;
	uint64_t offset;
	int      mode;
	fun_t    fun;
	bool     millisecond_precision;
};

typedef std::list<timeentry> TimeEntries;
namespace {
TimeEntries gTimeEntries;
}

static std::atomic<uint32_t> now;
static std::atomic<uint64_t> usecnow;

typedef void(*FunctionEntry)(void);
typedef int(*CanExitEntry)(void);
typedef std::list<FunctionEntry> EntryList;
typedef std::list<CanExitEntry> CanExitEntryList;
static EntryList gDestructEntries;
static CanExitEntryList gCanExitEntries;
static EntryList gWantExitEntries;
static EntryList gReloadEntries;
static EntryList gEachLoopEntries;

void eventloop_load_poll_timeout() {
	gPollTimeout = static_cast<int>(
	    cfg_get_minvalue<uint32_t>("POLL_TIMEOUT_MS", kDefaultPollTimeout, 0));

	safs_pretty_syslog(LOG_NOTICE, "poll timeout set to %d ms", gPollTimeout);
}

void eventloop_make_next_poll_nonblocking() {
	nextPollNonblocking = true;
}

void eventloop_destructregister (FunctionEntry fun) {
	gDestructEntries.push_front(fun);
}

void eventloop_canexitregister (CanExitEntry fun) {
	gCanExitEntries.push_front(fun);
}

void eventloop_wantexitregister (FunctionEntry fun) {
	gWantExitEntries.push_front(fun);
}

void eventloop_reloadregister (FunctionEntry fun) {
	gReloadEntries.push_front(fun);
}

void eventloop_pollregister(void (*desc)(std::vector<pollfd>&),void (*serve)(const std::vector<pollfd>&)) {
	gPollEntries.push_back({desc,serve});
}

void eventloop_eachloopregister (FunctionEntry fun) {
	gEachLoopEntries.push_front(fun);
}

void *eventloop_timeregister(int mode, uint64_t seconds, uint64_t offset, FunctionEntry fun) {
	if (seconds == 0 || offset >= seconds) {
		return NULL;
	}

	uint64_t nextevent = ((now + seconds) / seconds) * seconds + offset;

	gTimeEntries.push_front(timeentry(nextevent, seconds, offset, mode, fun, false));
	return &gTimeEntries.front();
}

void *eventloop_timeregister_ms(uint64_t period, FunctionEntry fun) {
	if (period == 0) {
		return NULL;
	}

	uint64_t nextevent = usecnow / 1000 + period;

	gTimeEntries.push_front(timeentry(nextevent, period, 0, TIMEMODE_RUN_LATE, fun, true));
	return &gTimeEntries.front();
}

void eventloop_timeunregister(void* handler) {
	for (TimeEntries::iterator it = gTimeEntries.begin(); it != gTimeEntries.end(); ++it) {
		if (&(*it) == handler) {
			gTimeEntries.erase(it);
			return;
		}
	}
	mabort("unregistering unknown handle from time table");
}

int eventloop_timechange(void* handle, int mode, uint64_t seconds, uint64_t offset) {
	timeentry *aux = (timeentry*)handle;
	if (seconds == 0 || offset >= seconds) {
		return -1;
	}
	aux->nextevent = ((now + seconds) / seconds) * seconds + offset;
	aux->period = seconds;
	aux->offset = offset;
	aux->mode = mode;
	return 0;
}

int eventloop_timechange_ms(void* handle, uint64_t period) {
	timeentry *aux = (timeentry*)handle;
	if (period == 0) {
		return -1;
	}
	aux->nextevent = ((usecnow / 1000 + period) / period) * period;
	aux->period = period;
	return 0;
}

void eventloop_destruct() {
	for (const FunctionEntry &fun : gDestructEntries) {
		try {
			fun();
		} catch (Exception& ex) {
			safs_pretty_syslog(LOG_WARNING, "term error: %s", ex.what());
		}
	}
}

void eventloop_release_resources(void) {
	gDestructEntries.clear();
	gCanExitEntries.clear();
	gWantExitEntries.clear();
	gReloadEntries.clear();
	gEachLoopEntries.clear();
	gPollEntries.clear();
	gTimeEntries.clear();
}

/* internal */
bool canexit() {
	for (const CanExitEntry &fun : gCanExitEntries) {
		if (fun() == 0) {
			return false;
		}
	}
	return true;
}

uint32_t eventloop_time() {
	return now;
}

uint64_t eventloop_utime() {
	return usecnow;
}

uint8_t eventloop_want_to_terminate() {
	if (gExitingStatus == ExitingStatus::kRunning) {
		gExitingStatus = ExitingStatus::kWantExit;
		safs_pretty_syslog(LOG_INFO, "Exiting on internal request.");
		return SAUNAFS_STATUS_OK;
	} else {
		safs_pretty_syslog(LOG_ERR, "Unable to exit on internal request.");
		return SAUNAFS_ERROR_NOTPOSSIBLE;
	}
}

void eventloop_want_to_reload() {
	gReloadRequested = true;
}

void eventloop_run() {
	uint32_t prevtime  = 0;
	uint64_t prevmtime = 0;
	std::vector<pollfd> pdesc;
	int i;

	eventloop_load_poll_timeout();

	while (gExitingStatus != ExitingStatus::kDoExit) {
		pdesc.clear();
		for (auto &pollit: gPollEntries) {
			pollit.desc(pdesc);
		}
#if defined(_WIN32)
		i = tcppoll(pdesc, nextPollNonblocking ? 0 : gPollTimeout);
#else
		i = poll(pdesc.data(), pdesc.size(),
		         nextPollNonblocking ? 0 : gPollTimeout);
#endif
		nextPollNonblocking = false;
		eventloop_updatetime();
		if (i<0) {
			if (errno==EAGAIN) {
				safs_pretty_syslog(LOG_WARNING,"poll returned EAGAIN");
				usleep(100000);
				continue;
			}
			if (errno!=EINTR) {
				safs_pretty_syslog(LOG_WARNING,"poll error: %s",strerr(errno));
				break;
			}
		} else {
			for (auto &pollit : gPollEntries) {
				eventloop_run_handler("poll", pollit.serve, pdesc);
			}
		}
		for (const FunctionEntry &fun : gEachLoopEntries) {
			eventloop_run_handler("each-loop", fun);
		}

		uint64_t msecnow = usecnow / 1000;

		if (msecnow < prevmtime) {
			// time went backward - recalculate next event time
			for (timeentry& timeit : gTimeEntries) {
				if (!timeit.millisecond_precision) {
					continue;
				}

				uint64_t previous_time_to_run = timeit.nextevent - prevmtime;
				previous_time_to_run = std::min(previous_time_to_run, timeit.period);
				timeit.nextevent = msecnow + previous_time_to_run;
			}
		}

		if (now<prevtime) {
			// time went backward !!! - recalculate "nextevent" time
			// adding previous_time_to_run prevents from running next event too soon.
			for (timeentry& timeit : gTimeEntries) {
				if (timeit.millisecond_precision) {
					continue;
				}

				uint64_t previous_time_to_run = timeit.nextevent - prevtime;
				previous_time_to_run = std::min(previous_time_to_run, timeit.period);
				timeit.nextevent = ((now + previous_time_to_run + timeit.period)
				                   / timeit.period) * timeit.period + timeit.offset;
			}
		} else if (now>prevtime+3600) {
			// time went forward !!! - just recalculate "nextevent" time
			for (timeentry& timeit : gTimeEntries) {
				if (timeit.millisecond_precision) {
					timeit.nextevent = msecnow + timeit.period;
					continue;
				}

				timeit.nextevent = ((now + timeit.period) / timeit.period)
				                    * timeit.period + timeit.offset;
			}
		}

		for (timeentry& timeit : gTimeEntries) {
			if (timeit.millisecond_precision) {
				if (msecnow >= timeit.nextevent) {
					timeit.nextevent = msecnow + timeit.period;
					eventloop_run_handler("timed", timeit.fun);
				}
				continue;
			}

			if (now >= timeit.nextevent) {
				if (timeit.mode == TIMEMODE_RUN_LATE) {
					eventloop_run_handler("timed", timeit.fun);
				} else { /* timeit.mode == TIMEMODE_SKIP_LATE */
					if (now == timeit.nextevent) {
						eventloop_run_handler("timed", timeit.fun);
					}
				}
				timeit.nextevent += ((now - timeit.nextevent + timeit.period)
				                    / timeit.period) * timeit.period;
			}
		}
		prevtime  = now;
		prevmtime = usecnow / 1000;
		if (gExitingStatus == ExitingStatus::kRunning && gReloadRequested) {
			cfg_reload();
			for (const FunctionEntry &fun : gReloadEntries) {
				try {
					fun();
				} catch (Exception& ex) {
					safs_pretty_syslog(LOG_WARNING, "reload error: %s", ex.what());
				}
			}
			gReloadRequested = false;
		}
		if (gExitingStatus == ExitingStatus::kWantExit) {
			for (const FunctionEntry &fun : gWantExitEntries) {
				fun();
			}
			gExitingStatus = ExitingStatus::kCanExit;
		}
		if (gExitingStatus == ExitingStatus::kCanExit) {
			if (canexit()) {
				gExitingStatus = ExitingStatus::kDoExit;
			}
		}
	}
}

void eventloop_updatetime() {
	struct timeval tv;

	gettimeofday(&tv,NULL);
	usecnow = tv.tv_sec * uint64_t(1000000) + tv.tv_usec;
	now = tv.tv_sec;
}
