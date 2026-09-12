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
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "kv/ifuture.h"
#include "kv/ikv_engine.h"
#include "kv/itransaction.h"
#include "kv/kv_utils.h"
#include "master/filesystem_metadata.h"
#include "master/kv_common_keys.h"
#include "master/metadata_backend_forkless.h"
#include "master/metadata_checkpoint_helpers.h"
#include "master/metadata_chunk_undo_recorder.h"
#include "master/metadata_edge_undo_recorder.h"
#include "master/metadata_node_undo_recorder.h"
#include "master/metadata_quota_undo_recorder.h"
#include "master/metadata_section_bootstrap_fdb.h"
#include "master/metadata_xattr_undo_recorder.h"

struct MetadataBackendForklessTestAccess {
	static void recordDetainedInode(MetadataBackendForkless &backend, inode_t inode,
	                                uint32_t timestamp) {
		backend.onFreeInodeDetained(inode, timestamp);
	}

	static void recordReleasedInode(MetadataBackendForkless &backend, inode_t inode) {
		backend.onFreeInodeReleased(inode);
	}

	static void attachWriter(MetadataBackendForkless &backend, kv::IKVEngine *kvEngine) {
		backend.metadataWriter_ = std::make_unique<MetadataWriterFDB>(kvEngine);
	}

	static void reconcileFreeInodes(MetadataBackendForkless &backend, uint64_t &persisted,
	                                uint64_t &removed) {
		backend.reconcileDirtyFreeInodesToFDB(persisted, removed);
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
