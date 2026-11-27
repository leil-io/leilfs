/*
   Copyright 2026      Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/platform.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "common/event_loop.h"
#include "kv/ifuture.h"
#include "master/filesystem_node.h"
#include "master/filesystem_operations.h"
#include "master/matoclserv.h"
#include "master/matoclserv_sessions.h"
#include "master/session_manager.h"
#include "protocol/cltoma.h"
#include "protocol/packet.h"
#include "unittests/TemporaryDirectory.h"

void matocl_session_check();
void matocl_session_release_a_bit();
void matoclserv_session_delete(matoclserventry *eptr, const uint8_t *data, uint32_t length);

namespace {

class TestSessionManager : public SessionManagerBase {
public:
	int initialize() override { return 0; }
	int loadSessions() override { return 0; }
	void storeSessions() override {}
};

/// Every inode exists and has no locks, so session teardown only releases files.
class OpenFileNodeOperations : public FilesystemNodeOperationsBase {
public:
	OpenFileNodeOperations() : node_(FSNode::create(FSNodeType::kFile)) {}
	~OpenFileNodeOperations() override { FSNode::destroy(node_); }

	FSNode *idToNodeInternal(const FilesystemOperationContext &, inode_t) const override {
		return node_;
	}

private:
	FSNode *node_;
};

class ReleaseRecordingOperations : public FilesystemOperationsBase {
public:
	ReleaseRecordingOperations()
	    : FilesystemOperationsBase(std::make_unique<OpenFileNodeOperations>()) {}

	void changeLog(const FilesystemOperationContext &, uint32_t, const char *format, ...) override {
		EXPECT_STREQ(format, "CLRLCK(%" PRIu8 ",0,%" PRIu32 ")");
		retirementOffsets.push_back(releases.size());
	}

	/// Number of file releases already observed at each retirement record.
	std::vector<size_t> retirementOffsets;

	int locksListInode(const FsContext &, const FilesystemOperationContext &, uint8_t, bool,
	                   inode_t inode, uint64_t, uint64_t,
	                   std::vector<safs_locks::Info> &locks) override {
		locks.clear();
		return unreadableLocks.contains(inode) ? SAUNAFS_ERROR_IO : SAUNAFS_STATUS_OK;
	}

	uint8_t release(const FsContext &, const FilesystemOperationContext &, inode_t inode,
	                uint32_t sessionId) override {
		if (releaseDelay.count() != 0) { std::this_thread::sleep_for(releaseDelay); }
		if (lockedFiles.contains(inode)) { return SAUNAFS_ERROR_LOCKED; }
		releases.emplace_back(inode, sessionId);
		return SAUNAFS_STATUS_OK;
	}

	std::vector<std::pair<inode_t, uint32_t>> releases;
	std::set<inode_t> unreadableLocks;
	/// Files whose release the backend refuses, as for a reserved file whose chunks are locked.
	std::set<inode_t> lockedFiles;
	std::chrono::milliseconds releaseDelay{};
};

class ReleaseTransaction : public kv::IReadWriteTransaction {
public:
	explicit ReleaseTransaction(bool &failCommit) : failCommit_(failCommit) {}
	std::optional<kv::Value> get(const kv::Key &) override { return std::nullopt; }
	std::optional<kv::Value> getSnapshot(const kv::Key &) override { return std::nullopt; }
	std::unique_ptr<kv::IFuture> getAsync(const kv::Key &) override { return nullptr; }
	std::unique_ptr<kv::IFuture> getSnapshotAsync(const kv::Key &) override { return nullptr; }
	kv::GetRangeResult getRange(const kv::KeySelector &, const kv::KeySelector &, int) override {
		return {{}, false};
	}
	std::unique_ptr<kv::IRangeFuture> getRangeAsync(const kv::KeySelector &,
	                                                const kv::KeySelector &, int) override {
		return nullptr;
	}
	void set(const kv::Key &, const kv::Value &) override {}
	void atomicAdd(const kv::Key &, const kv::Value &) override {}
	void atomicMax(const kv::Key &, const kv::Value &) override {}
	void remove(const kv::Key &) override {}
	void removeRange(const kv::Key &, const kv::Key &) override {}
	void addReadConflictKey(const kv::Key &) override {}
	bool commit() override { return !std::exchange(failCommit_, false); }
	std::unique_ptr<kv::ICommitFuture> commitAsync() override { return nullptr; }
	std::optional<int64_t> getCommittedVersion() const override { return std::nullopt; }
	uint64_t mutationCount() const override { return 0; }
	std::unique_ptr<kv::IVoidFuture> recoverAsync(int) override { return nullptr; }

private:
	bool &failCommit_;
};

class TransactionalReleaseOperations : public ReleaseRecordingOperations {
public:
	FilesystemOperationContext createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType) override {
		return FilesystemOperationContext(
		    std::unique_ptr<kv::IReadWriteTransaction>(new ReleaseTransaction(failNextCommit)));
	}
	bool failNextCommit = false;
};

class MatoclSessionReleaseTest : public ::testing::Test {
protected:
	void SetUp() override {
		eventloop_updatetime();
		previousSustainTime_ = gSessionSustainTime;
		gSessionSustainTime = 60;
		previousFilesystem_ = std::move(gFSOperations);
		auto filesystem = std::make_unique<ReleaseRecordingOperations>();
		filesystem_ = filesystem.get();
		gFSOperations = std::move(filesystem);
		auto manager = std::make_unique<TestSessionManager>();
		manager_ = manager.get();
		matoclserv_set_session_manager(std::move(manager));
		// Drop any release still pending from another test; this manager has no sessions.
		matocl_session_release_a_bit();
	}

	void TearDown() override {
		for (auto *connection : connections_) { matoclserv_test_disconnect(connection); }
		matoclserv_set_session_manager(std::make_unique<TestSessionManager>());
		gFSOperations = std::move(previousFilesystem_);
		gSessionSustainTime = previousSustainTime_;
	}

	/// A session holding files 2..fileCount+1 open, with its client connected.
	Session *addConnectedSession(uint32_t sessionId, size_t fileCount) {
		auto session = std::make_unique<Session>();
		session->sessionId = sessionId;
		session->peerIpAddress = 0x7F000001;
		session->newSession = 1;
		session->connections = 1;
		for (size_t index = 0; index < fileCount; ++index) {
			session->openFilesSet.insert(static_cast<inode_t>(index + 2));
		}
		return manager_->addSession(std::move(session));
	}

	/// A session whose client disconnected long ago, holding files 2..fileCount+1 open.
	Session *addExpiredSession(uint32_t sessionId, size_t fileCount) {
		auto *session = addConnectedSession(sessionId, fileCount);
		session->connections = 0;
		session->disconnectedTimestamp = 1;
		return session;
	}

	matoclserventry *connect(Session *session) {
		connections_.push_back(matoclserv_test_connection(session));
		return connections_.back();
	}

	void disconnect(matoclserventry *connection) {
		std::erase(connections_, connection);
		matoclserv_test_disconnect(connection);
	}

	void disconnectKilled() {
		const auto connections = connections_;
		for (auto *connection : connections) {
			if (matoclserv_test_connection_is_killed(connection)) { disconnect(connection); }
		}
	}

	SessionManagerFile *useFileManager() {
		auto manager = std::make_unique<SessionManagerFile>();
		auto *fileManager = manager.get();
		matoclserv_set_session_manager(std::move(manager));
		manager_ = fileManager;
		return fileManager;
	}

	TransactionalReleaseOperations *useTransactions() {
		auto filesystem = std::make_unique<TransactionalReleaseOperations>();
		auto *transactional = filesystem.get();
		filesystem_ = transactional;
		gFSOperations = std::move(filesystem);
		return transactional;
	}

	void deleteSession(uint32_t sessionId) {
		const auto request = cltoma::deleteSession::build(sessionId);
		matoclserv_session_delete(connect(nullptr), request.data() + PacketHeader::kSize,
		                          request.size() - PacketHeader::kSize);
	}

	/// Invokes continuations until one records no releases.
	/// Checks each recorded release count against the file budget.
	size_t runReleaseSteps() {
		size_t steps = 0;
		for (size_t before = filesystem_->releases.size();; ++steps) {
			matocl_session_release_a_bit();
			const size_t released = filesystem_->releases.size() - before;
			EXPECT_LE(released, kSessionReleaseBatchSize);
			if (released == 0) { return steps; }
			before += released;
		}
	}

	size_t managedSessions() const {
		size_t count = 0;
		matoclserv_for_each_session([&count](const Session &) { ++count; });
		return count;
	}

	/// The session a client reconnecting to @p sessionId gets, or nullptr when it is gone.
	static Session *reconnect(uint32_t sessionId) {
		const auto found = matoclserv_find_session(sessionId);
		if (found) { return *found; }
		EXPECT_EQ(found.error(), SAUNAFS_ERROR_BADSESSIONID);
		return nullptr;
	}

	/// Releases of files 2..fileCount+1 by one session, in the order a pass makes them.
	static std::vector<std::pair<inode_t, uint32_t>> releasesOf(uint32_t sessionId,
	                                                            size_t fileCount) {
		std::vector<std::pair<inode_t, uint32_t>> releases;
		for (size_t index = 0; index < fileCount; ++index) {
			releases.emplace_back(static_cast<inode_t>(index + 2), sessionId);
		}
		return releases;
	}

	ReleaseRecordingOperations *filesystem_ = nullptr;
	SessionManagerBase *manager_ = nullptr;
	std::vector<matoclserventry *> connections_;
	std::unique_ptr<IFilesystemOperations> previousFilesystem_;
	uint32_t previousSustainTime_ = 0;
};

}  // namespace

TEST_F(MatoclSessionReleaseTest, ExpiredSessionReleasesOneBatchPerLoopStep) {
	constexpr size_t kOpenFiles = 3 * kSessionReleaseBatchSize + 1;
	addExpiredSession(7, kOpenFiles);

	matocl_session_check();

	// Expiry starts the pass; its files are released by bounded continuations.
	EXPECT_LE(filesystem_->releases.size(), kSessionReleaseBatchSize);
	EXPECT_EQ(managedSessions(), 1U);
	// A session whose files are being released is gone for its client.
	EXPECT_EQ(reconnect(7), nullptr);

	matocl_session_release_a_bit();
	EXPECT_LE(filesystem_->releases.size(), kSessionReleaseBatchSize);
}

TEST_F(MatoclSessionReleaseTest, ExpiredSessionReleasesEveryFileOnceOverLoopSteps) {
	constexpr size_t kOpenFiles = 3 * kSessionReleaseBatchSize + 1;
	addExpiredSession(7, kOpenFiles);

	matocl_session_check();
	EXPECT_EQ(runReleaseSteps(), 4U);

	EXPECT_EQ(filesystem_->retirementOffsets, std::vector<size_t>{0});
	// Batching preserves the inode/session release sequence recorded by this fake backend.
	EXPECT_EQ(filesystem_->releases, releasesOf(7, kOpenFiles));
	EXPECT_EQ(managedSessions(), 0U);
}

TEST_F(MatoclSessionReleaseTest, ExpiringSessionsShareTheBatchOfALoopStep) {
	constexpr size_t kOpenFiles = kSessionReleaseBatchSize / 2 + 100;
	addExpiredSession(7, kOpenFiles);
	addExpiredSession(8, kOpenFiles);

	matocl_session_check();
	matocl_session_release_a_bit();
	EXPECT_EQ(filesystem_->releases.size(), kSessionReleaseBatchSize);
	EXPECT_EQ(reconnect(8), nullptr);

	EXPECT_EQ(runReleaseSteps(), 1U);
	EXPECT_EQ(filesystem_->releases.size(), 2 * kOpenFiles);
	EXPECT_EQ(managedSessions(), 0U);
}

TEST_F(MatoclSessionReleaseTest, DeletedSessionWithOneBatchRefusesReconnects) {
	auto *session = addConnectedSession(7, kSessionReleaseBatchSize);
	auto *connection = connect(session);

	deleteSession(7);

	// This fast fixture completes its batch before replying; deletion always blocks reconnects.
	EXPECT_EQ(filesystem_->releases, releasesOf(7, kSessionReleaseBatchSize));
	EXPECT_TRUE(session->closing);
	EXPECT_TRUE(session->openFilesSet.empty());
	disconnect(connection);
	EXPECT_EQ(reconnect(7), nullptr);
}

TEST_F(MatoclSessionReleaseTest, DeletedSessionReleasesTheRestAfterItsConnectionCloses) {
	constexpr size_t kOpenFiles = 2 * kSessionReleaseBatchSize + 5;
	auto *session = addConnectedSession(7, kOpenFiles);
	auto *connection = connect(session);

	deleteSession(7);
	EXPECT_EQ(filesystem_->releases.size(), kSessionReleaseBatchSize);
	EXPECT_TRUE(session->closing);
	EXPECT_EQ(runReleaseSteps(), 0U);

	disconnect(connection);
	EXPECT_EQ(reconnect(7), nullptr);
	EXPECT_EQ(runReleaseSteps(), 2U);
	EXPECT_EQ(filesystem_->releases, releasesOf(7, kOpenFiles));
	EXPECT_EQ(managedSessions(), 0U);
}

TEST_F(MatoclSessionReleaseTest, DeletedSessionClosesEveryConnectionAndFinishesCleanup) {
	constexpr size_t kOpenFiles = 2 * kSessionReleaseBatchSize + 5;
	auto *session = addConnectedSession(7, kOpenFiles);
	auto *first = connect(session);
	++session->connections;
	auto *second = connect(session);

	deleteSession(7);
	EXPECT_TRUE(matoclserv_test_connection_is_killed(first));
	EXPECT_TRUE(matoclserv_test_connection_is_killed(second));
	EXPECT_EQ(filesystem_->releases.size(), kSessionReleaseBatchSize);
	disconnectKilled();
	EXPECT_EQ(runReleaseSteps(), 2U);
	EXPECT_EQ(filesystem_->releases, releasesOf(7, kOpenFiles));
	EXPECT_EQ(managedSessions(), 0U);
}

TEST_F(MatoclSessionReleaseTest, RestartRefusesPartiallyReleasedSessionAndResumesCleanup) {
	TemporaryDirectory temporary("/tmp", "session-release-restart");
	const auto previous = std::filesystem::current_path();
	struct RestoreDirectory {
		std::filesystem::path path;
		~RestoreDirectory() { std::filesystem::current_path(path); }
	} restore{previous};
	std::filesystem::current_path(temporary.name());
	useFileManager();
	constexpr size_t kOpenFiles = 2 * kSessionReleaseBatchSize + 5;
	auto *session = addExpiredSession(7, kOpenFiles);
	matocl_session_check();
	matocl_session_release_a_bit();
	const auto remaining = session->openFilesSet;
	ASSERT_EQ(remaining.size(), kOpenFiles - kSessionReleaseBatchSize);

	// Retirement was written before the first batch, without another storeSessions() call.
	auto *restarted = useFileManager();
	ASSERT_EQ(restarted->loadSessions(), 1);
	for (inode_t inode : remaining) { restarted->addOpenFile(7, inode); }
	EXPECT_EQ(reconnect(7), nullptr);
	matocl_session_check();
	EXPECT_EQ(runReleaseSteps(), 2U);
	EXPECT_EQ(filesystem_->releases, releasesOf(7, kOpenFiles));
	EXPECT_EQ(managedSessions(), 0U);
}

TEST_F(MatoclSessionReleaseTest, RetirementWriteFailureDefersEveryFileRelease) {
	TemporaryDirectory temporary("/tmp", "session-release-store-failure");
	const auto previous = std::filesystem::current_path();
	struct RestoreDirectory {
		std::filesystem::path path;
		~RestoreDirectory() { std::filesystem::current_path(path); }
	} restore{previous};
	std::filesystem::current_path(temporary.name());
	useFileManager();
	addExpiredSession(7, 10);
	std::filesystem::create_directory("sessions.sfs.tmp");
	matocl_session_check();
	EXPECT_EQ(runReleaseSteps(), 0U);
	EXPECT_TRUE(filesystem_->releases.empty());
	std::filesystem::remove("sessions.sfs.tmp");
	matocl_session_check();
	EXPECT_EQ(runReleaseSteps(), 1U);
	EXPECT_EQ(filesystem_->releases, releasesOf(7, 10));
}

TEST_F(MatoclSessionReleaseTest, TransactionalSessionsShareTheBudgetAndReleaseEveryFile) {
	useTransactions();
	constexpr size_t kOpenFiles = 2 * kSessionReleaseBatchSize + 5;
	addExpiredSession(7, kOpenFiles);
	addExpiredSession(8, kOpenFiles);
	matocl_session_check();
	EXPECT_TRUE(filesystem_->releases.empty());
	EXPECT_EQ(runReleaseSteps(), 5U);
	EXPECT_TRUE(filesystem_->retirementOffsets.empty());
	EXPECT_EQ(filesystem_->releases.size(), 2 * kOpenFiles);
	EXPECT_EQ(managedSessions(), 0U);
}

TEST_F(MatoclSessionReleaseTest, ExpensiveReleasesShareTheDeadlineAndResumeAtTheNextInode) {
	auto *transactional = useTransactions();
	transactional->releaseDelay = std::chrono::milliseconds(60);
	addExpiredSession(7, 2);
	addExpiredSession(8, 1);
	matocl_session_check();
	matocl_session_release_a_bit();
	EXPECT_EQ(filesystem_->releases.size(), 1U);
	EXPECT_EQ(managedSessions(), 2U);
	EXPECT_EQ(runReleaseSteps(), 2U);
	EXPECT_EQ(filesystem_->releases,
	          (std::vector<std::pair<inode_t, uint32_t>>{{2, 7}, {3, 7}, {2, 8}}));
	EXPECT_EQ(managedSessions(), 0U);
}

TEST_F(MatoclSessionReleaseTest, FailedTransactionalBatchKeepsFilesForTheNextPass) {
	auto *transactional = useTransactions();
	auto *session = addExpiredSession(7, 2 * kSessionReleaseBatchSize + 5);
	const auto openFiles = session->openFilesSet;
	matocl_session_check();
	transactional->failNextCommit = true;
	matocl_session_release_a_bit();
	EXPECT_EQ(session->openFilesSet, openFiles);
	EXPECT_EQ(runReleaseSteps(), 0U);
	matocl_session_check();
	EXPECT_EQ(runReleaseSteps(), 3U);
	EXPECT_EQ(managedSessions(), 0U);
}

TEST_F(MatoclSessionReleaseTest, FileWithUnreadableLocksWaitsForTheNextSessionCheck) {
	constexpr size_t kOpenFiles = 2 * kSessionReleaseBatchSize;
	auto *session = addExpiredSession(7, kOpenFiles);
	filesystem_->unreadableLocks.insert(2);

	// The pass goes on past the file, which stays open without a retry in every loop step.
	matocl_session_check();
	EXPECT_EQ(runReleaseSteps(), 2U);
	EXPECT_EQ(filesystem_->releases.size(), kOpenFiles - 1);
	EXPECT_EQ(session->openFilesSet, Session::OpenFilesSet{2});
	EXPECT_EQ(reconnect(7), nullptr);

	filesystem_->unreadableLocks.clear();
	matocl_session_check();
	EXPECT_EQ(runReleaseSteps(), 1U);
	EXPECT_EQ(filesystem_->releases.back(), std::make_pair(inode_t{2}, uint32_t{7}));
	EXPECT_EQ(managedSessions(), 0U);
}

TEST_F(MatoclSessionReleaseTest, FileWhoseReleaseIsRefusedWaitsForTheNextSessionCheck) {
	constexpr size_t kOpenFiles = 2 * kSessionReleaseBatchSize;
	auto *session = addExpiredSession(7, kOpenFiles);
	filesystem_->lockedFiles.insert(kOpenFiles + 1);

	matocl_session_check();
	EXPECT_EQ(runReleaseSteps(), 2U);
	EXPECT_EQ(filesystem_->releases.size(), kOpenFiles - 1);
	EXPECT_EQ(session->openFilesSet, Session::OpenFilesSet{kOpenFiles + 1});
	EXPECT_EQ(reconnect(7), nullptr);

	filesystem_->lockedFiles.clear();
	matocl_session_check();
	EXPECT_EQ(runReleaseSteps(), 1U);
	EXPECT_EQ(filesystem_->releases.back(), std::make_pair(inode_t{kOpenFiles + 1}, uint32_t{7}));
	EXPECT_EQ(managedSessions(), 0U);
}
