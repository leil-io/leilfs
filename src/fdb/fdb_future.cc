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
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include <cassert>
#include <chrono>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <vector>

#include <foundationdb/fdb_c_types.h>

#include "fdb/fdb.h"
#include "fdb/fdb_errors.h"
#include "fdb/fdb_future.h"
#include "slogger/slogger.h"

namespace fdb {

using detail::throwTransactionError;

namespace {

/// Pops the next TEST-ONLY scripted read failure, or 0 when none is armed.
fdb_error_t injectedReadError(FaultInjection *fault) {
	if (fault == nullptr) { return 0; }
	return FaultInjection::next(fault->readErrors);
}

/// Records a failed read in the owning transaction's state (when wired): retryable
/// errors leave it replayable via onErrorAsync, everything else is terminal. Left inert
/// unless the transaction is still Active AND still on the attempt that issued this
/// future. Reads are only issued while Active, so a transaction that already moved on
/// (commit in flight, committed, maybe-committed, or an earlier recorded failure) is
/// left untouched: a late-resolving read must never regress a commit outcome. The
/// generation guard adds the other half: a future from a superseded attempt (after
/// reset() or a successful recovery re-activated the handle) must not stamp the FRESH
/// attempt with the old attempt's error, which recovery could then act on.
void recordReadFailure(const ReadFailureSink &sink, fdb_error_t err) {
	if (sink.state == nullptr || *sink.state != TransactionState::kActive) { return; }
	if (sink.attemptGeneration != nullptr && *sink.attemptGeneration != sink.issuedGeneration) {
		return;
	}
	*sink.state = detail::isRetryableReadError(err) ? TransactionState::kRetryPending
	                                                : TransactionState::kFailed;
	if (sink.lastFailureCode != nullptr) { *sink.lastFailureCode = err; }
}

}  // namespace

FDBFutureValue::FDBFutureValue(FDBFuture *future, FaultInjection *fault, ReadFailureSink sink)
    : future_(future), faultInjection_(fault), sink_(sink) {}

FDBFutureValue::~FDBFutureValue() {
	if (future_ != nullptr) { fdb_future_destroy(future_); }
}

bool FDBFutureValue::isReady() {
	if (future_ == nullptr) { return false; }

	return fdb_future_is_ready(future_) != 0;
}

std::optional<kv::Value> FDBFutureValue::get(int *error) {
	// Wrong-state call per the kv error-domain rules: posing as a missing key would hide
	// the caller's bug, and no real backend error code exists to report.
	if (consumed_ || future_ == nullptr) {
		throw kv::TransactionStateError(consumed_ ? "FDBFutureValue::get: future already consumed"
		                                          : "FDBFutureValue::get: invalid future");
	}

	// Mark as consumed
	consumed_ = true;

	// TEST-ONLY scripted read failure; short-circuits before waiting on the cluster and
	// takes exactly the real error path below.
	if (fdb_error_t injected = injectedReadError(faultInjection_); injected != 0) {
		if (error != nullptr) { *error = static_cast<int>(injected); }
		recordReadFailure(sink_, injected);
		throwTransactionError(injected);
	}

	// Block until the future is ready. Times the wait so a pipelined read (issued
	// earlier, already resolved here) shows as near-zero, while a serial read shows
	// its full round-trip; this attributes per-op latency to read-wait.
	const bool profiling = opCountersEnabled();
	const auto waitStart =
	    profiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	fdb_error_t fdbError = fdb_future_block_until_ready(future_);
	if (profiling) {
		addReadWaitNanos(static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(
		        std::chrono::steady_clock::now() - waitStart)
		        .count()));
	}

	if (fdbError != 0) {
		safs::log_err("FDBFutureValue::get: fdb_future_block_until_ready: error: {}",
		              fdb_get_error(fdbError));
		if (error != nullptr) { *error = static_cast<int>(fdbError); }
		recordReadFailure(sink_, fdbError);
		throwTransactionError(fdbError);
	}

	// Extract the value from the future
	fdb_bool_t valuePresent{};
	const uint8_t *valueRead{};
	int valueLength{};

	fdbError = fdb_future_get_value(future_, &valuePresent, &valueRead, &valueLength);

	if (fdbError != 0) {
		safs::log_err("FDBFutureValue::get: fdb_future_get_value: error: {}",
		              fdb_get_error(fdbError));
		if (error != nullptr) { *error = static_cast<int>(fdbError); }
		recordReadFailure(sink_, fdbError);
		throwTransactionError(fdbError);
	}

	// Set error to success
	if (error != nullptr) { *error = 0; }

	// Return the value if present
	if (valuePresent != 0) {
		kv::Value value(valueRead, std::next(valueRead, valueLength));
		return value;
	}

	// Key not found
	return std::nullopt;
}

FDBFutureRange::FDBFutureRange(FDBFuture *future, FaultInjection *fault, ReadFailureSink sink)
    : future_(future), faultInjection_(fault), sink_(sink) {}

FDBFutureRange::~FDBFutureRange() {
	if (future_ != nullptr) { fdb_future_destroy(future_); }
}

bool FDBFutureRange::isReady() {
	if (future_ == nullptr) { return false; }

	return fdb_future_is_ready(future_) != 0;
}

kv::GetRangeResult FDBFutureRange::get(int *error) {
	// Wrong-state call per the kv error-domain rules: posing as an empty range would
	// hide the caller's bug, and no real backend error code exists to report.
	if (consumed_ || future_ == nullptr) {
		throw kv::TransactionStateError(consumed_ ? "FDBFutureRange::get: future already consumed"
		                                          : "FDBFutureRange::get: invalid future");
	}

	consumed_ = true;

	// TEST-ONLY scripted read failure; short-circuits before waiting on the cluster and
	// takes exactly the real error path below.
	if (fdb_error_t injected = injectedReadError(faultInjection_); injected != 0) {
		if (error != nullptr) { *error = static_cast<int>(injected); }
		recordReadFailure(sink_, injected);
		throwTransactionError(injected);
	}

	const bool profiling = opCountersEnabled();
	const auto waitStart =
	    profiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	fdb_error_t fdbError = fdb_future_block_until_ready(future_);
	if (profiling) {
		addReadWaitNanos(static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(
		        std::chrono::steady_clock::now() - waitStart)
		        .count()));
	}

	if (fdbError != 0) {
		safs::log_err("FDBFutureRange::get: fdb_future_block_until_ready: error: {}",
		              fdb_get_error(fdbError));
		if (error != nullptr) { *error = static_cast<int>(fdbError); }
		recordReadFailure(sink_, fdbError);
		throwTransactionError(fdbError);
	}

	const FDBKeyValue *keyValues = nullptr;
	int count = 0;
	fdb_bool_t more = 0;

	fdbError = fdb_future_get_keyvalue_array(future_, &keyValues, &count, &more);

	if (fdbError != 0) {
		safs::log_err("FDBFutureRange::get: fdb_future_get_keyvalue_array: error: {}",
		              fdb_get_error(fdbError));
		if (error != nullptr) { *error = static_cast<int>(fdbError); }
		recordReadFailure(sink_, fdbError);
		throwTransactionError(fdbError);
	}

	std::vector<kv::KeyValuePair> pairs;
	pairs.reserve(count);

	for (int i = 0; i < count; ++i) {
		assert(keyValues[i].key != nullptr || keyValues[i].key_length == 0);
		const auto *keyData = static_cast<const uint8_t *>(keyValues[i].key);
		kv::Key key(keyData, keyData + keyValues[i].key_length);

		assert(keyValues[i].value != nullptr || keyValues[i].value_length == 0);
		const auto *valueData = static_cast<const uint8_t *>(keyValues[i].value);
		kv::Value value(valueData, valueData + keyValues[i].value_length);

		pairs.emplace_back(std::move(key), std::move(value));
	}

	if (error != nullptr) { *error = 0; }
	return {std::move(pairs), more != 0};
}

}  // namespace fdb
