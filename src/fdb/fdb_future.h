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

#pragma once

#include "common/platform.h"

// Needs to be included before foundationdb/fdb_c.h
#include <fdb/fdb_api_version.h>

#include <foundationdb/fdb_c.h>

#include "kv/ifuture.h"

namespace fdb {

struct FaultInjection;

/// Wraps a FoundationDB future (FDBFuture*) with RAII semantics.
/// Implements the kv::IFuture interface for asynchronous get operations.
///
/// @note The transaction that created this future must remain alive until get() is called.
/// @note This future is single-use: get() can only be called once.
class FDBFutureValue final : public kv::IFuture {
public:
	/// Constructs a FDBFutureValue wrapping the given FDBFuture*.
	/// @param future The FDB future to wrap. Takes ownership.
	/// @param fault Optional TEST-ONLY fault-injection script (not owned, may be null).
	explicit FDBFutureValue(FDBFuture *future, FaultInjection *fault = nullptr);

	/// Destructor. Destroys the wrapped FDB future.
	~FDBFutureValue() override;

	// Non-copyable, non-movable
	FDBFutureValue(const FDBFutureValue &) = delete;
	FDBFutureValue &operator=(const FDBFutureValue &) = delete;
	FDBFutureValue(FDBFutureValue &&) = delete;
	FDBFutureValue &operator=(FDBFutureValue &&) = delete;

	/// Checks if the future result is ready without blocking.
	/// @return True if the result is ready, false otherwise.
	bool isReady() override;

	/// Blocks until the result is ready and retrieves the value.
	/// @param error Optional pointer to store error code (0 on success, non-zero on error).
	/// @return The value if successful and present, std::nullopt on error or if key not found.
	/// @note This method can only be called once. Subsequent calls return std::nullopt.
	std::optional<kv::Value> get(int *error = nullptr) override;

private:
	FDBFuture *future_;
	bool consumed_ = false;
	/// TEST-ONLY fault-injection script; null in production.
	FaultInjection *faultInjection_;
};

/// Wraps a FoundationDB range future (FDBFuture*) with RAII semantics.
/// Implements the kv::IRangeFuture interface for asynchronous get_range operations.
///
/// @note The transaction that created this future must remain alive until get() is called.
/// @note This future is single-use: get() can only be called once.
class FDBFutureRange final : public kv::IRangeFuture {
public:
	/// Constructs a FDBFutureRange wrapping the given FDBFuture*.
	/// @param future The FDB future to wrap. Takes ownership.
	/// @param fault Optional TEST-ONLY fault-injection script (not owned, may be null).
	explicit FDBFutureRange(FDBFuture *future, FaultInjection *fault = nullptr);

	/// Destructor. Destroys the wrapped FDB future.
	~FDBFutureRange() override;

	// Non-copyable, non-movable
	FDBFutureRange(const FDBFutureRange &) = delete;
	FDBFutureRange &operator=(const FDBFutureRange &) = delete;
	FDBFutureRange(FDBFutureRange &&) = delete;
	FDBFutureRange &operator=(FDBFutureRange &&) = delete;

	/// Checks if the future result is ready without blocking.
	/// @return True if the result is ready, false otherwise.
	bool isReady() override;

	/// Blocks until the result is ready and retrieves the range.
	/// @param error Optional pointer to store error code (0 on success, non-zero on error).
	/// @return The range result, or an empty result on error.
	/// @note This method can only be called once. Subsequent calls return an empty result.
	kv::GetRangeResult get(int *error = nullptr) override;

private:
	FDBFuture *future_;
	bool consumed_ = false;
	/// TEST-ONLY fault-injection script; null in production.
	FaultInjection *faultInjection_;
};

}  // namespace fdb
