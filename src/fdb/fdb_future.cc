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
#include <utility>
#include <vector>

#include <foundationdb/fdb_c_types.h>

#include "fdb/fdb.h"
#include "fdb/fdb_future.h"
#include "slogger/slogger.h"

namespace fdb {

namespace {

/// Translates a RETRYABLE FoundationDB read error into a typed exception so the
/// master's op boundary can replay the op on a fresh transaction instead of letting
/// the read failure (e.g. transaction_timed_out under load) propagate uncaught and
/// abort the whole MDS. Non-retryable errors are left for the caller to report via
/// the *error out-parameter as before.
void throwIfRetryable(fdb_error_t err) {
	// transaction_timed_out (1031) is deliberately absent from FDB's RETRYABLE
	// predicate because a timeout during COMMIT has an unknown result. This helper
	// guards READ futures only, and the op-boundary replay runs on a fresh
	// transaction with a fresh timeout budget, so a timed-out read is safe to
	// replay. The commit path (FDBCommitFuture::getResult) uses the raw predicate
	// and keeps 1031 non-retryable.
	constexpr fdb_error_t kTransactionTimedOut = 1031;
	if (err == kTransactionTimedOut ||
	    fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE, err) != 0) {
		throw kv::RetryableTransactionError(static_cast<int>(err), fdb_get_error(err));
	}
}

}  // namespace

FDBFutureValue::FDBFutureValue(FDBFuture *future) : future_(future) {}

FDBFutureValue::~FDBFutureValue() {
	if (future_ != nullptr) { fdb_future_destroy(future_); }
}

bool FDBFutureValue::isReady() {
	if (future_ == nullptr) { return false; }

	return fdb_future_is_ready(future_) != 0;
}

std::optional<kv::Value> FDBFutureValue::get(int *error) {
	// Return nullopt if already consumed or invalid
	if (consumed_ || future_ == nullptr) {
		if (error != nullptr) { *error = 1; }
		return std::nullopt;
	}

	// Mark as consumed
	consumed_ = true;

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
		throwIfRetryable(fdbError);
		return std::nullopt;
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
		throwIfRetryable(fdbError);
		return std::nullopt;
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

FDBFutureRange::FDBFutureRange(FDBFuture *future) : future_(future) {}

FDBFutureRange::~FDBFutureRange() {
	if (future_ != nullptr) { fdb_future_destroy(future_); }
}

bool FDBFutureRange::isReady() {
	if (future_ == nullptr) { return false; }

	return fdb_future_is_ready(future_) != 0;
}

kv::GetRangeResult FDBFutureRange::get(int *error) {
	// Return an empty result if already consumed or invalid.
	if (consumed_ || future_ == nullptr) {
		if (error != nullptr) { *error = 1; }
		return {{}, false};
	}

	consumed_ = true;

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
		throwIfRetryable(fdbError);
		return {{}, false};
	}

	const FDBKeyValue *keyValues = nullptr;
	int count = 0;
	fdb_bool_t more = 0;

	fdbError = fdb_future_get_keyvalue_array(future_, &keyValues, &count, &more);

	if (fdbError != 0) {
		safs::log_err("FDBFutureRange::get: fdb_future_get_keyvalue_array: error: {}",
		              fdb_get_error(fdbError));
		if (error != nullptr) { *error = static_cast<int>(fdbError); }
		throwIfRetryable(fdbError);
		return {{}, false};
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
