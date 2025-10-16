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

#include "master/filesystem_stats.h"

#include <cstddef>

FsStatsArray gFsStatsArray{};

void incrementFSStat(FsStats stat) {
	// As stat is now an enum class, it will receive only valid values.
	// Compilers might warn about the subscript usage, so we disable lint for the line.
	++gFsStatsArray[static_cast<size_t>(stat)];  // NOLINT
}

void retrieveFSStats(FsStatsArray &outputStats) {
	outputStats = gFsStatsArray;
	gFsStatsArray.fill(0);
}
