/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2017 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include "filesystem_node.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <type_traits>

#include "common/attributes.h"
#include "common/massert.h"
#include "common/slice_traits.h"
#include "common/special_inode_defs.h"
#include "common/type_defs.h"
#include "master/chunk_operations_interface.h"
#include "master/chunks.h"
#include "master/datacachemgr.h"
#include "master/filesystem_checksum.h"
#include "master/filesystem_metadata.h"
#include "master/filesystem_node_types.h"
#include "master/filesystem_operations.h"
#include "master/filesystem_periodic.h"
#include "master/filesystem_quota.h"
#include "master/fs_context.h"
#include "slogger/slogger.h"

// Private helper methods

uint32_t FilesystemNodeOperationsBase::lastChunkBlocks(FSNodeFile *node) {
	const uint64_t lastByte = node->length - 1;
	const uint32_t lastByteOffset = lastByte % SFSCHUNKSIZE;
	const uint32_t lastBlock = lastByteOffset / SFSBLOCKSIZE;
	const uint32_t blockCount = lastBlock + 1;
	return blockCount;
}

bool FilesystemNodeOperationsBase::isLastChunkNonEmpty(FSNodeFile *node) {
	std::size_t chunks = node->chunks.size();
	if (chunks == 0) {
		// no non-zero chunks, return now
		return false;
	}

	// file has non-zero length and contains at least one chunk
	const uint64_t lastByte = node->length - 1;
	const uint32_t lastChunk = lastByte / SFSCHUNKSIZE;
	if (lastChunk < chunks) {
		// last chunk exists, check if it isn't the zero chunk
		return node->chunks[lastChunk] != 0;
	}
	// last chunk hasn't been allocated yet
	return false;
}

uint32_t FilesystemNodeOperationsBase::fileChunksCount(FSNodeFile *node) {
	return std::accumulate(node->chunks.begin(), node->chunks.end(), (uint32_t)0,
	                       [](uint32_t sum, uint64_t v) { return sum + (v != 0); });
}

uint64_t FilesystemNodeOperationsBase::fileSize(FSNodeFile *node, uint32_t nonZeroChunks) {
	uint64_t size = static_cast<uint64_t>(nonZeroChunks) * (SFSCHUNKSIZE + SFSHDRSIZE);

	if (isLastChunkNonEmpty(node)) {
		size -= SFSCHUNKSIZE;
		size += lastChunkBlocks(node) * SFSBLOCKSIZE;
	}

	return size;
}

#ifndef METARESTORE
uint32_t FilesystemNodeOperationsBase::ecChunkRealSize(uint32_t blocks, uint32_t dataPartCount,
                                                       uint32_t parityPartCount) {
	constexpr uint32_t kTotalCrcSizePerChunkPart = SFSBLOCKSINCHUNK * sizeof(uint32_t);

	const uint32_t stripes = (blocks + dataPartCount - 1) / dataPartCount;
	uint32_t size = blocks * SFSBLOCKSIZE;             // file data
	size += parityPartCount * stripes * SFSBLOCKSIZE;  // parity data
	// CRCs of data and parity parts
	size += kTotalCrcSizePerChunkPart * (dataPartCount + parityPartCount);

	return size;
}
#endif

uint64_t FilesystemNodeOperationsBase::fileRealSize(FSNodeFile *node, uint32_t nonZeroChunks,
                                                    uint64_t logicalFileSize) {
#ifdef METARESTORE
	(void)node;
	(void)nonZeroChunks;
	(void)logicalFileSize;
	return 0;  // Doesn't really matter. Metarestore doesn't need this value
#else
	const Goal &goal = gFSOperations->getGoalDefinition(node->goal);

	uint64_t fullSize = 0;
	for (const auto &slice : goal) {
		if (slice_traits::isStandard(slice) || slice_traits::isTape(slice)) {
			fullSize += logicalFileSize * slice.getExpectedCopies();
		} else if (slice_traits::isXor(slice) || slice_traits::isEC(slice)) {
			int dataPartCount = slice_traits::getNumberOfDataParts(slice);
			int parityPartCount = slice_traits::getNumberOfParityParts(slice);

			uint32_t fullChunkRealSize =
			    ecChunkRealSize(SFSBLOCKSINCHUNK, dataPartCount, parityPartCount);
			uint64_t size = (uint64_t)nonZeroChunks * fullChunkRealSize;
			if (isLastChunkNonEmpty(node)) {
				size -= fullChunkRealSize;
				size += ecChunkRealSize(lastChunkBlocks(node), dataPartCount, parityPartCount);
			}
			fullSize += size;
		} else {
			safs::log_err("file_realsize: inode {} has unknown goal {:#x}", node->id, node->goal);
			return 0;
		}
	}

	return fullSize;
#endif
}

// Protected methods

FSNode *FilesystemNodeOperationsBase::idToNodeInternal(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, inode_t inode) const {
	// Find the node with the given id
	uint32_t nodeHashIndex = NODEHASHPOS(inode);

	for (const auto &node : gMetadata->nodeHash[nodeHashIndex]) {
		if (node->id == inode) { return node; }
	}

	return nullptr;
}

void FilesystemNodeOperationsBase::incrementNodeCounters(
    const FilesystemOperationContext &fsOpContext, FSNodeType type) {
	(void)fsOpContext;  // Unused parameter in this implementation

	gMetadata->nodes++;

	switch (type) {
	case FSNodeType::kDirectory:
		gMetadata->dirNodes++;
		break;
	case FSNodeType::kFile:
		gMetadata->fileNodes++;
		break;
	case FSNodeType::kSymlink:
		gMetadata->linkNodes++;
		break;
	default:
		break;
	}
}

void FilesystemNodeOperationsBase::preserveNode(const FilesystemOperationContext &fsOpContext,
                                                FSNode *node) {
	(void)fsOpContext;  // Unused parameter in this implementation
	gMetadata->addNode(node);
}

void FilesystemNodeOperationsBase::preserveEdge(const FilesystemOperationContext &fsOpContext,
                                                FSNodeDirectory *parent, FSNode *child,
                                                hstorage::Handle *handlePtr) {
	(void)fsOpContext;  // Unused parameter in this implementation

	// Just to keep the previous behavior
	gMetadata->edgeChangedSignal.emit(parent, child, handlePtr);
}

void FilesystemNodeOperationsBase::nodeQuotaUpdate(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, FSNode *node,
    const std::initializer_list<std::pair<QuotaResource, int64_t>> &resourceList) {
	fsnodes_quota_update(node, resourceList);
}

void FilesystemNodeOperationsBase::nodeQuotaRemove(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, QuotaOwnerType ownerType,
    inode_t ownerId) {
	fsnodes_quota_remove(ownerType, ownerId);
}

void FilesystemNodeOperationsBase::updateDetachedSpaceUsage(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, const FSNodeFile *nodeFile,
    uint64_t previousLength, uint64_t newLength) {
	if (previousLength == newLength) { return; }
	const uint64_t diffLength = newLength - previousLength;

	if (nodeFile->type == FSNodeType::kTrash) {
		gMetadata->trashSpace += diffLength;
	} else if (nodeFile->type == FSNodeType::kReserved) {
		gMetadata->reservedSpace += diffLength;
	}
}

// Public methods

FSNode *FilesystemNodeOperationsBase::lookup(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, FSNodeDirectory *node,
    const HString &name, bool isCaseInsensitive) const {
	// In-memory implementation: parameter intentionally unused to preserve legacy
	// behavior. This relies on the directory's caseInsensitive flag via find().
	// That flag is updated in getNodeForOperation when a directory node is accessed,
	// but may not reflect the current session's case-sensitivity for directories
	// previously accessed under different sessions. Alternative backend
	// implementations can override this method to use the isCaseInsensitive
	// parameter directly for consistent session-based lookups without relying on
	// mutable directory state.
	(void)isCaseInsensitive;

	auto iter = node->find(name);
	if (iter != node->end()) { return (*iter).second; }

	return nullptr;
}

void FilesystemNodeOperationsBase::updateCTime(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, FSNode *node, uint32_t ctime) {
	if (node->type == FSNodeType::kTrash && node->ctime != ctime) {
		auto oldKey = TrashPathKey(node);
		node->ctime = ctime;
		auto iter = gMetadata->trash.find(oldKey);
		if (iter != gMetadata->trash.end()) {
			updateTrashFromOldEntry(gMetadata->trash, node, oldKey);
		}
	} else {
		node->ctime = ctime;
	}
}

void FilesystemNodeOperationsBase::updateCTimeForTrashNode(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, FSNode *node, uint32_t newCtime,
    uint32_t oldTrashtime) {
	TrashPathKey oldKey(node->id, node->ctime, oldTrashtime);
	node->ctime = newCtime;
	updateTrashFromOldEntry(gMetadata->trash, node, oldKey);
}

std::string FilesystemNodeOperationsBase::escapeName(const std::string &name) {
	constexpr std::array<char, 16> hexDigits = {
	    {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'}};

	constexpr uint8_t kControlCharThreshold = 32;
	constexpr uint8_t kHighByteThreshold = 127;
	constexpr uint8_t kHexDigitShift = 4;
	constexpr uint8_t kHexDigitMask = 0xF;
	constexpr size_t kBytesPerEscapedChar = 3;  // '%' + 2 hex digits

	std::string result;

	// It could be possible to reserve 3 * name.length() bytes in result,
	// but it would lead to unnecessary allocations in some cases.
	// This would take much more time than computation of exact result size.
	// Hint: remember that std::string uses static allocation
	// for small string sizes.
	auto longCount = static_cast<size_t>(std::count_if(name.begin(), name.end(), [](char chr) {
		return chr < kControlCharThreshold || chr >= kHighByteThreshold || chr == ',' ||
		       chr == '%' || chr == '(' || chr == ')';
	}));

	result.reserve(((kBytesPerEscapedChar - 1) * longCount) + name.length());

	for (char chr : name) {
		if (chr < kControlCharThreshold || chr >= kHighByteThreshold || chr == ',' || chr == '%' ||
		    chr == '(' || chr == ')') {
			result.push_back('%');
			// Guaranteed to be in range 0-15 due to masking with kHexDigitMask (0xF)
			result.push_back(hexDigits[(chr >> kHexDigitShift) & kHexDigitMask]);
			result.push_back(hexDigits[chr & kHexDigitMask]);
		} else {
			result.push_back(chr);
		}
	}

	return result;
}

bool FilesystemNodeOperationsBase::isNameUsed(const FilesystemOperationContext &fsOpContext,
                                              FSNodeDirectory *node, const HString &name,
                                              bool isCaseInsensitive) {
	return lookup(fsOpContext, node, name, isCaseInsensitive) != nullptr;
}

bool FilesystemNodeOperationsBase::isAncestor(const FilesystemOperationContext &fsOpContext,
                                              FSNodeDirectory *ancestor, FSNode *node) {
	for (const auto &[parentId, _] : node->parents) {
		auto *dirNode = idToNodeVerify<FSNodeDirectory>(fsOpContext, parentId);

		while (dirNode != nullptr) {
			if (ancestor == dirNode) { return true; }

			assert(dirNode->parents.size() <= 1);

			if (!dirNode->parents.empty()) {
				dirNode = idToNodeVerify<FSNodeDirectory>(fsOpContext, dirNode->parents[0].first);
			} else {
				dirNode = nullptr;
			}
		}
	}

	return false;
}

bool FilesystemNodeOperationsBase::isAncestorOrNodeReservedOrTrash(
    const FilesystemOperationContext &fsOpContext, FSNodeDirectory *ancestor, FSNode *node) {
	// Return true if file is reserved:
	if (node && (node->type == FSNodeType::kReserved || node->type == FSNodeType::kTrash)) {
		return true;
	}
	// Or if ancestor is ancestor of node
	return isAncestor(fsOpContext, ancestor, node);
}

// stats

void FilesystemNodeOperationsBase::getStats(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, FSNode *node,
    StatsRecord *statsOut) {
	switch (node->type) {
	case FSNodeType::kDirectory:
		*statsOut = static_cast<FSNodeDirectory *>(node)->stats;
		statsOut->inodes++;
		statsOut->dirs++;
		break;
	case FSNodeType::kFile:
	case FSNodeType::kTrash:
	case FSNodeType::kReserved:
		statsOut->inodes = 1;
		statsOut->dirs = 0;
		statsOut->files = 1;
		statsOut->links = 0;
		statsOut->chunks = fileChunksCount(static_cast<FSNodeFile *>(node));
		statsOut->length = static_cast<FSNodeFile *>(node)->length;
		statsOut->size = fileSize(static_cast<FSNodeFile *>(node), statsOut->chunks);
		statsOut->realsize =
		    fileRealSize(static_cast<FSNodeFile *>(node), statsOut->chunks, statsOut->size);
		break;
	case FSNodeType::kSymlink:
		statsOut->inodes = 1;
		statsOut->links = 1;
		statsOut->files = 0;
		statsOut->dirs = 0;
		statsOut->chunks = 0;
		statsOut->length = static_cast<FSNodeSymlink *>(node)->path_length;
		statsOut->size = 0;
		statsOut->realsize = 0;
		break;
	default:
		statsOut->inodes = 1;
		statsOut->files = 0;
		statsOut->dirs = 0;
		statsOut->links = 0;
		statsOut->chunks = 0;
		statsOut->length = 0;
		statsOut->size = 0;
		statsOut->realsize = 0;
	}
}

int64_t FilesystemNodeOperationsBase::getSize(const FilesystemOperationContext &fsOpContext,
                                              FSNode *node) {
	StatsRecord stats;
	getStats(fsOpContext, node, &stats);
	return stats.size;
}

uint64_t FilesystemNodeOperationsBase::getNumberOfParents(
    const FilesystemOperationContext &fsOpContext, const FSNode *node) {
	(void)fsOpContext;  // unused in this implementation
	return node->parents.size();
}

FSNodeDirectory *FilesystemNodeOperationsBase::getFirstParent(
    const FilesystemOperationContext &fsOpContext, FSNode *node) {
	assert(node);

	if (!node->parents.empty()) {
		return idToNodeVerify<FSNodeDirectory>(fsOpContext, node->parents[0].first);
	}

	return gMetadata->root;
}

inode_t FilesystemNodeOperationsBase::getFirstParentId(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, FSNode *node) {
	assert(node);

	if (!node->parents.empty()) { return node->parents[0].first; }

	return 0;
}

std::vector<inode_t> FilesystemNodeOperationsBase::getParentIds(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, FSNode *node) {
	assert(node);

	std::vector<inode_t> parentIds;
	parentIds.reserve(node->parents.size());
	for (const auto &parent : node->parents) { parentIds.push_back(parent.first); }
	return parentIds;
}

std::string FilesystemNodeOperationsBase::getChildNameByParentId(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, inode_t parentId,
    const FSNode *node) {
	if (node == nullptr) { return {}; }
	for (const auto &[storedParentId, nameHandle] : node->parents) {
		if (storedParentId != parentId) { continue; }
		if (nameHandle == nullptr) { return {}; }
		return static_cast<std::string>(*nameHandle);
	}
	return {};
}

void FilesystemNodeOperationsBase::subStats(const FilesystemOperationContext &fsOpContext,
                                            FSNodeDirectory *parent, StatsRecord *stats) {
	if (parent != nullptr) {
		StatsRecord *parentStats = &parent->stats;
		parentStats->inodes -= stats->inodes;
		parentStats->dirs -= stats->dirs;
		parentStats->files -= stats->files;
		parentStats->links -= stats->links;
		parentStats->chunks -= stats->chunks;
		parentStats->length -= stats->length;
		parentStats->size -= stats->size;
		parentStats->realsize -= stats->realsize;

		if (parent != gMetadata->root) {
			for (auto const &[parentId, _] : parent->parents) {
				auto *node = idToNodeVerify<FSNodeDirectory>(fsOpContext, parentId);
				subStats(fsOpContext, node, stats);
			}
		}
	}
}

void FilesystemNodeOperationsBase::addStats(const FilesystemOperationContext &fsOpContext,
                                            FSNodeDirectory *parent, StatsRecord *stats) {
	if (parent != nullptr) {
		StatsRecord *parentStats = &parent->stats;
		parentStats->inodes += stats->inodes;
		parentStats->dirs += stats->dirs;
		parentStats->files += stats->files;
		parentStats->links += stats->links;
		parentStats->chunks += stats->chunks;
		parentStats->length += stats->length;
		parentStats->size += stats->size;
		parentStats->realsize += stats->realsize;

		if (parent != gMetadata->root) {
			for (auto const &[parentId, _] : parent->parents) {
				auto *node = idToNodeVerify<FSNodeDirectory>(fsOpContext, parentId);
				addStats(fsOpContext, node, stats);
			}
		}
	}
}

void FilesystemNodeOperationsBase::addSubStats(const FilesystemOperationContext &fsOpContext,
                                               FSNodeDirectory *parent, StatsRecord *newStats,
                                               StatsRecord *previousStats) {
	StatsRecord resultStats;
	resultStats.inodes = newStats->inodes - previousStats->inodes;
	resultStats.dirs = newStats->dirs - previousStats->dirs;
	resultStats.files = newStats->files - previousStats->files;
	resultStats.links = newStats->links - previousStats->links;
	resultStats.chunks = newStats->chunks - previousStats->chunks;
	resultStats.length = newStats->length - previousStats->length;
	resultStats.size = newStats->size - previousStats->size;
	resultStats.realsize = newStats->realsize - previousStats->realsize;
	addStats(fsOpContext, parent, &resultStats);
}

void FilesystemNodeOperationsBase::updateParentStatsForNode(
    const FilesystemOperationContext &fsOpContext, FSNode *node, StatsRecord *newStats,
    StatsRecord *previousStats) {
	for (const auto &[parentId, _] : node->parents) {
		auto *parentNode = idToNodeVerify<FSNodeDirectory>(fsOpContext, parentId);
		addSubStats(fsOpContext, parentNode, newStats, previousStats);
	}
}

void FilesystemNodeOperationsBase::fillAttr(const FilesystemOperationContext &fsOpContext,
                                            FSNode *node, FSNode *parent, uint32_t uid,
                                            uint32_t gid, uint32_t auid, uint32_t agid,
                                            uint8_t sesflags, Attributes &attr) {
#ifdef METARESTORE
	mabort("Bad code path - fsnodes_fill_attr() shall not be executed in metarestore context.");
#endif /* METARESTORE */
	uint8_t *ptr = attr.data();
	uint32_t nlink;

	// Type
	if (node->type == FSNodeType::kTrash || node->type == FSNodeType::kReserved) {
		put8bit(&ptr, FSNodeType::kFile);
	} else {
		put8bit(&ptr, node->type);
	}

	// Extract permission bits and discard extra attributes to allow selective re-application
	// based on inheritance and session flags
	uint16_t mode = node->mode & kPermissionsMask;

	// Inherit entry cache disable flag from parent directory
	if (parent != nullptr) {
		if (parent->mode & (EATTR_NOECACHE << EATTR_BIT_OFFSET)) {
			mode |= (MATTR_NOECACHE << EATTR_BIT_OFFSET);
		}
	}

	// Disable attribute caching if node has NOOWNER/NOACACHE or client is mapped to all users
	if ((node->mode & ((EATTR_NOOWNER | EATTR_NOACACHE) << EATTR_BIT_OFFSET)) ||
	    (sesflags & SESFLAG_MAPALL)) {
		mode |= (MATTR_NOACACHE << EATTR_BIT_OFFSET);
	}

	// Enable data caching unless node has NODATACACHE attribute
	if ((node->mode & (EATTR_NODATACACHE << EATTR_BIT_OFFSET)) == 0) {
		mode |= (MATTR_ALLOWDATACACHE << EATTR_BIT_OFFSET);
	}

	put16bit(&ptr, mode);

	// Effective UID and GID
	if ((node->mode & (EATTR_NOOWNER << EATTR_BIT_OFFSET)) && uid != 0) {
		if (sesflags & SESFLAG_MAPALL) {
			put32bit(&ptr, auid);
			put32bit(&ptr, agid);
		} else {
			put32bit(&ptr, uid);
			put32bit(&ptr, gid);
		}
	} else {
		if (sesflags & SESFLAG_MAPALL && auid != 0) {
			if (node->uid == uid) {
				put32bit(&ptr, auid);
			} else {
				put32bit(&ptr, 0);
			}
			if (node->gid == gid) {
				put32bit(&ptr, agid);
			} else {
				put32bit(&ptr, 0);
			}
		} else {
			put32bit(&ptr, node->uid);
			put32bit(&ptr, node->gid);
		}
	}

	// Timestamps. A directory's mtime/ctime are reconciled with its conflict-free
	// child-change time: a child create/remove records that key via atomicMax
	// instead of rewriting the parent node, so the node's own mtime/ctime can lag behind.
	uint32_t mtime = node->mtime;
	uint32_t ctime = node->ctime;
	if (node->type == FSNodeType::kDirectory) {
		const uint32_t childChange =
		    getDirChildChangeTime(fsOpContext, static_cast<FSNodeDirectory *>(node));
		if (childChange > mtime) { mtime = childChange; }
		if (childChange > ctime) { ctime = childChange; }
	}
	// atime is reconciled with its conflict-free read-advance key: a read records
	// atime via atomicMax on NODE_ATIME_ instead of rewriting the node, so node->atime can lag.
	uint32_t atime = node->atime;
	const uint32_t accessTime = getNodeAtime(fsOpContext, node);
	if (accessTime > atime) { atime = accessTime; }
	put32bit(&ptr, atime);
	put32bit(&ptr, mtime);
	put32bit(&ptr, ctime);

	// Number of links
	nlink = getNumberOfParents(fsOpContext, node);

	// Type-specific attributes

	constexpr uint32_t kGBBitShift = 30;  // Shift to convert bytes to GB

	switch (node->type) {
	case FSNodeType::kFile:
	case FSNodeType::kTrash:
	case FSNodeType::kReserved:
		put32bit(&ptr, nlink);
		put64bit(&ptr, static_cast<FSNodeFile *>(node)->length);
		break;
	case FSNodeType::kDirectory:
		// Served via getDirNlink so the KV backend can return 2 + persisted direct-subdir
		// count; the in-memory master returns the node's own nlink field.
		put32bit(&ptr, getDirNlink(fsOpContext, static_cast<FSNodeDirectory *>(node)));
		// Rescale length to GB (reduces size to 32-bit length)
		put64bit(&ptr, static_cast<FSNodeDirectory *>(node)->stats.length >> kGBBitShift);
		break;
	case FSNodeType::kSymlink:
		put32bit(&ptr, nlink);
		*ptr++ = 0;
		*ptr++ = 0;
		*ptr++ = 0;
		*ptr++ = 0;
		put32bit(&ptr, static_cast<FSNodeSymlink *>(node)->path_length);
		break;
	case FSNodeType::kBlockDev:
	case FSNodeType::kCharDev:
		put32bit(&ptr, nlink);
		put32bit(&ptr, static_cast<FSNodeDevice *>(node)->rdev);
		*ptr++ = 0;
		*ptr++ = 0;
		*ptr++ = 0;
		*ptr++ = 0;
		break;
	default:
		put32bit(&ptr, nlink);
		*ptr++ = 0;
		*ptr++ = 0;
		*ptr++ = 0;
		*ptr++ = 0;
		*ptr++ = 0;
		*ptr++ = 0;
		*ptr++ = 0;
		*ptr++ = 0;
	}
}

void FilesystemNodeOperationsBase::fillAttr(const FsContext &context,
                                            const FilesystemOperationContext &fsOpContext,
                                            FSNode *node, FSNode *parent, Attributes &attr) {
#ifdef METARESTORE
	mabort("Bad code path - fsnodes_fill_attr() shall not be executed in metarestore context.");
#endif /* METARESTORE */
	sassert(context.hasSessionData() && context.hasUidGidData());
	fillAttr(fsOpContext, node, parent, context.uid(), context.gid(), context.auid(),
	         context.agid(), context.sesflags(), attr);
}

void FilesystemNodeOperationsBase::removeEdge(const FilesystemOperationContext &fsOpContext,
                                              uint32_t timeStamp, FSNodeDirectory *parent,
                                              const HString &childName, FSNode *childNode) {
	(void)fsOpContext;  // Unused in this implementation
	assert(parent);

	auto dirIter = parent->find(childName);
	assert(dirIter != parent->end());
	assert((*dirIter).second == childNode);
	auto *handlePtrToErase = dirIter->first;

	if (dirIter != parent->end()) {
		parent->entries.erase(dirIter);
		parent->entries_hash ^= childName.hash();

		if (parent->caseInsensitive) {
			auto lowerCaseIt = parent->find_lowercase_container(childName);
			delete lowerCaseIt->first;
			parent->lowerCaseEntries.erase(lowerCaseIt);
			HString lowerCaseName = HString::hstringToLowerCase(childName);
		}
	}

	StatsRecord childStats;
	getStats(fsOpContext, childNode, &childStats);
	subStats(fsOpContext, parent, &childStats);

	parent->mtime = parent->ctime = timeStamp;

	if (childNode->type == FSNodeType::kDirectory) {
		parent->nlink--;
		persistDirSubdirCountDelta(fsOpContext, parent, -1);
	}

	fsnodes_update_checksum(parent);
	// Persist the parent's child-change time conflict-free; see link().
	persistDirChildChangeTime(fsOpContext, parent, timeStamp);

	// Emit node changed signal to notify changes in the parent directory
	if (gMetadata->nodeChangedSignal.size() > 0) { gMetadata->nodeChangedSignal.emit(parent); }

	HString currentName = childName;
	if (parent->caseInsensitive) { currentName = HString::hstringToLowerCase(childName); }

	auto iter = std::find_if(
	    childNode->parents.begin(), childNode->parents.end(),
	    [parent, currentName](const std::pair<inode_t, const hstorage::Handle *> &parentEntry) {
		    return parentEntry.first == parent->id &&
		           (parent->caseInsensitive ? HString::hstringToLowerCase(parentEntry.second->get())
		                                    : parentEntry.second->get()) == currentName;
	    });

	if (iter != childNode->parents.end()) { childNode->parents.erase(iter); }

	// Delete the handle after the check in the parent vector in the son is done.
	delete handlePtrToErase;

	assert(childNode->type != FSNodeType::kTrash);
	childNode->ctime = timeStamp;
	fsnodes_update_checksum(childNode);

	// Emit node changed signal to notify changes in the child node
	if (gMetadata->nodeChangedSignal.size() > 0) { gMetadata->nodeChangedSignal.emit(childNode); }

	gMetadata->edgeRemovedSignal.emit(parent->id, childName);
}

void FilesystemNodeOperationsBase::persistDirChildChangeTime(
    const FilesystemOperationContext & /*fsOpContext*/, FSNodeDirectory * /*dir*/,
    uint32_t /*timeStamp*/) {
	// Default no-op: the in-memory master keeps dir mtime/ctime in the node; the KV backend overrides.
}

uint32_t FilesystemNodeOperationsBase::getDirChildChangeTime(
    const FilesystemOperationContext & /*fsOpContext*/, const FSNodeDirectory * /*dir*/) {
	return 0;
}

void FilesystemNodeOperationsBase::resetDirChildChangeTime(
    const FilesystemOperationContext & /*fsOpContext*/, FSNodeDirectory * /*dir*/,
    uint32_t /*opTimeStamp*/) {
	// Default no-op: the in-memory master keeps a directory's mtime in the node itself.
}

void FilesystemNodeOperationsBase::persistDirSubdirCountDelta(
    const FilesystemOperationContext & /*fsOpContext*/, FSNodeDirectory * /*dir*/,
    int64_t /*delta*/) {
	// Default no-op: the in-memory master keeps a directory's nlink in the node itself.
}

uint32_t FilesystemNodeOperationsBase::getDirNlink(
    const FilesystemOperationContext & /*fsOpContext*/, const FSNodeDirectory *dir) {
	return dir->nlink;
}

void FilesystemNodeOperationsBase::persistNodeAtime(
    const FilesystemOperationContext & /*fsOpContext*/, FSNode * /*node*/, uint32_t /*timeStamp*/) {
	// Default no-op: the in-memory master keeps atime in the node; the KV backend overrides.
}

uint32_t FilesystemNodeOperationsBase::getNodeAtime(
    const FilesystemOperationContext & /*fsOpContext*/, const FSNode * /*node*/) {
	return 0;
}

void FilesystemNodeOperationsBase::resetNodeAtime(
    const FilesystemOperationContext & /*fsOpContext*/, FSNode * /*node*/) {
	// Default no-op: the in-memory master keeps a node's atime in the node itself.
}

void FilesystemNodeOperationsBase::link(const FilesystemOperationContext &fsOpContext,
                                        uint32_t timeStamp, FSNodeDirectory *parent, FSNode *child,
                                        const HString &name) {
	// Needs to be freed in fsnodes_remove_edge
	auto *handlePtr = new hstorage::Handle(name);
	parent->entries.insert({handlePtr, child});
	parent->entries_hash ^= name.hash();

	if (parent->caseInsensitive) {
		HString lowerCaseName = HString::hstringToLowerCase(name);
		// Needs to be freed in fsnodes_remove_edge
		auto *lowercaseHandlePtr = new hstorage::Handle(std::string(lowerCaseName.c_str()));
		parent->lowerCaseEntries.insert({lowercaseHandlePtr, child});
	}

	child->parents.push_back({parent->id, handlePtr});

	// Implementation specific (virtual) edge preservation (in-memory, FDB, etc.)
	preserveEdge(fsOpContext, parent, child, handlePtr);

	if (child->type == FSNodeType::kDirectory) {
		parent->nlink++;
		persistDirSubdirCountDelta(fsOpContext, parent, 1);
	}

	StatsRecord childStats;
	getStats(fsOpContext, child, &childStats);
	addStats(fsOpContext, parent, &childStats);

	if (timeStamp > 0) {
		parent->mtime = parent->ctime = timeStamp;
		fsnodes_update_checksum(parent);
		// Persist the parent's child-change time conflict-free so it survives a
		// reload without rewriting (and conflicting on) the whole parent node.
		persistDirChildChangeTime(fsOpContext, parent, timeStamp);
		// Emit node changed signal to notify changes in the parent directory
		if (gMetadata->nodeChangedSignal.size() > 0) { gMetadata->nodeChangedSignal.emit(parent); }
		assert(child->type != FSNodeType::kTrash);
		child->ctime = timeStamp;
		fsnodes_update_checksum(child);
		// Emit node changed signal to notify changes in the child node
		if (gMetadata->nodeChangedSignal.size() > 0) { gMetadata->nodeChangedSignal.emit(child); }
	}
}

FSNode *FilesystemNodeOperationsBase::createNode(
    const FilesystemOperationContext &fsOpContext, uint32_t timeStamp, FSNodeDirectory *parent,
    const HString &name, FSNodeType type, uint16_t mode, uint16_t umask, uint32_t uid, uint32_t gid,
    uint8_t copysgid, AclInheritance inheritAcl, inode_t requestedINode) {
	assert(type != FSNodeType::kTrash);

	FSNode *node = FSNode::create(type);
	incrementNodeCounters(fsOpContext, type);  // Increment global metadata counters

	// Ask for a node id
	node->id = gInodeIdGenerator->getNextId(timeStamp, requestedINode);

	// Init timestamps
	node->ctime = node->mtime = node->atime = timeStamp;

	// Inherit goal and trashtime from parent if applicable
	if (type == FSNodeType::kDirectory || type == FSNodeType::kFile) {
		node->goal = parent->goal;
		node->trashtime = parent->trashtime;
	} else {
		node->goal = DEFAULT_GOAL;
		node->trashtime = kDefaultTrashTime;
	}

	// Set mode (attributes + extra flags)
	if (type == FSNodeType::kDirectory) {
		node->mode = (mode & kPermissionsMask) | (parent->mode & kExtraAttributesMask);
	} else {
		node->mode =
		    (mode & kPermissionsMask) |
		    (parent->mode & (kExtraAttributesMask & (~(EATTR_NOECACHE << EATTR_BIT_OFFSET))));
	}

	// If desired, node inherits permissions from parent's default ACL
	std::optional<RichACL> parentAclScratch;
	const RichACL *parentAcl = (inheritAcl == AclInheritance::kInheritAcl)
	                               ? getAclForAccess(fsOpContext, parent, parentAclScratch)
	                               : nullptr;
	if (parentAcl != nullptr) {
		RichACL acl;
		uint16_t mode = node->mode;
		if (RichACL::inheritInode(*parentAcl, mode, acl, umask, type == FSNodeType::kDirectory)) {
			storeInheritedAcl(fsOpContext, node, std::move(acl));
		}
		// Set effective permissions as the intersection of mode and ACL
		node->mode &= mode | ~kStandardPermissionsMask;
	} else {
		// Apply umask
		node->mode &= ~(umask & kStandardPermissionsMask);  // umask must be applied manually
	}

	node->uid = uid;  // owner

	if ((parent->mode & S_ISGID) == S_ISGID) {  // Set-GID flag is set in the parent directory?
		node->gid = parent->gid;
		if (copysgid && type == FSNodeType::kDirectory) { node->mode |= S_ISGID; }
	} else {
		node->gid = gid;
	}

	// Implementation specific (virtual) node preservation (in-memory, FDB, etc.)
	preserveNode(fsOpContext, node);

	fsnodes_update_checksum(node);
	link(fsOpContext, timeStamp, parent, node, name);
	nodeQuotaUpdate(fsOpContext, node, {{QuotaResource::kInodes, +1}});

	if (type == FSNodeType::kFile) {
		nodeQuotaUpdate(fsOpContext, node, {{QuotaResource::kSize, +getSize(fsOpContext, node)}});
	}

	return node;
}

void FilesystemNodeOperationsBase::updateNode(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, [[maybe_unused]] FSNode *node) {
	// Default implementation does nothing, it is not needed for the in-memory backend
}

uint32_t FilesystemNodeOperationsBase::getPathSize(const FilesystemOperationContext &fsOpContext,
                                                   FSNodeDirectory *parent, FSNode *child) {
	if (parent == nullptr || child == nullptr) { return 0; }

	std::string name = parent->getChildName(child);
	uint32_t size = name.length();

	while (parent != gMetadata->root && !parent->parents.empty()) {
		child = parent;
		assert(child->parents.size() == 1);
		parent = idToNodeVerify<FSNodeDirectory>(fsOpContext, child->parents[0].first);
		name = parent->getChildName(child);
		size += name.length() + 1;
	}

	return size;
}

void FilesystemNodeOperationsBase::getPathData(const FilesystemOperationContext &fsOpContext,
                                               FSNodeDirectory *parent, FSNode *child,
                                               uint8_t *path, uint32_t size) {
	if (parent == nullptr || child == nullptr) { return; }

	std::string name = parent->getChildName(child);

	if (size >= name.length()) {
		size -= name.length();
		memcpy(path + size, name.c_str(), name.length());
	} else if (size > 0) {
		memcpy(path, name.c_str() + (name.length() - size), size);
		size = 0;
	}

	if (size > 0) { path[--size] = '/'; }

	while (parent != gMetadata->root && !parent->parents.empty()) {
		child = parent;
		assert(child->parents.size() == 1);
		parent = idToNodeVerify<FSNodeDirectory>(fsOpContext, child->parents[0].first);
		name = parent->getChildName(child);

		if (size >= name.length()) {
			size -= name.length();
			memcpy(path + size, name.c_str(), name.length());
		} else if (size > 0) {
			memcpy(path, name.c_str() + (name.length() - size), size);
			size = 0;
		}

		if (size > 0) { path[--size] = '/'; }
	}
}

void FilesystemNodeOperationsBase::getPath(const FilesystemOperationContext &fsOpContext,
                                           FSNodeDirectory *parent, FSNode *child,
                                           std::string &path) {
	uint32_t size = getPathSize(fsOpContext, parent, child);

	if (size > FSNode::kEdgeNameMaxSize) {
		safs::log_warn("path too long !!! - truncate");
		size = FSNode::kEdgeNameMaxSize;
	}

	path.resize(size);

	getPathData(fsOpContext, parent, child, (uint8_t *)path.data(), size);
}

#ifndef METARESTORE
constexpr uint32_t kOldPathContainerLimit = 1000000;

// Constants for detached data handling (trash/reserved paths)

constexpr uint32_t kDetachedMaxNameLength = 240;
constexpr uint32_t kDetachedEllipsisLength = 5;  // length of "(...)"
constexpr uint32_t kDetachedTruncatedLength = kDetachedMaxNameLength - kDetachedEllipsisLength;
constexpr uint32_t kDetachedSizeByteLength = 1;  // Size of the length byte for detached names

// Inlined implementation details for detached data

template <class T>
static inline uint32_t getDetachedSizeGenericInternal(const T &data) {
	static_assert(std::is_same_v<T, TrashPathContainer> || std::is_same_v<T, ReservedPathContainer>,
	              "unsupported container");
	uint32_t result = 0;
	std::string name;
	uint32_t count = 0;

	for (const auto &entry : data) {
		if (count > kOldPathContainerLimit) {
			// See explanation in getDetachedData
			break;
		}

		name = (std::string)entry.second;

		if (name.length() > kDetachedMaxNameLength) {
			result += kDetachedSizeByteLength + kDetachedEllipsisLength + kDetachedTruncatedLength +
			          kinode_t_size;
		} else {
			result += kDetachedSizeByteLength + name.length() + kinode_t_size;
		}

		count++;
	}

	return result;
}

static inline inode_t getDetachedDataNodeId(const TrashPathContainer::key_type &key) {
	return key.id;
}

static inline inode_t getDetachedDataNodeId(const inode_t &key) { return key; }

template <class T>
static inline void getDetachedDataGenericInternal(const T &data, uint8_t *dbuff) {
	static_assert(std::is_same_v<T, TrashPathContainer> || std::is_same_v<T, ReservedPathContainer>,
	              "unsupported container");

	uint8_t *sptr;
	uint8_t c;
	std::string name;
	// Limit to 1 million, see explanation below
	uint32_t count = 0;

	for (const auto &entry : data) {
		if (count > kOldPathContainerLimit) {
			// Due to the size limit of packets (at time of writing,
			// UINT32_MAX), if we write any more we risk overflowing buffer.
			// While this could be done better rather than an arbitrary limit,
			// there's already an alternative in client library that allows
			// buffered reads from trash/reserved.
			safs::log_warn("getdetachedsize: path container size longer than {}, truncating",
			               kOldPathContainerLimit);
			break;
		}

		name = (std::string)entry.second;

		if (name.length() > kDetachedMaxNameLength) {
			*dbuff = kDetachedMaxNameLength;  // Serialize name length byte
			dbuff++;
			memcpy(dbuff, "(...)", kDetachedEllipsisLength);
			dbuff += kDetachedEllipsisLength;
			sptr = (uint8_t *)name.c_str() + (name.length() - kDetachedTruncatedLength);
			for (c = 0; c < kDetachedTruncatedLength; c++) {
				if (*sptr == '/') {
					*dbuff = '|';
				} else {
					*dbuff = *sptr;
				}
				sptr++;
				dbuff++;
			}
		} else {
			*dbuff = name.length();  // Serialize name length byte
			dbuff++;
			sptr = (uint8_t *)name.c_str();
			for (c = 0; c < name.length(); c++) {
				if (*sptr == '/') {
					*dbuff = '|';
				} else {
					*dbuff = *sptr;
				}
				sptr++;
				dbuff++;
			}
		}

		count++;
		putINode(&dbuff, getDetachedDataNodeId(entry.first));
	}
}

uint32_t FilesystemNodeOperationsBase::getDetachedSize(const TrashPathContainer &data) {
	return getDetachedSizeGenericInternal(data);
}

void FilesystemNodeOperationsBase::getDetachedData(const TrashPathContainer &data,
                                                   uint8_t *outBuffer) {
	getDetachedDataGenericInternal(data, outBuffer);
}

void FilesystemNodeOperationsBase::getDetachedData(const TrashPathContainer &data, uint32_t offset,
                                                   uint32_t maxEntries,
                                                   std::vector<NamedInodeEntry> &entries) {
#if defined(SAUNAFS_HAVE_64BIT_JUDY) && !defined(DISABLE_JUDY_FOR_TRASHPATHCONTAINER)
	auto iter = data.find_nth(offset);
#else
	auto iter = offset < data.size() ? std::next(data.begin(), offset) : data.end();
#endif
	for (; maxEntries > 0 && iter != data.end(); maxEntries--, ++iter) {
		entries.emplace_back((std::string)(*iter).second, (*iter).first.id);
	}
}

void FilesystemNodeOperationsBase::getDetachedData(const FilesystemOperationContext &fsOpContext,
                                                   const HandleIndexContainer &data,
                                                   uint64_t handleOffset, uint32_t maxEntries,
                                                   std::vector<HandleInodeEntry> &entries,
                                                   bool fromTrash) {
	uint64_t start = (handleOffset & ~k64SignBitMask);
	auto iter = data.lower_bound(HandleIndexKey(start));

	for (; maxEntries > 0 && iter != data.end(); --maxEntries, ++iter) {
		// Ensure we only return entries with the sign bit cleared
		// to the client to avoid sending negative offsets to fuse
		// when requesting next one
		uint64_t handleValueForClient = (*iter).first.data & ~k64SignBitMask;
		std::string nameForClient;

		if (fromTrash) {
			FSNode *node = idToNode(fsOpContext, (*iter).second);
			nameForClient = gMetadata->trash.at(TrashPathKey(node)).get().c_str();
		} else {
			nameForClient = gMetadata->reserved.at((*iter).second).get().c_str();
		}

		entries.emplace_back(handleValueForClient, nameForClient, (*iter).second);
	}
}

uint32_t FilesystemNodeOperationsBase::getDetachedSize(const ReservedPathContainer &data) {
	return getDetachedSizeGenericInternal(data);
}

void FilesystemNodeOperationsBase::getDetachedData(const ReservedPathContainer &data,
                                                   uint8_t *outBuffer) {
	getDetachedDataGenericInternal(data, outBuffer);
}

void FilesystemNodeOperationsBase::getDetachedData(const ReservedPathContainer &data,
                                                   uint32_t offset, uint32_t maxEntries,
                                                   std::vector<NamedInodeEntry> &entries) {
#if defined(SAUNAFS_HAVE_64BIT_JUDY) && !defined(DISABLE_JUDY_FOR_RESERVEDPATHCONTAINER)
	auto iter = data.find_nth(offset);
#else
	auto iter = offset < data.size() ? std::next(data.begin(), offset) : data.end();
#endif
	for (; maxEntries > 0 && iter != data.end(); maxEntries--, ++iter) {
		entries.emplace_back((std::string)(*iter).second, (*iter).first);
	}
}

uint32_t FilesystemNodeOperationsBase::getDirSize(const FSNodeDirectory *nodeDir,
                                                  uint8_t withAttr) {
	uint32_t entryBaseSize = kDetachedSizeByteLength + (withAttr ? kDirEntryWithAttributesSize
	                                                             : kDirEntryWithoutAttributesSize);
	uint32_t result = (entryBaseSize * 2) + kDotEntrySize + kDotDotEntrySize;  // for '.' and '..'
	std::string name;

	for (const auto &entry : nodeDir->entries) {
		name = (std::string)(*entry.first);
		result += entryBaseSize + name.length();
	}

	return result;
}

void FilesystemNodeOperationsBase::getDirData(const FilesystemOperationContext &fsOpContext,
                                              inode_t rootINode, uint32_t uid, uint32_t gid,
                                              uint32_t auid, uint32_t agid, uint8_t sesflags,
                                              FSNodeDirectory *nodeDir, uint8_t *outBuffer,
                                              uint8_t withAttr) {
	// '.' - self
	outBuffer[0] = kDotEntrySize;
	outBuffer[1] = '.';
	outBuffer += kDotEntrySize + kDetachedSizeByteLength;
	if (nodeDir->id != rootINode) {
		putINode(&outBuffer, nodeDir->id);
	} else {
		putINode(&outBuffer, SPECIAL_INODE_ROOT);
	}

	// attributes
	Attributes attr;

	if (withAttr) {
		fillAttr(fsOpContext, nodeDir, nodeDir, uid, gid, auid, agid, sesflags, attr);
		::memcpy(outBuffer, attr.data(), attr.size());
		outBuffer += attr.size();
	} else {
		put8bit(&outBuffer, static_cast<uint8_t>(FSNodeType::kDirectory));
	}

	// '..' - parent
	outBuffer[0] = kDotDotEntrySize;
	outBuffer[1] = '.';
	outBuffer[2] = '.';
	outBuffer += kDotDotEntrySize + kDetachedSizeByteLength;

	if (nodeDir->id == rootINode) {  // root node should returns self as its parent
		putINode(&outBuffer, SPECIAL_INODE_ROOT);

		// parent attributes
		if (withAttr) {
			fillAttr(fsOpContext, nodeDir, nodeDir, uid, gid, auid, agid, sesflags, attr);
			::memcpy(outBuffer, attr.data(), attr.size());
			outBuffer += attr.size();
		} else {
			put8bit(&outBuffer, static_cast<uint8_t>(FSNodeType::kDirectory));
		}
	} else {
		if (!nodeDir->parents.empty() && nodeDir->parents[0].first != rootINode) {
			putINode(&outBuffer, nodeDir->parents[0].first);
		} else {
			putINode(&outBuffer, SPECIAL_INODE_ROOT);
		}

		// parent attributes
		if (withAttr) {
			if (!nodeDir->parents.empty()) {
				auto *parent = idToNodeVerify<FSNode>(fsOpContext, nodeDir->parents[0].first);
				fillAttr(fsOpContext, parent, nodeDir, uid, gid, auid, agid, sesflags, attr);
				::memcpy(outBuffer, attr.data(), attr.size());
			} else {
				if (rootINode == SPECIAL_INODE_ROOT) {
					fillAttr(fsOpContext, gMetadata->root, nodeDir, uid, gid, auid, agid, sesflags,
					         attr);
					::memcpy(outBuffer, attr.data(), attr.size());
				} else {
					FSNode *foundRootNode = idToNode(fsOpContext, rootINode);
					if (foundRootNode) {  // it should be always true because it's checked
						// before, but better check than sorry
						fillAttr(fsOpContext, foundRootNode, nodeDir, uid, gid, auid, agid,
						         sesflags, attr);
						::memcpy(outBuffer, attr.data(), attr.size());
					} else {
						memset(outBuffer, 0, attr.size());
					}
				}
			}

			outBuffer += attr.size();
		} else {
			put8bit(&outBuffer, static_cast<uint8_t>(FSNodeType::kDirectory));
		}
	}

	// entries
	std::string name;
	for (const auto &entry : nodeDir->entries) {
		// entry name and inode
		name = (std::string)(*entry.first);
		outBuffer[0] = name.size();
		outBuffer++;
		memcpy(outBuffer, name.c_str(), name.length());
		outBuffer += name.length();
		putINode(&outBuffer, entry.second->id);

		// entry attributes
		if (withAttr) {
			fillAttr(fsOpContext, entry.second, nodeDir, uid, gid, auid, agid, sesflags, attr);
			::memcpy(outBuffer, attr.data(), attr.size());
			outBuffer += attr.size();
		} else {
			put8bit(&outBuffer, static_cast<uint8_t>(entry.second->type));
		}
	}
}

uint64_t FilesystemNodeOperationsBase::getNumberOfDirEntries(
    const FilesystemOperationContext &fsOpContext, const FSNodeDirectory *nodeDir) {
	(void)fsOpContext;  // unused in this implementation
	return nodeDir->entries.size();
}

void FilesystemNodeOperationsBase::getDir(const FilesystemOperationContext &fsOpContext,
                                          inode_t rootINode, uint32_t uid, uint32_t gid,
                                          uint32_t auid, uint32_t agid, uint8_t sesflags,
                                          FSNodeDirectory *nodeDir, uint64_t firstEntry,
                                          uint64_t numberOfEntries,
                                          std::vector<DirectoryEntry> &dirEntriesOut) {
	sassert(!(firstEntry & kSignBit64));

	FSNodeDirectory *parent;
	inode_t inode;
	Attributes attr;

	// Handle the "." entry if starting from there and entries are requested
	if (firstEntry == kDotEntryIndex && numberOfEntries >= 1) {
		inode = nodeDir->id != rootINode ? nodeDir->id : SPECIAL_INODE_ROOT;
		parent = idToNodeVerify<FSNodeDirectory>(
		    fsOpContext, nodeDir->parents.empty() ? SPECIAL_INODE_ROOT : nodeDir->parents[0].first);
		fillAttr(fsOpContext, nodeDir, parent, uid, gid, auid, agid, sesflags, attr);
		dirEntriesOut.emplace_back(kDotEntryIndex, kDotDotEntryIndex, inode, ".", attr);

		firstEntry = kDotDotEntryIndex;
		--numberOfEntries;
	}

	// Handle the ".." entry if starting from its index and entries are requested
	if (firstEntry == kDotDotEntryIndex && numberOfEntries >= 1) {
		if (nodeDir->id == rootINode) {
			inode = SPECIAL_INODE_ROOT;
			parent = idToNodeVerify<FSNodeDirectory>(fsOpContext, nodeDir->parents.empty()
			                                                          ? SPECIAL_INODE_ROOT
			                                                          : nodeDir->parents[0].first);
			fillAttr(fsOpContext, nodeDir, parent, uid, gid, auid, agid, sesflags, attr);
		} else {
			if (!nodeDir->parents.empty() && nodeDir->parents[0].first != rootINode) {
				inode = nodeDir->parents[0].first;
			} else {
				inode = SPECIAL_INODE_ROOT;
			}

			parent = idToNodeVerify<FSNodeDirectory>(fsOpContext, nodeDir->parents.empty()
			                                                          ? SPECIAL_INODE_ROOT
			                                                          : nodeDir->parents[0].first);
			auto *grandparent = idToNodeVerify<FSNodeDirectory>(
			    fsOpContext,
			    parent->parents.empty() ? SPECIAL_INODE_ROOT : parent->parents[0].first);
			fillAttr(fsOpContext, parent, grandparent, uid, gid, auid, agid, sesflags, attr);
		}

		uint64_t nextIndex = kUnusedEntryIndex;
		if (!nodeDir->entries.empty()) {
			auto firstDirentIt = nodeDir->find_nth(0);
			nextIndex = (*firstDirentIt).first->data() & ~kSignBit64;
		}
		dirEntriesOut.emplace_back(kDotDotEntryIndex, nextIndex, inode, "..", attr);

		firstEntry = nextIndex;
		--numberOfEntries;
	}

	// Early exit if no more entries to process or directory is empty
	if (numberOfEntries == 0 || nodeDir->entries.empty()) { return; }

	std::string name;
	hstorage::Handle firstIndex(firstEntry);

	// We're trying to find the first entry in the directory that has index
	// equal to firstEntry. We don't know the second part of the pair, so we
	// use kUnknownNode as a placeholder, and it is also the minimum possible.
	auto pairToFind = std::make_pair(&firstIndex, kUnknownNode);
	auto iter = nodeDir->entries.lower_bound(pairToFind);

	if (iter != nodeDir->entries.end() && (*iter).first->data() != firstEntry) {
		// We assume that we received hash that had its most significant bit stripped so we try new
		// find with this supposedly stripped bit set again.
		firstIndex.unlink();  // do not try to unbind the resource under this
		                      // possibly-fake handle in destructor
		firstIndex = hstorage::Handle(firstEntry | kSignBit64);
		pairToFind = std::make_pair(&firstIndex, kUnknownNode);
		iter = nodeDir->entries.lower_bound(pairToFind);
		if (iter != nodeDir->entries.end() && (*iter).first->data() != (firstEntry | kSignBit64)) {
			iter = nodeDir->entries.end();
		}
	}

	// Do not try to unbind the resource under this possibly-fake handle in destructor
	pairToFind.first->unlink();

	// Iterate through the directory entries, collecting up to numberOfEntries entries
	while (iter != nodeDir->entries.end() && numberOfEntries > 0) {
		name = static_cast<std::string>(*(*iter).first);
		inode = (*iter).second->id;
		fillAttr(fsOpContext, (*iter).second, nodeDir, uid, gid, auid, agid, sesflags, attr);

		firstEntry = (*iter).first->data() & ~kSignBit64;

		uint64_t nextIndex = kUnusedEntryIndex;
		if (++iter != nodeDir->entries.end()) { nextIndex = (*iter).first->data() & ~kSignBit64; }

		dirEntriesOut.emplace_back(firstEntry, nextIndex, inode, std::move(name), attr);

		--numberOfEntries;
	}
}

void FilesystemNodeOperationsBase::checkFile(FSNodeFile *nodeFile, ChunkCountArray &chunkCount) {
	uint8_t count;

	chunkCount.fill(0);

	for (const auto &chunkid : nodeFile->chunks) {
		if (chunkid > 0) {
			gChunkOperations->getFullCopies(chunkid, &count);
			count = std::min<unsigned>(count, CHUNK_MATRIX_SIZE - 1);
			chunkCount[count]++;
		}
	}
}
#endif

std::vector<inode_t> FilesystemNodeOperationsBase::getDirectoryChildInodes(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext,
    const FSNodeDirectory *nodeDir) {
	std::vector<inode_t> childInodes;
	if (nodeDir == nullptr) { return childInodes; }

	childInodes.reserve(nodeDir->entries.size());
	for (const auto &entry : nodeDir->entries) {
		if (entry.second != nullptr) { childInodes.push_back(entry.second->id); }
	}

	return childInodes;
}

std::vector<std::pair<HString, inode_t>> FilesystemNodeOperationsBase::getDirectoryChildEdges(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, const FSNodeDirectory *nodeDir) {
	std::vector<std::pair<HString, inode_t>> childEdges;
	if (nodeDir == nullptr) { return childEdges; }

	childEdges.reserve(nodeDir->entries.size());
	for (const auto &entry : nodeDir->entries) {
		if (entry.second != nullptr) {
			childEdges.emplace_back(HString(*entry.first), entry.second->id);
		}
	}

	return childEdges;
}

std::string FilesystemNodeOperationsBase::getBaseStoredChildName(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, FSNodeDirectory *nodeDir,
    const HString &anyCaseName) {
	if (nodeDir == nullptr) { return {}; }
	return nodeDir->getBaseStoredChildName(anyCaseName);
}

uint8_t FilesystemNodeOperationsBase::appendChunks(const FilesystemOperationContext &fsOpContext,
                                                   uint32_t timeStamp, FSNodeFile *destNodeFile,
                                                   FSNodeFile *srcNodeFile) {
	if (srcNodeFile->chunks.empty()) { return SAUNAFS_STATUS_OK; }

	uint32_t srcChunks = srcNodeFile->chunkCount();
	uint32_t dstChunks = destNodeFile->chunkCount();

	if (((uint64_t)srcChunks + (uint64_t)dstChunks) > ((uint64_t)kMaxChunkIndex + 1)) {
		return SAUNAFS_ERROR_INDEXTOOBIG;
	}

	StatsRecord previousStats;
	StatsRecord newStats;
	getStats(fsOpContext, destNodeFile, &previousStats);

	uint32_t resultChunks = srcChunks + dstChunks;
	destNodeFile->chunks.resize(resultChunks, 0);

	// Copy source chunks to the end of destination chunks
	std::copy(srcNodeFile->chunks.begin(), srcNodeFile->chunks.begin() + srcChunks,
	          destNodeFile->chunks.begin() + dstChunks);

	// Add each source chunk to the destination file's goal and handle errors
	for (uint32_t i = 0; i < srcChunks; ++i) {
		auto chunkId = srcNodeFile->chunks[i];
		if (chunkId > 0) {
			if (gChunkOperations->addFile(fsOpContext, chunkId, destNodeFile->goal) !=
			    SAUNAFS_STATUS_OK) {
				safs::log_err("structure error - chunk {:016X} not found (inode: {} ; index: {})",
				              chunkId, srcNodeFile->id, i);
			}
		}
	}

	uint64_t previousLength = destNodeFile->length;

	// Calculate the new total length after appending
	uint64_t length = (static_cast<uint64_t>(dstChunks) << SFSCHUNKBITS) + srcNodeFile->length;

	updateDetachedSpaceUsage(fsOpContext, destNodeFile, previousLength, length);

	destNodeFile->length = length;
	getStats(fsOpContext, destNodeFile, &newStats);

	// Update quotas based on the change in file size
	nodeQuotaUpdate(fsOpContext, destNodeFile,
	                {{QuotaResource::kSize, newStats.size - previousStats.size}});

	// Update stats for all parent directories via backend-specific parent lookup.
	for (inode_t parentId : getParentIds(fsOpContext, destNodeFile)) {
		auto *parentNode = idToNodeVerify<FSNodeDirectory>(fsOpContext, parentId);
		addSubStats(fsOpContext, parentNode, &newStats, &previousStats);
	}

	// Update timestamps and checksums

	destNodeFile->mtime = timeStamp;
	destNodeFile->atime = timeStamp;
	srcNodeFile->atime = timeStamp;

	fsnodes_update_checksum(srcNodeFile);
	fsnodes_update_checksum(destNodeFile);

	return SAUNAFS_STATUS_OK;
}

void FilesystemNodeOperationsBase::changeFileGoal(const FilesystemOperationContext &fsOpContext,
                                                  FSNodeFile *nodeFile, uint8_t goal) {
	uint8_t oldGoal = nodeFile->goal;
	StatsRecord previousStats;
	StatsRecord newStats;

	getStats(fsOpContext, nodeFile, &previousStats);
	nodeFile->goal = goal;

	newStats = previousStats;
	newStats.realsize = fileRealSize(nodeFile, newStats.chunks, newStats.size);

	for (const auto &[parentId, _] : nodeFile->parents) {
		auto *parentNode = idToNodeVerify<FSNodeDirectory>(fsOpContext, parentId);
		addSubStats(fsOpContext, parentNode, &newStats, &previousStats);
	}

	for (const auto &chunkId : nodeFile->chunks) {
		if (chunkId > 0) { gChunkOperations->changeFile(fsOpContext, chunkId, oldGoal, goal); }
	}

	fsnodes_update_checksum(nodeFile);

	if (!fsOpContext.hasReadWriteTransaction()) { gMetadata->nodeChangedSignal.emit(nodeFile); }
}

void FilesystemNodeOperationsBase::setLength(const FilesystemOperationContext &fsOpContext,
                                             FSNodeFile *nodeFile, uint64_t length,
                                             bool eraseFurtherChunks) {
	uint32_t chunks = 0;
	StatsRecord previousStats;
	StatsRecord newStats;
	uint64_t previousLength = nodeFile->length;
	getStats(fsOpContext, nodeFile, &previousStats);

	updateDetachedSpaceUsage(fsOpContext, nodeFile, previousLength, length);

	nodeFile->length = length;

	if (eraseFurtherChunks) {
		if (length > 0) {
			chunks = ((length - 1) >> SFSCHUNKBITS) + 1;
		} else {
			chunks = 0;
		}

		for (uint32_t i = chunks; i < nodeFile->chunks.size(); i++) {
			uint64_t chunkId = nodeFile->chunks[i];
			if (chunkId > 0) {
				if (gChunkOperations->deleteFile(fsOpContext, chunkId, nodeFile->goal) !=
				    SAUNAFS_STATUS_OK) {
					safs::log_err(
					    "structure error - chunk {:#016x} not found (inode: {} ; index: {})",
					    chunkId, nodeFile->id, i);
				}
			}
		}

		if (chunks < nodeFile->chunks.size()) { nodeFile->chunks.resize(chunks); }
	}

	getStats(fsOpContext, nodeFile, &newStats);

	nodeQuotaUpdate(fsOpContext, nodeFile,
	                {{QuotaResource::kSize, newStats.size - previousStats.size}});

	updateParentStatsForNode(fsOpContext, nodeFile, &newStats, &previousStats);

	fsnodes_update_checksum(nodeFile);

	if (!fsOpContext.hasTransaction()) { gMetadata->nodeChangedSignal.emit(nodeFile); }
}

void FilesystemNodeOperationsBase::changeUidGid(const FilesystemOperationContext &fsOpContext,
                                                FSNode *node, uint32_t uid, uint32_t gid) {
	int64_t size = 0;

	// Decrease quota for old owner
	if (node->type == FSNodeType::kFile || node->type == FSNodeType::kTrash ||
	    node->type == FSNodeType::kReserved) {
		size = getSize(fsOpContext, node);
		nodeQuotaUpdate(fsOpContext, node,
		                {{QuotaResource::kInodes, -1}, {QuotaResource::kSize, -size}});
	} else {
		nodeQuotaUpdate(fsOpContext, node, {{QuotaResource::kInodes, -1}});
	}

	// Change ownership
	node->uid = uid;
	node->gid = gid;

	// Increase quota for new owner
	if (node->type == FSNodeType::kFile || node->type == FSNodeType::kTrash ||
	    node->type == FSNodeType::kReserved) {
		nodeQuotaUpdate(fsOpContext, node,
		                {{QuotaResource::kInodes, +1}, {QuotaResource::kSize, +size}});
	} else {
		nodeQuotaUpdate(fsOpContext, node, {{QuotaResource::kInodes, +1}});
	}
}

namespace {

/// Returns true when a path segment of the given length consisting entirely of dots is a
/// dot-only name (".") or dot-dot name (".."), which are invalid in trash paths.
bool isDotOnlySegment(uint32_t segmentLength, uint32_t dotCount) {
	return segmentLength > 0 && segmentLength == dotCount && segmentLength <= 2;
}

}  // namespace

uint8_t FilesystemNodeOperationsBase::validateTrashPath(const std::string &pathStr) {
	if (pathStr.empty()) { return SAUNAFS_ERROR_CANTCREATEPATH; }

	// Skip leading slashes
	size_t start = pathStr.find_first_not_of('/');
	if (start == std::string::npos) { return SAUNAFS_ERROR_CANTCREATEPATH; }

	std::string_view path(pathStr);
	path.remove_prefix(start);

	uint32_t partLength = 0;
	uint32_t dots = 0;

	for (char chr : path) {
		if (chr == '\0') { return SAUNAFS_ERROR_CANTCREATEPATH; }

		if (chr == '/') {
			if (partLength == 0) { return SAUNAFS_ERROR_CANTCREATEPATH; }  // "//"
			if (isDotOnlySegment(partLength, dots)) { return SAUNAFS_ERROR_CANTCREATEPATH; }
			partLength = 0;
			dots = 0;
		} else {
			if (chr == '.') { dots++; }
			partLength++;
			if (partLength > kMaxFileNameLength) { return SAUNAFS_ERROR_CANTCREATEPATH; }
		}
	}

	// Last segment must be non-empty and not a dot-only name
	if (partLength == 0) { return SAUNAFS_ERROR_CANTCREATEPATH; }
	if (isDotOnlySegment(partLength, dots)) { return SAUNAFS_ERROR_CANTCREATEPATH; }

	return SAUNAFS_STATUS_OK;
}

void FilesystemNodeOperationsBase::removeNode(const FilesystemOperationContext &fsOpContext,
                                              uint32_t timeStamp, FSNode *node) {
	if (!node->parents.empty()) { return; }

	if (gChecksumBackgroundUpdater.isNodeIncluded(node)) {
		removeFromChecksum(gChecksumBackgroundUpdater.fsNodesChecksum, node->checksum);
	}

	removeFromChecksum(gMetadata->fsNodesChecksum, node->checksum);

	// and free
	gMetadata->nodes--;
	gMetadata->aclStorage.erase(node->id);

	if (node->type == FSNodeType::kDirectory) { gMetadata->dirNodes--; }

	if (node->type == FSNodeType::kFile || node->type == FSNodeType::kTrash ||
	    node->type == FSNodeType::kReserved) {
		gMetadata->fileNodes--;
		for (uint32_t i = 0; i < static_cast<FSNodeFile *>(node)->chunks.size(); ++i) {
			uint64_t chunkid = static_cast<FSNodeFile *>(node)->chunks[i];
			if (chunkid > 0) {
				if (gChunkOperations->deleteFile(fsOpContext, chunkid, node->goal) !=
				    SAUNAFS_STATUS_OK) {
					safs::log_err(
					    "structure error - chunk {:#016x} not found (inode: {} ; index: {})",
					    chunkid, node->id, i);
				}
			}
		}
	}

	if (node->type == FSNodeType::kSymlink) { gMetadata->linkNodes--; }

	gMetadata->inodePool.release(node->id, timeStamp, true);
	xattr_removeinode(node->id);
	if (node->type == FSNodeType::kFile || node->type == FSNodeType::kTrash ||
	    node->type == FSNodeType::kReserved) {
		nodeQuotaUpdate(
		    fsOpContext, node,
		    {{QuotaResource::kInodes, -1}, {QuotaResource::kSize, -getSize(fsOpContext, node)}});
	} else {
		nodeQuotaUpdate(fsOpContext, node, {{QuotaResource::kInodes, -1}});
	}
	nodeQuotaRemove(fsOpContext, QuotaOwnerType::kInode, node->id);
#ifndef METARESTORE
	fsnodes_periodic_remove(fsOpContext, node->id);
	dcm_modify(node->id, 0);
#endif

	// remove node from nodeHash
	uint32_t nodeHashIndex = NODEHASHPOS(node->id);
	auto nodeIterator = std::find(gMetadata->nodeHash[nodeHashIndex].begin(),
	                              gMetadata->nodeHash[nodeHashIndex].end(), node);

	if (nodeIterator != gMetadata->nodeHash[nodeHashIndex].end()) {
		auto lastElement = gMetadata->nodeHash[nodeHashIndex].end() - 1;
		std::iter_swap(nodeIterator, lastElement);  // Swap with last element to avoid erase: O(1)
		gMetadata->nodeHash[nodeHashIndex].pop_back();  // Remove the last element: O(1)
		gMetadata->nodeRemovedSignal.emit(node->id);
	}

	FSNode::destroy(node);
}

void FilesystemNodeOperationsBase::unlink(const FilesystemOperationContext &fsOpContext,
                                          uint32_t timeStamp, FSNodeDirectory *parent,
                                          const HString &childName, FSNode *childNode) {
	std::string path;

	if (getNumberOfParents(fsOpContext, childNode) == 1) {  // last link
		// go to trash or reserved ? - get path
		if (childNode->type == FSNodeType::kFile &&
		    (childNode->trashtime > 0 ||
		     !static_cast<FSNodeFile *>(childNode)->sessionIds.empty())) {
			getPath(fsOpContext, parent, childNode, path);
		}
	}

	removeEdge(fsOpContext, timeStamp, parent, childName, childNode);

	if (getNumberOfParents(fsOpContext, childNode) != 0) { return; }

	// last link
	if (childNode->type == FSNodeType::kFile) {
		auto *fileNode = static_cast<FSNodeFile *>(childNode);
		if (childNode->trashtime > 0) {
			childNode->type = FSNodeType::kTrash;
			childNode->ctime = timeStamp;
			fsnodes_update_checksum(childNode);

			// Emit node changed signal to notify the node is now in Trash
			if (gMetadata->nodeChangedSignal.size() > 0) {
				gMetadata->nodeChangedSignal.emit(childNode);
			}

			addTrashEntry(gMetadata->trash, gMetadata->trashHandlesIndex,
			              gMetadata->trashReservedToId, childNode, path);

			gMetadata->trashSpace += fileNode->length;
			gMetadata->trashNodes++;
		} else if (!fileNode->sessionIds.empty()) {
			childNode->type = FSNodeType::kReserved;
			fsnodes_update_checksum(childNode);

			// Emit node changed signal to notify the node is now Reserved
			if (gMetadata->nodeChangedSignal.size() > 0) {
				gMetadata->nodeChangedSignal.emit(childNode);
			}

			addReservedEntry(gMetadata->reserved, gMetadata->reservedHandlesIndex,
			                 gMetadata->trashReservedToId, childNode, path);

			gMetadata->reservedSpace += fileNode->length;
			gMetadata->reservedNodes++;
		} else {
			removeNode(fsOpContext, timeStamp, childNode);
		}
	} else {
		removeNode(fsOpContext, timeStamp, childNode);
	}
}

int FilesystemNodeOperationsBase::purge(const FilesystemOperationContext &fsOpContext,
                                        uint32_t timeStamp, FSNode *node) {
	if (node->type == FSNodeType::kTrash) {
		auto *fileNode = static_cast<FSNodeFile *>(node);
		gMetadata->trashSpace -= fileNode->length;
		gMetadata->trashNodes--;

		// If the file has active sessions, move it to Reserved instead of deleting
		if (!fileNode->sessionIds.empty()) {
			fileNode->type = FSNodeType::kReserved;
			fsnodes_update_checksum(fileNode);

			// Emit node changed signal to notify the node's type has changed
			if (gMetadata->nodeChangedSignal.size() > 0) {
				gMetadata->nodeChangedSignal.emit(fileNode);
			}

			gMetadata->reservedSpace += fileNode->length;
			gMetadata->reservedNodes++;

			moveTrashToReservedEntry(gMetadata->trash, gMetadata->trashHandlesIndex,
			                         gMetadata->reserved, gMetadata->reservedHandlesIndex,
			                         gMetadata->trashReservedToId, node);

			return 0;  // Return 0 to indicate the node was moved to Reserved, not deleted
		}

		removeTrashEntry(gMetadata->trash, gMetadata->trashHandlesIndex,
		                 gMetadata->trashReservedToId, node);
		node->ctime = timeStamp;
		fsnodes_update_checksum(node);

		// Emit node changed signal to notify the node's ctime has changed
		if (gMetadata->nodeChangedSignal.size() > 0) {
			gMetadata->nodeChangedSignal.emit(node);
		}

		removeNode(fsOpContext, timeStamp, node);

		return 1;  // Return 1 to indicate the node was successfully deleted
	}

	if (node->type == FSNodeType::kReserved) {
		auto *fileNode = static_cast<FSNodeFile *>(node);

		gMetadata->reservedSpace -= fileNode->length;
		gMetadata->reservedNodes--;

		removeReservedEntry(gMetadata->reserved, gMetadata->reservedHandlesIndex,
		                    gMetadata->trashReservedToId, node->id);

		fileNode->ctime = timeStamp;
		fsnodes_update_checksum(fileNode);

		// Emit node changed signal to notify the node's ctime has changed
		if (gMetadata->nodeChangedSignal.size() > 0) {
			gMetadata->nodeChangedSignal.emit(fileNode);
		}

		removeNode(fsOpContext, timeStamp, fileNode);
		return 1;
	}

	return -1;
}

uint8_t FilesystemNodeOperationsBase::undel(const FilesystemOperationContext &fsOpContext,
                                            uint32_t timeStamp, FSNodeFile *node) {
	// Path validation

	std::string pathStr;

	if (node->type == FSNodeType::kTrash) {
		pathStr = (std::string)gMetadata->trash.at(TrashPathKey(node));
	} else {
		assert(node->type == FSNodeType::kReserved);
		pathStr = (std::string)gMetadata->reserved.at(node->id);
	}

	uint8_t validationStatus = validateTrashPath(pathStr);
	if (validationStatus != SAUNAFS_STATUS_OK) { return validationStatus; }

	// Strip leading slashes for path reconstruction
	std::string_view pathView = pathStr;
	const size_t firstNonSlash = pathView.find_first_not_of('/');
	if (firstNonSlash != std::string_view::npos) { pathView.remove_prefix(firstNonSlash); }
	const char *path = pathView.data();
	auto pathLength = static_cast<unsigned>(pathView.size());

	// Path reconstruction

	uint32_t partLength = 0;
	FSNode *currentNode = nullptr;
	FSNodeDirectory *currentParent = gMetadata->root;

	bool isNew = false;

	for (;;) {
		partLength = 0;
		while ((partLength < pathLength) && (path[partLength] != '/')) { partLength++; }

		HString name(path, partLength);

		if (partLength == pathLength) {  // last name
			if (isNameUsed(fsOpContext, currentParent, name)) { return SAUNAFS_ERROR_EEXIST; }

			// remove from trash and link to new parent
			if (node->type == FSNodeType::kTrash) {
				removeTrashEntry(gMetadata->trash, gMetadata->trashHandlesIndex,
				                 gMetadata->trashReservedToId, node);
			} else {
				removeReservedEntry(gMetadata->reserved, gMetadata->reservedHandlesIndex,
				                    gMetadata->trashReservedToId, node->id);
			}

			node->type = FSNodeType::kFile;
			node->ctime = timeStamp;
			fsnodes_update_checksum(node);
			link(fsOpContext, timeStamp, currentParent, node, name);
			gMetadata->trashSpace -= node->length;
			gMetadata->trashNodes--;

			return SAUNAFS_STATUS_OK;
		}

		// Directory handling (only runs for intermediate segments)
		if (!isNew) {
			currentNode = lookup(fsOpContext, currentParent, name);
			if (currentNode == nullptr) {
				isNew = true;
			} else {
				if (currentNode->type != FSNodeType::kDirectory) {
					return SAUNAFS_ERROR_CANTCREATEPATH;
				}
			}
		}

		if (isNew) {
			currentNode =
			    createNode(fsOpContext, timeStamp, currentParent, name, FSNodeType::kDirectory,
			               kUndelDirectoryMode, 0, 0, 0, 0, AclInheritance::kDontInheritAcl);

#ifndef METARESTORE
			assert(metadataserver::isMaster());
#endif

			gFSOperations->changeLog(
			    fsOpContext, timeStamp,
			    "CREATE(%" PRIiNode ",%s,%c,%d,%" PRIu32 ",%" PRIu32 ",%" PRIu32 "):%" PRIiNode,
			    currentParent->id, escapeName(name).c_str(),
			    static_cast<char>(FSNodeType::kDirectory), currentNode->mode & kPermissionsMask,
			    (uint32_t)0, (uint32_t)0, (uint32_t)0, currentNode->id);
		}

		currentParent = static_cast<FSNodeDirectory *>(currentNode);
		assert(currentNode->type == FSNodeType::kDirectory);

		path += partLength + 1;
		pathLength -= partLength + 1;
	}
}

#ifndef METARESTORE

void FilesystemNodeOperationsBase::getGoalRecursive(const FilesystemOperationContext &fsOpContext,
                                                    FSNode *node, uint8_t gmode,
                                                    GoalStatistics &fileGoalsTab,
                                                    GoalStatistics &dirGoalsTab) {
	if (node->type == FSNodeType::kFile || node->type == FSNodeType::kTrash ||
	    node->type == FSNodeType::kReserved) {
		if (!GoalId::isValid(node->goal)) {
			safs::log_warn("file inode {}: unknown goal !!! - fixing", node->id);
			// changeFileGoal updates the checksum and emits nodeChangedSignal itself.
			changeFileGoal(fsOpContext, static_cast<FSNodeFile *>(node), DEFAULT_GOAL);
		}

		fileGoalsTab[node->goal]++;
	} else if (node->type == FSNodeType::kDirectory) {
		if (!GoalId::isValid(node->goal)) {
			safs::log_warn("directory inode {}: unknown goal !!! - fixing", node->id);
			node->goal = DEFAULT_GOAL;
			// Persist the fix: refresh the checksum and emit only on an actual change so a
			// read-only GETGOAL walk does not enqueue every visited node.
			fsnodes_update_checksum(node);
			if (!fsOpContext.hasReadWriteTransaction() &&
			    gMetadata->nodeChangedSignal.size() > 0) {
				gMetadata->nodeChangedSignal.emit(node);
			}
		}

		dirGoalsTab[node->goal]++;

		if (gmode == GMODE_RECURSIVE) {
			const auto *dirNode = static_cast<const FSNodeDirectory *>(node);

			for (const auto &entry : dirNode->entries) {
				getGoalRecursive(fsOpContext, entry.second, gmode, fileGoalsTab, dirGoalsTab);
			}
		}
	}
}

void FilesystemNodeOperationsBase::getTrashTimeRecursive(FSNode *node, uint8_t gmode,
                                                         TrashtimeMap &fileTrashtimes,
                                                         TrashtimeMap &dirTrashtimes) {
	if (node->type == FSNodeType::kFile || node->type == FSNodeType::kTrash ||
	    node->type == FSNodeType::kReserved) {
		fileTrashtimes[node->trashtime] += 1;
	} else if (node->type == FSNodeType::kDirectory) {
		dirTrashtimes[node->trashtime] += 1;

		if (gmode == GMODE_RECURSIVE) {
			const auto *dirNode = static_cast<const FSNodeDirectory *>(node);

			for (const auto &entry : dirNode->entries) {
				getTrashTimeRecursive(entry.second, gmode, fileTrashtimes, dirTrashtimes);
			}
		}
	}
}

void FilesystemNodeOperationsBase::getExtraAttrRecursive(FSNode *node, uint8_t gmode,
                                                         ExtraAttributesArray &fileEAttrTab,
                                                         ExtraAttributesArray &dirEAttrTab) {
	if (node->type != FSNodeType::kDirectory) {
		fileEAttrTab[(node->mode >> EATTR_BIT_OFFSET) &
		             (EATTR_NOOWNER | EATTR_NOACACHE | EATTR_NODATACACHE)]++;
	} else {
		dirEAttrTab[(node->mode >> EATTR_BIT_OFFSET)]++;

		if (gmode == GMODE_RECURSIVE) {
			const auto *dirNode = static_cast<const FSNodeDirectory *>(node);

			for (const auto &entry : dirNode->entries) {
				getExtraAttrRecursive(entry.second, gmode, fileEAttrTab, dirEAttrTab);
			}
		}
	}
}

#endif  // METARESTORE

void FilesystemNodeOperationsBase::setgoalRecursive(const FilesystemOperationContext &fsOpContext,
                                                    FSNode *node, uint32_t timeStamp, uint32_t uid,
                                                    uint8_t goal, uint8_t smode,
                                                    inode_t *modifiedINodesOut,
                                                    inode_t *unchangedINodesOut,
                                                    inode_t *permissionDeniedINodesOut) {
	bool nodeChanged = false;

	if (node->type == FSNodeType::kFile || node->type == FSNodeType::kDirectory ||
	    node->type == FSNodeType::kTrash || node->type == FSNodeType::kReserved) {
		if ((node->mode & (EATTR_NOOWNER << EATTR_BIT_OFFSET)) == 0 && uid != 0 &&
		    node->uid != uid) {
			(*permissionDeniedINodesOut)++;
		} else {
			if ((smode & SMODE_TMASK) == SMODE_SET && node->goal != goal) {
				if (node->type != FSNodeType::kDirectory) {
					changeFileGoal(fsOpContext, static_cast<FSNodeFile *>(node), goal);
					(*modifiedINodesOut)++;
				} else {
					node->goal = goal;
					if (!fsOpContext.hasReadWriteTransaction()) {
						gMetadata->nodeChangedSignal.emit(node);
					}
					(*modifiedINodesOut)++;
				}

				updateCTime(fsOpContext, node, timeStamp);
				fsnodes_update_checksum(node);
				nodeChanged = true;
			} else {
				(*unchangedINodesOut)++;
			}
		}

		if (node->type == FSNodeType::kDirectory && (smode & SMODE_RMASK)) {
			for (const auto &entry : static_cast<const FSNodeDirectory *>(node)->entries) {
				setgoalRecursive(fsOpContext, entry.second, timeStamp, uid, goal, smode,
				                 modifiedINodesOut, unchangedINodesOut, permissionDeniedINodesOut);
			}
		}
	}

	if (nodeChanged && fsOpContext.hasReadWriteTransaction()) { updateNode(fsOpContext, node); }
	// Emit node changed signal to notify changes during the goal change
	if (nodeChanged && gMetadata->nodeChangedSignal.size() > 0) { gMetadata->nodeChangedSignal.emit(node); }
}

void FilesystemNodeOperationsBase::setTrashTimeRecursive(FSNode *node, uint32_t timeStamp,
                                                         uint32_t uid, uint32_t trashtime,
                                                         uint8_t smode, inode_t *modifiedINodesOut,
                                                         inode_t *unchangedINodesOut,
                                                         inode_t *permissionDeniedINodesOut) {
	if (node->type == FSNodeType::kFile || node->type == FSNodeType::kDirectory ||
	    node->type == FSNodeType::kTrash || node->type == FSNodeType::kReserved) {
		if ((node->mode & (EATTR_NOOWNER << EATTR_BIT_OFFSET)) == 0 && uid != 0 &&
		    node->uid != uid) {
			(*permissionDeniedINodesOut)++;
		} else {
			bool wasSet = false;
			auto oldTrashKey = TrashPathKey(node);

			switch (smode & SMODE_TMASK) {
			case SMODE_SET:
				if (node->trashtime != trashtime) {
					node->trashtime = trashtime;
					wasSet = true;
				}
				break;
			case SMODE_INCREASE:
				if (node->trashtime < trashtime) {
					node->trashtime = trashtime;
					wasSet = true;
				}
				break;
			case SMODE_DECREASE:
				if (node->trashtime > trashtime) {
					node->trashtime = trashtime;
					wasSet = true;
				}
				break;
			}

			if (wasSet) {
				(*modifiedINodesOut)++;
				node->ctime = timeStamp;

				if (node->type == FSNodeType::kTrash) {
					updateTrashFromOldEntry(gMetadata->trash, node, oldTrashKey);
				}

				fsnodes_update_checksum(node);

				// Emit node changed signal to notify changes during the trashtime update
				if (gMetadata->nodeChangedSignal.size() > 0) {
					gMetadata->nodeChangedSignal.emit(node);
				}
			} else {
				(*unchangedINodesOut)++;
			}
		}

		if (node->type == FSNodeType::kDirectory && (smode & SMODE_RMASK)) {
			for (const auto &entry : static_cast<const FSNodeDirectory *>(node)->entries) {
				setTrashTimeRecursive(entry.second, timeStamp, uid, trashtime, smode,
				                      modifiedINodesOut, unchangedINodesOut,
				                      permissionDeniedINodesOut);
			}
		}
	}
}

void FilesystemNodeOperationsBase::setExtraAttrRecursive(
    const FilesystemOperationContext &fsOpContext, FSNode *node, uint32_t timeStamp, uint32_t uid,
    uint8_t eattr, uint8_t smode, inode_t *modifiedINodesOut, inode_t *unchangedINodesOut,
    inode_t *permissionDeniedINodesOut) {
	bool nodeChanged = false;

	// Check permission
	if ((node->mode & (EATTR_NOOWNER << EATTR_BIT_OFFSET)) == 0 && uid != 0 && node->uid != uid) {
		(*permissionDeniedINodesOut)++;
	} else {
		// Sanitize attributes: remove NOECACHE flag for non-directory nodes
		uint8_t adjustedExtraAttr = eattr;
		if (node->type != FSNodeType::kDirectory) {
			const uint16_t oldMode = node->mode;
			node->mode &= ~(EATTR_NOECACHE << EATTR_BIT_OFFSET);
			adjustedExtraAttr &= ~(EATTR_NOECACHE);
			if (node->mode != oldMode) { nodeChanged = true; }
		}

		// Compute new extra attributes based on smode
		uint8_t newExtraAttr = (node->mode >> EATTR_BIT_OFFSET);

		switch (smode & SMODE_TMASK) {
		case SMODE_SET:
			newExtraAttr = adjustedExtraAttr;
			break;
		case SMODE_INCREASE:
			newExtraAttr |= adjustedExtraAttr;
			break;
		case SMODE_DECREASE:
			newExtraAttr &= ~adjustedExtraAttr;
			break;
		}

		// Update node if attributes changed
		if (newExtraAttr != (node->mode >> EATTR_BIT_OFFSET)) {
			node->mode =
			    (node->mode & kPermissionsMask) | (((uint16_t)newExtraAttr) << EATTR_BIT_OFFSET);
			syncAclWithMode(fsOpContext, node);

			(*modifiedINodesOut)++;
			updateCTime(fsOpContext, node, timeStamp);
			nodeChanged = true;
		} else {
			(*unchangedINodesOut)++;
		}
	}

	// Recursively apply to directory children
	if (node->type == FSNodeType::kDirectory && (smode & SMODE_RMASK)) {
		const auto *dirNode = static_cast<const FSNodeDirectory *>(node);

		for (const auto &entry : dirNode->entries) {
			setExtraAttrRecursive(fsOpContext, entry.second, timeStamp, uid, eattr, smode,
			                      modifiedINodesOut, unchangedINodesOut, permissionDeniedINodesOut);
		}
	}

	fsnodes_update_checksum(node);

	// Make the change persistent for KV backends
	if (nodeChanged && fsOpContext.hasReadWriteTransaction()) { updateNode(fsOpContext, node); }

	// Emit node changed signal to notify changes during the extra attribute update
	if (nodeChanged && gMetadata->nodeChangedSignal.size() > 0) {
		gMetadata->nodeChangedSignal.emit(node);
	}
}

uint8_t FilesystemNodeOperationsBase::deleteAcl(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, FSNode *node, AclType type,
    uint32_t timeStamp) {
	if (type == AclType::kRichACL) {
		gMetadata->aclStorage.erase(node->id);
	} else if (type == AclType::kDefault) {
		if (node->type != FSNodeType::kDirectory) { return SAUNAFS_ERROR_ENOTSUP; }
		const RichACL *nodeAcl = gMetadata->aclStorage.get(node->id);
		if (nodeAcl != nullptr) {
			RichACL newAcl = *nodeAcl;
			newAcl.createExplicitInheritance();

			newAcl.removeInheritOnly(true);
			if (newAcl.size() == 0) {
				gMetadata->aclStorage.erase(node->id);
			} else {
				gMetadata->aclStorage.set(node->id, std::move(newAcl));
			}
		}
	} else if (type == AclType::kAccess) {
		const RichACL *nodeAcl = gMetadata->aclStorage.get(node->id);
		if (nodeAcl != nullptr) {
			RichACL newAcl = *nodeAcl;
			newAcl.createExplicitInheritance();
			newAcl.removeInheritOnly(false);

			if (newAcl.size() == 0) {
				gMetadata->aclStorage.erase(node->id);
			} else {
				gMetadata->aclStorage.set(node->id, std::move(newAcl));
			}
		}
	} else {
		return SAUNAFS_ERROR_EINVAL;
	}

	updateCTime(fsOpContext, node, timeStamp);
	fsnodes_update_checksum(node);

	// Emit node changed signal to notify changes during the ACL deletion
	if (gMetadata->nodeChangedSignal.size() > 0) { gMetadata->nodeChangedSignal.emit(node); }

	return SAUNAFS_STATUS_OK;
}

void FilesystemNodeOperationsBase::syncAclWithMode(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, FSNode *node) {
	gMetadata->aclStorage.setMode(node->id, node->mode, node->type == FSNodeType::kDirectory);
}

#ifndef METARESTORE
uint8_t FilesystemNodeOperationsBase::getAcl(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, FSNode *node, RichACL &acl) {
	const RichACL *richAcl = gMetadata->aclStorage.get(node->id);

	if (!richAcl) { return SAUNAFS_ERROR_ENOATTR; }

	acl = *richAcl;
	assert((node->mode & kStandardPermissionsMask) == richAcl->getMode());

	return SAUNAFS_STATUS_OK;
}
#endif

uint8_t FilesystemNodeOperationsBase::setAcl(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, FSNode *node,
    const RichACL &acl, uint32_t timeStamp) {
	if (!acl.checkInheritFlags(node->type == FSNodeType::kDirectory)) {
		return SAUNAFS_ERROR_ENOTSUP;
	}

	uint16_t mode = node->mode;
	if (RichACL::equivMode(acl, mode, node->type == FSNodeType::kDirectory)) {
		node->mode = (node->mode & ~kStandardPermissionsMask) | (mode & kStandardPermissionsMask);
		gMetadata->aclStorage.erase(node->id);
	} else {
		if (!acl.isAutoSetMode()) {
			node->mode = (node->mode & ~kStandardPermissionsMask) |
			             (acl.getMode() & kStandardPermissionsMask);
		}

		RichACL newAcl = acl;

		if (acl.isAutoSetMode()) {
			newAcl.setFlags(newAcl.getFlags() & ~RichACL::kAutoSetMode);
			newAcl.setMode(node->mode, node->type == FSNodeType::kDirectory);
		}

		gMetadata->aclStorage.set(node->id, std::move(newAcl));
	}

	updateCTime(fsOpContext, node, timeStamp);
	fsnodes_update_checksum(node);

	// Emit node changed signal to notify changes during the ACL update
	if (gMetadata->nodeChangedSignal.size() > 0) { gMetadata->nodeChangedSignal.emit(node); }
	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemNodeOperationsBase::setAcl(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, FSNode *node, AclType type,
    const AccessControlList &acl, uint32_t timeStamp) {
	if (type != AclType::kDefault && type != AclType::kAccess) { return SAUNAFS_ERROR_EINVAL; }

	if (type == AclType::kDefault && node->type != FSNodeType::kDirectory) {
		return SAUNAFS_ERROR_ENOTSUP;
	}

	const RichACL *nodeAcl = gMetadata->aclStorage.get(node->id);
	RichACL newAcl;

	if (nodeAcl != nullptr) {
		newAcl = *nodeAcl;
		newAcl.createExplicitInheritance();
		newAcl.removeInheritOnly(type == AclType::kDefault);
	}

	if (type == AclType::kDefault) {
		newAcl.appendDefaultPosixACL(acl);
		newAcl.setMode(node->mode, true);
	} else {
		newAcl.appendPosixACL(acl, node->type == FSNodeType::kDirectory);
		node->mode = (node->mode & ~kStandardPermissionsMask) |
		             (newAcl.getMode() & kStandardPermissionsMask);
	}
	gMetadata->aclStorage.set(node->id, std::move(newAcl));

	updateCTime(fsOpContext, node, timeStamp);
	fsnodes_update_checksum(node);

	// Emit node changed signal to notify changes during the ACL update
	if (gMetadata->nodeChangedSignal.size() > 0) { gMetadata->nodeChangedSignal.emit(node); }
	return SAUNAFS_STATUS_OK;
}

int FilesystemNodeOperationsBase::nameCheck(const std::string &name) {
	if (name.length() == 0 || name.length() > kMaxFileNameLength) { return -1; }

	if (name[0] == '.') {
		if (name.length() == 1) { return -1; }
		if (name.length() == 2 && name[1] == '.') { return -1; }
	}

	for (uint32_t i = 0; i < name.length(); i++) {
		if (name[i] == '\0' || name[i] == '/') { return -1; }
	}

	return 0;
}

const RichACL *FilesystemNodeOperationsBase::getAclForAccess(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, FSNode *node,
    [[maybe_unused]] std::optional<RichACL> &scratch) {
	return gMetadata->aclStorage.get(node->id);
}

void FilesystemNodeOperationsBase::storeInheritedAcl(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, FSNode *node, RichACL &&acl) {
	gMetadata->aclStorage.set(node->id, std::move(acl));
}

int FilesystemNodeOperationsBase::access(const FsContext &context,
                                         const FilesystemOperationContext &fsOpContext,
                                         FSNode *node, uint8_t modeMask) {
	if ((context.sesflags() & SESFLAG_NOMASTERPERMCHECK) || context.uid() == 0) {
		return 1;  // super user or no permission check
	}

	std::optional<RichACL> aclScratch;  // not constructed here; KV backends emplace() as needed
	const RichACL *nodeAcl = getAclForAccess(fsOpContext, node, aclScratch);

	// If ACLs are present, use it for permission checking
	if (nodeAcl != nullptr) {
		assert((node->mode & kStandardPermissionsMask) == nodeAcl->getMode());

		uint32_t mask = RichACL::convertMode2Mask(modeMask);
		if (node->type != FSNodeType::kDirectory) { mask &= ~RichACL::Ace::kDeleteChild; }

		return nodeAcl->checkPermission(mask, node->uid, node->gid, context.uid(),
		                                context.groups());
	}

	// No ACLs: use traditional Unix permission checking

	constexpr uint8_t kUserModeBits = 6;            // >> 6 for user bits
	constexpr uint8_t kGroupModeBits = 3;           // >> 3 for group bits
	constexpr uint8_t kSingleClassPermissions = 7;  // & 7 to extract 3 bits

	uint8_t nodeMode;

	// Determine which permission class applies to the requesting user
	if (context.uid() == node->uid || (node->mode & (EATTR_NOOWNER << EATTR_BIT_OFFSET))) {
		// User owns the file: use user permissions (bits 6-8)
		nodeMode = ((node->mode) >> kUserModeBits) & kSingleClassPermissions;
	} else if (context.sesflags() & SESFLAG_IGNOREGID) {
		// IGNOREGID flag: use group OR other permissions
		nodeMode = (((node->mode) >> kGroupModeBits) | (node->mode)) & kSingleClassPermissions;
	} else if (context.hasGroup(node->gid)) {
		// User in file's group: use group permissions (bits 3-5)
		nodeMode = ((node->mode) >> kGroupModeBits) & kSingleClassPermissions;
	} else {
		// Otherwise: use other permissions (bits 0-2)
		nodeMode = (node->mode & kSingleClassPermissions);
	}

	// Check if extracted permissions satisfy the request
	if ((nodeMode & modeMask) == modeMask) { return 1; }

	return 0;
}

int FilesystemNodeOperationsBase::stickyAccess(FSNode *parent, FSNode *node, uint32_t uid) {
	constexpr uint16_t kStickyBit = 01000;  // sticky bit is at bit position 9

	if (uid == 0 || (parent->mode & kStickyBit) == 0) {  // super user or sticky bit is not set
		return 1;
	}

	if (uid == parent->uid || (parent->mode & (EATTR_NOOWNER << EATTR_BIT_OFFSET)) ||
	    uid == node->uid || (node->mode & (EATTR_NOOWNER << EATTR_BIT_OFFSET))) {
		return 1;
	}

	return 0;
}

uint8_t FilesystemNodeOperationsBase::verifySession(const FsContext &context,
                                                    OperationMode operationMode,
                                                    SessionType sessionType) {
	if (context.hasSessionData() && (context.sesflags() & SESFLAG_READONLY) &&
	    (operationMode == OperationMode::kReadWrite)) {
		return SAUNAFS_ERROR_EROFS;
	}

	if (context.hasSessionData() && (context.rootinode() == 0) &&
	    (sessionType == SessionType::kNotMeta)) {
		return SAUNAFS_ERROR_ENOENT;
	}

	if (context.hasSessionData() && (context.rootinode() != 0) &&
	    (sessionType == SessionType::kOnlyMeta)) {
		return SAUNAFS_ERROR_EPERM;
	}

	return SAUNAFS_STATUS_OK;
}

FSNodeDirectory *FilesystemNodeOperationsBase::getRootNode(
    const FilesystemOperationContext &fsOpContext) {
	(void)fsOpContext;  // unused parameter in this implementation
	return gMetadata->root;
}

uint8_t FilesystemNodeOperationsBase::getNodeForOperation(
    const FsContext &context, const FilesystemOperationContext &fsOpContext,
    ExpectedNodeType expectedNodeType, uint8_t modeMask, inode_t inode, FSNode **nodeOut,
    FSNodeDirectory **rootDirOut) {
	FSNode *candidateNode;
	FSNodeDirectory *candidateRoot;

	// Node lookup

	if (!context.hasSessionData()) {
		candidateRoot = nullptr;
		candidateNode = idToNode(fsOpContext, inode);

		if (candidateNode == nullptr) { return SAUNAFS_ERROR_ENOENT; }
	} else if (context.rootinode() == SPECIAL_INODE_ROOT || (context.rootinode() == 0)) {
		candidateRoot = nullptr;

		if (inode == SPECIAL_INODE_ROOT) {
			candidateRoot = getRootNode(fsOpContext);
			candidateNode = candidateRoot;
		} else {
			candidateNode = idToNode(fsOpContext, inode);
			// Only pay the root-node round-trip when the node resolved and a caller
			// asked for the root; a null node returns ENOENT just below.
			if (candidateNode != nullptr && rootDirOut != nullptr) {
				candidateRoot = getRootNode(fsOpContext);
			}
		}

		if (candidateNode == nullptr) { return SAUNAFS_ERROR_ENOENT; }

		if (context.rootinode() == 0 && candidateNode->type != FSNodeType::kTrash &&
		    candidateNode->type != FSNodeType::kReserved) {
			return SAUNAFS_ERROR_EPERM;
		}
	} else {
		candidateRoot = idToNode<FSNodeDirectory>(fsOpContext, context.rootinode());

		if ((candidateRoot == nullptr) || candidateRoot->type != FSNodeType::kDirectory) {
			return SAUNAFS_ERROR_ENOENT;
		}

		if (inode == SPECIAL_INODE_ROOT || inode == context.rootinode()) {
			candidateNode = candidateRoot;
		} else {
			candidateNode = idToNode(fsOpContext, inode);

			if (candidateNode == nullptr) { return SAUNAFS_ERROR_ENOENT; }

			if (!isAncestorOrNodeReservedOrTrash(fsOpContext, candidateRoot, candidateNode)) {
				return SAUNAFS_ERROR_EPERM;
			}
		}
	}

	// Node type validation

	if ((expectedNodeType == ExpectedNodeType::kDirectory) &&
	    (candidateNode->type != FSNodeType::kDirectory)) {
		return SAUNAFS_ERROR_ENOTDIR;
	}

	if ((expectedNodeType == ExpectedNodeType::kNotDirectory) &&
	    (candidateNode->type == FSNodeType::kDirectory)) {
		return SAUNAFS_ERROR_EPERM;
	}

	if ((expectedNodeType == ExpectedNodeType::kFile) &&
	    (candidateNode->type != FSNodeType::kFile) &&
	    (candidateNode->type != FSNodeType::kReserved) &&
	    (candidateNode->type != FSNodeType::kTrash)) {
		return SAUNAFS_ERROR_EPERM;
	}

	if ((expectedNodeType == ExpectedNodeType::kFileOrDirectory) &&
	    (candidateNode->type != FSNodeType::kDirectory) &&
	    (candidateNode->type != FSNodeType::kFile) &&
	    (candidateNode->type != FSNodeType::kReserved) &&
	    (candidateNode->type != FSNodeType::kTrash)) {
		return SAUNAFS_ERROR_EPERM;
	}

	if (context.canCheckPermissions() && !access(context, fsOpContext, candidateNode, modeMask)) {
		return SAUNAFS_ERROR_EACCES;
	}

	// Case insensitive update check for the given context
	if (context.hasSessionData() && candidateNode->type == FSNodeType::kDirectory) {
		auto *dir = static_cast<FSNodeDirectory *>(candidateNode);
		const bool sessionIsCaseInsensitive = (context.sesflags() & SESFLAG_CASEINSENSITIVE) != 0;
		if (dir->caseInsensitive != sessionIsCaseInsensitive) {
			dir->caseInsensitive = sessionIsCaseInsensitive;
			dir->updateLowerCaseEntries();
		}
	}

	*nodeOut = candidateNode;
	if (rootDirOut != nullptr) { *rootDirOut = candidateRoot; }

	return SAUNAFS_STATUS_OK;
}
