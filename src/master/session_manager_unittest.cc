/*
   Copyright 2026      Leil Storage OÜ

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

#include "master/session_manager.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "master/session.h"

namespace {

/// Fixed timeout window applied to legacy pre-1.5.13 clients (newSession==0).
/// Must stay in sync with the literal in SessionManagerBase::isTimedOut().
constexpr uint32_t kLegacyClientTimeoutSeconds = 7200;

/// Concrete SessionManagerBase used by these tests.
///
/// Stubs the pure-virtual persistence hooks so the class is instantiable, and
/// records onSessionRemoved() invocations so tests can assert on reaping hook
/// behavior without touching any backend.
class TestSessionManager : public SessionManagerBase {
public:
	int initialize() override { return 0; }
	int loadSessions() override { return 0; }
	void storeSessions() override { ++storeSessionsCallCount; }

	using SessionManagerBase::findSessionEntry;

	int storeSessionsCallCount = 0;
	std::vector<uint32_t> removedSessionIds;
	std::vector<std::string> hookEvents;

protected:
	void onSessionRemoved(const Session &session) override {
		removedSessionIds.push_back(session.sessionId);
		hookEvents.push_back("removed:" + std::to_string(session.sessionId));
	}
};

std::unique_ptr<Session> makeSession(uint32_t sessionId) {
	auto session = std::make_unique<Session>();
	session->sessionId = sessionId;
	session->newSession = 1;
	session->connections = 1;
	session->disconnectedTimestamp = 0;
	return session;
}

}  // namespace

// ---------------------------------------------------------------------------
// findSession()
// ---------------------------------------------------------------------------

TEST(SessionManagerBaseTests, FindSessionReturnsNullForUnknownId) {
	TestSessionManager manager;
	manager.addSession(makeSession(100));
	EXPECT_EQ(manager.findSession(999), nullptr);
}

TEST(SessionManagerBaseTests, FindSessionRejectsIdZero) {
	TestSessionManager manager;
	manager.addSession(makeSession(0));
	EXPECT_EQ(manager.findSession(0), nullptr);
}

TEST(SessionManagerBaseTests, FindSessionRevivesDisconnectedSession) {
	TestSessionManager manager;
	auto session = makeSession(42);
	session->connections = 0;
	session->disconnectedTimestamp = 12345;
	const Session *stored = manager.addSession(std::move(session));

	Session *found = manager.findSession(42);

	ASSERT_EQ(found, stored);
	EXPECT_EQ(found->connections, 1U);
	EXPECT_EQ(found->disconnectedTimestamp, 0U);
}

TEST(SessionManagerBaseTests, FindSessionClearsPendingCloseMarker) {
	TestSessionManager manager;
	auto session = makeSession(42);
	// newSession == 3 means "regular session (1)" with the pending-close marker
	// (bit 1) set by a prior closeSession() call.
	session->newSession = 3;
	session->connections = 0;
	manager.addSession(std::move(session));

	Session *found = manager.findSession(42);

	ASSERT_NE(found, nullptr);
	EXPECT_EQ(found->newSession, 1U);
	EXPECT_EQ(found->connections, 1U);
}

TEST(SessionManagerBaseTests, FindSessionLeavesRegularNewSessionUntouched) {
	TestSessionManager manager;
	auto session = makeSession(42);
	session->newSession = 1;
	session->connections = 1;
	manager.addSession(std::move(session));

	Session *found = manager.findSession(42);

	ASSERT_NE(found, nullptr);
	EXPECT_EQ(found->newSession, 1U);
	EXPECT_EQ(found->connections, 2U);
}

// ---------------------------------------------------------------------------
// closeSession()
// ---------------------------------------------------------------------------

TEST(SessionManagerBaseTests, CloseSessionSetsMarkerOnLastConnection) {
	TestSessionManager manager;
	auto session = makeSession(42);
	session->newSession = 1;
	session->connections = 1;
	manager.addSession(std::move(session));

	manager.closeSession(42);

	ASSERT_NE(manager.findSessionEntry(42), nullptr);
	EXPECT_EQ(manager.findSessionEntry(42)->newSession, 3U);
}

TEST(SessionManagerBaseTests, CloseSessionIsNoOpWithMultipleConnections) {
	TestSessionManager manager;
	auto session = makeSession(42);
	session->newSession = 1;
	session->connections = 2;
	manager.addSession(std::move(session));

	manager.closeSession(42);

	EXPECT_EQ(manager.findSessionEntry(42)->newSession, 1U);
}

TEST(SessionManagerBaseTests, CloseSessionDoesNotReapplyMarker) {
	TestSessionManager manager;
	auto session = makeSession(42);
	session->newSession = 3;  // already marked
	session->connections = 1;
	manager.addSession(std::move(session));

	manager.closeSession(42);

	EXPECT_EQ(manager.findSessionEntry(42)->newSession, 3U);
}

TEST(SessionManagerBaseTests, CloseSessionOnUnknownIdIsNoOp) {
	TestSessionManager manager;
	manager.addSession(makeSession(42));

	manager.closeSession(999);

	ASSERT_NE(manager.findSessionEntry(42), nullptr);
	EXPECT_EQ(manager.findSessionEntry(42)->newSession, 1U);
}

// ---------------------------------------------------------------------------
// removeTimedOutSessions() — tri-level threshold + hook ordering.
//
// isTimedOut():
//   connections != 0                                    => not timed out
//   newSession > 1 && dts < now                         => timed out (pending close)
//   newSession == 1 && dts + sustain < now              => timed out (regular)
//   newSession == 0 && dts + 7200 < now                 => timed out (legacy)
// ---------------------------------------------------------------------------

TEST(SessionManagerBaseTests, RemoveTimedOutSkipsActiveSessions) {
	TestSessionManager manager;
	auto session = makeSession(1);
	session->connections = 1;
	session->disconnectedTimestamp = 0;
	manager.addSession(std::move(session));

	std::vector<uint32_t> timedOut;
	manager.removeTimedOutSessions(1'000'000, 60, [&](Session *timedOutSession) {
		timedOut.push_back(timedOutSession->sessionId);
		return true;
	});

	EXPECT_TRUE(timedOut.empty());
	EXPECT_TRUE(manager.removedSessionIds.empty());
	EXPECT_NE(manager.findSessionEntry(1), nullptr);
}

TEST(SessionManagerBaseTests, RemoveTimedOutReapsPendingCloseSession) {
	TestSessionManager manager;
	auto session = makeSession(1);
	session->newSession = 3;  // pending close
	session->connections = 0;
	session->disconnectedTimestamp = 100;
	manager.addSession(std::move(session));

	std::vector<uint32_t> timedOut;
	manager.removeTimedOutSessions(101, 100'000, [&](Session *timedOutSession) {
		timedOut.push_back(timedOutSession->sessionId);
		return true;
	});

	EXPECT_EQ(timedOut, std::vector<uint32_t>{1});
	EXPECT_EQ(manager.removedSessionIds, std::vector<uint32_t>{1});
	EXPECT_EQ(manager.findSessionEntry(1), nullptr);
}

TEST(SessionManagerBaseTests, RemoveTimedOutHonorsStrictLessThanSustainBoundary) {
	TestSessionManager manager;
	auto session = makeSession(1);
	session->newSession = 1;
	session->connections = 0;
	session->disconnectedTimestamp = 100;
	manager.addSession(std::move(session));

	std::vector<uint32_t> timedOut;

	// isTimedOut for newSession==1 uses dts + sustain < now (strict <).
	// now == dts + sustain => NOT timed out.
	manager.removeTimedOutSessions(150, 50, [&](Session *timedOutSession) {
		timedOut.push_back(timedOutSession->sessionId);
		return true;
	});
	EXPECT_TRUE(timedOut.empty());
	EXPECT_NE(manager.findSessionEntry(1), nullptr);

	// now == dts + sustain + 1 => timed out.
	manager.removeTimedOutSessions(151, 50, [&](Session *timedOutSession) {
		timedOut.push_back(timedOutSession->sessionId);
		return true;
	});
	EXPECT_EQ(timedOut, std::vector<uint32_t>{1});
	EXPECT_EQ(manager.findSessionEntry(1), nullptr);
}

TEST(SessionManagerBaseTests, RemoveTimedOutUsesLegacyThresholdForOldClients) {
	constexpr uint32_t kDisconnectedAt = 100;
	constexpr uint32_t kIgnoredSustain = 50;

	TestSessionManager manager;
	auto session = makeSession(1);
	session->newSession = 0;  // legacy pre-1.5.13 client
	session->connections = 0;
	session->disconnectedTimestamp = kDisconnectedAt;
	manager.addSession(std::move(session));

	std::vector<uint32_t> timedOut;
	auto record = [&](Session *timedOutSession) {
		timedOut.push_back(timedOutSession->sessionId);
		return true;
	};

	// Legacy path uses kLegacyClientTimeoutSeconds and ignores sessionSustainTime.
	// dts + kLegacyClientTimeoutSeconds == now => NOT timed out.
	manager.removeTimedOutSessions(kDisconnectedAt + kLegacyClientTimeoutSeconds, kIgnoredSustain,
	                               record);
	EXPECT_TRUE(timedOut.empty());
	EXPECT_NE(manager.findSessionEntry(1), nullptr);

	// dts + kLegacyClientTimeoutSeconds < now => timed out.
	manager.removeTimedOutSessions(kDisconnectedAt + kLegacyClientTimeoutSeconds + 1,
	                               kIgnoredSustain, record);
	EXPECT_EQ(timedOut, std::vector<uint32_t>{1});
	EXPECT_EQ(manager.findSessionEntry(1), nullptr);
}

TEST(SessionManagerBaseTests, RemoveTimedOutInvokesOnTimedOutBeforeOnSessionRemoved) {
	TestSessionManager manager;
	auto session = makeSession(1);
	session->newSession = 1;
	session->connections = 0;
	session->disconnectedTimestamp = 100;
	manager.addSession(std::move(session));

	manager.removeTimedOutSessions(200, 50, [&](Session *timedOutSession) {
		manager.hookEvents.push_back("timedOut:" + std::to_string(timedOutSession->sessionId));
		return true;
	});

	ASSERT_EQ(manager.hookEvents.size(), 2U);
	EXPECT_EQ(manager.hookEvents[0], "timedOut:1");
	EXPECT_EQ(manager.hookEvents[1], "removed:1");
	EXPECT_EQ(manager.findSessionEntry(1), nullptr);
}

TEST(SessionManagerBaseTests, RemoveTimedOutProcessesMixedSetCorrectly) {
	TestSessionManager manager;

	auto active = makeSession(1);  // active — skip
	active->connections = 1;
	manager.addSession(std::move(active));

	auto fresh = makeSession(2);  // disconnected within sustain — skip
	fresh->connections = 0;
	fresh->disconnectedTimestamp = 150;
	manager.addSession(std::move(fresh));

	auto stale = makeSession(3);  // disconnected past sustain — reap
	stale->connections = 0;
	stale->disconnectedTimestamp = 10;
	manager.addSession(std::move(stale));

	std::vector<uint32_t> timedOut;
	manager.removeTimedOutSessions(200, 100, [&](Session *timedOutSession) {
		timedOut.push_back(timedOutSession->sessionId);
		return true;
	});

	EXPECT_EQ(timedOut, std::vector<uint32_t>{3});
	EXPECT_EQ(manager.removedSessionIds, std::vector<uint32_t>{3});
	EXPECT_NE(manager.findSessionEntry(1), nullptr);
	EXPECT_NE(manager.findSessionEntry(2), nullptr);
	EXPECT_EQ(manager.findSessionEntry(3), nullptr);
}

TEST(SessionManagerBaseTests, RemoveTimedOutKeepsSessionWhenTeardownFails) {
	TestSessionManager manager;
	auto session = makeSession(1);
	session->newSession = 1;
	session->connections = 0;
	session->disconnectedTimestamp = 100;
	manager.addSession(std::move(session));

	std::vector<uint32_t> timedOut;
	manager.removeTimedOutSessions(200, 50, [&](Session *timedOutSession) {
		timedOut.push_back(timedOutSession->sessionId);
		return false;
	});

	EXPECT_EQ(timedOut, std::vector<uint32_t>{1});
	EXPECT_TRUE(manager.removedSessionIds.empty());
	EXPECT_NE(manager.findSessionEntry(1), nullptr);
}

TEST(SessionManagerBaseTests, RemoveTimedOutKeepsOnlyFailedTeardownSessions) {
	TestSessionManager manager;
	auto failed = makeSession(1);
	failed->newSession = 1;
	failed->connections = 0;
	failed->disconnectedTimestamp = 100;
	manager.addSession(std::move(failed));

	auto completed = makeSession(2);
	completed->newSession = 1;
	completed->connections = 0;
	completed->disconnectedTimestamp = 100;
	manager.addSession(std::move(completed));

	std::vector<uint32_t> timedOut;
	manager.removeTimedOutSessions(200, 50, [&](Session *timedOutSession) {
		timedOut.push_back(timedOutSession->sessionId);
		return timedOutSession->sessionId != 1;
	});

	EXPECT_EQ(timedOut, (std::vector<uint32_t>{1, 2}));
	EXPECT_EQ(manager.removedSessionIds, std::vector<uint32_t>{2});
	EXPECT_NE(manager.findSessionEntry(1), nullptr);
	EXPECT_EQ(manager.findSessionEntry(2), nullptr);
}

// ---------------------------------------------------------------------------
// rotateStats()
// ---------------------------------------------------------------------------

TEST(SessionManagerBaseTests, RotateStatsMovesCurrentToPrevAndZeroesCurrent) {
	TestSessionManager manager;
	auto session = makeSession(1);
	session->currHourOperationsStats.fill(0);
	session->currHourOperationsStats[0] = 7;
	session->currHourOperationsStats[5] = 42;
	session->prevHourOperationsStats.fill(99);
	manager.addSession(std::move(session));

	manager.rotateStats();

	const Session *stored = manager.findSessionEntry(1);
	ASSERT_NE(stored, nullptr);
	EXPECT_EQ(stored->prevHourOperationsStats[0], 7U);
	EXPECT_EQ(stored->prevHourOperationsStats[5], 42U);
	EXPECT_EQ(stored->currHourOperationsStats[0], 0U);
	EXPECT_EQ(stored->currHourOperationsStats[5], 0U);
	EXPECT_EQ(manager.storeSessionsCallCount, 1);
}

TEST(SessionManagerBaseTests, PersistSessionDefaultsToStoreSessions) {
	TestSessionManager manager;
	const Session *stored = manager.addSession(makeSession(1));

	manager.persistSession(*stored);

	EXPECT_EQ(manager.storeSessionsCallCount, 1);
}

// ---------------------------------------------------------------------------
// removeOpenFile()
// ---------------------------------------------------------------------------

TEST(SessionManagerBaseTests, RemoveOpenFileErasesInodeFromSet) {
	TestSessionManager manager;
	auto session = makeSession(1);
	session->openFilesSet.insert(100);
	session->openFilesSet.insert(200);
	manager.addSession(std::move(session));

	manager.removeOpenFile(1, 100);

	const Session *stored = manager.findSessionEntry(1);
	ASSERT_NE(stored, nullptr);
	EXPECT_FALSE(stored->openFilesSet.contains(100));
	EXPECT_TRUE(stored->openFilesSet.contains(200));
}

TEST(SessionManagerBaseTests, RemoveOpenFileIsNoOpForUnknownInode) {
	TestSessionManager manager;
	auto session = makeSession(1);
	session->openFilesSet.insert(100);
	manager.addSession(std::move(session));

	manager.removeOpenFile(1, 999);

	EXPECT_TRUE(manager.findSessionEntry(1)->openFilesSet.contains(100));
}

TEST(SessionManagerBaseTests, RemoveOpenFileOnUnknownSessionIsNoOp) {
	TestSessionManager manager;
	auto session = makeSession(1);
	session->openFilesSet.insert(100);
	manager.addSession(std::move(session));

	manager.removeOpenFile(999, 100);

	const Session *stored = manager.findSessionEntry(1);
	ASSERT_NE(stored, nullptr);
	EXPECT_TRUE(stored->openFilesSet.contains(100));
}

// ---------------------------------------------------------------------------
// unload() / forEachSession()
// ---------------------------------------------------------------------------

TEST(SessionManagerBaseTests, UnloadClearsAllSessions) {
	TestSessionManager manager;
	auto session = makeSession(1);
	session->openFilesSet.insert(100);
	manager.addSession(std::move(session));
	manager.addSession(makeSession(2));

	manager.unload();

	EXPECT_EQ(manager.findSessionEntry(1), nullptr);
	EXPECT_EQ(manager.findSessionEntry(2), nullptr);
}

TEST(SessionManagerBaseTests, ForEachSessionVisitsInInsertionOrder) {
	TestSessionManager manager;
	manager.addSession(makeSession(10));
	manager.addSession(makeSession(20));
	manager.addSession(makeSession(30));

	std::vector<uint32_t> visited;
	manager.forEachSession([&](const Session &session) { visited.push_back(session.sessionId); });

	EXPECT_EQ(visited, (std::vector<uint32_t>{10, 20, 30}));
}

TEST(SessionManagerBaseTests, ListSessionsReturnsActiveSessionSummaries) {
	TestSessionManager manager;
	auto disconnected = makeSession(10);
	disconnected->connections = 0;
	disconnected->openFilesSet.insert(100);
	manager.addSession(std::move(disconnected));

	auto active = makeSession(20);
	active->peerIpAddress = 0x7F000001;
	active->openFilesSet.insert(200);
	active->openFilesSet.insert(300);
	manager.addSession(std::move(active));

	auto summaries = manager.listSessions();

	ASSERT_EQ(summaries.size(), 1U);
	EXPECT_EQ(summaries[0].sessionId, 20U);
	EXPECT_EQ(summaries[0].peerIp, 0x7F000001U);
	EXPECT_EQ(summaries[0].filesNumber, 2U);
}
