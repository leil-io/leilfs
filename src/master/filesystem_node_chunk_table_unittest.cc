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

#include <memory>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "errors/saunafs_error_codes.h"
#include "kv/ifuture.h"
#include "master/chunk_operations_base.h"
#include "master/chunks.h"
#include "master/filesystem_metadata.h"
#include "master/filesystem_node.h"
#include "master/filesystem_node_types.h"
#include "master/filesystem_operation_context.h"
#include "master/filesystem_operations.h"

namespace {

class ChunkTableOps : public FilesystemNodeOperationsBase {};

class AppendChunkTableOps : public ChunkTableOps {
public:
	void getStats(const FilesystemOperationContext &, FSNode *, StatsRecord *statsOut) override {
		*statsOut = {};
	}
};

class AppendTestFilesystemOperations : public FilesystemOperationsBase {
public:
	AppendTestFilesystemOperations()
	    : FilesystemOperationsBase(std::make_unique<AppendChunkTableOps>()) {}

	const Goal &getGoalDefinition(uint8_t) const override { return goal_; }

private:
	Goal goal_;
};

class NoopReadWriteTransaction : public kv::IReadWriteTransaction {
public:
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
	bool commit() override { return true; }
	std::unique_ptr<kv::ICommitFuture> commitAsync() override { return nullptr; }
	std::optional<int64_t> getCommittedVersion() const override { return std::nullopt; }
	uint64_t mutationCount() const override { return 0; }
	std::unique_ptr<kv::IVoidFuture> recoverAsync(int) override { return nullptr; }
};

class FailThirdAddChunkOperations : public ChunkOperationsBase {
public:
	int addFile(const FilesystemOperationContext &fsOpContext, uint64_t chunkid, uint8_t goal,
	            bool isMetadataLoading) override {
		addAttempts_++;
		if (addAttempts_ == 3) { return SAUNAFS_ERROR_IO; }
		return ChunkOperationsBase::addFile(fsOpContext, chunkid, goal, isMetadataLoading);
	}

private:
	uint32_t addAttempts_ = 0;
};

class NoopTransactionEffects : public FilesystemTransactionEffects {
public:
	void finish(FilesystemTransactionOutcome) noexcept override {}
};

class FailThirdDeferredAddChunkOperations : public ChunkOperationsBase {
public:
	int addFile(const FilesystemOperationContext &fsOpContext, uint64_t, uint8_t, bool) override {
		if (fsOpContext.transactionEffects() == nullptr) {
			fsOpContext.setTransactionEffects(std::make_unique<NoopTransactionEffects>());
		}
		return ++addAttempts_ == 3 ? SAUNAFS_ERROR_IO : SAUNAFS_STATUS_OK;
	}
	bool defersFileReferenceMutations(
	    const FilesystemOperationContext & /*fsOpContext*/) const override {
		return true;
	}

private:
	uint32_t addAttempts_ = 0;
};

class AppendChunksRollbackTest : public ::testing::Test {
protected:
	void SetUp() override {
		gFSOperations = std::make_unique<AppendTestFilesystemOperations>();
		ASSERT_EQ(1, chunk_strinit());
		gChunkOperations = std::make_unique<FailThirdAddChunkOperations>();
	}

	void TearDown() override {
		gChunkOperations.reset();
		chunk_unload();
		gFSOperations.reset();
	}
};

/// Admits the fence but refuses the truncate preflight, standing in for a backend that
/// cannot release the discarded tail without dropping references. Session and node
/// lookup are stubbed so the truncate entry points can run without a live metadata tree.
class RefuseTruncateOps : public AppendChunkTableOps {
public:
	FSNodeFile *node = nullptr;
	uint32_t preflights = 0;

protected:
	uint8_t verifySession(const FsContext &, OperationMode, SessionType) override {
		return SAUNAFS_STATUS_OK;
	}

	uint8_t getNodeForOperation(const FsContext &, const FilesystemOperationContext &,
	                            ExpectedNodeType, uint8_t, inode_t, FSNode **nodeOut,
	                            FSNodeDirectory **rootDirOut) override {
		if (rootDirOut != nullptr) { *rootDirOut = nullptr; }
		*nodeOut = node;
		return SAUNAFS_STATUS_OK;
	}

	uint8_t canTruncateFileChunks(const FilesystemOperationContext &, const FSNodeFile *,
	                              uint64_t) override {
		++preflights;
		return SAUNAFS_ERROR_IO;
	}
};

class CountingTruncateChunkOperations : public ChunkOperationsBase {
public:
	uint32_t truncates = 0;

	uint8_t multiTruncate(const FilesystemOperationContext &, uint64_t, uint32_t, uint32_t, uint8_t,
	                      bool, bool, uint64_t *newChunkId) override {
		++truncates;
		*newChunkId = 999;
		return SAUNAFS_STATUS_OK;
	}
};

class TruncateGuardFilesystemOperations : public FilesystemOperationsBase {
public:
	explicit TruncateGuardFilesystemOperations(std::unique_ptr<RefuseTruncateOps> ops)
	    : FilesystemOperationsBase(std::move(ops)) {}

	const Goal &getGoalDefinition(uint8_t) const override { return goal_; }

private:
	Goal goal_;
};

class TruncatePreflightTest : public ::testing::Test {
protected:
	void SetUp() override {
		gMetadata = new FilesystemMetadata;
		auto ops = std::make_unique<RefuseTruncateOps>();
		ops_ = ops.get();
		operations_ = std::make_unique<TruncateGuardFilesystemOperations>(std::move(ops));
		gChunkOperations = std::make_unique<CountingTruncateChunkOperations>();

		file_.id = 1;
		file_.goal = 2;
		file_.length = 2 * SFSCHUNKSIZE;
		ops_->node = &file_;
		ops_->resizeFileChunkTable(context_, &file_, 2);
		ops_->setFileChunkId(context_, &file_, 1, kBoundaryChunkId);
	}

	void TearDown() override {
		gChunkOperations.reset();
		operations_.reset();
		delete gMetadata;
		gMetadata = nullptr;
	}

	uint32_t truncateCalls() const {
		return static_cast<CountingTruncateChunkOperations *>(gChunkOperations.get())->truncates;
	}

	static constexpr uint64_t kBoundaryChunkId = 555;

	FilesystemOperationContext context_;
	FSNodeFile file_{FSNodeType::kFile};
	RefuseTruncateOps *ops_ = nullptr;
	std::unique_ptr<TruncateGuardFilesystemOperations> operations_;
};

}  // namespace

TEST(FileChunkTableResizeTest, ResizeShrinksRoundedTrailingPadding) {
	// writeChunk grows the table in rounded blocks, so a file whose last live chunk
	// sits at index 8 carries a 16-slot vector. Append sizes its result from the
	// trimmed counts and must drop that padding, or chunks-info reports phantom
	// trailing holes until serialization trims them on the next restart.
	ChunkTableOps ops;
	FilesystemOperationContext ctx;
	FSNodeFile node(FSNodeType::kFile);
	ops.resizeFileChunkTable(ctx, &node, 16);
	for (uint32_t index = 0; index <= 8; ++index) { node.chunks[index] = 100 + index; }

	ASSERT_EQ(9U, ops.getFileChunkCount(ctx, &node));
	ASSERT_EQ(16U, ops.getFileChunkTableSize(ctx, &node));

	// The append sequence: exact resize to trimmed dst + trimmed src, then the copy.
	ops.resizeFileChunkTable(ctx, &node, 10);
	ASSERT_EQ(10U, ops.getFileChunkTableSize(ctx, &node));
	ops.setFileChunkId(ctx, &node, 9, 200);

	EXPECT_EQ(10U, ops.getFileChunkTableSize(ctx, &node));
	EXPECT_EQ(10U, ops.getFileChunkCount(ctx, &node));
	EXPECT_EQ(200U, ops.getFileChunkId(ctx, &node, 9));
	for (uint32_t index = 0; index <= 8; ++index) {
		EXPECT_EQ(100U + index, ops.getFileChunkId(ctx, &node, index));
	}
}

TEST(FileChunkTableResizeTest, ResizeGrowsWithHoles) {
	ChunkTableOps ops;
	FilesystemOperationContext ctx;
	FSNodeFile node(FSNodeType::kFile);
	ops.resizeFileChunkTable(ctx, &node, 2);
	node.chunks[1] = 7;

	ops.resizeFileChunkTable(ctx, &node, 5);
	EXPECT_EQ(5U, ops.getFileChunkTableSize(ctx, &node));
	EXPECT_EQ(2U, ops.getFileChunkCount(ctx, &node));
	EXPECT_EQ(0U, ops.getFileChunkId(ctx, &node, 4));
}

TEST(FileChunkTableResizeTest, GrowNeverShrinks) {
	ChunkTableOps ops;
	FilesystemOperationContext ctx;
	FSNodeFile node(FSNodeType::kFile);
	ops.resizeFileChunkTable(ctx, &node, 16);
	node.chunks[8] = 1;

	ops.growFileChunkTable(ctx, &node, 10);
	EXPECT_EQ(16U, ops.getFileChunkTableSize(ctx, &node));
	ops.growFileChunkTable(ctx, &node, 32);
	EXPECT_EQ(32U, ops.getFileChunkTableSize(ctx, &node));
}

TEST_F(AppendChunksRollbackTest, FailedAddReversesSuccessfulPrefix) {
	constexpr uint8_t goal = 2;
	const std::vector<uint64_t> chunkIds = {101, 102, 103};
	for (uint64_t chunkId : chunkIds) {
		chunk_create_with_goal_counters(chunkId, 1, {{goal, 1}}, 0, 0);
	}

	AppendChunkTableOps operations;
	std::unique_ptr<kv::IReadWriteTransaction> transaction =
	    std::make_unique<NoopReadWriteTransaction>();
	FilesystemOperationContext context(std::move(transaction));
	FSNodeFile source(FSNodeType::kFile);
	source.goal = goal;
	source.length = chunkIds.size() * SFSCHUNKSIZE;
	operations.resizeFileChunkTable(context, &source, chunkIds.size());
	for (size_t index = 0; index < chunkIds.size(); ++index) {
		operations.setFileChunkId(context, &source, index, chunkIds[index]);
	}
	FSNodeFile destination(FSNodeType::kFile);
	destination.goal = goal;

	EXPECT_EQ(SAUNAFS_ERROR_IO, operations.appendChunks(context, 1, &destination, &source));

	for (uint64_t chunkId : chunkIds) {
		uint32_t version = 0;
		ChunkGoalCounters counters;
		ASSERT_TRUE(chunk_get_version_and_goal_counters(chunkId, version, counters));
		EXPECT_EQ(1U, counters.fileCount());
	}
}

TEST_F(TruncatePreflightTest, TrySetLengthRefusesBeforeTouchingTheBoundaryChunk) {
	// The boundary chunk is truncated in place and written to the changelog before the
	// table is cut, so a refusal that arrives later cannot undo it.
	Attributes attr;
	uint64_t chunkId = 0;
	const uint64_t newLength = SFSCHUNKSIZE + 100;

	EXPECT_EQ(SAUNAFS_ERROR_IO,
	          operations_->trySetLength(FsContext::getForMaster(0), context_, file_.id, 1,
	                                    newLength, false, 0, attr, &chunkId));

	EXPECT_EQ(1U, ops_->preflights);
	EXPECT_EQ(0U, truncateCalls());
	EXPECT_EQ(kBoundaryChunkId, ops_->getFileChunkId(context_, &file_, 1));
}

TEST_F(TruncatePreflightTest, DoSetLengthRefusesBeforeChangingTheLength) {
	// setLength() returns void, so a backend that refuses the cut here would otherwise
	// leave the shortened length published with its discarded tail still referenced.
	Attributes attr;
	const uint64_t previousLength = file_.length;

	EXPECT_EQ(SAUNAFS_ERROR_IO,
	          operations_->doSetLength(FsContext::getForMaster(0), context_, file_.id, 0, attr));

	EXPECT_EQ(1U, ops_->preflights);
	EXPECT_EQ(previousLength, file_.length);
	EXPECT_EQ(2U, ops_->getFileChunkTableSize(context_, &file_));
}

TEST_F(AppendChunksRollbackTest, FailedDeferredAddDoesNotTouchLiveRegistry) {
	constexpr uint8_t goal = 2;
	const std::vector<uint64_t> chunkIds = {111, 112, 113};
	for (uint64_t chunkId : chunkIds) {
		chunk_create_with_goal_counters(chunkId, 1, {{goal, 1}}, 0, 0);
	}
	gChunkOperations = std::make_unique<FailThirdDeferredAddChunkOperations>();

	AppendChunkTableOps operations;
	std::unique_ptr<kv::IReadWriteTransaction> transaction =
	    std::make_unique<NoopReadWriteTransaction>();
	FilesystemOperationContext context(std::move(transaction));
	FSNodeFile source(FSNodeType::kFile);
	source.goal = goal;
	source.length = chunkIds.size() * SFSCHUNKSIZE;
	operations.resizeFileChunkTable(context, &source, chunkIds.size());
	for (size_t index = 0; index < chunkIds.size(); ++index) {
		operations.setFileChunkId(context, &source, index, chunkIds[index]);
	}
	FSNodeFile destination(FSNodeType::kFile);
	destination.goal = goal;

	EXPECT_EQ(SAUNAFS_ERROR_IO, operations.appendChunks(context, 1, &destination, &source));

	for (uint64_t chunkId : chunkIds) {
		uint32_t version = 0;
		ChunkGoalCounters counters;
		ASSERT_TRUE(chunk_get_version_and_goal_counters(chunkId, version, counters));
		EXPECT_EQ(1U, counters.fileCount());
	}
}
