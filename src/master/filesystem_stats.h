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

#include <array>
#include <cstddef>
#include <cstdint>

/// Used as indexes for gFsStatsArray
enum class FsStats : uint8_t {
	Statfs = 0,
	Getattr,
	Setattr,
	Lookup,
	Mkdir,
	Rmdir,
	Symlink,
	Readlink,
	Mknod,
	Unlink,
	Rename,
	Link,
	Readdir,
	Open,
	Read,
	Write,
	Size
};

/// Helper size_t constant with the value of the auto-generated Size of FsStats enum.
/// Useful for iterating without casting.
constexpr size_t kFsStatsSize = static_cast<size_t>(FsStats::Size);

/// Just a convenience alias
using FsStatsArray = std::array<uint32_t, kFsStatsSize>;

/// Global array holding filesystem operation statistics
extern FsStatsArray gFsStatsArray;  // NOLINT

/// Increments a filesystem operation statistic counter
/// @param stat Statistic to increment
void incrementFSStat(FsStats stat);

/// Retrieves current filesystem operation statistics and resets the counters
/// @param outputStats Output array to fill with current statistics
void retrieveFSStats(FsStatsArray &outputStats);
