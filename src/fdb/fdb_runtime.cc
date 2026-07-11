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

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <stdexcept>

// Needs to be included before foundationdb/fdb_c.h
#include <fdb/fdb_api_version.h>

#include <foundationdb/fdb_c.h>

#include "fdb/fdb_errors.h"
#include "fdb/fdb_runtime.h"
#include "slogger/slogger.h"

namespace fdb {

namespace detail {

RuntimeApi defaultRuntimeApi() {
	return {
	    [](int version) { return fdb_select_api_version(version); },
	    []() { return fdb_setup_network(); },
	    []() { return fdb_run_network(); },
	    []() { return fdb_stop_network(); },
	};
}

RuntimeCore::~RuntimeCore() { shutdown(); }

void RuntimeCore::ensureStarted(int apiVersion) {
	std::lock_guard<std::mutex> guard(mutex_);
	if (state_ == State::kRunning) {
		// A healthy run_network never returns; if it has, the event loop is gone (rejected
		// at startup, failed later, or stopped out-of-band) and cannot be relaunched, so
		// pretending the runtime is healthy would hand out futures that never resolve.
		bool networkReturned = false;
		fdb_error_t networkError = 0;
		{
			std::lock_guard<std::mutex> startupLock(netState_->startupMutex);
			networkReturned = netState_->runNetworkReturned;
			networkError = netState_->error.load();
		}
		if (networkReturned) {
			throw FDBException(static_cast<int>(networkError),
			                   "FDB network is not running; the client cannot make progress");
		}
		return;
	}
	if (state_ == State::kStopped) {
		// fdb_stop_network is final for the process; a restart would hang or crash
		// inside the client library, so fail loudly instead. This is a wrong-state
		// call by the caller, not an fdb_c failure, hence std::logic_error and not
		// FDBException (see the error-domain rules in kv/ifuture.h).
		throw std::logic_error("FDB runtime cannot be restarted after shutdown");
	}

	// 2201 (api_version_already_set) is fine: a previous start attempt may have
	// selected the version before failing at a later step.
	constexpr fdb_error_t kApiVersionAlreadySet = 2201;
	fdb_error_t selectError = api_.selectApiVersion(apiVersion);
	if (selectError != kApiVersionAlreadySet) {
		checkFdbError(selectError, "Failed to select FDB API version");
	}
	// Same for 2009 (network_already_setup): a previous attempt may have set up the
	// network and then failed to launch the network thread, leaving state_ at
	// kNotStarted with the setup already done.
	constexpr fdb_error_t kNetworkAlreadySetup = 2009;
	fdb_error_t setupError = api_.setupNetwork();
	if (setupError != kNetworkAlreadySetup) {
		checkFdbError(setupError, "Failed to set up FDB network");
	}

	// The shared_ptr copy keeps the shared state alive if the thread outlives this object
	// (the detach path in shutdown()); the lambda must not capture `this`.
	networkThread_ = std::thread([api = api_, netState = netState_] {
		pthread_setname_np(pthread_self(), "fdb_network");
		{
			std::lock_guard<std::mutex> startupLock(netState->startupMutex);
			netState->enteredRunNetwork = true;
		}
		netState->startupCv.notify_all();
		fdb_error_t err = api.runNetwork();
		// run_network returns only on failure or, after a healthy run, once the network is
		// stopped. Sample startupCommitted and shutdownRequested under the same lock
		// ensureStarted()/shutdown() set them under, so the fail-stop decision is race-free.
		bool committed = false;
		bool shutdownRequested = false;
		{
			std::lock_guard<std::mutex> startupLock(netState->startupMutex);
			if (err != 0) { netState->error.store(err); }
			netState->runNetworkReturned = true;
			committed = netState->startupCommitted;
			shutdownRequested = netState->shutdownRequested;
		}
		netState->startupCv.notify_all();
		if (err != 0) {
			safs::log_err("FDB network thread exited with error: {}", fdb_get_error(err));
		}
		// Any return after startup that shutdown() did not request is fatal, INCLUDING a
		// clean one: run_network only returns 0 once the network is stopped, so a clean
		// return we did not ask for means it was stopped out-of-band. The event loop is gone
		// either way, so fail-stop (supervisor restarts) rather than let later FDB calls run
		// after stop or hang every future. A startup-time failure is not committed and is
		// surfaced to the first ensureStarted() instead.
		if (committed && !shutdownRequested) {
			safs::log_err("FDB network event loop exited unexpectedly, aborting process");
			std::abort();
		}
	});

	// fdb_run_network rejects synchronously at entry (bad init, already run), so an
	// immediate failure surfaces within a short grace window. A healthy run_network
	// blocks and never resolves the handshake, so the wait simply times out and we
	// proceed. Without this a single-client process (the master creates exactly one
	// FDBContext) would miss the failure entirely and hang on its first future, because
	// no later ensureStarted() runs to observe netState_->error. The wait is only paid
	// on the success path and only once per process.
	constexpr auto kStartupFailureGrace = std::chrono::milliseconds(100);
	fdb_error_t startupError = 0;
	bool networkReturned = false;
	{
		std::unique_lock<std::mutex> startupLock(netState_->startupMutex);
		// Wait for the thread to actually enter run_network first, so the grace bounds only
		// the synchronous entry-to-rejection window, not thread-scheduling latency (which
		// could otherwise let a real rejection go unobserved on a loaded box).
		netState_->startupCv.wait(startupLock, [this] { return netState_->enteredRunNetwork; });
		netState_->startupCv.wait_for(startupLock, kStartupFailureGrace,
		                              [this] { return netState_->runNetworkReturned; });
		startupError = netState_->error.load();
		networkReturned = netState_->runNetworkReturned;
		// Commit only if the loop is still running. A healthy run_network never returns, so
		// ANY return during startup (a synchronous rejection, or an out-of-band stop even a
		// clean one) means a dead network the thread cannot relaunch.
		if (!networkReturned) { netState_->startupCommitted = true; }
	}
	// Match the kRunning branch: the network was set up and the thread launched, so the
	// runtime is kRunning even when doomed; a doomed one then refuses every start.
	state_ = State::kRunning;
	if (networkReturned) {
		throw FDBException(
		    static_cast<int>(startupError),
		    "FDB network stopped or failed at startup; the client cannot make progress");
	}
}

void RuntimeCore::shutdown() {
	std::lock_guard<std::mutex> guard(mutex_);
	if (state_ != State::kRunning) {
		state_ = State::kStopped;
		return;
	}
	state_ = State::kStopped;

	// Mark the stop as coordinated before requesting it, so the network thread's imminent
	// run_network return is expected and does not trip the post-startup fail-stop.
	{
		std::lock_guard<std::mutex> startupLock(netState_->startupMutex);
		netState_->shutdownRequested = true;
	}

	fdb_error_t err = api_.stopNetwork();
	if (err != 0) {
		// The network refused to stop, so the thread will never exit: joining would
		// hang shutdown forever. Detach and leak the thread; the process is exiting
		// this runtime anyway.
		safs::log_err("FDB stop_network failed, detaching network thread: {}", fdb_get_error(err));
		if (networkThread_.joinable()) { networkThread_.detach(); }
		return;
	}
	if (networkThread_.joinable()) { networkThread_.join(); }
}

RuntimeCore::State RuntimeCore::state() const {
	std::lock_guard<std::mutex> guard(mutex_);
	return state_;
}

}  // namespace detail

namespace {

detail::RuntimeCore &globalCore() {
	// Deliberately leaked (never deleted): a function-local static would register a
	// destructor that stops the network at process exit, but fdb_stop_network must be
	// the LAST fdb_c call, after every DB and future is destroyed. Static-destructor
	// order cannot guarantee that against DB objects owned elsewhere (e.g. the master's
	// filesystem operations), so an at-exit stop could call into the client after
	// teardown. Leaking leaves the network thread for the OS to reclaim; an explicit
	// Runtime::shutdown() still stops it cleanly in a known-safe order.
	static detail::RuntimeCore *core = new detail::RuntimeCore();
	return *core;
}

}  // namespace

void Runtime::ensureStarted() { globalCore().ensureStarted(FDB_API_VERSION); }

void Runtime::shutdown() { globalCore().shutdown(); }

detail::RuntimeCore::State Runtime::state() { return globalCore().state(); }

}  // namespace fdb
