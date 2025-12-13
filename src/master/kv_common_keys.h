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
inline constexpr std::string_view kMetaHeaderKey = "META_HEADER";

/// Prefix for persisted session records.
/// Format: SESS_REC_<SessionId>:<SerializedSessionRecord>
/// - SessionId: uint32_t serialized as Big Endian
/// - Value: versioned serialized Session record (kSessionRecordVersion + payload)
/// @note SessionId is Big Endian to preserve numeric ordering in lexicographical
/// scans by the SESS_REC_ prefix.
/// @note This must stay disjoint from per-session secondary-index prefixes such as
/// kSessionOpenKeyPrefix because session records are loaded by prefix scan.
inline constexpr std::string_view kSessionKeyPrefix = "SESS_REC_";

/// Prefix for the per-session open-file index.
/// Format: SESS_OPEN_<SessionId><InodeId>:<EmptyValue>
/// - SessionId: uint32_t serialized as Big Endian
/// - InodeId: inode_t serialized as Big Endian
/// @note The (SessionId, InodeId) key layout enables efficient per-session scans
/// and range deletion of all open-file rows for a session.
/// @note Value is empty; key presence encodes membership in the open-file set.
inline constexpr std::string_view kSessionOpenKeyPrefix = "SESS_OPEN_";

/// Prefix for per-session runtime ownership state.
/// Format: SESS_STATE_<SessionId>:<SerializedSessionState>
/// - SessionId: uint32_t serialized as Big Endian
/// - Value: versioned runtime state used by the FDB session manager.
/// @note This row is intentionally separate from @c SESS_REC_ so the stable
///       session record schema can remain unchanged while MDS-local ownership
///       and disconnect state evolves.
inline constexpr std::string_view kSessionStateKeyPrefix = "SESS_STATE_";

inline constexpr std::string_view kInodeRangeStartKey = "META_NEXT_INODE_RANGE";
inline constexpr std::string_view kChunkRangeStartKey = "META_NEXT_CHUNK_RANGE";
inline constexpr std::string_view kSessionRangeStartKey = "META_NEXT_SESSION_RANGE";

/// Cluster-wide counter used to allocate stable mds_id values.
/// Format: META_NEXT_MDS_ID : <uint32_t LE>
/// Each MDS bootstrap does atomicAdd(+1) on this key then reads the
/// post-increment value as its assigned mds_id. Value 0 means "no mds_id
/// has ever been allocated"; allocated ids start at 1.
inline constexpr std::string_view kMetaNextMdsIdKey = "META_NEXT_MDS_ID";

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
inline constexpr std::string_view kNodeKeyPrefix = "NODE_";  // Section NODE 1.0

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
inline constexpr std::string_view kEdgeKeyPrefix = "EDGE_";  // Section EDGE 1.0

/// Prefix for free/reusable inode ids
/// Format: FREE_<InodeId>:<TimeStamp>
/// @note InodeId is serialized as Big Endian. TimeStamp is a 32-bit Big Endian value indicating
/// when the inode was freed.
inline constexpr std::string_view kFreeKeyPrefix = "FREE_";  // Section FREE 1.0

/// Prefix for chunk records (durable refcount + version)
/// Format: CHNK_<ChunkId> -> <Version><GoalCount>[<Goal><Count>]*
/// @note ChunkId (64-bit) is serialized as Big Endian in the key, keeping the keyspace in
/// chunk-id order for range scans. The value is a single datapack big-endian blob: Version
/// (32-bit), GoalCount (16-bit, number of (Goal, Count) pairs), then that many per-goal reference
/// counts (Goal 8-bit, Count 8-bit), mirroring ChunkGoalCounters. GoalCount is 16-bit because
/// per-goal overflow can split one goal across several entries, so the count may exceed 255.
/// Write locks live elsewhere (a future CHNK_LOCK_ prefix), so they are not part of this record.
inline constexpr std::string_view kChunkKeyPrefix = "CHNK_";  // Section CHNK 1.0

/// Prefix for extended attributes (xattrs)
/// Format: XATR_<InodeId><AttributeName>:<AttributeValue>
/// e.g.: XATR_1999UserAttr:UserValue
/// @note InodeId is serialized as Big Endian to maintain numeric order in lexicographical sorting,
/// enabling efficient range queries for all xattrs of a specific inode.
inline constexpr std::string_view kXAttrKeyPrefix = "XATR_";  // Section XATR 1.0

/// Prefix for Access Control Lists (ACLs)
/// Format: ACLS_<InodeId>:<binary RichACL>
/// e.g.: ACLS_1999:<binary data>
/// @note InodeId is serialized as Big Endian to maintain numeric order in lexicographical sorting,
/// enabling efficient range queries.
inline constexpr std::string_view kACLsKeyPrefix = "ACLS_";  // Section ACLS 1.2

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

/// Prefix for trash time-ordered entries (for periodic expiry scans)
/// Format: TRSH_TIME_<ExpiryTimestamp><InodeId>:<empty>
/// - ExpiryTimestamp: ctime + trashtime, 32-bit Big Endian
/// - InodeId: inode_t Big Endian (sizeof(inode_t) bytes)
/// @note Lexicographic order matches expiry time order, enabling efficient scan up to a deadline.
/// @note Value is empty; the key itself encodes all information needed for expiry scans.
inline constexpr std::string_view kTrashTimeKeyPrefix = "TRSH_TIME_";  // Section TRSH 1.0

/// Prefix for trash path entries (path lookup by inode)
/// Format: TRSH_PATH_<InodeId>:<PathString>
/// @note Written when a file enters trash; deleted on undel or purge.
inline constexpr std::string_view kTrashPathKeyPrefix = "TRSH_PATH_";  // Section TRSH 1.0

/// Prefix for reserved path entries (path lookup by inode)
/// Format: RSVD_PATH_<InodeId>:<PathString>
/// @note Written when a file enters the reserved list (unlinked with active sessions).
/// Deleted/Purged when all sessions pointing to the file release it.
inline constexpr std::string_view kReservedPathKeyPrefix = "RSVD_PATH_";  // Section RSVD 1.0

/// File-test scanner lease.
/// Format: FILETEST_LEASE:<OwnerMdsId><ExpiryTime><LeaseEpoch><ActiveScanGeneration>
/// - OwnerMdsId: uint32_t serialized as Big Endian
/// - ExpiryTime: uint32_t Unix timestamp serialized as Big Endian
/// - LeaseEpoch: uint64_t serialized as Big Endian
/// - ActiveScanGeneration: uint64_t serialized as Big Endian
inline constexpr std::string_view kFileTestLeaseKey = "FILETEST_LEASE";

/// Last-committed NODE_ key from the in-progress scan; the next page resumes after it.
/// Absent when no scan is in progress or at cycle start.
/// Format: FILETEST_CURSOR:<LastCommittedNodeKey>
/// - LastCommittedNodeKey: raw NODE_<InodeId> key bytes
/// @note InodeId inside LastCommittedNodeKey is serialized as Big Endian.
inline constexpr std::string_view kFileTestCursorKey = "FILETEST_CURSOR";

/// Running counters for the in-progress scan cycle; written by the lease owner each committed page.
/// Reset to a fresh Stats with the new generation when a cycle completes or a new one begins.
/// Format: FILETEST_ACTIVE_STATS:<ScanGeneration><LoopStart><LoopEnd><Files>
///                               <UnderGoalFiles><MissingFiles><Chunks>
///                               <UnderGoalChunks><MissingChunks><NotFoundChunks>
///                               <UnavailableChunks><UnavailableFiles>
///                               <UnavailableTrashFiles><UnavailableReservedFiles>
/// - ScanGeneration: uint64_t serialized as Big Endian
/// - LoopStart, LoopEnd: uint32_t Unix timestamps serialized as Big Endian
/// - Files, UnderGoalFiles, MissingFiles: inode_t serialized as Big Endian
/// - Chunks, UnderGoalChunks, MissingChunks, NotFoundChunks, UnavailableChunks:
///   uint32_t serialized as Big Endian
/// - UnavailableFiles, UnavailableTrashFiles, UnavailableReservedFiles:
///   inode_t serialized as Big Endian
inline constexpr std::string_view kFileTestActiveStatsKey = "FILETEST_ACTIVE_STATS";

/// Completed-cycle counters swapped atomically from FILETEST_ACTIVE_STATS at end-of-cycle.
/// Read by FSTEST_INFO and as the generation anchor for list-defective-files scans.
/// Format: FILETEST_PUBLISHED_STATS:<ScanGeneration><LoopStart><LoopEnd><Files>
///                                  <UnderGoalFiles><MissingFiles><Chunks>
///                                  <UnderGoalChunks><MissingChunks><NotFoundChunks>
///                                  <UnavailableChunks><UnavailableFiles>
///                                  <UnavailableTrashFiles><UnavailableReservedFiles>
/// @see kFileTestActiveStatsKey for field types and order.
inline constexpr std::string_view kFileTestPublishedStatsKey = "FILETEST_PUBLISHED_STATS";

/// Rendered human-readable report swapped atomically alongside FILETEST_PUBLISHED_STATS.
/// Built from the completed generation's FILETEST_REPORT_ rows; capped below FDB's value limit.
/// Format: FILETEST_PUBLISHED_REPORT:<ReportText>
/// - ReportText: raw bytes of the rendered report string with no length prefix
inline constexpr std::string_view kFileTestPublishedReportKey = "FILETEST_PUBLISHED_REPORT";

/// Prefix for per-defective-inode file-test rows.
/// Format: FILETEST_DEFECTIVE_<InodeId>:<ScanGeneration><Flags>
/// - InodeId: inode_t serialized as Big Endian
/// - ScanGeneration: uint64_t serialized as Big Endian
/// - Flags: uint8_t NodeErrorFlag bitmask
/// @note InodeId is encoded in the key to preserve numeric ordering for scans.
inline constexpr std::string_view kFileTestDefectiveKeyPrefix = "FILETEST_DEFECTIVE_";

/// Prefix for per-inode pre-rendered report fragments used to build FILETEST_PUBLISHED_REPORT.
/// Generation-scoped so publish can range-scan only the completed cycle and ignore stale rows.
/// Only inodes with visible FSTEST_INFO lines have a row; under-goal-only inodes are omitted
/// (Master also produces no report text for them, this is an intentional parity).
/// Format: FILETEST_REPORT_<KeyScanGeneration><InodeId>:<ScanGeneration><Errors><Text>
/// - KeyScanGeneration: uint64_t serialized as Big Endian
/// - InodeId: inode_t serialized as Big Endian
/// - ScanGeneration: uint64_t serialized as Big Endian
/// - Errors: uint32_t serialized as Big Endian
/// - Text: raw bytes of the pre-rendered report fragment with no length prefix
inline constexpr std::string_view kFileTestReportKeyPrefix = "FILETEST_REPORT_";

/// Prefix for locks
/// Format: FLCK_<InodeId:BE><Type:u8><Status:u8> → serialized lock entries
/// - InodeId: inode_t serialized as Big Endian (enables per-inode prefix scan)
/// - Type: u8 (safs_locks::Type – kFlock or kPosix)
/// - Status: u8 (0 = active, 1 = pending)
/// @note Inode-first key layout allows efficient per-inode scans using
/// prefix FLCK_<InodeId>. Numeric fields in the key are serialized as
/// Big Endian to preserve numeric order in lexicographical sorting.
/// The value contains all lock entries for the given (inode, type, status)
/// tuple, serialized as a contiguous sequence via FileLocks serialization.
inline constexpr std::string_view kLocksKeyPrefix = "FLCK_";  // Section FLCK 1.0

// Case-insensitive directory support

/// Prefix for case-insensitive edges
/// Format: LOWER_EDGE_<ParentId><LowercaseName>:<ChildId>
inline constexpr std::string_view kEdgeLowerKeyPrefix = "LOWER_EDGE_";

// Extra indexes for edges' reverse lookups/traversals

/// Prefix for reverse index for directories
/// Only one parent is allowed to maintain tree structure (preventing cycles or circular references)
/// Format: DIR_PARENT_<ChildId>:<ParentId>
inline constexpr std::string_view kDirParentKeyPrefix = "DIR_PARENT_";

/// Prefix for the name-bearing reverse index for all node types.
/// Directories have exactly one entry; files and links may have multiple (hard links).
/// Use this index to resolve an edge name given a (child, parent) pair efficiently.
/// @see kDirParentKeyPrefix for the parent-ID-only index used by directories.
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
