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

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "kv/ifuture.h"
#include "kv/ikv_engine.h"
#include "kv/itransaction.h"
#include "kv/kv_utils.h"
#include "master/filesystem_metadata.h"
#include "master/kv_common_keys.h"
#include "master/metadata_checkpoint_helpers.h"
#include "master/metadata_quota_undo_recorder.h"

namespace {

using DurableStore = std::map<kv::Key, kv::Value>;

class StoreReadOnlyTransaction final : public kv::IReadOnlyTransaction {
public:
	explicit StoreReadOnlyTransaction(const DurableStore &store) : store_(store) {}

	std::optional<kv::Value> get(const kv::Key &key) override {
		if (const auto iterator = store_.find(key); iterator != store_.end()) {
			return iterator->second;
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
			throw std::logic_error("StoreReadOnlyTransaction does not support selector offsets");
		}
		if (limit <= 0) {
			throw std::invalid_argument("StoreReadOnlyTransaction requires a positive range limit");
		}

		auto iterator = start.isInclusive() ? store_.lower_bound(start.getKey())
		                                    : store_.upper_bound(start.getKey());
		auto isBeforeEnd = [&end](const kv::Key &key) {
			return end.isInclusive() ? key <= end.getKey() : key < end.getKey();
		};

		std::vector<kv::KeyValuePair> pairs;
		for (; iterator != store_.end() && isBeforeEnd(iterator->first); ++iterator) {
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

private:
	const DurableStore &store_;
};

class StoreKVEngine final : public kv::IKVEngine {
public:
	std::unique_ptr<kv::IReadOnlyTransaction> createReadOnlyTransaction() override {
		return std::make_unique<StoreReadOnlyTransaction>(store_);
	}

	std::unique_ptr<kv::IReadWriteTransaction> createReadWriteTransaction() override {
		return nullptr;
	}

	DurableStore &store() { return store_; }

private:
	DurableStore store_;
};

kv::Key quotaUndoKey(uint64_t checkpointVersion, uint8_t ownerType, inode_t ownerId) {
	kv::Key key = kv::encodeKeyBE(kQuotaUndoKeyPrefix, checkpointVersion);
	key.push_back(ownerType);
	const kv::Value ownerIdBytes = kv::toBytesBE(ownerId);
	key.insert(key.end(), ownerIdBytes.begin(), ownerIdBytes.end());
	return key;
}

class QuotaRecoveryStateTest : public ::testing::Test {
protected:
	void SetUp() override {
		previousMetadata_ = gMetadata;
		gMetadata = new FilesystemMetadata;
		engine_.store()[kv::toBytes(kMetaCheckpointVersionsKey)] =
		    checkpoints::serializeCheckpointVersions({kCheckpointVersion});
	}

	void TearDown() override {
		delete gMetadata;
		gMetadata = previousMetadata_;
	}

	static constexpr uint64_t kCheckpointVersion = 17;
	static constexpr inode_t kOwnerId = 42;

	FilesystemMetadata *previousMetadata_ = nullptr;
	StoreKVEngine engine_;
};

TEST_F(QuotaRecoveryStateTest, TombstoneClearsLimitsButPreservesReconstructedUsage) {
	constexpr QuotaOwnerType kOwnerType = QuotaOwnerType::kUser;
	auto &quotaDatabase = gMetadata->quotaDatabase;
	quotaDatabase.set(kOwnerType, kOwnerId, QuotaRigor::kUsed, QuotaResource::kInodes, 3);
	quotaDatabase.set(kOwnerType, kOwnerId, QuotaRigor::kUsed, QuotaResource::kSize, 4096);
	quotaDatabase.set(kOwnerType, kOwnerId, QuotaRigor::kSoft, QuotaResource::kInodes, 10);
	quotaDatabase.set(kOwnerType, kOwnerId, QuotaRigor::kHard, QuotaResource::kSize, 8192);
	engine_.store()[quotaUndoKey(kCheckpointVersion, static_cast<uint8_t>(kOwnerType), kOwnerId)] =
	    kv::Value{0};

	QuotaUndoRecorder recorder(&engine_);
	ASSERT_TRUE(recorder.restoreToCheckpointVersion(kCheckpointVersion));

	const auto *limits = quotaDatabase.get(kOwnerType, kOwnerId);
	ASSERT_NE(limits, nullptr);
	EXPECT_EQ(
	    (*limits)[static_cast<int>(QuotaRigor::kUsed)][static_cast<int>(QuotaResource::kInodes)],
	    3U);
	EXPECT_EQ(
	    (*limits)[static_cast<int>(QuotaRigor::kUsed)][static_cast<int>(QuotaResource::kSize)],
	    4096U);
	for (const auto rigor : {QuotaRigor::kSoft, QuotaRigor::kHard}) {
		for (const auto resource : {QuotaResource::kInodes, QuotaResource::kSize}) {
			EXPECT_EQ((*limits)[static_cast<int>(rigor)][static_cast<int>(resource)], 0U);
		}
	}
}

TEST_F(QuotaRecoveryStateTest, InvalidUndoOwnerTypeIsIgnored) {
	engine_.store()[quotaUndoKey(kCheckpointVersion, /*ownerType=*/0xff, kOwnerId)] = kv::Value{0};

	QuotaUndoRecorder recorder(&engine_);
	EXPECT_TRUE(recorder.restoreToCheckpointVersion(kCheckpointVersion));
	EXPECT_EQ(gMetadata->quotaDatabase.get(QuotaOwnerType::kUser, kOwnerId), nullptr);
	EXPECT_EQ(gMetadata->quotaDatabase.get(QuotaOwnerType::kGroup, kOwnerId), nullptr);
	EXPECT_EQ(gMetadata->quotaDatabase.get(QuotaOwnerType::kInode, kOwnerId), nullptr);
}

}  // namespace
