/*
   Copyright 2026      Leil Storage OÜ

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

#include "common/platform.h"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "errors/leil_error_codes.h"
#include "master/filesystem_checksum_updater.h"
#include "master/filesystem_metadata.h"
#include "master/filesystem_node.h"
#include "master/filesystem_operation_context.h"
#include "master/filesystem_operations.h"
#include "master/fs_context.h"
#include "master/restore.h"

namespace {

class LockTestNodes : public FilesystemNodeOperationsBase {
public:
	bool exists = true;

	uint8_t getNodeForOperation(const FsContext &, const FilesystemOperationContext &,
	                            ExpectedNodeType, uint8_t, inode_t, FSNode **nodeOut,
	                            FSNodeDirectory ** = nullptr) override {
		if (!exists) { return SAUNAFS_ERROR_ENOENT; }
		*nodeOut = &node_;
		return SAUNAFS_STATUS_OK;
	}

private:
	FSNodeFile node_{FSNodeType::kFile};
};

class LockTestOperations : public FilesystemOperationsBase {
public:
	explicit LockTestOperations(std::unique_ptr<LockTestNodes> nodes)
	    : FilesystemOperationsBase(std::move(nodes)) {}

	void changeLog(const FilesystemOperationContext &, uint32_t ts, const char *format,
	               ...) override {
		std::array<char, 512> entry{};
		va_list args;
		va_start(args, format);
		vsnprintf(entry.data(), entry.size(), format, args);
		va_end(args);
		entries.emplace_back(gMetadata->metadataVersion++,
		                     ": " + std::to_string(ts) + "|" + entry.data());
		++records;
	}

	unsigned records = 0;
	std::vector<std::pair<uint64_t, std::string>> entries;
};

class LockChangelogTest : public ::testing::Test {
protected:
	void SetUp() override {
		gMetadata = new FilesystemMetadata;
		gMetadata->metadataVersion = 1;
		ChecksumUpdater::setPeriod(std::numeric_limits<uint32_t>::max());
		previous_ = std::move(gFSOperations);
		auto nodes = std::make_unique<LockTestNodes>();
		nodes_ = nodes.get();
		auto operations = std::make_unique<LockTestOperations>(std::move(nodes));
		operations_ = operations.get();
		gFSOperations = std::move(operations);
	}

	void TearDown() override {
		restore_reset();
		gFSOperations = std::move(previous_);
		ChecksumUpdater::setPeriod(0);
		delete gMetadata;
		gMetadata = nullptr;
	}

	int lock(bool posix, const FsContext &context, uint64_t owner, uint16_t op,
	         bool nonblocking = false) {
		std::vector<FileLocks::Owner> applied;
		if (posix) {
			return operations_->posixLockOperation(context, fsOpContext_, 42, 0, 10, owner, 1, 0, 0,
			                                       op, nonblocking, applied);
		}
		return operations_->flockOperation(context, fsOpContext_, 42, owner, 1, 0, 0, op,
		                                   nonblocking, applied);
	}

	void resetMetadata() {
		delete gMetadata;
		gMetadata = new FilesystemMetadata;
		gMetadata->metadataVersion = 1;
		restore_reset();
	}

	void replay(size_t count = std::numeric_limits<size_t>::max()) {
		resetMetadata();
		for (size_t i = 0; i < operations_->entries.size() && i < count; ++i) {
			const auto &[version, entry] = operations_->entries[i];
			ASSERT_EQ(SAUNAFS_STATUS_OK, restore("lock-test", version, entry.c_str(),
			                                     RestoreRigor::kDontIgnoreAnyErrors));
		}
	}

	FilesystemOperationContext fsOpContext_;
	LockTestNodes *nodes_ = nullptr;
	LockTestOperations *operations_ = nullptr;
	std::unique_ptr<IFilesystemOperations> previous_;
};

TEST_F(LockChangelogTest, FailedOperationsDoNotCreateRecordsButWaitingRequestsDo) {
	const auto master = FsContext::getForMaster(1);
	const auto shadow = FsContext::getForRestore(1);
	for (const bool posix : {false, true}) {
		const auto before = operations_->records;
		nodes_->exists = false;
		EXPECT_EQ(SAUNAFS_ERROR_ENOENT, lock(posix, master, 1, safs_locks::kRelease));
		EXPECT_EQ(before, operations_->records);
		const auto version = gMetadata->metadataVersion;
		EXPECT_EQ(SAUNAFS_ERROR_ENOENT, lock(posix, shadow, 1, safs_locks::kRelease));
		EXPECT_EQ(version, gMetadata->metadataVersion);

		nodes_->exists = true;
		EXPECT_EQ(SAUNAFS_ERROR_EINVAL, lock(posix, master, 1, safs_locks::kInvalid));
		EXPECT_EQ(before, operations_->records);
		EXPECT_EQ(SAUNAFS_STATUS_OK, lock(posix, master, 1, safs_locks::kExclusive));
		EXPECT_EQ(SAUNAFS_ERROR_WAITING, lock(posix, master, 2, safs_locks::kExclusive));
		EXPECT_EQ(before + 2, operations_->records);
	}
}

// Known limitation, not the desired cleanup policy: LS-1307 tracks removing locks
// from swept inodes. LS-1305 only prevents their failed releases entering the journal.
// https://linear.app/leil/issue/LS-1307
TEST_F(LockChangelogTest, KnownLimitationMissingInodeReleaseRetainsLock) {
	const auto master = FsContext::getForMaster(1);
	for (const bool posix : {false, true}) {
		SCOPED_TRACE(posix ? "posix" : "flock");
		nodes_->exists = true;
		ASSERT_EQ(SAUNAFS_STATUS_OK, lock(posix, master, 7, safs_locks::kExclusive));
		auto &locks = posix ? gMetadata->posixLocks : gMetadata->flockLocks;
		ASSERT_NE(nullptr, locks.getActiveLocks(42));

		// The reserved-file sweep can remove an inode while its lock is still held.
		nodes_->exists = false;
		EXPECT_EQ(SAUNAFS_ERROR_ENOENT, lock(posix, master, 7, safs_locks::kRelease));
		const auto *active = locks.getActiveLocks(42);
		ASSERT_NE(nullptr, active);
		ASSERT_EQ(1U, active->size());
		ASSERT_EQ(1U, active->begin()->owners.size());
		EXPECT_EQ(7U, active->begin()->owners.begin()->owner);
		EXPECT_EQ(1U, active->begin()->owners.begin()->sessionid);
	}
}

TEST_F(LockChangelogTest, FailedNonblockingSharedRequestDoesNotGrantOtherWaiters) {
	const auto master = FsContext::getForMaster(1);
	ASSERT_EQ(SAUNAFS_STATUS_OK, lock(true, master, 1, safs_locks::kExclusive));

	// Seed an eligible waiter in a disjoint subrange of the failed request. The
	// nonblocking request must not grant it as an unrelated side effect.
	auto &locks = gMetadata->posixLocks;
	locks.insertPending(42, FileLocks::Lock{LockRange::Type::kShared, 10, 20, {3, 1, 9, 10}});
	const auto records = operations_->records;
	std::vector<FileLocks::Owner> applied;
	EXPECT_EQ(SAUNAFS_ERROR_WAITING,
	          operations_->posixLockOperation(master, fsOpContext_, 42, 0, 20, 2, 1, 0, 0,
	                                          safs_locks::kShared, true, applied));
	EXPECT_TRUE(applied.empty());
	EXPECT_EQ(records, operations_->records);
	ASSERT_NE(nullptr, locks.getActiveLocks(42));
	ASSERT_EQ(1U, locks.getActiveLocks(42)->size());
	EXPECT_EQ(1U, locks.getActiveLocks(42)->begin()->owner().owner);
	EXPECT_EQ(LockRange::Type::kExclusive, locks.getActiveLocks(42)->begin()->type);
	ASSERT_NE(nullptr, locks.getPendingLocks(42));
	ASSERT_EQ(1U, locks.getPendingLocks(42)->size());
	const auto &pending = locks.getPendingLocks(42)->front();
	EXPECT_EQ(3U, pending.owner().owner);
	EXPECT_EQ(10U, pending.start);
	EXPECT_EQ(20U, pending.end);
}

TEST_F(LockChangelogTest, FailedNonblockingRequestsDoNotBecomeWaitersOnReplay) {
	const auto master = FsContext::getForMaster(1);
	for (const bool posix : {false, true}) {
		for (const uint16_t requested : {safs_locks::kShared, safs_locks::kExclusive}) {
			SCOPED_TRACE(posix ? "posix" : "flock");
			SCOPED_TRACE(requested);
			resetMetadata();
			operations_->records = 0;
			operations_->entries.clear();
			ASSERT_EQ(SAUNAFS_STATUS_OK, lock(posix, master, 1, safs_locks::kExclusive));
			EXPECT_EQ(SAUNAFS_ERROR_WAITING, lock(posix, master, 2, requested, true));
			EXPECT_EQ(1U, operations_->records);
			ASSERT_EQ(SAUNAFS_STATUS_OK, lock(posix, master, 1, safs_locks::kRelease));
			std::vector<safs_locks::Info> active, pending;
			auto &locks = posix ? gMetadata->posixLocks : gMetadata->flockLocks;
			locks.copyActiveToVector(42, 0, 10, active);
			locks.copyPendingToVector(42, 0, 10, pending);
			ASSERT_TRUE(active.empty());
			ASSERT_TRUE(pending.empty());

			replay();
			auto &restored = posix ? gMetadata->posixLocks : gMetadata->flockLocks;
			restored.copyActiveToVector(42, 0, 10, active);
			restored.copyPendingToVector(42, 0, 10, pending);
			EXPECT_TRUE(active.empty());
			EXPECT_TRUE(pending.empty());
		}
	}
}

TEST_F(LockChangelogTest, SuccessfulAndQueuedBlockingRequestsReplay) {
	const auto master = FsContext::getForMaster(1);
	for (const bool posix : {false, true}) {
		for (const bool holderNonblocking : {false, true}) {
			for (const uint16_t requested : {safs_locks::kShared, safs_locks::kExclusive}) {
				SCOPED_TRACE(posix ? "posix" : "flock");
				SCOPED_TRACE(holderNonblocking);
				SCOPED_TRACE(requested);
				resetMetadata();
				operations_->entries.clear();
				ASSERT_EQ(SAUNAFS_STATUS_OK,
				          lock(posix, master, 1, safs_locks::kExclusive, holderNonblocking));
				ASSERT_EQ(SAUNAFS_ERROR_WAITING, lock(posix, master, 2, requested));
				ASSERT_EQ(SAUNAFS_STATUS_OK, lock(posix, master, 1, safs_locks::kRelease));
				ASSERT_EQ(3U, operations_->entries.size());
				const auto masterVersion = gMetadata->metadataVersion;

				// Before the release, replay must preserve the blocking waiter.
				replay(2);
				auto &queued = posix ? gMetadata->posixLocks : gMetadata->flockLocks;
				ASSERT_NE(nullptr, queued.getPendingLocks(42));
				ASSERT_EQ(1U, queued.getPendingLocks(42)->size());
				EXPECT_EQ(2U, queued.getPendingLocks(42)->front().owner().owner);
				EXPECT_EQ(static_cast<LockRange::Type>(requested),
				          queued.getPendingLocks(42)->front().type);

				// Replaying the release must grant the waiter with its requested type.
				replay();
				auto &granted = posix ? gMetadata->posixLocks : gMetadata->flockLocks;
				ASSERT_NE(nullptr, granted.getActiveLocks(42));
				ASSERT_EQ(1U, granted.getActiveLocks(42)->size());
				EXPECT_EQ(2U, granted.getActiveLocks(42)->begin()->owner().owner);
				EXPECT_EQ(static_cast<LockRange::Type>(requested),
				          granted.getActiveLocks(42)->begin()->type);
				std::vector<safs_locks::Info> pending;
				granted.copyPendingToVector(42, 0, 10, pending);
				EXPECT_TRUE(pending.empty());
				EXPECT_EQ(masterVersion, gMetadata->metadataVersion);
			}
		}
	}
}

}  // namespace
