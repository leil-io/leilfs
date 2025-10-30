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

#include "fdb/fdb_future.h"

#include "slogger/slogger.h"

namespace fdb {

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

	// Block until the future is ready
	fdb_error_t fdbError = fdb_future_block_until_ready(future_);

	if (fdbError != 0) {
		safs::log_err("FDBFutureValue::get: fdb_future_block_until_ready: error: {}",
		              fdb_get_error(fdbError));
		if (error != nullptr) { *error = static_cast<int>(fdbError); }
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

}  // namespace fdb
