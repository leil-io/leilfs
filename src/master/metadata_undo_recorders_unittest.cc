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
#include "master/filesystem_metadata.h"
#include "master/filesystem_node.h"
#include "master/filesystem_operations.h"
#include "master/filesystem_trash_reserved_files.h"
#include "master/hstring_memstorage.h"
#include "master/kv_common_keys.h"
#include "master/metadata_backend_forkless.h"
#include "master/metadata_checkpoint_helpers.h"
#include "master/metadata_chunk_undo_recorder.h"
#include "master/metadata_node_undo_recorder.h"
#include "master/metadata_section_bootstrap_fdb.h"
#include "protocol/SFSCommunication.h"

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

	// An empty section can request an import even after the store has advanced beyond the source
	// metadata.sfs image. The partial import must leave the initialized store descriptor and its
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

