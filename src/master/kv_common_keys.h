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

/// Prefix for FSNode entries
/// Format: NODE_<InodeId>:<SerializedFSNodeData>
/// @note InodeId is serialized as Big Endian to maintain numeric order in lexicographical sorting
/// enabling efficient range queries for all nodes or specific inode ranges.
inline constexpr std::string_view kNodeKeyPrefix = "NODE_";    // Section NODE 1.0

/// Prefix for edges (directory entries)
/// Format: EDGE_<ParentId><Name>:<ChildId>
/// e.g.: EDGE_1999ChildName: 2535
/// @note ParentId is serialized as Big Endian to maintain numeric order in lexicographical sorting,
/// enabling efficient range queries for all edges of a specific parent.
/// @note ChildId is also serialized as Big Endian.
/// @note The in-memory implementation only needs to persist the EDGE section in metadata.sfs,
/// but the KV implementations need additional reverse indexes for efficient lookups and traversals,
/// which are stored as separate keys with their own prefixes (e.g., DIR_PARENT_, PARENT_).
/// @see kEdgeLowerKeyPrefix, kDirParentKeyPrefix, kParentKeyPrefix, kDirNodesCountPrefix,
/// kDirStatsPrefix
inline constexpr std::string_view kEdgeKeyPrefix = "EDGE_";    // Section EDGE 1.0

/// Prefix for free/reusable inode ids
/// Format: FREE_<InodeId>:<TimeStamp>
/// @note InodeId is serialized as Big Endian. TimeStamp is a 32-bit Big Endian value indicating
/// when the inode was freed.
inline constexpr std::string_view kFreeKeyPrefix = "FREE_";    // Section FREE 1.0

/// Prefix for chunk entries (mostly used for chunk locking)
/// Format: CHNK_<ChunkId><ChunkVersion>:<LockedTo><LockId>
/// @note ChunkId (64-bit) and ChunkVersion (32-bit) are serialized as Big Endian in the key.
/// LockedTo and LockId (both 32-bit) are also Big Endian.
inline constexpr std::string_view kChunkKeyPrefix = "CHNK_";   // Section CHNK 1.0

/// Prefix for extended attributes (xattrs)
/// Format: XATR_<InodeId><AttributeName>:<AttributeValue>
/// e.g.: XATR_1999UserAttr:UserValue
/// @note InodeId is serialized as Big Endian to maintain numeric order in lexicographical sorting,
/// enabling efficient range queries for all xattrs of a specific inode.
inline constexpr std::string_view kXAttrKeyPrefix = "XATR_";   // Section XATR 1.0

/// Prefix for Access Control Lists (ACLs)
/// Format: ACLS_<InodeId>:<binary RichACL>
/// e.g.: ACLS_1999:<binary data>
/// @note InodeId is serialized as Big Endian to maintain numeric order in lexicographical sorting,
/// enabling efficient range queries.
inline constexpr std::string_view kACLsKeyPrefix = "ACLS_";    // Section ACLS 1.2

/// Prefix for quotas
/// Format: QUOT_<OwnerType><OwnerId><Rigor><Resource>:<Value>
/// - OwnerType: u8 (user/group/inode)
/// - OwnerId: inode_t serialized as Big Endian
/// - Rigor: u8 (soft/hard/used)
/// - Resource: u8 (inodes/size)
/// - Value: u64 serialized as Big Endian
/// @note Numeric fields in the key are serialized as Big Endian to preserve numeric order in
/// lexicographical sorting, enabling efficient scans by owner prefix and global quota prefix.
inline constexpr std::string_view kQuotasKeyPrefix = "QUOT_";  // Section QUOT 1.1
inline constexpr std::string_view kLocksKeyPrefix = "FLCK_";   // Section FLCK 1.0

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
/// Format: PARENT_<ChildId><ParentId><EdgeName>:<EmptyValue>
inline constexpr std::string_view kParentKeyPrefix = "PARENT_";

/// Prefix for counting directory nodes without querying all entries
/// Format: DIR_NODES_COUNT_<ParentId>:<DirEntriesCount>.
/// DirEntriesCount is stored as little-endian int64_t for atomic updates.
/// @note Signed integers are used in FDB storage to enable simpler atomic add/subtract
/// operations without unsigned arithmetic underflow concerns.
inline constexpr std::string_view kDirNodesCountPrefix = "DIR_NODES_COUNT_";

// Directory stats

/// Prefix for directory statistics
/// Format: DIR_STATS_<DirId><SuffixByte>:<Value>
/// Each directory has 8 keys with different suffix bytes for each stat field.
/// Values are stored as little-endian int64_t for atomic updates.
/// @note Although StatsRecord uses unsigned types in memory, signed integers are used
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
