/*
   Copyright 2026 Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include <initializer_list>
#include <string>
#include <string_view>

/// Before the rebrand, the data files of master, metalogger and chunkserver had a ".sfs"
/// extension (metadata.sfs, changelog.sfs.1, csstats.sfs, ...); they have ".leil" now
/// (metadata.leil, changelog.leil.1, csstats.leil, ...). Like the pre-rebrand config files and
/// data directory, the old names keep being used where they are the only ones: a daemon never
/// renames them, an administrator does, with the daemon stopped.
namespace legacy_data_files {

/// The pre-rebrand name of the data file @p name ("metadata.leil.1" -> "metadata.sfs.1"),
/// or @p name itself if it is not a ".leil" data file.
std::string legacyName(std::string_view name);

/// Whether the data files @p families in @p dir still have their pre-rebrand names only, e.g.
/// family "changelog" covers changelog.sfs and changelog.sfs.N: some exist under the old name
/// and none under the new one. A daemon then keeps reading and writing the old names, so it
/// continues with exactly the files it used before. False for a directory with neither.
bool usesLegacyNames(const std::string &dir, std::initializer_list<std::string_view> families);

}  // namespace legacy_data_files
