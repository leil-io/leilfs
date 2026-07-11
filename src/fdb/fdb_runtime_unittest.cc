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

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>

#include "fdb/fdb_errors.h"
#include "fdb/fdb_runtime.h"

using fdb::detail::RuntimeApi;
using fdb::detail::RuntimeCore;
using State = fdb::detail::RuntimeCore::State;

namespace {

constexpr int kApiVersion = 730;
constexpr fdb_error_t kClientInvalidOperation = 2000;
constexpr fdb_error_t kNetworkAlreadySetup = 2009;
constexpr fdb_error_t kApiVersionAlreadySet = 2201;

// Hermetic stubs for the process-wide fdb_c calls (RuntimeApi holds plain function
// pointers, so state lives at file scope and is reset per test).
struct StubState {
	std::atomic<int> selectCalls{0};
	std::atomic<int> setupCalls{0};
	std::atomic<int> runCalls{0};
	std::atomic<int> stopCalls{0};
	std::atomic<fdb_error_t> selectResult{0};
	std::atomic<fdb_error_t> setupResult{0};
	std::atomic<fdb_error_t> runResult{0};
	std::atomic<fdb_error_t> stopResult{0};
	std::atomic<bool> blockRun{false};
	std::atomic<bool> runReleased{false};
	std::atomic<bool> networkStopped{false};

	void reset() {
		selectCalls = 0;
		setupCalls = 0;
		runCalls = 0;
		stopCalls = 0;
		selectResult = 0;
		setupResult = 0;
		runResult = 0;
		stopResult = 0;
		blockRun = false;
		runReleased = false;
		networkStopped = false;
	}
};

StubState gStub;

RuntimeApi stubApi() {
	return {
	    [](int /*version*/) {
		    gStub.selectCalls++;
		    return gStub.selectResult.load();
	    },
	    []() {
		    gStub.setupCalls++;
		    return gStub.setupResult.load();
	    },
	    []() -> fdb_error_t {
		    gStub.runCalls++;
		    // A synchronous rejection returns at once; a healthy loop blocks until the
		    // network is stopped, like the real fdb_run_network (blockRun forces blocking
		    // even for a scripted nonzero result, for the post-startup-death tests).
		    if (gStub.runResult != 0 && !gStub.blockRun) { return gStub.runResult.load(); }
		    while (!gStub.networkStopped && !gStub.runReleased) {
			    std::this_thread::sleep_for(std::chrono::milliseconds(1));
		    }
		    return gStub.runResult.load();
	    },
	    []() {
		    gStub.stopCalls++;
		    const fdb_error_t result = gStub.stopResult.load();
		    if (result == 0) { gStub.networkStopped = true; }  // a successful stop ends run_network
		    return result;
	    },
	};
}

class FDBRuntimeCoreTest : public ::testing::Test {
protected:
	void SetUp() override { gStub.reset(); }
};

}  // namespace

TEST_F(FDBRuntimeCoreTest, StartIsIdempotentAndShutdownStopsOnce) {
	RuntimeCore core(stubApi());
	EXPECT_EQ(core.state(), State::kNotStarted);

	core.ensureStarted(kApiVersion);
	core.ensureStarted(kApiVersion);
	EXPECT_EQ(core.state(), State::kRunning);
	EXPECT_EQ(gStub.selectCalls, 1);
	EXPECT_EQ(gStub.setupCalls, 1);

	core.shutdown();
	core.shutdown();
	EXPECT_EQ(core.state(), State::kStopped);
	EXPECT_EQ(gStub.stopCalls, 1);
	EXPECT_EQ(gStub.runCalls, 1);
}

TEST_F(FDBRuntimeCoreTest, SetupFailureThrowsAndAllowsRetry) {
	RuntimeCore core(stubApi());
	gStub.setupResult = kClientInvalidOperation;  // arbitrary failure code

	EXPECT_THROW(core.ensureStarted(kApiVersion), fdb::FDBException);
	EXPECT_EQ(core.state(), State::kNotStarted);

	gStub.setupResult = 0;
	// The retry re-selects the API version; 2201 (already set) must not fail it.
	gStub.selectResult = kApiVersionAlreadySet;
	core.ensureStarted(kApiVersion);
	EXPECT_EQ(core.state(), State::kRunning);
	core.shutdown();
}

TEST_F(FDBRuntimeCoreTest, NetworkAlreadySetupIsTolerated) {
	RuntimeCore core(stubApi());
	// Simulates a retry after a first attempt that set up the network but failed to
	// launch the network thread: setup reports 2009 (network_already_setup), which
	// must not fail the retry.
	gStub.selectResult = kApiVersionAlreadySet;
	gStub.setupResult = kNetworkAlreadySetup;
	core.ensureStarted(kApiVersion);
	EXPECT_EQ(core.state(), State::kRunning);
	core.shutdown();
}

TEST_F(FDBRuntimeCoreTest, ImmediateNetworkFailureFailsFirstStart) {
	RuntimeCore core(stubApi());
	// run_network rejects at once (blockRun stays false): the startup handshake must let
	// the FIRST start observe it instead of reporting a healthy runtime. This is the
	// single-client case (the master creates exactly one FDBContext), where no later
	// ensureStarted() would run to surface the failure.
	gStub.runResult = kClientInvalidOperation;

	try {
		core.ensureStarted(kApiVersion);
		FAIL() << "The first start must surface an immediate run_network failure.";
	} catch (const fdb::FDBException &e) { EXPECT_EQ(e.errorCode(), kClientInvalidOperation); }

	// kRunning (network set up, thread launched) but doomed, so every start refuses.
	EXPECT_EQ(core.state(), State::kRunning);
	EXPECT_THROW(core.ensureStarted(kApiVersion), fdb::FDBException);

	core.shutdown();
	EXPECT_EQ(core.state(), State::kStopped);
}

TEST_F(FDBRuntimeCoreTest, ImmediateCleanNetworkExitFailsFirstStart) {
	RuntimeCore core(stubApi());
	// The network is stopped out-of-band so run_network returns SUCCESS inside the startup
	// window. A healthy loop never returns, so this clean return must fail the first start
	// (not pass as healthy) and every later start must keep refusing it.
	gStub.networkStopped = true;
	gStub.runResult = 0;

	EXPECT_THROW(core.ensureStarted(kApiVersion), fdb::FDBException);
	EXPECT_EQ(core.state(), State::kRunning);
	EXPECT_THROW(core.ensureStarted(kApiVersion), fdb::FDBException);

	core.shutdown();
	EXPECT_EQ(core.state(), State::kStopped);
}

TEST_F(FDBRuntimeCoreTest, PostStartupNetworkDeathAbortsProcess) {
	// A run_network return AFTER startup committed is an unobservable post-startup death:
	// the single-client master would hang every future forever, so the network thread
	// fail-stops. run_network blocks past the grace (startup commits healthy), then is
	// released to return an error. The empty matcher asserts the process dies without
	// depending on the test logger reaching stderr.
	EXPECT_DEATH(
	    {
		    gStub.reset();
		    gStub.blockRun = true;
		    gStub.runResult = kClientInvalidOperation;
		    RuntimeCore core(stubApi());
		    core.ensureStarted(kApiVersion);
		    gStub.runReleased = true;
		    std::this_thread::sleep_for(std::chrono::seconds(5));
	    },
	    "");
}

TEST_F(FDBRuntimeCoreTest, PostStartupCleanNetworkExitAbortsProcess) {
	// Zero-exit twin of the above: run_network returns SUCCESS after startup because the
	// network was stopped out-of-band (not via shutdown()). The event loop is gone all the
	// same, so an unrequested clean return must fail-stop, not silently report a healthy
	// runtime that later FDB calls would use after stop.
	EXPECT_DEATH(
	    {
		    gStub.reset();
		    gStub.blockRun = true;
		    gStub.runResult = 0;  // clean return once released, with no shutdown() requested
		    RuntimeCore core(stubApi());
		    core.ensureStarted(kApiVersion);
		    gStub.runReleased = true;
		    std::this_thread::sleep_for(std::chrono::seconds(5));
	    },
	    "");
}

TEST_F(FDBRuntimeCoreTest, RestartAfterShutdownThrows) {
	RuntimeCore core(stubApi());
	core.ensureStarted(kApiVersion);
	core.shutdown();

	EXPECT_THROW(core.ensureStarted(kApiVersion), std::logic_error);
	EXPECT_EQ(core.state(), State::kStopped);
	EXPECT_EQ(gStub.setupCalls, 1);
}

TEST_F(FDBRuntimeCoreTest, ShutdownWithoutStartIsTerminal) {
	RuntimeCore core(stubApi());
	core.shutdown();
	EXPECT_EQ(core.state(), State::kStopped);
	EXPECT_EQ(gStub.stopCalls, 0) << "Nothing was running, nothing to stop.";
	EXPECT_THROW(core.ensureStarted(kApiVersion), std::logic_error);
}

TEST_F(FDBRuntimeCoreTest, StopFailureDetachesInsteadOfHanging) {
	gStub.blockRun = true;
	gStub.stopResult = 1;  // stop_network failure: the network thread never exits

	auto core = std::make_unique<RuntimeCore>(stubApi());
	core->ensureStarted(kApiVersion);

	// Must return promptly (detach) although the network thread is still blocked; a
	// join here would hang forever.
	core->shutdown();
	EXPECT_EQ(core->state(), State::kStopped);
	core.reset();

	// Unblock and give the detached thread time to exit before the test ends.
	gStub.runReleased = true;
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
}
