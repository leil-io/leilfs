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

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include <foundationdb/fdb_c_types.h>

namespace fdb {

namespace detail {

/// Indirection over the process-wide fdb_c calls so RuntimeCore is testable without a
/// real client library initialization (which is once-per-process and irreversible).
/// Production uses defaultRuntimeApi(); tests substitute failing/counting stubs.
struct RuntimeApi {
	fdb_error_t (*selectApiVersion)(int version);
	fdb_error_t (*setupNetwork)();
	fdb_error_t (*runNetwork)();
	fdb_error_t (*stopNetwork)();
};

/// The real fdb_c entry points.
RuntimeApi defaultRuntimeApi();

/// State shared with the network thread. The thread can outlive its RuntimeCore on the
/// detach path in shutdown() (a refused fdb_stop_network never lets it exit), so this
/// lives on the heap behind a shared_ptr and the thread lambda captures that copy, never
/// `this`. Carries the thread's exit error plus the startup handshake the first
/// ensureStarted() blocks on to catch an immediate fdb_run_network failure.
struct NetworkThreadState {
	/// Nonzero once fdb_run_network returned an error and the client can no longer make
	/// progress. ensureStarted() reads it to refuse a doomed runtime instead of handing
	/// out futures that never resolve.
	std::atomic<fdb_error_t> error{0};
	std::mutex startupMutex;
	std::condition_variable startupCv;
	/// Set by the network thread immediately before it calls run_network. ensureStarted()
	/// waits on this first, so the startup grace bounds only the synchronous entry-to-
	/// rejection window and not thread-scheduling latency under load (which could otherwise
	/// let a real rejection go unobserved and a doomed runtime pass as healthy). Guarded by
	/// startupMutex.
	bool enteredRunNetwork = false;
	/// Set once fdb_run_network has returned. During startup that only happens when it
	/// rejected (a healthy run_network blocks until fdb_stop_network), so it is the
	/// signal the first ensureStarted() waits on. Guarded by startupMutex.
	bool runNetworkReturned = false;
	/// Set by ensureStarted() once startup committed without error. After that, a
	/// fdb_run_network return is an unexpected post-startup death that no later
	/// ensureStarted() observes (a single-client process makes none), so the network thread
	/// fail-stops rather than hang every future. Read under startupMutex, race-free against
	/// ensureStarted() deciding to commit.
	bool startupCommitted = false;
	/// Set by shutdown() before it stops the network, so the thread can tell a coordinated
	/// stop (run_network returns as asked) from an out-of-band one (stopped behind our back,
	/// which fail-stops even on a clean return). Guarded by startupMutex.
	bool shutdownRequested = false;
};

/// Lifecycle of the process-wide FoundationDB client: API-version selection, network
/// setup, the network thread, and shutdown. The underlying client is once-per-process
/// and cannot be restarted after fdb_stop_network, which is why the state machine is
/// one-way: NotStarted -> Running -> Stopped, and start() from Stopped fails.
///
/// This class holds the logic and is instantiable so tests can drive every path with
/// stubbed RuntimeApi hooks; production goes through the fdb::Runtime singleton facade.
class RuntimeCore {
public:
	enum class State {
		kNotStarted,
		kRunning,
		kStopped
	};

	explicit RuntimeCore(RuntimeApi api = defaultRuntimeApi()) : api_(api) {}

	RuntimeCore(const RuntimeCore &) = delete;
	RuntimeCore &operator=(const RuntimeCore &) = delete;
	RuntimeCore(RuntimeCore &&) = delete;
	RuntimeCore &operator=(RuntimeCore &&) = delete;

	/// Joins the network thread if still running (see shutdown()).
	~RuntimeCore();

	/// Selects the API version, configures the network, and starts the network thread.
	/// Thread-safe and idempotent while Running. The first start briefly waits for the
	/// network thread so an fdb_run_network that rejects at startup is surfaced here
	/// rather than only on a later call (which a single-client process never makes).
	/// @throws FDBException if initialization fails, if the network thread rejected at
	///   startup, or when it has already exited with an error: the event loop is gone,
	///   every future would hang, and fdb_run_network cannot be called again in-process.
	/// @throws std::logic_error when called after shutdown(): a wrong-state call, since
	///   the client cannot be restarted in-process.
	void ensureStarted(int apiVersion);

	/// Stops the network and joins its thread. Thread-safe and idempotent; safe to call
	/// without a prior ensureStarted(). After this the FDB client is unusable for the
	/// rest of the process lifetime.
	void shutdown();

	State state() const;

private:
	RuntimeApi api_;
	mutable std::mutex mutex_;
	State state_ = State::kNotStarted;
	std::thread networkThread_;
	/// Shared with the network thread (which may outlive this object on the detach path
	/// in shutdown()): the thread's exit error and the startup handshake. See
	/// NetworkThreadState.
	std::shared_ptr<NetworkThreadState> netState_ = std::make_shared<NetworkThreadState>();
};

}  // namespace detail

/// Process-wide FoundationDB client runtime (singleton facade over RuntimeCore).
///
/// Every FDBContext shares this runtime: contexts are cheap handles, and destroying
/// them never stops the network (a stopped client cannot be restarted, so refcounted
/// stop-on-last-owner would break any later create). The singleton is deliberately
/// leaked at process exit: fdb_stop_network must be the last fdb_c call, after every DB
/// and future is destroyed, and static-destructor order cannot guarantee that against
/// DB objects owned elsewhere. Leaking leaves the network thread for the OS to reclaim;
/// an explicit shutdown() (tests, controlled teardown) still stops it in a safe order.
class Runtime {
public:
	/// Starts the shared runtime with the compiled FDB_API_VERSION if not yet running.
	/// @throws FDBException on failure.
	/// @throws std::logic_error when the runtime was already shut down (it cannot be
	///   restarted in-process).
	static void ensureStarted();

	/// Stops the shared runtime. Idempotent.
	static void shutdown();

	static detail::RuntimeCore::State state();
};

}  // namespace fdb
