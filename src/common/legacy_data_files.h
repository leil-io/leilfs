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
/// (metadata.leil, changelog.leil.1, csstats.leil, ...). A daemon uses the new names where its
/// data directory has them, and the old names otherwise, as it did before: it never renames its
/// files, an administrator does, with the daemon stopped.
namespace legacy_data_files {

/// The pre-rebrand name of the data file @p name ("metadata.leil.1" -> "metadata.sfs.1"),
/// or @p name itself if it is not a ".leil" data file.
std::string legacyName(std::string_view name);

/// Whether the daemon with data files @p families in @p dir uses their pre-rebrand names: none
/// of them exists under the new name, e.g. family "changelog" covers changelog.leil and
/// changelog.leil.N. It then reads and writes the old names, so it continues with exactly the
/// files it used before, and creates them under those names in a directory with neither.
bool usesLegacyNames(const std::string &dir, std::initializer_list<std::string_view> families);

}  // namespace legacy_data_files
