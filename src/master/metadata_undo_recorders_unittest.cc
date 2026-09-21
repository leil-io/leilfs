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
#include "master/chunks.h"
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
#include "master/metadata_section_bootstrap_fdb.h"
#include "master/metadata_xattr_undo_recorder.h"
#include "protocol/SFSCommunication.h"

struct MetadataBackendForklessTestAccess {
	static void prepareCheckpointLoad(MetadataBackendForkless &backend, kv::IKVEngine *kvEngine,
	                                  uint64_t checkpointVersion) {
		backend.checkpointManager_ = std::make_unique<MetadataCheckpointManager>(kvEngine);
		backend.loadedCheckpointDescriptor_.metadataVersion = checkpointVersion;
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

TEST(MetadataSectionBootstrapFDBTest, PreservesInitializedStoreMetadata) {
	RecordingKVEngine engine;
	constexpr inode_t kCurrentMaxInodeId = 900;
	constexpr uint64_t kCurrentVersion = 300;
	constexpr uint32_t kCurrentNextSessionId = 17;
	const std::vector<uint64_t> currentCheckpointVersions{200, kCurrentVersion};

	// Header finalization is also guarded against another initializer publishing META_HEADER after
	// bootstrap eligibility was checked. It must leave that initialized store descriptor and its
	// retained-checkpoint catalog untouched.
	engine.store()[kv::toBytes(kMetaHeaderKey)] = kv::toBytes(SFSSIGNATURE "M 2.9");
	engine.store()[kv::toBytes(kMetaFormatKey)] = kv::toBytes("1.0");
	engine.store()[kv::toBytes(kMetaMaxInodeIdKey)] = kv::toBytesBE(kCurrentMaxInodeId);
	engine.store()[kv::toBytes(kMetaVersionKey)] = kv::toBytesBE(kCurrentVersion);
	engine.store()[kv::toBytes(kMetaNextSessionKey)] = kv::toBytesBE(kCurrentNextSessionId);
	engine.store()[kv::toBytes(kMetaCheckpointVersionsKey)] =
	    checkpoints::serializeCheckpointVersions(currentCheckpointVersions);
	const DurableStore initializedStore = engine.store();

	MetadataSectionBootstrapFDB bootstrap(&engine);
	ASSERT_EQ(MetadataSectionBootstrapFDBTestAccess::saveMetadataHeader(
	              bootstrap, /*maxInodeId=*/42, /*metadataVersion=*/123, /*nextSessionId=*/7),
	          kOpSuccess);

	EXPECT_EQ(engine.store(), initializedStore);
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

TEST(MetadataUndoRecorderRestore, ChunkRejectsZeroUndoId) {
	ASSERT_EQ(1, chunk_strinit());
	RecordingKVEngine engine;
	ChunkUndoRecorder recorder(&engine);
	engine.store()[kv::toBytes(kMetaCheckpointVersionsKey)] =
	    checkpoints::serializeCheckpointVersions({kCheckpointVersion});
	// Chunk ID zero denotes a hole; even an empty undo value is malformed for it.
	engine.store()[kv::encodeKeyBE(kChunkUndoKeyPrefix, kCheckpointVersion, uint64_t{0})] = {};

	EXPECT_FALSE(recorder.restoreToCheckpointVersion(kCheckpointVersion));
	chunk_unload();
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

TEST(MetadataUndoRecorderRestore, EdgeRejectsOversizedUndoName) {
	EXPECT_EXIT(
	    {
		    hstorage::Storage::reset(new hstorage::MemStorage());
		    gMetadata = new FilesystemMetadata;
		    gFSOperations = std::make_unique<FilesystemOperationsBase>(
		        std::make_unique<FilesystemNodeOperationsBase>());

		    auto restoreWithName = [](size_t nameLength) {
			    RecordingKVEngine engine;
			    EdgeUndoRecorder recorder(&engine);
			    engine.store()[kv::toBytes(kMetaCheckpointVersionsKey)] =
			        checkpoints::serializeCheckpointVersions({kCheckpointVersion});
			    kv::Key key = kv::encodeKeyBE(kEdgeUndoKeyPrefix, kCheckpointVersion, inode_t{41});
			    key.insert(key.end(), nameLength, uint8_t{'n'});
			    engine.store()[key] = {};  // Removing an already-absent edge is otherwise valid.
			    return recorder.restoreToCheckpointVersion(kCheckpointVersion);
		    };

		    if (!restoreWithName(SFS_NAME_MAX)) { std::_Exit(2); }
		    std::_Exit(restoreWithName(SFS_NAME_MAX + 1) ? 1 : 0);
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

TEST(MetadataUndoRecorderRetry, XAttrRemovalPreservesOriginalPreimage) {
	RecordingKVEngine engine;
	XAttrUndoRecorder recorder(&engine);

	constexpr inode_t kInode = 48;
	const std::vector<uint8_t> name{'u', 's', 'e', 'r', '.', 'g', 'o', 'n', 'e'};
	kv::Key liveKey = kv::encodeKeyBE(kXAttrKeyPrefix, kInode);
	liveKey.insert(liveKey.end(), name.begin(), name.end());
	kv::Key undoKey = kv::encodeKeyBE(kXAttrUndoKeyPrefix, kCheckpointVersion, kInode);
	undoKey.insert(undoKey.end(), name.begin(), name.end());
	const kv::Value originalValue{0x10, 0x11, 0x12};
	kv::Value expectedUndoValue{0x01};
	expectedUndoValue.insert(expectedUndoValue.end(), originalValue.begin(), originalValue.end());
	engine.store()[liveKey] = originalValue;

	// Inode cleanup now emits one removal per name. A failed batch must not poison its retry,
	// and a later removal must not replace the original pre-image with a tombstone.
	const MetadataMutation mutation = XAttrRemoveMutation{
	    .inode = kInode,
	    .name = name,
	    .liveKey = liveKey,
	};
	expectFailedFirstTouchRetryPreservesPreimage(
	    recorder, engine.store(), mutation, undoKey, expectedUndoValue,
	    [&](RecordingTransaction &transaction, bool /*laterMutation*/) {
		    transaction.remove(liveKey);
	    });
	EXPECT_FALSE(engine.store().contains(liveKey));
}
