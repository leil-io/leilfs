/*
   Copyright 2023      Leil Storage OÜ

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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <foundationdb/fdb_c_types.h>

#include "fdb/fdb.h"
#include "fdb/fdb_errors.h"
#include "fdb/fdb_future.h"
#include "slogger/slogger.h"

namespace fdb {

namespace {

/// Process-wide profiling counters. Relaxed atomics: the cost is negligible
/// next to an FDB round-trip and exact ordering between fields is not needed.
struct AtomicOpCounters {
	std::atomic<uint64_t> pointReads{0};
	std::atomic<uint64_t> rangeReads{0};
	std::atomic<uint64_t> sets{0};
	std::atomic<uint64_t> atomicAdds{0};
	std::atomic<uint64_t> clears{0};
	std::atomic<uint64_t> clearRanges{0};
	std::atomic<uint64_t> commits{0};
	std::atomic<uint64_t> commitConflicts{0};
	std::atomic<uint64_t> commitFailures{0};
	std::atomic<uint64_t> readWaitNanos{0};
	std::atomic<uint64_t> commitWaitNanos{0};
};

/// Process-wide counter instance. Namespace-scope with constant initialization
/// (atomic members default to 0), so the disabled path avoids the guard check a
/// function-local static would add on every FDB call.
AtomicOpCounters gOpCounters;

/// Gates op-counter accounting. Off by default: production clusters pay only a
/// single relaxed bool load per FDB call, while tests can switch it on.
std::atomic<bool> gOpCountersEnabled{false};

inline void bump(std::atomic<uint64_t> &counter) {
	if (!gOpCountersEnabled.load(std::memory_order_relaxed)) { return; }
	counter.fetch_add(1, std::memory_order_relaxed);
}

inline void addNanos(std::atomic<uint64_t> &counter, uint64_t nanos) {
	if (!gOpCountersEnabled.load(std::memory_order_relaxed)) { return; }
	counter.fetch_add(nanos, std::memory_order_relaxed);
}

/// Attributes a failed commit to the conflict or failure counter. Gated on accounting
/// because evaluatePredicate() calls into the FDB C API. Matches
/// FDBCommitFuture::getResult: a commit_unknown_result / maybe-committed error is a
/// commitFailure, not a (definitely-not-committed) conflict.
inline void countFailedCommit(fdb_error_t err) {
	if (!opCountersEnabled()) { return; }
	auto &counter = DB::evaluatePredicate(FDB_ERROR_PREDICATE_RETRYABLE_NOT_COMMITTED, err)
	                    ? gOpCounters.commitConflicts
	                    : gOpCounters.commitFailures;
	counter.fetch_add(1, std::memory_order_relaxed);
}

/// Nanoseconds elapsed since `start` on the steady clock, for accumulating the
/// wall time the single MDS thread spends blocked in FDB read/commit futures.
inline uint64_t elapsedNanosSince(std::chrono::steady_clock::time_point start) {
	return static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
	        .count());
}

/// Per-key-prefix read histogram, for attributing the read round-trips a single
/// operation costs to the key families it touches (NODE_, EDGE_, QUOT_, ...).
/// Only populated while op-counter accounting is enabled; guarded by its own
/// mutex because, although MDS reads issue from the single event-loop thread
/// today, the diagnostic must stay correct if that ever changes.
std::mutex gReadPrefixMutex;
std::map<std::string, uint64_t> gReadPrefixHistogram;

/// Extracts the leading printable tag of a key (the run of [A-Z_] bytes the MDS
/// key schema uses before the binary id), e.g. "NODE_", "DIR_PARENT_". Falls
/// back to "<other>" for keys that do not start with such a tag. The scan is
/// capped at 24 bytes, which comfortably covers every current key-family tag and
/// bounds the histogram key length; a hypothetical longer tag would bucket under
/// its first 24 bytes.
std::string readPrefixTag(const kv::Key &key) {
	constexpr size_t kMaxTagLen = 24;
	size_t end = 0;
	while (end < key.size() && end < kMaxTagLen) {
		const auto byte = static_cast<unsigned char>(key[end]);
		const bool isTag = (byte >= 'A' && byte <= 'Z') || byte == '_';
		if (!isTag) { break; }
		++end;
	}
	if (end == 0) { return "<other>"; }
	return std::string(reinterpret_cast<const char *>(key.data()), end);
}

/// Buckets one read by its key prefix when accounting is on.
inline void bumpReadPrefix(const kv::Key &key) {
	if (!gOpCountersEnabled.load(std::memory_order_relaxed)) { return; }
	// Parse the tag (string scan + allocation) outside the lock to keep the
	// global mutex's hold time to just the map insertion.
	std::string tag = readPrefixTag(key);
	std::lock_guard<std::mutex> guard(gReadPrefixMutex);
	++gReadPrefixHistogram[std::move(tag)];
}

}  // namespace

void setOpCountersEnabled(bool enabled) {
	gOpCountersEnabled.store(enabled, std::memory_order_relaxed);
}

bool opCountersEnabled() { return gOpCountersEnabled.load(std::memory_order_relaxed); }

void addReadWaitNanos(uint64_t nanos) { addNanos(gOpCounters.readWaitNanos, nanos); }

FdbOpCounters getOpCounters() {
	auto &c = gOpCounters;
	return {
	    c.pointReads.load(std::memory_order_relaxed),
	    c.rangeReads.load(std::memory_order_relaxed),
	    c.sets.load(std::memory_order_relaxed),
	    c.atomicAdds.load(std::memory_order_relaxed),
	    c.clears.load(std::memory_order_relaxed),
	    c.clearRanges.load(std::memory_order_relaxed),
	    c.commits.load(std::memory_order_relaxed),
	    c.commitConflicts.load(std::memory_order_relaxed),
	    c.commitFailures.load(std::memory_order_relaxed),
	    c.readWaitNanos.load(std::memory_order_relaxed),
	    c.commitWaitNanos.load(std::memory_order_relaxed),
	};
}

std::vector<std::pair<std::string, uint64_t>> getReadPrefixHistogram() {
	std::vector<std::pair<std::string, uint64_t>> out;
	{
		std::lock_guard<std::mutex> guard(gReadPrefixMutex);
		out.assign(gReadPrefixHistogram.begin(), gReadPrefixHistogram.end());
	}
	std::sort(out.begin(), out.end(),
	          [](const auto &lhs, const auto &rhs) { return lhs.second > rhs.second; });
	return out;
}

const uint8_t *toU8(std::string_view str) { return reinterpret_cast<const uint8_t *>(str.data()); }

// Static

fdb_error_t DB::selectAPIVersion(int version) { return fdb_select_api_version(version); }

std::string_view DB::errorMsg(fdb_error_t code) { return fdb_get_error(code); }

bool DB::evaluatePredicate(int predicate_test, fdb_error_t code) {
	return static_cast<bool>(fdb_error_predicate(predicate_test, code));
}

// network

fdb_error_t DB::setNetworkOption(FDBNetworkOption option, std::string_view value /* = {} */) {
	return fdb_network_set_option(option, toU8(value), static_cast<int>(value.length()));
}

// DB

fdb_error_t DB::setOption(FDBDatabaseOption option, std::string_view value) {
	return fdb_database_set_option(db_.get(), option, toU8(value),
	                               static_cast<int>(value.length()));
}

// Transaction

fdb_error_t Transaction::setOption(FDBTransactionOption option, std::string_view value) {
	requireHandle("setOption");
	return fdb_transaction_set_option(tr_.get(), option, toU8(value),
	                                  static_cast<int>(value.length()));
}

void Transaction::requireHandle(const char *operation) const {
	if (tr_) { return; }
	throw std::logic_error(std::string("fdb::Transaction::") + operation +
	                       ": no backend transaction handle");
}

std::optional<kv::Value> Transaction::get(const kv::Key &key, bool snapshot) {
	requireHandle("get");

	bump(gOpCounters.pointReads);
	bumpReadPrefix(key);

	// TEST-ONLY scripted read failure; short-circuits before contacting the cluster and
	// takes exactly the real error path below.
	if (faultInjection_ != nullptr) {
		fdb_error_t injected = FaultInjection::next(faultInjection_->readErrors);
		if (injected != 0) {
			error_ = injected;
			safs::log_err("Transaction::get: injected error: {}", fdb_get_error(injected));
			detail::throwTransactionError(injected);
		}
	}

	UniqueFDBFuture future(
	    fdb_transaction_get(tr_.get(), key.data(), static_cast<int>(key.size()), snapshot));

	const bool profiling = opCountersEnabled();
	const auto waitStart =
	    profiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	error_ = fdb_future_block_until_ready(future.get());
	if (profiling) { addNanos(gOpCounters.readWaitNanos, elapsedNanosSince(waitStart)); }

	if (error_ != 0) {
		safs::log_err("Transaction::get: fdb_future_block_until_ready: error: {}",
		              fdb_get_error(error_));
		detail::throwTransactionError(error_);
	}

	fdb_bool_t valuePresent{};
	const uint8_t *valueRead{};
	int valueLength{};

	error_ = fdb_future_get_value(future.get(), &valuePresent, &valueRead, &valueLength);

	if (error_ != 0) {
		safs::log_err("Transaction::get: fdb_future_get_value: error: {}", fdb_get_error(error_));
		detail::throwTransactionError(error_);
	}

	if (valuePresent == 0) {
		// Missing key is normal control flow; callers should handle the empty optional.
		return std::nullopt;
	}

	return kv::Value(valueRead, std::next(valueRead, valueLength));
}

std::unique_ptr<kv::IFuture> Transaction::getAsync(const kv::Key &key, bool snapshot) {
	requireHandle("getAsync");

	bump(gOpCounters.pointReads);
	bumpReadPrefix(key);
	FDBFuture *future = fdb_transaction_get(tr_.get(), key.data(), static_cast<int>(key.size()),
	                                        static_cast<fdb_bool_t>(snapshot));

	return std::make_unique<FDBFutureValue>(future, faultInjection_);
}

kv::GetRangeResult Transaction::getRange(
    const kv::KeySelector &begin, const kv::KeySelector &end, int limit, int iteration /* = 0 */,
    bool snapshot /* = false */, bool reverse /* = false */,
    FDBStreamingMode streamingMode /* = FDB_STREAMING_MODE_SERIAL */) {
	auto future = getRangeAsync(begin, end, limit, iteration, snapshot, reverse, streamingMode);

	return future->get();
}

std::unique_ptr<kv::IRangeFuture> Transaction::getRangeAsync(
    const kv::KeySelector &begin, const kv::KeySelector &end, int limit, int iteration /* = 0 */,
    bool snapshot /* = false */, bool reverse /* = false */,
    FDBStreamingMode streamingMode /* = FDB_STREAMING_MODE_SERIAL */) {
	requireHandle("getRangeAsync");

	bump(gOpCounters.rangeReads);
	bumpReadPrefix(begin.getKey());
	static constexpr int kBytesLimit = 0;

	// For begin selectors: orEqual=0 means >= (inclusive), orEqual=1 means > (exclusive)
	const fdb_bool_t beginOrEqual = begin.isInclusive() ? 0 : 1;
	// FDB offsets are 1-based.
	const int beginOffset = 1 + begin.getOffset();

	// For end selectors (which define an exclusive boundary in FDB):
	// - To include end key in results: use orEqual=1
	// - To exclude end key from results: use orEqual=0
	const fdb_bool_t endOrEqual = end.isInclusive() ? 1 : 0;
	// FDB offsets are 1-based.
	const int endOffset = 1 + end.getOffset();

	FDBFuture *future = fdb_transaction_get_range(
	    tr_.get(), begin.getKey().data(), static_cast<int>(begin.getKey().size()), beginOrEqual,
	    beginOffset, end.getKey().data(), static_cast<int>(end.getKey().size()), endOrEqual,
	    endOffset, limit, kBytesLimit, streamingMode, iteration, static_cast<fdb_bool_t>(snapshot),
	    static_cast<fdb_bool_t>(reverse));

	return std::make_unique<FDBFutureRange>(future, faultInjection_);
}

void Transaction::set(const kv::Key &key, const kv::Value &value) {
	requireHandle("set");

	bump(gOpCounters.sets);
	fdb_transaction_set(tr_.get(), key.data(), static_cast<int>(key.size()), value.data(),
	                    static_cast<int>(value.size()));
}

void Transaction::atomicAdd(const kv::Key &key, const kv::Value &delta) {
	requireHandle("atomicAdd");

	bump(gOpCounters.atomicAdds);
	fdb_transaction_atomic_op(tr_.get(), key.data(), static_cast<int>(key.size()), delta.data(),
	                          static_cast<int>(delta.size()), FDB_MUTATION_TYPE_ADD);
}

void Transaction::atomicMax(const kv::Key &key, const kv::Value &value) {
	requireHandle("atomicMax");

	// Counted under atomicAdds is wrong, and adding a positional counter field would
	// desync the FdbOpCounters snapshot init; atomicMax is profiling-irrelevant, so it
	// intentionally bumps no op counter.
	fdb_transaction_atomic_op(tr_.get(), key.data(), static_cast<int>(key.size()), value.data(),
	                          static_cast<int>(value.size()), FDB_MUTATION_TYPE_MAX);
}

void Transaction::remove(const kv::Key &key) {
	requireHandle("remove");

	bump(gOpCounters.clears);
	fdb_transaction_clear(tr_.get(), key.data(), static_cast<int>(key.size()));
}

void Transaction::removeRange(const kv::Key &start, const kv::Key &end) {
	requireHandle("removeRange");

	bump(gOpCounters.clearRanges);
	fdb_transaction_clear_range(tr_.get(), start.data(), static_cast<int>(start.size()), end.data(),
	                            static_cast<int>(end.size()));
}

void Transaction::addReadConflictKey(const kv::Key &key) {
	requireHandle("addReadConflictKey");

	// FDB conflict ranges are half-open; a single key is [key, key + '\0').
	kv::Key end = key;
	end.push_back('\0');
	const fdb_error_t err = fdb_transaction_add_conflict_range(
	    tr_.get(), key.data(), static_cast<int>(key.size()), end.data(),
	    static_cast<int>(end.size()), FDB_CONFLICT_RANGE_TYPE_READ);
	if (err != 0) {
		// add_conflict_range is a synchronous client call; a failure is local misuse,
		// not a backend read failure, and must not silently drop the caller's protection.
		safs::log_err("Transaction::addReadConflictKey: error: {}", fdb_get_error(err));
		throw std::invalid_argument("fdb::Transaction::addReadConflictKey: " +
		                            std::string(fdb_get_error(err)));
	}
}

bool Transaction::commit() {
	requireHandle("commit");

	// A new attempt invalidates the previous attempt's version: a failed re-commit on a
	// reused transaction must not leave a stale success visible via getCommittedVersion.
	committedVersion_.reset();

	bump(gOpCounters.commits);

	// TEST-ONLY scripted pre-commit failure: nothing is submitted to the cluster; the
	// scripted code takes the same classification path as a real failed commit.
	if (faultInjection_ != nullptr) {
		fdb_error_t injected = FaultInjection::next(faultInjection_->preCommitErrors);
		if (injected != 0) {
			error_ = injected;
			safs::log_err("Transaction::commit: injected pre-commit error: {}",
			              fdb_get_error(injected));
			countFailedCommit(injected);
			return false;
		}
	}

	UniqueFDBFuture future(fdb_transaction_commit(tr_.get()));

	const bool profiling = opCountersEnabled();
	const auto waitStart =
	    profiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	error_ = fdb_future_block_until_ready(future.get());
	if (profiling) { addNanos(gOpCounters.commitWaitNanos, elapsedNanosSince(waitStart)); }

	if (error_ != 0) {
		safs::log_err("Transaction::commit: fdb_future_block_until_ready: error: {}",
		              fdb_get_error(error_));
		countFailedCommit(error_);
		return false;
	}

	// fdb_future_block_until_ready() only signals the future is ready; the
	// actual commit outcome (e.g. conflict, transaction_too_old) is in the
	// future itself and must be checked with fdb_future_get_error().
	error_ = fdb_future_get_error(future.get());

	// TEST-ONLY scripted post-commit override: at this point the commit IS durable; the
	// caller is nevertheless told it failed with the scripted code (commit_unknown_result
	// 1021 reproduces the maybe-committed case).
	if (error_ == 0 && faultInjection_ != nullptr) {
		error_ = FaultInjection::next(faultInjection_->postCommitErrors);
	}

	if (error_ != 0) {
		safs::log_err("Transaction::commit: commit failed: {}", fdb_get_error(error_));
		countFailedCommit(error_);
		return false;
	}

	int64_t version{};
	fdb_error_t versionError = fdb_transaction_get_committed_version(tr_.get(), &version);

	// TEST-ONLY scripted committed-version-lookup failure after a successful commit.
	if (versionError == 0 && faultInjection_ != nullptr) {
		versionError = FaultInjection::next(faultInjection_->committedVersionErrors);
	}

	if (versionError != 0) {
		// The commit itself succeeded and is durable; only the version lookup failed.
		// Report success without a version (committedVersion_ stays cleared from the
		// attempt start): signaling failure here would invite the caller to replay an
		// already-applied operation.
		safs::log_err(
		    "Transaction::commit: fdb_transaction_get_committed_version failed after a "
		    "successful commit, continuing without a committed version: {}",
		    fdb_get_error(versionError));
		return true;
	}

	committedVersion_ = version;

	return true;
}

std::optional<uint64_t> Transaction::getApproximateSize() const {
	if (!tr_) { return std::nullopt; }

	// This is an advisory query: on failure we return std::nullopt and let the caller fall back
	// to a count-based bound. Errors are kept in a local variable rather than the member error_,
	// so a transient/unsupported query does not poison the transaction's validity (operator bool).
	UniqueFDBFuture future(fdb_transaction_get_approximate_size(tr_.get()));
	fdb_error_t error = fdb_future_block_until_ready(future.get());
	if (error != 0) {
		safs::log_err("Transaction::getApproximateSize: fdb_future_block_until_ready: error: {}",
		              fdb_get_error(error));
		return std::nullopt;
	}

	int64_t size{};
	error = fdb_future_get_int64(future.get(), &size);
	if (error != 0) {
		safs::log_err("Transaction::getApproximateSize: fdb_future_get_int64: error: {}",
		              fdb_get_error(error));
		return std::nullopt;
	}

	return static_cast<uint64_t>(size);
}

namespace {

/// Pollable wrapper around an in-flight fdb_transaction_commit() future.
/// Mirrors Transaction::commit()'s outcome handling (error mapping, conflict vs
/// failure accounting, committed-version capture) but without blocking at submit
/// time. The back-pointers reference the owning Transaction's members, so that
/// Transaction must outlive this future.
class FDBCommitFuture final : public kv::ICommitFuture {
public:
	FDBCommitFuture(FDBFuture *commitFuture, FDBTransaction *trHandle,
	                std::optional<int64_t> *committedVersionOut, fdb_error_t *errorOut,
	                FaultInjection *fault)
	    : future_(commitFuture),
	      trHandle_(trHandle),
	      committedVersionOut_(committedVersionOut),
	      errorOut_(errorOut),
	      faultInjection_(fault) {}

	~FDBCommitFuture() override {
		if (future_ != nullptr) { fdb_future_destroy(future_); }
	}

	FDBCommitFuture(const FDBCommitFuture &) = delete;
	FDBCommitFuture &operator=(const FDBCommitFuture &) = delete;
	FDBCommitFuture(FDBCommitFuture &&) = delete;
	FDBCommitFuture &operator=(FDBCommitFuture &&) = delete;

	bool isReady() override {
		return future_ == nullptr || fdb_future_is_ready(future_) != 0;
	}

	void setReadyCallback(void (*callback)(void *), void *arg) override {
		// No wakeup requested: nothing to arm, and arming an FDB callback for a null
		// user callback would only risk the one-time node leak for no benefit.
		if (callback == nullptr) { return; }
		if (future_ == nullptr) {
			callback(arg);
			return;
		}
		// Route the wakeup through a heap node rather than `this`: the event-loop
		// thread may destroy this future (after observing isReady()) concurrently
		// with the FDB network thread firing the callback, so the callback must not
		// dereference the future. The node owns only a plain function pointer and
		// arg and is freed by the callback. If this future is destroyed before the
		// commit ever becomes ready, the callback never fires and the node leaks
		// once: a bounded, one-time leak per such future, which is harmless.
		auto *node = new ReadyNode{callback, arg};
		fdb_error_t err = fdb_future_set_callback(future_, &FDBCommitFuture::onReady, node);
		if (err != 0) {
			// Wakeup not armed; the caller's poll loop still finalizes this commit
			// at its normal cadence, just without the immediate wakeup.
			delete node;
			safs::log_warn("FDBCommitFuture::setReadyCallback: fdb_future_set_callback: {}",
			               fdb_get_error(err));
		}
	}

	bool getResult(int *error, bool *retryable) override {
		if (retryable != nullptr) { *retryable = false; }
		if (consumed_ || future_ == nullptr) {
			// A consumed future re-reports the first call's outcome (consumedError_, 0
			// after a successful commit). A null future is a defensive failure
			// (commitAsync never builds one: it throws when there is no transaction
			// handle) and reports a generic non-zero error instead.
			if (error != nullptr) { *error = future_ == nullptr ? 1 : consumedError_; }
			if (retryable != nullptr) { *retryable = consumedRetryable_; }
			return false;
		}
		consumed_ = true;

		// Mirror the synchronous commit() path: attribute any blocked wait to
		// commitWaitNanos. Usually near-zero here, since the caller's poll loop
		// finalizes only after isReady(), but a forced finalize can still block.
		const bool profiling = opCountersEnabled();
		const auto waitStart =
		    profiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
		fdb_error_t err = fdb_future_block_until_ready(future_);
		if (profiling) { addNanos(gOpCounters.commitWaitNanos, elapsedNanosSince(waitStart)); }
		if (err == 0) { err = fdb_future_get_error(future_); }

		// TEST-ONLY scripted post-commit override: at this point the commit IS durable;
		// the caller is nevertheless told it failed with the scripted code
		// (commit_unknown_result 1021 reproduces the maybe-committed case).
		if (err == 0 && faultInjection_ != nullptr) {
			err = FaultInjection::next(faultInjection_->postCommitErrors);
		}

		if (err != 0) {
			*errorOut_ = err;
			consumedError_ = err;
			// RETRYABLE_NOT_COMMITTED, NOT the broad RETRYABLE predicate: RETRYABLE also
			// returns true for commit_unknown_result (1021) and the rest of the
			// MAYBE_COMMITTED class, where the commit may already have applied. A blind
			// replay of such an op would double-apply it (a second inode/chunk, a double
			// atomicAdd). Reporting only the definitely-not-committed conflicts
			// (not_committed 1020, transaction_too_old 1007, ...) as retryable lets callers
			// safely replay those and surface an unknown-result commit as an error instead.
			const bool isRetryable =
			    DB::evaluatePredicate(FDB_ERROR_PREDICATE_RETRYABLE_NOT_COMMITTED, err);
			consumedRetryable_ = isRetryable;
			safs::log_err("FDBCommitFuture::getResult: commit failed: {}", fdb_get_error(err));
			if (opCountersEnabled()) {
				auto &counter =
				    isRetryable ? gOpCounters.commitConflicts : gOpCounters.commitFailures;
				counter.fetch_add(1, std::memory_order_relaxed);
			}
			if (error != nullptr) { *error = err; }
			if (retryable != nullptr) { *retryable = isRetryable; }
			return false;
		}

		int64_t version{};
		fdb_error_t versionError = fdb_transaction_get_committed_version(trHandle_, &version);

		// TEST-ONLY scripted committed-version-lookup failure after a successful commit.
		if (versionError == 0 && faultInjection_ != nullptr) {
			versionError = FaultInjection::next(faultInjection_->committedVersionErrors);
		}

		if (versionError != 0) {
			// The commit itself succeeded and is durable; only the version lookup
			// failed. Report success without a version (the version out-param stays
			// cleared from the attempt start): signaling failure here would invite the
			// caller to replay an already-applied operation.
			safs::log_err(
			    "FDBCommitFuture::getResult: fdb_transaction_get_committed_version failed "
			    "after a successful commit, continuing without a committed version: {}",
			    fdb_get_error(versionError));
			*errorOut_ = 0;
			if (error != nullptr) { *error = 0; }
			return true;
		}

		*committedVersionOut_ = version;
		*errorOut_ = 0;
		if (error != nullptr) { *error = 0; }
		return true;
	}

private:
	/// Owns the wakeup target across the future's lifetime so the FDB network
	/// thread never touches the (possibly freed) future. Freed by onReady().
	struct ReadyNode {
		void (*callback)(void *);
		void *arg;
	};

	static void onReady(FDBFuture * /*future*/, void *param) {
		std::unique_ptr<ReadyNode> node(static_cast<ReadyNode *>(param));
		if (node->callback != nullptr) { node->callback(node->arg); }
	}

	FDBFuture *future_;
	FDBTransaction *trHandle_;
	std::optional<int64_t> *committedVersionOut_;
	fdb_error_t *errorOut_;
	FaultInjection *faultInjection_;
	bool consumed_ = false;
	fdb_error_t consumedError_ = 0;
	bool consumedRetryable_ = false;
};

/// TEST-ONLY already-failed commit future for a scripted pre-commit error: reports the
/// scripted code with the same RETRYABLE_NOT_COMMITTED classification and accounting as
/// a real failed commit, but nothing was submitted to the cluster.
class InjectedCommitFuture final : public kv::ICommitFuture {
public:
	explicit InjectedCommitFuture(fdb_error_t err) : error_(err) {}

	bool isReady() override { return true; }

	void setReadyCallback(void (*callback)(void *), void *arg) override {
		if (callback != nullptr) { callback(arg); }
	}

	bool getResult(int *error, bool *retryable) override {
		const bool isRetryable =
		    DB::evaluatePredicate(FDB_ERROR_PREDICATE_RETRYABLE_NOT_COMMITTED, error_);
		if (error != nullptr) { *error = error_; }
		if (retryable != nullptr) { *retryable = isRetryable; }
		if (consumed_) { return false; }
		consumed_ = true;
		countFailedCommit(error_);
		return false;
	}

private:
	fdb_error_t error_;
	bool consumed_ = false;
};

}  // namespace

std::unique_ptr<kv::ICommitFuture> Transaction::commitAsync() {
	requireHandle("commitAsync");

	// A new attempt invalidates the previous attempt's version: a failed re-commit on a
	// reused transaction must not leave a stale success visible via getCommittedVersion.
	committedVersion_.reset();

	bump(gOpCounters.commits);

	// TEST-ONLY scripted pre-commit failure: nothing is submitted to the cluster.
	if (faultInjection_ != nullptr) {
		fdb_error_t injected = FaultInjection::next(faultInjection_->preCommitErrors);
		if (injected != 0) {
			error_ = injected;
			safs::log_err("Transaction::commitAsync: injected pre-commit error: {}",
			              fdb_get_error(injected));
			return std::make_unique<InjectedCommitFuture>(injected);
		}
	}

	FDBFuture *future = fdb_transaction_commit(tr_.get());

	return std::make_unique<FDBCommitFuture>(future, tr_.get(), &committedVersion_, &error_,
	                                         faultInjection_);
}

}  // namespace fdb
