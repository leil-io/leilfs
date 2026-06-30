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
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>

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
	/// @param appliedSink Optional cross-transaction counter incremented once per buffered
	///   mutation. The writer destroys each transaction after committing it, so a test that needs
	///   to know how many events actually reached the backend has to accumulate outside them.
	explicit NoopTransaction(std::atomic<uint64_t> *appliedSink = nullptr)
	    : appliedSink_(appliedSink) {}

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

	void set(const kv::Key & /*key*/, const kv::Value & /*value*/) override { noteMutation(); }
	void atomicAdd(const kv::Key & /*key*/, const kv::Value & /*delta*/) override {
		noteMutation();
	}
	void atomicMax(const kv::Key & /*key*/, const kv::Value & /*value*/) override {
		noteMutation();
	}
	void remove(const kv::Key & /*key*/) override { noteMutation(); }
	void removeRange(const kv::Key & /*start*/, const kv::Key & /*end*/) override {
		noteMutation();
	}
	// Conflict annotation, not a buffered write: leave mutationCount_ untouched.
	void addReadConflictKey(const kv::Key & /*key*/) override {}

	bool commit() override { return true; }
	// The async pipeline dereferences this future unconditionally when it reaps the commit, so it
	// must not be nullptr. An in-memory backend is durable the moment the mutations are buffered.
	std::unique_ptr<kv::ICommitFuture> commitAsync() override {
		return std::make_unique<kv::ImmediateCommitFuture>(/*success=*/true);
	}
	// Contract forbids nullptr; an in-memory backend has no recovery work, so hand back an
	// already-successful future (this mock never fails a commit, so it is never called).
	std::unique_ptr<kv::IVoidFuture> recoverAsync(int /*backendErrorCode*/) override {
		return std::make_unique<kv::ImmediateVoidFuture>();
	}
	std::optional<int64_t> getCommittedVersion() const override { return std::nullopt; }
	uint64_t mutationCount() const override { return mutationCount_; }

private:
	void noteMutation() {
		++mutationCount_;
		if (appliedSink_ != nullptr) { appliedSink_->fetch_add(1, std::memory_order_relaxed); }
	}

	std::atomic<uint64_t> *appliedSink_;
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

// Engine whose read-write transaction factory throws until released, modelling an FDB client
// failure at transaction-construction time (client not initialised, network thread dead, ...).
// This is the one commit failure the async pipeline cannot observe through
// ICommitFuture::getResult(): the commit is never submitted, so nothing is ever recorded in
// inFlight_ and the worker's reap block -- the usual source of the drained notification -- is
// skipped entirely.
class ThrowingKVEngine : public kv::IKVEngine {
public:
	std::unique_ptr<kv::IReadOnlyTransaction> createReadOnlyTransaction() override {
		return std::make_unique<NoopTransaction>();
	}

	std::unique_ptr<kv::IReadWriteTransaction> createReadWriteTransaction() override {
		if (throwing_.load(std::memory_order_acquire)) {
			throwCount_.fetch_add(1, std::memory_order_relaxed);
			throw std::runtime_error("simulated FDB client failure");
		}
		return std::make_unique<NoopTransaction>(&applied_);
	}

	/// Lets the worker build transactions again, simulating the client recovering.
	void stopThrowing() { throwing_.store(false, std::memory_order_release); }

	/// How many times the factory has thrown, i.e. how many times the worker hit the failure path.
	uint64_t throwCount() const { return throwCount_.load(std::memory_order_relaxed); }

	/// Total mutations buffered across every transaction this engine handed out.
	uint64_t applied() const { return applied_.load(std::memory_order_relaxed); }

private:
	std::atomic<bool> throwing_{true};
	std::atomic<uint64_t> throwCount_{0};
	std::atomic<uint64_t> applied_{0};
};

std::unique_ptr<IMetadataUpdateEvent> makeEvent(uint64_t seed) {
	return std::make_unique<NodeRemoveEvent>(static_cast<inode_t>(seed + 1));
}

/// Spins (bounded) until `predicate` holds, so the async worker's progress is awaited rather than
/// slept on: fast on an idle machine, tolerant on a loaded CI box. Returns whether it held.
template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds limit) {
	const auto deadline = std::chrono::steady_clock::now() + limit;
	while (std::chrono::steady_clock::now() < deadline) {
		if (predicate()) { return true; }
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return predicate();
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

// A transaction that never gets built leaves the pipeline empty, so the worker skips the reap
// block that normally notifies drainedCv_. flushAndWait() must still be released: its predicate is
// already satisfied (the failure sets lastFlushFailed_), and without a notification the checkpoint
// seal would park forever behind a failing FDB client while the worker retried in the background.
TEST(MetadataWriterFDBAsync, FlushAndWaitReturnsWhenTransactionBuildThrows) {
	ThrowingKVEngine engine;
	MetadataWriterFDB writer(&engine, /*checkpointManager=*/nullptr,
	                         MetadataWriterFDB::kDefaultBacklogHighWatermark_,
	                         /*asyncFlush=*/true);

	writer.enqueue(makeEvent(0));

	std::promise<bool> outcome;
	auto released = outcome.get_future();
	std::thread sealer([&] { outcome.set_value(writer.flushAndWait()); });

	const bool returned = released.wait_for(std::chrono::seconds(5)) == std::future_status::ready;

	// Release the worker before joining, whether or not the wait succeeded: on a regression the
	// sealer is parked until something can commit, and joining it would hang the whole suite
	// instead of failing this one test.
	engine.stopThrowing();
	sealer.join();

	EXPECT_TRUE(returned) << "flushAndWait() was not released by the build-failure path; a "
	                         "checkpoint seal would hang while the FDB client is unavailable";
	if (returned) {
		EXPECT_FALSE(released.get()) << "a failed transaction build must be reported as a failed "
		                                "flush, not a successful drain";
	}
}

// A build failure must requeue its batch intact rather than drop it: the changelog is the
// durability record, but the FDB mirror still has to converge once the client recovers.
TEST(MetadataWriterFDBAsync, BuildFailureRequeuesEventsInsteadOfDroppingThem) {
	ThrowingKVEngine engine;
	MetadataWriterFDB writer(&engine, /*checkpointManager=*/nullptr,
	                         MetadataWriterFDB::kDefaultBacklogHighWatermark_,
	                         /*asyncFlush=*/true);

	constexpr size_t kEvents = 16;
	for (size_t i = 0; i < kEvents; ++i) { writer.enqueue(makeEvent(i)); }

	// Confirm the worker actually reached the failure path, so the assertions below are not just
	// racing past a queue that was never touched.
	ASSERT_TRUE(waitFor([&] { return engine.throwCount() > 0; }, std::chrono::seconds(5)))
	    << "worker never attempted to build a transaction";
	EXPECT_EQ(writer.pendingCount(), kEvents) << "a failed build must requeue its whole batch";
	EXPECT_EQ(engine.applied(), 0U) << "nothing may reach the backend while the build fails";

	// Once the client recovers, every event must land exactly once and in one drain.
	engine.stopThrowing();
	EXPECT_TRUE(writer.flushAndWait());
	EXPECT_EQ(writer.pendingCount(), 0U);
	EXPECT_EQ(engine.applied(), kEvents) << "events were lost or duplicated across the requeue";
}
