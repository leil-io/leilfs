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

#include <cstdint>
#include <utility>
#include <vector>

namespace kv {

using Bytes = std::vector<uint8_t>;
using Key = Bytes;
using Value = Bytes;

/// Represents a key-value pair in the key-value store.
/// Keys and values are stored as vectors of bytes.
struct KeyValuePair {
	Key key;
	Value value;
};

/// Result of a range query with information if there are more results available.
class GetRangeResult {
public:
	GetRangeResult(std::vector<KeyValuePair> pairs, bool hasMore)
	    : pairs_(std::move(pairs)), hasMore_(hasMore) {}

	/// Returns the key-value pairs in the result.
	const std::vector<KeyValuePair> &getPairs() const { return pairs_; }

	/// Returns whether there are more results available.
	bool hasMore() const { return hasMore_; }

private:
	std::vector<KeyValuePair> pairs_;  ///< The key-value pairs retrieved in the range query.
	bool hasMore_{false};              ///< True if more results are available beyond this range.
};

}  // namespace kv
