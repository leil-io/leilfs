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
#include <string_view>

inline constexpr std::string_view kMetaFormatKey = "META_FORMAT";
inline constexpr std::string_view kMetaVersionKey = "META_VERSION";

inline constexpr std::string_view kMetaNextSessionKey = "META_NEXT_SESSION";

inline constexpr std::string_view kInodeRangeStartKey = "META_NEXT_INODE_RANGE";
inline constexpr std::string_view kChunkRangeStartKey = "META_NEXT_CHUNK_RANGE";

// Keys for filesystem statistics
inline constexpr std::string_view kMetaNodesKey = "META_NODES";
inline constexpr std::string_view kMetaTrashSpaceKey = "META_TRASH_SPACE";
inline constexpr std::string_view kMetaReservedSpaceKey = "META_RESERVED_SPACE";
inline constexpr std::string_view kMetaTrashNodesKey = "META_TRASH_NODES";
inline constexpr std::string_view kMetaReservedNodesKey = "META_RESERVED_NODES";
inline constexpr std::string_view kMetaFileNodesKey = "META_FILE_NODES";
inline constexpr std::string_view kMetaDirNodesKey = "META_DIR_NODES";
inline constexpr std::string_view kMetaLinkNodesKey = "META_LINK_NODES";

// Metadata sections prefixes
inline constexpr std::string_view kNodeKeyPrefix = "NODE_";    // Section NODE 1.0
inline constexpr std::string_view kEdgeKeyPrefix = "EDGE_";    // Section EDGE 1.0
inline constexpr std::string_view kFreeKeyPrefix = "FREE_";    // Section FREE 1.0
inline constexpr std::string_view kXAttrKeyPrefix = "XATR_";   // Section XATR 1.0
inline constexpr std::string_view kACLsKeyPrefix = "ACLS_";    // Section ACLS 1.2
inline constexpr std::string_view kQuotasKeyPrefix = "QUOT_";  // Section QUOT 1.1
inline constexpr std::string_view kLocksKeyPrefix = "FLCK_";   // Section FLCK 1.0
inline constexpr std::string_view kChunkKeyPrefix = "CHNK_";   // Section CHNK 1.0

// Case-insensitive directory support

/// Prefix for case-insensitive edges
/// Format: LOWER_EDGE_<ParentId><LowercaseName>:<ChildId>
inline constexpr std::string_view kEdgeLowerKeyPrefix = "LOWER_EDGE_";

// Extra indexes for edges' reverse lookups/traversals

/// Prefix for reverse index for directories
/// Only one parent is allowed to maintain tree structure (preventing cycles or circular references)
/// Format: DIR_PARENT_<ChildId>:<ParentId>
inline constexpr std::string_view kDirParentKeyPrefix = "DIR_PARENT_";

/// Prefix for reverse index for files and links (multiple parents allowed via hard links)
/// Format: PARENT_<ChildId><ParentId>:<Empty value>
inline constexpr std::string_view kParentKeyPrefix = "PARENT_";

/// Prefix for counting directory nodes without querying all entries
/// Format: DIR_NODES_COUNT_<ParentId>:<DirEntriesCount>.
/// DirEntriesCount is stored as little-endian int64_t for atomic updates.
/// Note: Signed integers are used in FDB storage to enable simpler atomic add/subtract
/// operations without unsigned arithmetic underflow concerns.
inline constexpr std::string_view kDirNodesCountPrefix = "DIR_NODES_COUNT_";

// Directory stats

/// Prefix for directory statistics
/// Format: DIR_STATS_<DirId><SuffixByte>:<Value>
/// Each directory has 8 keys with different suffix bytes for each stat field.
/// Values are stored as little-endian int64_t for atomic updates.
/// Note: Although StatsRecord uses unsigned types in memory, signed integers are used
/// in FDB storage to enable simpler atomic add/subtract operations without unsigned
/// arithmetic underflow concerns. Values are converted between types during serialization.
/// Stats are recursively aggregated (sum of all descendants) and maintained incrementally
/// as children are added/removed, enabling efficient queries like 'saunafs dirinfo'.
inline constexpr std::string_view kDirStatsPrefix = "DIR_STATS_";

/// Suffix bytes for directory statistics fields.
/// Used to differentiate the 8 stats keys per directory without string comparisons.
enum class StatsSuffix : uint8_t {
	Inodes = 0x01,   // Total inode count
	Dirs = 0x02,     // Subdirectory count
	Files = 0x03,    // File count
	Links = 0x04,    // Symlink count
	Chunks = 0x05,   // Total chunk count
	Length = 0x06,   // Logical size
	Size = 0x07,     // Physical size
	Realsize = 0x08  // Real storage size
};
