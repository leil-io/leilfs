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

#include <string_view>

inline constexpr std::string_view kMetaFormatKey = "META_FORMAT";
inline constexpr std::string_view kMetaVersionKey = "META_VERSION";

inline constexpr std::string_view kMetaNextSessionKey = "META_NEXT_SESSION";

inline constexpr std::string_view kInodeRangeStartKey = "META_NEXT_INODE_RANGE";
inline constexpr std::string_view kChunkRangeStartKey = "META_NEXT_CHUNK_RANGE";

// Metadata sections prefixes
inline constexpr std::string_view kNodeKeyPrefix = "NODE_";    // Section NODE 1.0
inline constexpr std::string_view kEdgeKeyPrefix = "EDGE_";    // Section EDGE 1.0
inline constexpr std::string_view kFreeKeyPrefix = "FREE_";    // Section FREE 1.0
inline constexpr std::string_view kXAttrKeyPrefix = "XATR_";   // Section XATR 1.0
inline constexpr std::string_view kACLsKeyPrefix = "ACLS_";    // Section ACLS 1.2
inline constexpr std::string_view kQuotasKeyPrefix = "QUOT_";  // Section QUOT 1.1
inline constexpr std::string_view kLocksKeyPrefix = "FLCK_";   // Section FLCK 1.0
inline constexpr std::string_view kChunkKeyPrefix = "CHNK_";   // Section CHNK 1.0
