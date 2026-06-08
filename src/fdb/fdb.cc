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

#include <atomic>
#include <cstdint>
#include <iterator>
#include <memory>

#include <foundationdb/fdb_c_types.h>

#include "fdb/fdb.h"
#include "fdb/fdb_future.h"
#include "kv/kv_utils.h"
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

}  // namespace

void setOpCountersEnabled(bool enabled) {
	gOpCountersEnabled.store(enabled, std::memory_order_relaxed);
}

bool opCountersEnabled() { return gOpCountersEnabled.load(std::memory_order_relaxed); }

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
	};
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

fdb_error_t DB::setupNetwork() { return fdb_setup_network(); }

fdb_error_t DB::runNetwork() { return fdb_run_network(); }

fdb_error_t DB::stopNetwork() { return fdb_stop_network(); }

// DB

fdb_error_t DB::setOption(FDBDatabaseOption option, std::string_view value) {
	return fdb_database_set_option(db_.get(), option, toU8(value),
	                               static_cast<int>(value.length()));
}

// Transaction

fdb_error_t Transaction::setOption(FDBTransactionOption option, std::string_view value) {
	return fdb_transaction_set_option(tr_.get(), option, toU8(value),
	                                  static_cast<int>(value.length()));
}

std::optional<kv::Value> Transaction::get(const kv::Key &key, bool snapshot) {
	if (!tr_) { return std::nullopt; }

	bump(gOpCounters.pointReads);
	UniqueFDBFuture future(
	    fdb_transaction_get(tr_.get(), key.data(), static_cast<int>(key.size()), snapshot));

	error_ = fdb_future_block_until_ready(future.get());

	if (error_ != 0) {
		safs::log_err("Transaction::get: fdb_future_block_until_ready: error: {}",
		              fdb_get_error(error_));
		return std::nullopt;
	}

	fdb_bool_t valuePresent{};
	const uint8_t *valueRead{};
	int valueLength{};

	error_ = fdb_future_get_value(future.get(), &valuePresent, &valueRead, &valueLength);

	if (error_ != 0) {
		safs::log_err("Transaction::get: fdb_future_get_value: error: {}", fdb_get_error(error_));
		return std::nullopt;
	}

	if (valuePresent == 0) {
		safs::log_info("Transaction::get: key not found: {}", kv::keyToEscapedAscii(key));
		return std::nullopt;
	}

	return kv::Value(valueRead, std::next(valueRead, valueLength));
}

std::unique_ptr<kv::IFuture> Transaction::getAsync(const kv::Key &key, bool snapshot) {
	if (!tr_) { return nullptr; }

	bump(gOpCounters.pointReads);
	FDBFuture *future = fdb_transaction_get(tr_.get(), key.data(), static_cast<int>(key.size()),
	                                        static_cast<fdb_bool_t>(snapshot));

	return std::make_unique<FDBFutureValue>(future);
}

kv::GetRangeResult Transaction::getRange(
    const kv::KeySelector &begin, const kv::KeySelector &end, int limit, int iteration /* = 0 */,
    bool snapshot /* = false */, bool reverse /* = false */,
    FDBStreamingMode streamingMode /* = FDB_STREAMING_MODE_SERIAL */) {
	auto future = getRangeAsync(begin, end, limit, iteration, snapshot, reverse, streamingMode);
	if (!future) { return {{}, false}; }

	return future->get();
}

std::unique_ptr<kv::IRangeFuture> Transaction::getRangeAsync(
    const kv::KeySelector &begin, const kv::KeySelector &end, int limit, int iteration /* = 0 */,
    bool snapshot /* = false */, bool reverse /* = false */,
    FDBStreamingMode streamingMode /* = FDB_STREAMING_MODE_SERIAL */) {
	if (!tr_) { return nullptr; }

	bump(gOpCounters.rangeReads);
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

	return std::make_unique<FDBFutureRange>(future);
}

void Transaction::set(const kv::Key &key, const kv::Value &value) {
	if (!tr_) { return; }

	bump(gOpCounters.sets);
	fdb_transaction_set(tr_.get(), key.data(), static_cast<int>(key.size()), value.data(),
	                    static_cast<int>(value.size()));
}

void Transaction::atomicAdd(const kv::Key &key, const kv::Value &delta) {
	if (!tr_) { return; }

	bump(gOpCounters.atomicAdds);
	fdb_transaction_atomic_op(tr_.get(), key.data(), static_cast<int>(key.size()), delta.data(),
	                          static_cast<int>(delta.size()), FDB_MUTATION_TYPE_ADD);
}

void Transaction::remove(const kv::Key &key) {
	if (!tr_) { return; }

	bump(gOpCounters.clears);
	fdb_transaction_clear(tr_.get(), key.data(), static_cast<int>(key.size()));
}

void Transaction::removeRange(const kv::Key &start, const kv::Key &end) {
	if (!tr_) { return; }

	bump(gOpCounters.clearRanges);
	fdb_transaction_clear_range(tr_.get(), start.data(), static_cast<int>(start.size()), end.data(),
	                            static_cast<int>(end.size()));
}

bool Transaction::commit() {
	if (!tr_) { return false; }

	bump(gOpCounters.commits);
	UniqueFDBFuture future(fdb_transaction_commit(tr_.get()));

	error_ = fdb_future_block_until_ready(future.get());

	if (error_ != 0) {
		safs::log_err("Transaction::commit: fdb_future_block_until_ready: error: {}",
		              fdb_get_error(error_));
		bump(gOpCounters.commitFailures);
		return false;
	}

	// fdb_future_block_until_ready() only signals the future is ready; the
	// actual commit outcome (e.g. conflict, transaction_too_old) is in the
	// future itself and must be checked with fdb_future_get_error().
	error_ = fdb_future_get_error(future.get());

	if (error_ != 0) {
		safs::log_err("Transaction::commit: commit failed: {}", fdb_get_error(error_));
		// evaluatePredicate() calls into the FDB C API, so only pay for it when
		// accounting is on; keeps the disabled path a single relaxed bool load.
		// The gate is already known here, so increment directly instead of bump().
		if (opCountersEnabled()) {
			auto &counter = DB::evaluatePredicate(FDB_ERROR_PREDICATE_RETRYABLE, error_)
			                    ? gOpCounters.commitConflicts
			                    : gOpCounters.commitFailures;
			counter.fetch_add(1, std::memory_order_relaxed);
		}
		return false;
	}

	int64_t version{};
	fdb_error_t versionError = fdb_transaction_get_committed_version(tr_.get(), &version);

	if (versionError != 0) {
		safs::log_err("Transaction::commit: fdb_transaction_get_committed_version: error: {}",
		              fdb_get_error(versionError));
		error_ = versionError;
		return false;
	}

	committedVersion_ = version;

	return true;
}

}  // namespace fdb
