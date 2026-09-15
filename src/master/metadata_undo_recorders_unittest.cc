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
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/platform.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "kv/ifuture.h"
#include "kv/ikv_engine.h"
#include "kv/itransaction.h"
#include "kv/kv_utils.h"
#include "master/filesystem_metadata.h"
#include "master/filesystem_node.h"
#include "master/filesystem_operations.h"
#include "master/filesystem_trash_reserved_files.h"
#include "master/hstring_memstorage.h"
#include "master/kv_common_keys.h"
#include "master/metadata_backend_forkless.h"
#include "master/metadata_checkpoint_helpers.h"
#include "master/metadata_chunk_undo_recorder.h"
#include "master/metadata_edge_undo_recorder.h"
#include "master/metadata_node_undo_recorder.h"
#include "master/metadata_quota_undo_recorder.h"
#include "master/metadata_section_bootstrap_fdb.h"
#include "master/metadata_xattr_undo_recorder.h"
#include "protocol/SFSCommunication.h"

struct MetadataBackendForklessTestAccess {
	static void recordDetainedInode(MetadataBackendForkless &backend, inode_t inode,
	                                uint32_t timestamp) {
		backend.onFreeInodeDetained(inode, timestamp);
	}

	static void recordReleasedInode(MetadataBackendForkless &backend, inode_t inode) {
		backend.onFreeInodeReleased(inode);
	}

	static void recordChangedEdge(MetadataBackendForkless &backend, inode_t parentId,
	                              inode_t childId, const HString &name) {
		backend.onEdgeChanged(parentId, childId, name);
	}

	static void recordRemovedEdge(MetadataBackendForkless &backend, inode_t parentId,
	                              const HString &name) {
		backend.onEdgeRemoved(parentId, name);
	}

	static void recordChangedDetachedPath(MetadataBackendForkless &backend, inode_t inode,
	                                      FSNodeType nodeType, const HString &path) {
		backend.onDetachedPathChanged(inode, nodeType, path);
	}

	static void recordRemovedDetachedPath(MetadataBackendForkless &backend, inode_t inode) {
		backend.onDetachedPathRemoved(inode);
	}

	static void attachWriter(MetadataBackendForkless &backend, kv::IKVEngine *kvEngine) {
		backend.metadataWriter_ = std::make_unique<MetadataWriterFDB>(kvEngine);
	}

	static void reconcileFreeInodes(MetadataBackendForkless &backend, uint64_t &persisted,
	                                uint64_t &removed) {
		backend.reconcileDirtyFreeInodesToFDB(persisted, removed);
	}

	static void reconcileEdges(MetadataBackendForkless &backend, uint64_t &persisted,
	                           uint64_t &removed) {
		backend.reconcileDirtyEdgesToFDB(persisted, removed);
	}

	static void reconcileDetachedPaths(MetadataBackendForkless &backend, uint64_t &persisted,
	                                   uint64_t &removed) {
		backend.reconcileDirtyDetachedPathsToFDB(persisted, removed);
	}

	static void prepareCheckpointLoad(MetadataBackendForkless &backend, kv::IKVEngine *kvEngine,
	                                  uint64_t checkpointVersion) {
		backend.checkpointManager_ = std::make_unique<MetadataCheckpointManager>(kvEngine);
		backend.loadedCheckpointDescriptor_.metadataVersion = checkpointVersion;
	}

	static int8_t loadEdge(MetadataBackendForkless &backend, inode_t parentId, inode_t childId,
	                       const std::string &name) {
		return backend.loadEdge(FilesystemOperationContext{}, parentId, childId, name,
		                        /*ignoreFlag=*/false, /*init=*/false);
	}

	static int8_t loadDetachedPath(MetadataBackendForkless &backend, inode_t inode,
	                               FSNodeType nodeType, const std::string &path) {
		return backend.loadDetachedPath(FilesystemOperationContext{}, inode, nodeType, path,
		                                /*ignoreFlag=*/false);
	}

	static bool restoreEdges(MetadataBackendForkless &backend, uint64_t checkpointVersion) {
		return backend.restoreEdgesToCheckpointVersion(checkpointVersion) == kOpSuccess;
	}
};

struct MetadataSectionBootstrapFDBTestAccess {
	static int8_t saveMetadataHeader(MetadataSectionBootstrapFDB &bootstrap, inode_t maxInodeId,
	                                 uint64_t metadataVersion, uint32_t nextSessionId) {
		bootstrap.maxInodeId_ = maxInodeId;
		bootstrap.metadataVersion_ = metadataVersion;
		bootstrap.nextSessionId_ = nextSessionId;
		return bootstrap.saveMetadataHeader();
	}
};

namespace {

using DurableStore = std::map<kv::Key, kv::Value>;
using PendingWrites = std::map<kv::Key, std::optional<kv::Value>>;

// Models transaction-local writes separately from durable state, including read-your-writes for
// point and range reads. A failed commit discards the pending undo and live-key mutations when the
// transaction is destroyed, exactly the behavior needed to exercise a fresh retry.
class RecordingTransaction final : public kv::IReadWriteTransaction {
public:
	RecordingTransaction(DurableStore &store, bool commitSucceeds)
	    : store_(store), commitSucceeds_(commitSucceeds) {}

	std::optional<kv::Value> get(const kv::Key &key) override {
		if (const auto pending = writes_.find(key); pending != writes_.end()) {
			return pending->second;
		}
		if (const auto durable = store_.find(key); durable != store_.end()) {
			return durable->second;
		}
		return std::nullopt;
	}

	std::optional<kv::Value> getSnapshot(const kv::Key &key) override { return get(key); }

	std::unique_ptr<kv::IFuture> getAsync(const kv::Key & /*key*/) override { return nullptr; }

	std::unique_ptr<kv::IFuture> getSnapshotAsync(const kv::Key & /*key*/) override {
		return nullptr;
	}

	kv::GetRangeResult getRange(const kv::KeySelector &start, const kv::KeySelector &end,
	                            int limit) override {
		if (start.getOffset() != 0 || end.getOffset() != 0) {
			throw std::logic_error("RecordingTransaction does not support selector offsets");
		}
		if (limit <= 0) {
			throw std::invalid_argument("RecordingTransaction requires a positive range limit");
		}

		DurableStore visible = materialize();
		auto iterator = start.isInclusive() ? visible.lower_bound(start.getKey())
		                                    : visible.upper_bound(start.getKey());
		auto isBeforeEnd = [&end](const kv::Key &key) {
			return end.isInclusive() ? key <= end.getKey() : key < end.getKey();
		};

		std::vector<kv::KeyValuePair> pairs;
		for (; iterator != visible.end() && isBeforeEnd(iterator->first); ++iterator) {
			if (pairs.size() == static_cast<size_t>(limit)) { return {std::move(pairs), true}; }
			pairs.push_back({.key = iterator->first, .value = iterator->second});
		}
		return {std::move(pairs), false};
	}

	std::unique_ptr<kv::IRangeFuture> getRangeAsync(const kv::KeySelector & /*start*/,
	                                                const kv::KeySelector & /*end*/,
	                                                int /*limit*/) override {
		return nullptr;
	}

	void set(const kv::Key &key, const kv::Value &value) override {
		writes_[key] = value;
		++mutationCount_;
	}

	void atomicAdd(const kv::Key & /*key*/, const kv::Value & /*delta*/) override {
		throw std::logic_error("RecordingTransaction does not support atomicAdd");
	}

	void atomicMax(const kv::Key & /*key*/, const kv::Value & /*value*/) override {
		throw std::logic_error("RecordingTransaction does not support atomicMax");
	}

	void remove(const kv::Key &key) override {
		writes_[key] = std::nullopt;
		++mutationCount_;
	}

	void removeRange(const kv::Key &start, const kv::Key &end) override {
		const DurableStore visible = materialize();
		for (auto iterator = visible.lower_bound(start);
		     iterator != visible.end() && iterator->first < end; ++iterator) {
			writes_[iterator->first] = std::nullopt;
		}
		++mutationCount_;
	}

	void addReadConflictKey(const kv::Key & /*key*/) override {}

	bool commit() override {
		if (!commitSucceeds_) { return false; }
		for (const auto &[key, value] : writes_) {
			if (value.has_value()) {
				store_[key] = *value;
			} else {
				store_.erase(key);
			}
		}
		return true;
	}

	std::unique_ptr<kv::ICommitFuture> commitAsync() override { return nullptr; }

	std::unique_ptr<kv::IVoidFuture> recoverAsync(int /*backendErrorCode*/) override {
		return std::make_unique<kv::ImmediateVoidFuture>();
	}

	std::optional<int64_t> getCommittedVersion() const override { return std::nullopt; }

	uint64_t mutationCount() const override { return mutationCount_; }

private:
	DurableStore materialize() const {
		DurableStore visible = store_;
		for (const auto &[key, value] : writes_) {
			if (value.has_value()) {
				visible[key] = *value;
			} else {
				visible.erase(key);
			}
		}
		return visible;
	}

	DurableStore &store_;
	bool commitSucceeds_;
	PendingWrites writes_;
	uint64_t mutationCount_{0};
};

class RecordingKVEngine final : public kv::IKVEngine {
public:
	std::unique_ptr<kv::IReadOnlyTransaction> createReadOnlyTransaction() override {
		return std::make_unique<RecordingTransaction>(store_, /*commitSucceeds=*/true);
	}

	std::unique_ptr<kv::IReadWriteTransaction> createReadWriteTransaction() override {
		return std::make_unique<RecordingTransaction>(store_, /*commitSucceeds=*/true);
	}

	DurableStore &store() { return store_; }

private:
	DurableStore store_;
};

// Promotion reconciliation must be able to replay the shadow's recorded FREE delta without
// consulting the complete in-memory inode pool. Run the scenario in a child process with
// gMetadata unset: a full-pool scan would crash the child, while recorded-state-only
// reconciliation reaches the expected zero exit code and keeps the test failure contained.
TEST(MetadataBackendForklessTest, FreePromotionReconcileUsesOnlyRecordedDirtyState) {
	EXPECT_EXIT(
	    {
		    // Deliberately make the authoritative inode pool unavailable. The recorded dirty values
		    // must contain everything promotion needs to reconstruct the affected FREE keys.
		    gMetadata = nullptr;

		    RecordingKVEngine engine;
		    constexpr inode_t kDetainedInode = 41;
		    constexpr inode_t kReleasedInode = 42;
		    constexpr uint32_t kTimestamp = 1234;
		    // Model stale persisted state: inode 42 was detained in the last FDB image, but the
		    // shadow will subsequently observe its release and must remove this key on promotion.
		    engine.store()[kv::encodeKeyBE(kFreeKeyPrefix, kReleasedInode)] =
		        kv::toBytesBE(uint32_t{5678});

		    // A newly constructed backend has no writer, matching a shadow. Drive both inodes
		    // through opposite transitions to prove that only the last state per key is retained:
		    //   inode 41: released -> detained(1234) => persist FREE_41 = 1234
		    //   inode 42: detained(4321) -> released => remove  FREE_42
		    MetadataBackendForkless backend;
		    MetadataBackendForklessTestAccess::recordReleasedInode(backend, kDetainedInode);
		    MetadataBackendForklessTestAccess::recordDetainedInode(backend, kDetainedInode,
		                                                           kTimestamp);
		    MetadataBackendForklessTestAccess::recordDetainedInode(backend, kReleasedInode,
		                                                           /*timestamp=*/4321);
		    MetadataBackendForklessTestAccess::recordReleasedInode(backend, kReleasedInode);

		    // Attaching the writer after the transitions models promotion. Reconciliation should
		    // enqueue exactly one final update and one final removal, not all four transitions.
		    MetadataBackendForklessTestAccess::attachWriter(backend, &engine);

		    uint64_t persisted = 0;
		    uint64_t removed = 0;
		    MetadataBackendForklessTestAccess::reconcileFreeInodes(backend, persisted, removed);
		    if (persisted != 1 || removed != 1 || !backend.flushPendingUpdates(true)) {
			    std::_Exit(1);
		    }

		    // Flushing must materialize the final shadow-observed state: inode 41 is detained with
		    // its last timestamp, and the stale row for the now-released inode 42 is gone.
		    const auto detained =
		        engine.store().find(kv::encodeKeyBE(kFreeKeyPrefix, kDetainedInode));
		    if (detained == engine.store().end() || detained->second != kv::toBytesBE(kTimestamp) ||
		        engine.store().contains(kv::encodeKeyBE(kFreeKeyPrefix, kReleasedInode))) {
			    std::_Exit(2);
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

// Detached paths use the same inode-keyed rows as MDS. Two trash entries may legitimately have
// the same original path (delete, recreate, and delete the same name), so the path must be the
// value rather than the durable identity. Also exercise a trash-to-reserved transition and a
// stale inode whose final shadow state is absent.
TEST(MetadataBackendForklessTest, DetachedPathPromotionReconcileUsesMdsKeyspace) {
	EXPECT_EXIT(
	    {
		    constexpr inode_t kFirstTrashInode = 41;
		    constexpr inode_t kSecondTrashInode = 42;
		    constexpr inode_t kReservedInode = 43;
		    constexpr inode_t kRemovedInode = 44;
		    const HString duplicatePath("same/original/path");
		    const HString reservedPath("reserved/path");

		    RecordingKVEngine engine;
		    engine.store()[kv::encodeKeyBE(kTrashPathKeyPrefix, kRemovedInode)] =
		        kv::toBytes("stale/trash");
		    engine.store()[kv::encodeKeyBE(kReservedPathKeyPrefix, kRemovedInode)] =
		        kv::toBytes("stale/reserved");

		    // These are the final per-inode states observed while replaying as a shadow. The last
		    // state for inode 43 wins, changing its target key family from trash to reserved.
		    MetadataBackendForkless backend;
		    MetadataBackendForklessTestAccess::recordChangedDetachedPath(
		        backend, kFirstTrashInode, FSNodeType::kTrash, duplicatePath);
		    MetadataBackendForklessTestAccess::recordChangedDetachedPath(
		        backend, kSecondTrashInode, FSNodeType::kTrash, duplicatePath);
		    MetadataBackendForklessTestAccess::recordChangedDetachedPath(
		        backend, kReservedInode, FSNodeType::kTrash, HString("old/trash/path"));
		    MetadataBackendForklessTestAccess::recordChangedDetachedPath(
		        backend, kReservedInode, FSNodeType::kReserved, reservedPath);
		    MetadataBackendForklessTestAccess::recordRemovedDetachedPath(backend, kRemovedInode);

		    MetadataBackendForklessTestAccess::attachWriter(backend, &engine);
		    uint64_t persisted = 0;
		    uint64_t removed = 0;
		    MetadataBackendForklessTestAccess::reconcileDetachedPaths(backend, persisted, removed);
		    if (persisted != 3 || removed != 1 || !backend.flushPendingUpdates(true)) {
			    std::_Exit(1);
		    }

		    const auto firstTrash =
		        engine.store().find(kv::encodeKeyBE(kTrashPathKeyPrefix, kFirstTrashInode));
		    const auto secondTrash =
		        engine.store().find(kv::encodeKeyBE(kTrashPathKeyPrefix, kSecondTrashInode));
		    const auto reserved =
		        engine.store().find(kv::encodeKeyBE(kReservedPathKeyPrefix, kReservedInode));
		    if (firstTrash == engine.store().end() ||
		        firstTrash->second != kv::toBytes(duplicatePath) ||
		        secondTrash == engine.store().end() ||
		        secondTrash->second != kv::toBytes(duplicatePath) ||
		        reserved == engine.store().end() || reserved->second != kv::toBytes(reservedPath) ||
		        engine.store().contains(kv::encodeKeyBE(kTrashPathKeyPrefix, kReservedInode)) ||
		        engine.store().contains(kv::encodeKeyBE(kTrashPathKeyPrefix, kRemovedInode)) ||
		        engine.store().contains(kv::encodeKeyBE(kReservedPathKeyPrefix, kRemovedInode))) {
			    std::_Exit(2);
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

// The persisted key is the inode, not the path. Reconstruct two trash nodes carrying the same
// original path and verify neither overwrites the other in the authoritative trash container.
TEST(MetadataBackendForklessTest, LoadsDuplicateDetachedPathsByInode) {
	EXPECT_EXIT(
	    {
		    hstorage::Storage::reset(new hstorage::MemStorage());
		    gMetadata = new FilesystemMetadata;
		    gFSOperations = std::make_unique<FilesystemOperationsBase>(
		        std::make_unique<FilesystemNodeOperationsBase>());

		    constexpr inode_t kFirstInode = 41;
		    constexpr inode_t kSecondInode = 42;
		    const std::string duplicatePath("directory/file");
		    for (inode_t inode : {kFirstInode, kSecondInode}) {
			    auto *node = new FSNodeFile(FSNodeType::kTrash);
			    node->id = inode;
			    gMetadata->addNode(node, /*isFromScan=*/true);
		    }

		    MetadataBackendForkless backend;
		    if (MetadataBackendForklessTestAccess::loadDetachedPath(
		            backend, kFirstInode, FSNodeType::kTrash, duplicatePath) != kOpSuccess ||
		        MetadataBackendForklessTestAccess::loadDetachedPath(
		            backend, kSecondInode, FSNodeType::kTrash, duplicatePath) != kOpSuccess) {
			    std::_Exit(1);
		    }

		    if (gMetadata->trash.size() != 2 || gMetadata->trashNodes != 2) { std::_Exit(2); }
		    bool foundFirst = false;
		    bool foundSecond = false;
		    for (const auto &entry : gMetadata->trash) {
			    if (entry.second.get() != duplicatePath) { std::_Exit(3); }
			    foundFirst |= entry.first.id == kFirstInode;
			    foundSecond |= entry.first.id == kSecondInode;
		    }
		    std::_Exit(foundFirst && foundSecond ? 0 : 4);
	    },
	    ::testing::ExitedWithCode(0), "");
}

// NODE rollback runs before detached path loading. Model a file that was regular at checkpoint 17
// and was subsequently unlinked into trash: the restored NODE body is regular, while the hot
// TRSH_PATH_ row is newer. The inode-keyed EDGEU_ tombstone proves that the path did not exist at
// the checkpoint, so loading can defer the mismatch until edge-section rollback removes it.
TEST(MetadataBackendForklessTest, DefersPostCheckpointTrashPathUntilEdgeRollback) {
	EXPECT_EXIT(
	    {
		    hstorage::Storage::reset(new hstorage::MemStorage());
		    gMetadata = new FilesystemMetadata;
		    gFSOperations = std::make_unique<FilesystemOperationsBase>(
		        std::make_unique<FilesystemNodeOperationsBase>());

		    constexpr inode_t kFileInode = 41;
		    constexpr uint64_t kCheckpointVersion = 17;
		    const HString trashPath("directory/file");

		    // This is the checkpoint NODE state after node rollback, not the latest kTrash body.
		    auto *checkpointFile = new FSNodeFile(FSNodeType::kFile);
		    checkpointFile->id = kFileInode;
		    gMetadata->addNode(checkpointFile, /*isFromScan=*/true);

		    RecordingKVEngine engine;
		    engine.store()[kv::toBytes(kMetaCheckpointVersionsKey)] =
		        checkpoints::serializeCheckpointVersions({kCheckpointVersion});
		    const kv::Key undoKey =
		        kv::encodeKeyBE(kEdgeUndoKeyPrefix, kCheckpointVersion, inode_t{0}, kFileInode);
		    engine.store()[undoKey] = {};  // The inode had no detached path at the checkpoint.

		    MetadataBackendForkless backend;
		    MetadataBackendForklessTestAccess::prepareCheckpointLoad(backend, &engine,
		                                                             kCheckpointVersion);

		    // This row comes from the newer hot TRSH_PATH_ image. It must not be materialized
		    // against the already-restored regular node, but its applicable undo row makes it safe
		    // to defer.
		    if (MetadataBackendForklessTestAccess::loadDetachedPath(
		            backend, kFileInode, FSNodeType::kTrash, trashPath) != kOpSuccess) {
			    std::_Exit(1);
		    }
		    if (!MetadataBackendForklessTestAccess::restoreEdges(backend, kCheckpointVersion)) {
			    std::_Exit(2);
		    }

		    // The checkpoint state has neither a detached path nor a detached node type.
		    if (checkpointFile->type != FSNodeType::kFile || !gMetadata->trash.empty() ||
		        !gMetadata->reserved.empty()) {
			    std::_Exit(3);
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

// Deferral is not a general relaxation of detached-path validation. Without a matching EDGEU_ row,
// the same mixed-looking live row is not proven to be post-checkpoint drift and the section restore
// must reject it as inconsistent metadata.
TEST(MetadataBackendForklessTest, RejectsIncompatibleDetachedPathWithoutApplicableUndo) {
	EXPECT_EXIT(
	    {
		    hstorage::Storage::reset(new hstorage::MemStorage());
		    gMetadata = new FilesystemMetadata;
		    gFSOperations = std::make_unique<FilesystemOperationsBase>(
		        std::make_unique<FilesystemNodeOperationsBase>());

		    constexpr inode_t kFileInode = 41;
		    constexpr uint64_t kCheckpointVersion = 17;
		    const std::string trashPath("directory/file");

		    auto *checkpointFile = new FSNodeFile(FSNodeType::kFile);
		    checkpointFile->id = kFileInode;
		    gMetadata->addNode(checkpointFile, /*isFromScan=*/true);

		    RecordingKVEngine engine;
		    engine.store()[kv::toBytes(kMetaCheckpointVersionsKey)] =
		        checkpoints::serializeCheckpointVersions({kCheckpointVersion});

		    MetadataBackendForkless backend;
		    MetadataBackendForklessTestAccess::prepareCheckpointLoad(backend, &engine,
		                                                             kCheckpointVersion);
		    if (MetadataBackendForklessTestAccess::loadDetachedPath(
		            backend, kFileInode, FSNodeType::kTrash, trashPath) != kOpSuccess) {
			    std::_Exit(1);
		    }

		    std::_Exit(MetadataBackendForklessTestAccess::restoreEdges(backend, kCheckpointVersion)
		                   ? 2
		                   : 0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(MetadataSectionBootstrapFDBTest, SeedsImportedVersionAsInitialCheckpoint) {
	RecordingKVEngine engine;
	MetadataSectionBootstrapFDB bootstrap(&engine);
	constexpr uint64_t kImportedVersion = 123;

	ASSERT_EQ(MetadataSectionBootstrapFDBTestAccess::saveMetadataHeader(
	              bootstrap, /*maxInodeId=*/42, kImportedVersion, /*nextSessionId=*/7),
	          kOpSuccess);

	EXPECT_EQ(checkpoints::loadCheckpointVersions(&engine),
	          std::vector<uint64_t>{kImportedVersion});
}

template <typename ApplyMutation>
void expectFailedFirstTouchRetryPreservesPreimage(
    ISectionUndoRecorder &recorder, DurableStore &store, const MetadataMutation &mutation,
    const kv::Key &undoKey, const kv::Value &expectedUndoValue, ApplyMutation applyMutation) {
	const DurableStore originalStore = store;

	// The first attempt writes both undo and live state transaction-locally, but neither may become
	// durable when commit fails.
	{
		RecordingTransaction failedTransaction(store, /*commitSucceeds=*/false);
		recorder.beforeMutation(
		    MetadataMutationContext{
		        .transaction = &failedTransaction,
		        .checkpointVersion = 17,
		    },
		    mutation);
		applyMutation(failedTransaction, /*laterMutation=*/false);
		EXPECT_FALSE(failedTransaction.commit());
	}
	EXPECT_EQ(store, originalStore);
	EXPECT_FALSE(store.contains(undoKey));

	// A fresh retry must not be suppressed by process-local state left by the failed attempt.
	{
		RecordingTransaction retryTransaction(store, /*commitSucceeds=*/true);
		recorder.beforeMutation(
		    MetadataMutationContext{
		        .transaction = &retryTransaction,
		        .checkpointVersion = 17,
		    },
		    mutation);
		applyMutation(retryTransaction, /*laterMutation=*/false);
		ASSERT_TRUE(retryTransaction.commit());
	}
	ASSERT_TRUE(store.contains(undoKey));
	EXPECT_EQ(store.at(undoKey), expectedUndoValue);

	// Once committed, the durable undo row is the first-touch guard. A later mutation in the same
	// interval must update live state without replacing the interval-start pre-image.
	{
		RecordingTransaction laterTransaction(store, /*commitSucceeds=*/true);
		recorder.beforeMutation(
		    MetadataMutationContext{
		        .transaction = &laterTransaction,
		        .checkpointVersion = 17,
		    },
		    mutation);
		applyMutation(laterTransaction, /*laterMutation=*/true);
		ASSERT_TRUE(laterTransaction.commit());
	}
	ASSERT_TRUE(store.contains(undoKey));
	EXPECT_EQ(store.at(undoKey), expectedUndoValue);
}

template <typename T>
void appendBigEndian(kv::Bytes &destination, T value) {
	kv::Bytes bytes = kv::toBytesBE(value);
	destination.insert(destination.end(), bytes.begin(), bytes.end());
}

kv::Value chunkValue(uint32_t version, uint32_t lockedTo, uint32_t lockId) {
	kv::Value value;
	value.reserve(3 * sizeof(uint32_t));
	appendBigEndian(value, version);
	appendBigEndian(value, lockedTo);
	appendBigEndian(value, lockId);
	return value;
}

kv::Key namedKey(std::string_view prefix, inode_t inode, std::string_view name) {
	kv::Key key = kv::encodeKeyBE(prefix, inode);
	kv::appendStr(key, name);
	return key;
}

kv::Key quotaOwnerPrefix(QuotaOwnerType ownerType, inode_t ownerId) {
	kv::Key key = kv::toBytes(kQuotasKeyPrefix);
	key.push_back(static_cast<uint8_t>(ownerType));
	appendBigEndian(key, ownerId);
	return key;
}

kv::Key quotaUndoKey(uint64_t checkpointVersion, QuotaOwnerType ownerType, inode_t ownerId) {
	kv::Key key = kv::encodeKeyBE(kQuotaUndoKeyPrefix, checkpointVersion);
	key.push_back(static_cast<uint8_t>(ownerType));
	appendBigEndian(key, ownerId);
	return key;
}

using QuotaLimits = std::array<uint64_t, 4>;
using QuotaKeys = std::array<kv::Key, 4>;

QuotaKeys quotaKeys(QuotaOwnerType ownerType, inode_t ownerId) {
	const kv::Key ownerPrefix = quotaOwnerPrefix(ownerType, ownerId);
	auto makeKey = [&ownerPrefix](QuotaRigor rigor, QuotaResource resource) {
		kv::Key key = ownerPrefix;
		key.push_back(static_cast<uint8_t>(rigor));
		key.push_back(static_cast<uint8_t>(resource));
		return key;
	};

	return {
	    makeKey(QuotaRigor::kSoft, QuotaResource::kInodes),
	    makeKey(QuotaRigor::kSoft, QuotaResource::kSize),
	    makeKey(QuotaRigor::kHard, QuotaResource::kInodes),
	    makeKey(QuotaRigor::kHard, QuotaResource::kSize),
	};
}

void setQuotaLimits(RecordingTransaction &transaction, const QuotaKeys &keys,
                    const QuotaLimits &limits) {
	for (size_t i = 0; i < keys.size(); ++i) { transaction.set(keys[i], kv::toBytesBE(limits[i])); }
}

kv::Value quotaUndoValue(const QuotaLimits &limits) {
	kv::Value value{0x01};
	value.reserve(1 + (limits.size() * sizeof(uint64_t)));
	for (const uint64_t limit : limits) { appendBigEndian(value, limit); }
	return value;
}

constexpr uint64_t kCheckpointVersion = 17;

}  // namespace

TEST(MetadataUndoRecorderRestore, ChunkRejectsMalformedUndoKeys) {
	std::vector<kv::Key> malformedKeys;
	malformedKeys.push_back(kv::encodeKeyBE(kChunkUndoKeyPrefix, kCheckpointVersion));
	auto oversizedKey =
	    kv::encodeKeyBE(kChunkUndoKeyPrefix, kCheckpointVersion, uint64_t{41});
	oversizedKey.push_back(0xff);
	malformedKeys.push_back(std::move(oversizedKey));

	for (const kv::Key &malformedKey : malformedKeys) {
		SCOPED_TRACE(::testing::PrintToString(malformedKey));
		RecordingKVEngine engine;
		ChunkUndoRecorder recorder(&engine);
		engine.store()[kv::toBytes(kMetaCheckpointVersionsKey)] =
		    checkpoints::serializeCheckpointVersions({kCheckpointVersion});
		engine.store()[malformedKey] = {};

		EXPECT_FALSE(recorder.restoreToCheckpointVersion(kCheckpointVersion));
	}
}

TEST(MetadataUndoRecorderRestore, ChunkRejectsMalformedUndoValues) {
	RecordingKVEngine engine;
	ChunkUndoRecorder recorder(&engine);

	constexpr uint64_t kChunkId = 41;
	engine.store()[kv::toBytes(kMetaCheckpointVersionsKey)] =
	    checkpoints::serializeCheckpointVersions({kCheckpointVersion});
	engine.store()[kv::encodeKeyBE(kChunkUndoKeyPrefix, kCheckpointVersion, kChunkId)] =
	    kv::Value{0x01};

	EXPECT_FALSE(recorder.restoreToCheckpointVersion(kCheckpointVersion));
}

TEST(MetadataUndoRecorderRestore, EdgeRejectsMalformedUndoKeys) {
	EXPECT_EXIT(
	    {
		    hstorage::Storage::reset(new hstorage::MemStorage());
		    gMetadata = new FilesystemMetadata;
		    gFSOperations = std::make_unique<FilesystemOperationsBase>(
		        std::make_unique<FilesystemNodeOperationsBase>());

		    std::vector<kv::Key> malformedKeys;
		    // Missing parent id.
		    malformedKeys.push_back(kv::encodeKeyBE(kEdgeUndoKeyPrefix, kCheckpointVersion));
		    // A non-detached edge must contain a non-empty name after its parent id.
		    malformedKeys.push_back(
		        kv::encodeKeyBE(kEdgeUndoKeyPrefix, kCheckpointVersion, inode_t{41}));

		    for (const kv::Key &malformedKey : malformedKeys) {
			    RecordingKVEngine engine;
			    EdgeUndoRecorder recorder(&engine);
			    engine.store()[kv::toBytes(kMetaCheckpointVersionsKey)] =
			        checkpoints::serializeCheckpointVersions({kCheckpointVersion});
			    engine.store()[malformedKey] = {};
			    if (recorder.restoreToCheckpointVersion(kCheckpointVersion)) { std::_Exit(1); }
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(MetadataUndoRecorderRestore, EdgeRejectsMalformedUndoValues) {
	RecordingKVEngine engine;
	EdgeUndoRecorder recorder(&engine);

	constexpr inode_t kParentId = 41;
	kv::Key undoKey = kv::encodeKeyBE(kEdgeUndoKeyPrefix, kCheckpointVersion, kParentId);
	kv::appendStr(undoKey, "child");
	engine.store()[kv::toBytes(kMetaCheckpointVersionsKey)] =
	    checkpoints::serializeCheckpointVersions({kCheckpointVersion});
	engine.store()[undoKey] = kv::Value{0x01};

	EXPECT_FALSE(recorder.restoreToCheckpointVersion(kCheckpointVersion));
}

TEST(MetadataUndoRecorderRestore, DetachedPathRejectsMalformedUndoKeys) {
	EXPECT_EXIT(
	    {
		    hstorage::Storage::reset(new hstorage::MemStorage());
		    gMetadata = new FilesystemMetadata;
		    gFSOperations = std::make_unique<FilesystemOperationsBase>(
		        std::make_unique<FilesystemNodeOperationsBase>());

		    std::vector<kv::Key> malformedKeys;
		    // Missing the detached inode after the parent-zero discriminator.
		    auto truncatedKey =
		        kv::encodeKeyBE(kEdgeUndoKeyPrefix, kCheckpointVersion, inode_t{0});
		    truncatedKey.push_back(0xff);
		    malformedKeys.push_back(std::move(truncatedKey));
		    // Detached-path keys have no payload after the inode.
		    auto oversizedKey =
		        kv::encodeKeyBE(kEdgeUndoKeyPrefix, kCheckpointVersion, inode_t{0}, inode_t{41});
		    oversizedKey.push_back(0xff);
		    malformedKeys.push_back(std::move(oversizedKey));
		    // Inode 0 is only the detached-path discriminator, never a detached inode.
		    malformedKeys.push_back(
		        kv::encodeKeyBE(kEdgeUndoKeyPrefix, kCheckpointVersion, inode_t{0}, inode_t{0}));

		    for (const kv::Key &malformedKey : malformedKeys) {
			    RecordingKVEngine engine;
			    EdgeUndoRecorder recorder(&engine);
			    engine.store()[kv::toBytes(kMetaCheckpointVersionsKey)] =
			        checkpoints::serializeCheckpointVersions({kCheckpointVersion});
			    engine.store()[malformedKey] = {};
			    if (recorder.restoreToCheckpointVersion(kCheckpointVersion)) { std::_Exit(1); }
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(MetadataUndoRecorderRestore, DetachedPathRejectsMalformedUndoValues) {
	for (const kv::Value &malformedValue : {kv::Value{static_cast<uint8_t>(FSNodeType::kFile), 'p'},
	                                        kv::Value{static_cast<uint8_t>(FSNodeType::kTrash)}}) {
		RecordingKVEngine engine;
		EdgeUndoRecorder recorder(&engine);

		constexpr inode_t kInode = 41;
		engine.store()[kv::toBytes(kMetaCheckpointVersionsKey)] =
		    checkpoints::serializeCheckpointVersions({kCheckpointVersion});
		engine
		    .store()[kv::encodeKeyBE(kEdgeUndoKeyPrefix, kCheckpointVersion, inode_t{0}, kInode)] =
		    malformedValue;

		EXPECT_FALSE(recorder.restoreToCheckpointVersion(kCheckpointVersion));
	}
}

TEST(MetadataUndoRecorderRestore, XAttrRejectsMalformedUndoKeys) {
	EXPECT_EXIT(
	    {
		    gMetadata = new FilesystemMetadata;

		    std::vector<kv::Key> malformedKeys;
		    // Missing both the inode and attribute name.
		    malformedKeys.push_back(kv::encodeKeyBE(kXAttrUndoKeyPrefix, kCheckpointVersion));
		    // Attribute names must be non-empty.
		    malformedKeys.push_back(
		        kv::encodeKeyBE(kXAttrUndoKeyPrefix, kCheckpointVersion, inode_t{41}));
		    // Attribute names must fit the metadata name-length limit.
		    auto oversizedName =
		        kv::encodeKeyBE(kXAttrUndoKeyPrefix, kCheckpointVersion, inode_t{41});
		    oversizedName.insert(oversizedName.end(), SFS_XATTR_NAME_MAX + 1, uint8_t{0x78});
		    malformedKeys.push_back(std::move(oversizedName));
		    // Inode 0 is not a valid extended-attribute owner.
		    auto zeroInode =
		        kv::encodeKeyBE(kXAttrUndoKeyPrefix, kCheckpointVersion, inode_t{0});
		    kv::appendStr(zeroInode, "user.test");
		    malformedKeys.push_back(std::move(zeroInode));

		    for (const kv::Key &malformedKey : malformedKeys) {
			    RecordingKVEngine engine;
			    XAttrUndoRecorder recorder(&engine);
			    engine.store()[kv::toBytes(kMetaCheckpointVersionsKey)] =
			        checkpoints::serializeCheckpointVersions({kCheckpointVersion});
			    engine.store()[malformedKey] = kv::Value{0x00};
			    if (recorder.restoreToCheckpointVersion(kCheckpointVersion)) { std::_Exit(1); }
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(MetadataUndoRecorderRestore, XAttrRejectsMalformedUndoValues) {
	kv::Value oversizedPresent(SFS_XATTR_SIZE_MAX + 2, 0);
	oversizedPresent[0] = 0x01;
	const std::array<kv::Value, 4> malformedValues{
	    kv::Value{},
	    kv::Value{0x00, 0xff},
	    kv::Value{0x02},
	    std::move(oversizedPresent),
	};

	for (const auto &malformedValue : malformedValues) {
		SCOPED_TRACE(::testing::PrintToString(malformedValue));
		RecordingKVEngine engine;
		XAttrUndoRecorder recorder(&engine);

		constexpr inode_t kInode = 41;
		kv::Key undoKey = kv::encodeKeyBE(kXAttrUndoKeyPrefix, kCheckpointVersion, kInode);
		kv::appendStr(undoKey, "user.test");
		engine.store()[kv::toBytes(kMetaCheckpointVersionsKey)] =
		    checkpoints::serializeCheckpointVersions({kCheckpointVersion});
		engine.store()[undoKey] = malformedValue;

		EXPECT_FALSE(recorder.restoreToCheckpointVersion(kCheckpointVersion));
	}
}

TEST(MetadataUndoRecorderRestore, XAttrRejectsOversizedUndoName) {
	RecordingKVEngine engine;
	XAttrUndoRecorder recorder(&engine);

	constexpr inode_t kInode = 41;
	kv::Key undoKey = kv::encodeKeyBE(kXAttrUndoKeyPrefix, kCheckpointVersion, kInode);
	undoKey.insert(undoKey.end(), SFS_XATTR_NAME_MAX + 1, 'x');
	engine.store()[kv::toBytes(kMetaCheckpointVersionsKey)] =
	    checkpoints::serializeCheckpointVersions({kCheckpointVersion});
	engine.store()[undoKey] = kv::Value{0x00};

	EXPECT_FALSE(recorder.restoreToCheckpointVersion(kCheckpointVersion));
}

TEST(MetadataUndoRecorderRestore, QuotaRejectsMalformedUndoKeys) {
	EXPECT_EXIT(
	    {
		    gMetadata = new FilesystemMetadata;

		    std::vector<kv::Key> malformedKeys;
		    // Missing the owner type and id.
		    malformedKeys.push_back(kv::encodeKeyBE(kQuotaUndoKeyPrefix, kCheckpointVersion));
		    // Quota undo keys have no payload after the fixed-size owner identity.
		    auto oversizedKey =
		        quotaUndoKey(kCheckpointVersion, QuotaOwnerType::kUser, inode_t{41});
		    oversizedKey.push_back(0xff);
		    malformedKeys.push_back(std::move(oversizedKey));
		    // Only user, group, and inode are valid quota owner types.
		    auto invalidOwnerType = kv::encodeKeyBE(kQuotaUndoKeyPrefix, kCheckpointVersion);
		    invalidOwnerType.push_back(0xff);
		    appendBigEndian(invalidOwnerType, inode_t{41});
		    malformedKeys.push_back(std::move(invalidOwnerType));
		    // User and group id 0 are valid, but inode 0 is not.
		    malformedKeys.push_back(
		        quotaUndoKey(kCheckpointVersion, QuotaOwnerType::kInode, inode_t{0}));

		    for (const kv::Key &malformedKey : malformedKeys) {
			    RecordingKVEngine engine;
			    QuotaUndoRecorder recorder(&engine);
			    engine.store()[kv::toBytes(kMetaCheckpointVersionsKey)] =
			        checkpoints::serializeCheckpointVersions({kCheckpointVersion});
			    engine.store()[malformedKey] = kv::Value{0x00};
			    if (recorder.restoreToCheckpointVersion(kCheckpointVersion)) { std::_Exit(1); }
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(MetadataUndoRecorderRestore, QuotaRejectsMalformedUndoValues) {
	kv::Value unknownTag = quotaUndoValue({1, 2, 3, 4});
	unknownTag[0] = 0x02;
	kv::Value oversizedPresent = quotaUndoValue({1, 2, 3, 4});
	oversizedPresent.push_back(0xff);
	const std::array<kv::Value, 5> malformedValues{
	    kv::Value{},           kv::Value{0x00, 0xff},       kv::Value{0x01},
	    std::move(unknownTag), std::move(oversizedPresent),
	};

	for (const auto &malformedValue : malformedValues) {
		SCOPED_TRACE(::testing::PrintToString(malformedValue));
		RecordingKVEngine engine;
		QuotaUndoRecorder recorder(&engine);

		constexpr inode_t kOwnerId = 41;
		engine.store()[kv::toBytes(kMetaCheckpointVersionsKey)] =
		    checkpoints::serializeCheckpointVersions({kCheckpointVersion});
		engine.store()[quotaUndoKey(kCheckpointVersion, QuotaOwnerType::kUser, kOwnerId)] =
		    malformedValue;

		EXPECT_FALSE(recorder.restoreToCheckpointVersion(kCheckpointVersion));
	}
}

TEST(MetadataUndoRecorderRestore, NodeRejectsMalformedUndoKeys) {
	EXPECT_EXIT(
	    {
		    hstorage::Storage::reset(new hstorage::MemStorage());
		    gMetadata = new FilesystemMetadata;
		    gFSOperations = std::make_unique<FilesystemOperationsBase>(
		        std::make_unique<FilesystemNodeOperationsBase>());

		    std::vector<kv::Key> malformedKeys;
		    // Missing inode.
		    malformedKeys.push_back(kv::encodeKeyBE(kNodeUndoKeyPrefix, kCheckpointVersion));
		    // Inode 0 is not a valid NODE_ identity.
		    malformedKeys.push_back(
		        kv::encodeKeyBE(kNodeUndoKeyPrefix, kCheckpointVersion, inode_t{0}));

		    for (const kv::Key &malformedKey : malformedKeys) {
			    RecordingKVEngine engine;
			    NodeUndoRecorder recorder(&engine);
			    engine.store()[kv::toBytes(kMetaCheckpointVersionsKey)] =
			        checkpoints::serializeCheckpointVersions({kCheckpointVersion});
			    engine.store()[malformedKey] = {};
			    if (recorder.restoreToCheckpointVersion(kCheckpointVersion)) { std::_Exit(1); }
		    }
		    std::_Exit(0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(MetadataUndoRecorderRestore, NodeRejectsMalformedUndoValues) {
	kv::Value truncatedFile(FSNode::kNodeHeaderSize + sizeof(uint64_t), 0);
	truncatedFile[0] = static_cast<uint8_t>(FSNodeType::kFile);
	appendBigEndian(truncatedFile, uint32_t{1});
	appendBigEndian(truncatedFile, uint16_t{0});

	kv::Value truncatedSymlink(FSNode::kNodeHeaderSize, 0);
	truncatedSymlink[0] = static_cast<uint8_t>(FSNodeType::kSymlink);
	appendBigEndian(truncatedSymlink, uint32_t{1});

	kv::Value oversizedDirectory(FSNode::kNodeHeaderSize + 1, 0);
	oversizedDirectory[0] = static_cast<uint8_t>(FSNodeType::kDirectory);

	const std::array<kv::Value, 5> malformedValues{
	    kv::Value{0xff},
	    kv::Value{static_cast<uint8_t>(FSNodeType::kDirectory)},
	    std::move(oversizedDirectory),
	    std::move(truncatedFile),
	    std::move(truncatedSymlink),
	};

	for (const auto &malformedValue : malformedValues) {
		SCOPED_TRACE(::testing::PrintToString(malformedValue));
		RecordingKVEngine engine;
		NodeUndoRecorder recorder(&engine);

		constexpr inode_t kInode = 41;
		engine.store()[kv::toBytes(kMetaCheckpointVersionsKey)] =
		    checkpoints::serializeCheckpointVersions({kCheckpointVersion});
		engine.store()[kv::encodeKeyBE(kNodeUndoKeyPrefix, kCheckpointVersion, kInode)] =
		    malformedValue;

		EXPECT_FALSE(recorder.restoreToCheckpointVersion(kCheckpointVersion));
	}
}

TEST(MetadataUndoRecorderRestore, NodeRejectsMismatchedUndoInode) {
	RecordingKVEngine engine;
	NodeUndoRecorder recorder(&engine);

	constexpr inode_t kUndoKeyInode = 41;
	FSNodeDirectory serializedNode;
	serializedNode.id = 42;
	kv::Value undoValue(serializedNode.serializedSize());
	uint8_t *destination = undoValue.data();
	serializedNode.serialize(&destination);
	engine.store()[kv::toBytes(kMetaCheckpointVersionsKey)] =
	    checkpoints::serializeCheckpointVersions({kCheckpointVersion});
	engine.store()[kv::encodeKeyBE(kNodeUndoKeyPrefix, kCheckpointVersion, kUndoKeyInode)] =
	    std::move(undoValue);

	EXPECT_FALSE(recorder.restoreToCheckpointVersion(kCheckpointVersion));
}

TEST(MetadataUndoRecorderRetry, ChunkPreservesOriginalPreimage) {
	RecordingKVEngine engine;
	ChunkUndoRecorder recorder(&engine);

	constexpr uint64_t kChunkId = 41;
	const kv::Key liveKey = kv::encodeKeyBE(kChunkLatestKeyPrefix, kChunkId);
	const kv::Key undoKey = kv::encodeKeyBE(kChunkUndoKeyPrefix, kCheckpointVersion, kChunkId);
	const kv::Value originalValue = chunkValue(1, 2, 3);
	const kv::Value updatedValue = chunkValue(4, 5, 6);
	const kv::Value laterValue = chunkValue(7, 8, 9);
	engine.store()[liveKey] = originalValue;

	const MetadataMutation mutation = ChunkSetMutation{.chunkId = kChunkId, .liveKey = liveKey};
	expectFailedFirstTouchRetryPreservesPreimage(
	    recorder, engine.store(), mutation, undoKey, originalValue,
	    [&](RecordingTransaction &transaction, bool laterMutation) {
		    transaction.set(liveKey, laterMutation ? laterValue : updatedValue);
	    });
	EXPECT_EQ(engine.store().at(liveKey), laterValue);
}

TEST(MetadataUndoRecorderRetry, NodePreservesOriginalPreimage) {
	RecordingKVEngine engine;
	NodeUndoRecorder recorder(&engine);

	constexpr inode_t kInode = 42;
	const kv::Key liveKey = kv::encodeKeyBE(kNodeKeyPrefix, kInode);
	const kv::Key undoKey = kv::encodeKeyBE(kNodeUndoKeyPrefix, kCheckpointVersion, kInode);
	const kv::Value originalValue{0x01, 0x02, 0x03};
	const kv::Value updatedValue{0x04, 0x05, 0x06};
	const kv::Value laterValue{0x07, 0x08, 0x09};
	engine.store()[liveKey] = originalValue;

	const MetadataMutation mutation = NodeSetMutation{.inode = kInode, .liveKey = liveKey};
	expectFailedFirstTouchRetryPreservesPreimage(
	    recorder, engine.store(), mutation, undoKey, originalValue,
	    [&](RecordingTransaction &transaction, bool laterMutation) {
		    transaction.set(liveKey, laterMutation ? laterValue : updatedValue);
	    });
	EXPECT_EQ(engine.store().at(liveKey), laterValue);
}

TEST(MetadataUndoRecorderRetry, EdgePreservesOriginalPreimage) {
	RecordingKVEngine engine;
	EdgeUndoRecorder recorder(&engine);

	constexpr inode_t kParentId = 43;
	constexpr inode_t kOriginalChildId = 44;
	constexpr inode_t kUpdatedChildId = 45;
	constexpr inode_t kLaterChildId = 46;
	const HString name("entry");
	const kv::Key liveKey = namedKey(kEdgeKeyPrefix, kParentId, name);
	kv::Key undoKey = kv::encodeKeyBE(kEdgeUndoKeyPrefix, kCheckpointVersion, kParentId);
	kv::appendStr(undoKey, name);
	const kv::Value originalValue = kv::toBytesBE(kOriginalChildId);
	const kv::Value updatedValue = kv::toBytesBE(kUpdatedChildId);
	const kv::Value laterValue = kv::toBytesBE(kLaterChildId);
	engine.store()[liveKey] = originalValue;

	const MetadataMutation mutation = EdgeSetMutation{
	    .parentId = kParentId,
	    .childId = kUpdatedChildId,
	    .name = name,
	    .liveKey = liveKey,
	};
	expectFailedFirstTouchRetryPreservesPreimage(
	    recorder, engine.store(), mutation, undoKey, originalValue,
	    [&](RecordingTransaction &transaction, bool laterMutation) {
		    transaction.set(liveKey, laterMutation ? laterValue : updatedValue);
	    });
	EXPECT_EQ(engine.store().at(liveKey), laterValue);
}

TEST(MetadataUndoRecorderRetry, DetachedPathPreservesOriginalPreimage) {
	RecordingKVEngine engine;
	EdgeUndoRecorder recorder(&engine);

	constexpr inode_t kInode = 47;
	const kv::Key trashKey = kv::encodeKeyBE(kTrashPathKeyPrefix, kInode);
	const kv::Key reservedKey = kv::encodeKeyBE(kReservedPathKeyPrefix, kInode);
	const kv::Key undoKey =
	    kv::encodeKeyBE(kEdgeUndoKeyPrefix, kCheckpointVersion, inode_t{0}, kInode);
	const kv::Value originalValue = kv::toBytes("original/path");
	const kv::Value updatedValue = kv::toBytes("updated/path");
	const kv::Value laterValue = kv::toBytes("later/path");
	kv::Value expectedUndoValue{static_cast<uint8_t>(FSNodeType::kTrash)};
	expectedUndoValue.insert(expectedUndoValue.end(), originalValue.begin(), originalValue.end());
	engine.store()[trashKey] = originalValue;

	const MetadataMutation mutation =
	    DetachedPathSetMutation{.inode = kInode, .nodeType = FSNodeType::kReserved};
	expectFailedFirstTouchRetryPreservesPreimage(
	    recorder, engine.store(), mutation, undoKey, expectedUndoValue,
	    [&](RecordingTransaction &transaction, bool laterMutation) {
		    transaction.remove(trashKey);
		    transaction.set(reservedKey, laterMutation ? laterValue : updatedValue);
	    });
	EXPECT_FALSE(engine.store().contains(trashKey));
	EXPECT_EQ(engine.store().at(reservedKey), laterValue);
}

TEST(MetadataUndoRecorderRetry, XAttrPreservesOriginalPreimage) {
	RecordingKVEngine engine;
	XAttrUndoRecorder recorder(&engine);

	constexpr inode_t kInode = 47;
	const std::vector<uint8_t> name{'u', 's', 'e', 'r', '.', 'k', 'e', 'y'};
	kv::Key liveKey = kv::encodeKeyBE(kXAttrKeyPrefix, kInode);
	liveKey.insert(liveKey.end(), name.begin(), name.end());
	kv::Key undoKey = kv::encodeKeyBE(kXAttrUndoKeyPrefix, kCheckpointVersion, kInode);
	undoKey.insert(undoKey.end(), name.begin(), name.end());
	const kv::Value originalValue{0x10, 0x11};
	const kv::Value updatedValue{0x20, 0x21};
	const kv::Value laterValue{0x30, 0x31};
	kv::Value expectedUndoValue{0x01};
	expectedUndoValue.insert(expectedUndoValue.end(), originalValue.begin(), originalValue.end());
	engine.store()[liveKey] = originalValue;

	const MetadataMutation mutation = XAttrSetMutation{
	    .inode = kInode,
	    .name = name,
	    .liveKey = liveKey,
	};
	expectFailedFirstTouchRetryPreservesPreimage(
	    recorder, engine.store(), mutation, undoKey, expectedUndoValue,
	    [&](RecordingTransaction &transaction, bool laterMutation) {
		    transaction.set(liveKey, laterMutation ? laterValue : updatedValue);
	    });
	EXPECT_EQ(engine.store().at(liveKey), laterValue);
}

TEST(MetadataUndoRecorderRetry, QuotaPreservesOriginalPreimage) {
	RecordingKVEngine engine;
	QuotaUndoRecorder recorder(&engine);

	constexpr QuotaOwnerType kOwnerType = QuotaOwnerType::kUser;
	constexpr inode_t kOwnerId = 48;
	const QuotaKeys keys = quotaKeys(kOwnerType, kOwnerId);
	const QuotaLimits originalLimits{10, 20, 30, 40};
	const QuotaLimits updatedLimits{11, 21, 31, 41};
	const QuotaLimits laterLimits{12, 22, 32, 42};
	for (size_t i = 0; i < keys.size(); ++i) {
		engine.store()[keys[i]] = kv::toBytesBE(originalLimits[i]);
	}

	const kv::Key ownerPrefix = quotaOwnerPrefix(kOwnerType, kOwnerId);
	const MetadataMutation mutation = QuotaSetMutation{
	    .ownerType = kOwnerType,
	    .ownerId = kOwnerId,
	    .rangeBegin = ownerPrefix,
	    .rangeEnd = kv::prefixEnd(ownerPrefix),
	};
	const kv::Key undoKey = quotaUndoKey(kCheckpointVersion, kOwnerType, kOwnerId);
	expectFailedFirstTouchRetryPreservesPreimage(
	    recorder, engine.store(), mutation, undoKey, quotaUndoValue(originalLimits),
	    [&](RecordingTransaction &transaction, bool laterMutation) {
		    setQuotaLimits(transaction, keys, laterMutation ? laterLimits : updatedLimits);
	    });
	for (size_t i = 0; i < keys.size(); ++i) {
		EXPECT_EQ(engine.store().at(keys[i]), kv::toBytesBE(laterLimits[i]));
	}
}
