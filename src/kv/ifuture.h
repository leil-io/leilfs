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

#include <optional>

#include "kv/kv_utils.h"

namespace kv {

/// Interface for asynchronous future results from key-value operations.
/// Provides methods to check readiness and retrieve values from pending operations.
///
/// @note This future is single-use: get() can only be called once successfully.
/// @note The transaction that created this future must remain alive until get() is called.
class IFuture {
public:
	virtual ~IFuture() = default;

	// Non-copyable, non-movable
	IFuture(const IFuture &) = delete;
	IFuture &operator=(const IFuture &) = delete;
	IFuture(IFuture &&) = delete;
	IFuture &operator=(IFuture &&) = delete;

	/// Checks if the future result is ready without blocking.
	/// @return True if the result is ready, false otherwise.
	virtual bool isReady() = 0;

	/// Blocks until the result is ready and retrieves the value.
	/// @param error Optional pointer to store error code (0 on success, non-zero on error).
	/// @return The value if successful and present, std::nullopt on error or if key not found.
	/// @note This method can only be called once. Subsequent calls return std::nullopt.
	/// @note The caller must keep the transaction alive until this method returns.
	virtual std::optional<Value> get(int *error = nullptr) = 0;

protected:
	IFuture() = default;
};

}  // namespace kv
