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

#include <gtest/gtest.h>
#include <cstdint>
#include <memory>
#include <optional>

#include "kv/ifuture.h"
#include "kv/ikv_engine.h"
#include "kv/itransaction.h"
#include "master/metadata_writer_fdb.h"

namespace {

// Minimal in-memory transaction: writes are no-ops and commit always succeeds, so a flush drains
// the queue without a real FDB cluster. getApproximateSize() is left at the interface default
// (nullopt), so the writer falls back to count-based batching.
class NoopTransaction : public kv::IReadWriteTransaction {
public:
	std::optional<kv::Value> get(const kv::Key & /*key*/) override { return std::nullopt; }
	std::optional<kv::Value> getSnapshot(const kv::Key & /*key*/) override { return std::nullopt; }
	std::unique_ptr<kv::IFuture> getAsync(const kv::Key & /*key*/) override { return nullptr; }
	std::unique_ptr<kv::IFuture> getSnapshotAsync(const kv::Key & /*key*/) override {
		return nullptr;
	}
	kv::GetRangeResult getRange(const kv::KeySelector & /*start*/, const kv::KeySelector & /*end*/,
	                            int /*limit*/) override {
		return {{}, false};
	}
	std::unique_ptr<kv::IRangeFuture> getRangeAsync(const kv::KeySelector & /*start*/,
	                                                const kv::KeySelector & /*end*/,
	                                                int /*limit*/) override {
		return nullptr;
	}

	void set(const kv::Key & /*key*/, const kv::Value & /*value*/) override { ++mutationCount_; }
	void atomicAdd(const kv::Key & /*key*/, const kv::Value & /*delta*/) override {
		++mutationCount_;
	}
	void atomicMax(const kv::Key & /*key*/, const kv::Value & /*value*/) override {
		++mutationCount_;
	}
	void remove(const kv::Key & /*key*/) override { ++mutationCount_; }
	void removeRange(const kv::Key & /*start*/, const kv::Key & /*end*/) override {
		++mutationCount_;
	}
	// Conflict annotation, not a buffered write: leave mutationCount_ untouched.
	void addReadConflictKey(const kv::Key & /*key*/) override {}

	bool commit() override { return true; }
	std::unique_ptr<kv::ICommitFuture> commitAsync() override { return nullptr; }
	// Contract forbids nullptr; an in-memory backend has no recovery work, so hand back an
	// already-successful future (this mock never fails a commit, so it is never called).
	std::unique_ptr<kv::IVoidFuture> recoverAsync(int /*backendErrorCode*/) override {
		return std::make_unique<kv::ImmediateVoidFuture>();
	}
	std::optional<int64_t> getCommittedVersion() const override { return std::nullopt; }
	uint64_t mutationCount() const override { return mutationCount_; }

private:
	uint64_t mutationCount_{0};
};

class NoopKVEngine : public kv::IKVEngine {
public:
	std::unique_ptr<kv::IReadOnlyTransaction> createReadOnlyTransaction() override {
		return std::make_unique<NoopTransaction>();
	}
	std::unique_ptr<kv::IReadWriteTransaction> createReadWriteTransaction() override {
		return std::make_unique<NoopTransaction>();
	}
};

std::unique_ptr<IMetadataUpdateEvent> makeEvent(uint64_t seed) {
	return std::make_unique<NodeRemoveEvent>(static_cast<inode_t>(seed + 1));
}

}  // namespace

// The pending-update backlog is a health signal: crossing the injected high-watermark flips
// isBacklogCritical() and logs an escalation; growth within a step does not re-log; the next step
// re-logs; draining below the low-watermark clears the state; and after recovery the signal
// re-arms. A no-op engine lets the flush drain without a real FDB cluster.
TEST(MetadataWriterFDBBacklog, SignalEscalatesAndRecovers) {
	NoopKVEngine engine;
	constexpr size_t kHighWatermark = 8;  // step == high-watermark, low-watermark == 4
	MetadataWriterFDB writer(&engine, /*checkpointManager=*/nullptr, kHighWatermark);

	auto enqueueN = [&](size_t count) {
		for (size_t i = 0; i < count; ++i) { writer.enqueue(makeEvent(i)); }
	};

	// Below the high-watermark: not critical, nothing logged.
	enqueueN(kHighWatermark - 1);
	EXPECT_FALSE(writer.isBacklogCritical());
	EXPECT_EQ(writer.backlogEscalationCount(), 0U);

	// Crossing the high-watermark: critical, one escalation.
	enqueueN(1);  // depth == kHighWatermark
	EXPECT_TRUE(writer.isBacklogCritical());
	EXPECT_EQ(writer.backlogEscalationCount(), 1U);

	// Growing within the same step must not re-log.
	enqueueN(kHighWatermark - 1);  // depth in (high, 2*high)
	EXPECT_EQ(writer.backlogEscalationCount(), 1U);

	// Crossing the next step re-logs.
	enqueueN(1);  // depth == 2*high
	EXPECT_EQ(writer.backlogEscalationCount(), 2U);

	// Draining below the low-watermark clears the critical state.
	ASSERT_TRUE(writer.flush(MetadataWriterFDB::FlushMode::kDrainUntilEmpty));
	EXPECT_EQ(writer.pendingCount(), 0U);
	EXPECT_FALSE(writer.isBacklogCritical());

	// After recovery the signal re-arms: crossing again is a fresh escalation.
	enqueueN(kHighWatermark);
	EXPECT_TRUE(writer.isBacklogCritical());
	EXPECT_EQ(writer.backlogEscalationCount(), 3U);

	// Drain so the destructor's final flush has nothing to do.
	ASSERT_TRUE(writer.flush(MetadataWriterFDB::FlushMode::kDrainUntilEmpty));
	EXPECT_EQ(writer.pendingCount(), 0U);
}
