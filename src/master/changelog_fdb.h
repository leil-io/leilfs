/*
   Copyright 2025      Leil Storage OÜ

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

#include "kv/ikv_engine.h"

namespace master {

/// Applies changelog entries stored in the KV database (FoundationDB) to the current metadata state.
/// The entries are expected to be stored with keys prefixed by kChangelogPrefix (see kv_common_keys.h)
/// and with the version encoded as big-endian uint64 appended to the prefix. Values are the textual
/// changelog lines without the leading version and colon, i.e., the same payload as stored in the
/// legacy file-based changelog after the version separator.
///
/// The function mirrors the logic of load_changelog() from changelog.cc: it skips entries with a
/// version lower than the current fs_getversion(), applies the remaining entries using restore(),
/// and logs similar summary messages.
///
/// This function is a complementary loader that coexists with the file-based backend and does not
/// replace any existing behavior.
void load_changelogs_fdb(kv::IKVEngine* engine);

}  // namespace master
