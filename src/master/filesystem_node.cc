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
#include <cstdint>
#include <cstdlib>
#include <type_traits>

#include "common/attributes.h"
#include "common/massert.h"
#include "common/slice_traits.h"
#include "common/type_defs.h"
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

#ifndef NDEBUG
  #include "master/personality.h"
#endif

#define LOOKUPNOHASHLIMIT 10

#define MAXFNAMELENG 255


FSNode *FSNode::create(FSNodeType type) {
	switch (type) {
	case FSNodeType::kFile:
	case FSNodeType::kTrash:
	case FSNodeType::kReserved:
		return new FSNodeFile(type);
	case FSNodeType::kDirectory:
		return new FSNodeDirectory();
	case FSNodeType::kSymlink:
		return new FSNodeSymlink();
	case FSNodeType::kFifo:
	case FSNodeType::kSocket:
		return new FSNode(type);
	case FSNodeType::kBlockDev:
	case FSNodeType::kCharDev:
		return new FSNodeDevice(type);
	case FSNodeType::kUnknown:
	default:
		assert(!"invalid node type");
	}
	return nullptr;
}

void FSNode::destroy(FSNode *node) {
	for (auto const &[_, handlePtr] : node->parents) {
		delete handlePtr;
	}

	delete node;
}

// Private helper methods

uint32_t FilesystemNodeOperationsBase::lastChunkBlocks(FSNodeFile *node) {
	const uint64_t last_byte = node->length - 1;
	const uint32_t last_byte_offset = last_byte % SFSCHUNKSIZE;
	const uint32_t last_block = last_byte_offset / SFSBLOCKSIZE;
	const uint32_t block_count = last_block + 1;
	return block_count;
}

bool FilesystemNodeOperationsBase::isLastChunkNonEmpty(FSNodeFile *node) {
	std::size_t chunks = node->chunks.size();
	if (chunks == 0) {
		// no non-zero chunks, return now
		return false;
	}

	// file has non-zero length and contains at least one chunk
	const uint64_t last_byte = node->length - 1;
	const uint32_t last_chunk = last_byte / SFSCHUNKSIZE;
	if (last_chunk < chunks) {
		// last chunk exists, check if it isn't the zero chunk
		return node->chunks[last_chunk] != 0;
	}
	// last chunk hasn't been allocated yet
	return false;
}

uint32_t FilesystemNodeOperationsBase::fileChunksCount(FSNodeFile *node) {
	return std::accumulate(node->chunks.begin(), node->chunks.end(), (uint32_t)0,
	                       [](uint32_t sum, uint64_t v) { return sum + (v != 0); });
}

uint64_t FilesystemNodeOperationsBase::fileSize(FSNodeFile *node, uint32_t nonZeroChunks) {
	uint64_t size = (uint64_t)nonZeroChunks * (SFSCHUNKSIZE + SFSHDRSIZE);
	if (isLastChunkNonEmpty(node)) {
		size -= SFSCHUNKSIZE;
		size += lastChunkBlocks(node) * SFSBLOCKSIZE;
	}
	return size;
}

#ifndef METARESTORE
uint32_t FilesystemNodeOperationsBase::ecChunkRealSize(uint32_t blocks, uint32_t dataPartCount,
                                                       uint32_t parityPartCount) {
	const uint32_t stripes = (blocks + dataPartCount - 1) / dataPartCount;
	uint32_t size = blocks * SFSBLOCKSIZE;             // file data
	size += parityPartCount * stripes * SFSBLOCKSIZE;  // parity data
	size += 4096 * (dataPartCount + parityPartCount);  // headers of data and parity parts
	return size;
}
#endif

uint64_t FilesystemNodeOperationsBase::fileRealSize(FSNodeFile *node, uint32_t nonZeroChunks,
                                                    uint64_t logicalFileSize) {
#ifdef METARESTORE
	(void)node;
	(void)nonZeroChunks;
	(void)logicalFileSize;
	return 0; // Doesn't really matter. Metarestore doesn't need this value
#else
	const Goal &goal = gFSOperations->getGoalDefinition(node->goal);

	uint64_t full_size = 0;
	for (const auto &slice : goal) {
		if (slice_traits::isStandard(slice) || slice_traits::isTape(slice)) {
			full_size += logicalFileSize * slice.getExpectedCopies();
		} else if (slice_traits::isXor(slice) || slice_traits::isEC(slice)) {
			int data_part_count = slice_traits::getNumberOfDataParts(slice);
			int parity_part_count = slice_traits::getNumberOfParityParts(slice);

			uint32_t full_chunk_realsize =
			    ecChunkRealSize(SFSBLOCKSINCHUNK, data_part_count, parity_part_count);
			uint64_t size = (uint64_t)nonZeroChunks * full_chunk_realsize;
			if (isLastChunkNonEmpty(node)) {
				size -= full_chunk_realsize;
				size += ecChunkRealSize(lastChunkBlocks(node), data_part_count, parity_part_count);
			}
			full_size += size;
		} else {
			safs_pretty_syslog(LOG_ERR, "file_realsize: inode %" PRIiNode " has unknown goal 0x%" PRIx8, node->id,
			       node->goal);
			return 0;
		}
	}

	return full_size;
#endif
}

// Protected methods

FSNode *FilesystemNodeOperationsBase::idToNodeInternal(inode_t inode) {
	// Find the node with the given id
	uint32_t nodeHashIndex = NODEHASHPOS(inode);

	for (const auto &node : gMetadata->nodeHash[nodeHashIndex]) {
		if (node->id == inode) { return node; }
	}

	return nullptr;
}

// Public methods

FSNode *FilesystemNodeOperationsBase::lookup(FSNodeDirectory *node, const HString &name) const {
	auto it = node->find(name);
	if (it != node->end()) { return (*it).second; }

	return nullptr;
}

void FilesystemNodeOperationsBase::updateCTime(FSNode *node, uint32_t ctime) {
	if (node->type == FSNodeType::kTrash && node->ctime != ctime) {
		auto old_key = TrashPathKey(node);
		node->ctime = ctime;
		auto it = gMetadata->trash.find(old_key);
		if (it != gMetadata->trash.end()) {
			updateTrashFromOldEntry(gMetadata->trash, node, old_key);
		}
	} else {
		node->ctime = ctime;
	}
}

std::string FilesystemNodeOperationsBase::escapeName(const std::string &name) {
	constexpr std::array<char, 16> hex_digit = {{'0', '1', '2', '3', '4', '5', '6', '7',
	                                             '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'}};
	std::string result;

	// It could be possible to reserve 3 * name.length() bytes in result,
	// but it would lead to unnecessary allocations in some cases.
	// This would take much more time than computation of exact result size.
	// Hint: remember that std::string uses static allocation
	// for small string sizes.
	int long_count = std::count_if(name.begin(), name.end(), [](char c) {
		return c < 32 || c >= 127 || c == ',' || c == '%' || c == '(' || c == ')';
	});
	result.reserve(2 * long_count + name.length());

	for (char c : name) {
		if (c < 32 || c >= 127 || c == ',' || c == '%' || c == '(' || c == ')') {
			result.push_back('%');
			result.push_back(hex_digit[(c >> 4) & 0xF]);
			result.push_back(hex_digit[c & 0xF]);
		} else {
			result.push_back(c);
		}
	}

	return result;
}

int FilesystemNodeOperationsBase::isNameUsed(FSNodeDirectory *node, const HString &name) {
	return lookup(node, name) != nullptr;
}

bool FilesystemNodeOperationsBase::isAncestor(FSNodeDirectory *ancestor, FSNode *node) {
	for (const auto &[parentId, _] : node->parents) {
		auto *dir_node = idToNodeVerify<FSNodeDirectory>(parentId);

		while(dir_node) {
			if (ancestor == dir_node) {
				return true;
			}

			assert(dir_node->parents.size() <= 1);

			if (!dir_node->parents.empty()) {
				dir_node = idToNodeVerify<FSNodeDirectory>(dir_node->parents[0].first);
			} else {
				dir_node = nullptr;
			}
		}
	}

	return false;
}

bool FilesystemNodeOperationsBase::isAncestorOrNodeReservedOrTrash(FSNodeDirectory *ancestor,
                                                                   FSNode *node) {
	// Return true if file is reserved:
	if (node && (node->type == FSNodeType::kReserved || node->type == FSNodeType::kTrash)) {
		return true;
	}
	// Or if ancestor is ancestor of node
	return isAncestor(ancestor, node);
}

// stats

void FilesystemNodeOperationsBase::getStats(FSNode *node, StatsRecord *statsOut) {
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

int64_t FilesystemNodeOperationsBase::getSize(FSNode *node) {
	StatsRecord sr;
	getStats(node, &sr);
	return sr.size;
}

FSNodeDirectory *FilesystemNodeOperationsBase::getFirstParent(FSNode *node) {
	assert(node);
	FSNodeDirectory *parent;
	if (!node->parents.empty()) {
		parent = idToNodeVerify<FSNodeDirectory>(node->parents[0].first);
	} else {
		parent = gMetadata->root;
	}
	return parent;
}

void FilesystemNodeOperationsBase::subStats(FSNodeDirectory *parent, StatsRecord *stats) {
	StatsRecord *psr;
	if (parent) {
		psr = &parent->stats;
		psr->inodes -= stats->inodes;
		psr->dirs -= stats->dirs;
		psr->files -= stats->files;
		psr->links -= stats->links;
		psr->chunks -= stats->chunks;
		psr->length -= stats->length;
		psr->size -= stats->size;
		psr->realsize -= stats->realsize;
		if (parent != gMetadata->root) {
			for (auto const &[parentId, _] : parent->parents) {
				auto *node = idToNodeVerify<FSNodeDirectory>(parentId);
				subStats(node, stats);
			}
		}
	}
}

void FilesystemNodeOperationsBase::addStats(FSNodeDirectory *parent, StatsRecord *stats) {
	StatsRecord *psr;
	if (parent) {
		psr = &parent->stats;
		psr->inodes += stats->inodes;
		psr->dirs += stats->dirs;
		psr->files += stats->files;
		psr->links += stats->links;
		psr->chunks += stats->chunks;
		psr->length += stats->length;
		psr->size += stats->size;
		psr->realsize += stats->realsize;
		if (parent != gMetadata->root) {
			for (auto const &[parentId, _] : parent->parents) {
				auto *node = idToNodeVerify<FSNodeDirectory>(parentId);
				addStats(node, stats);
			}
		}
	}
}

void FilesystemNodeOperationsBase::addSubStats(FSNodeDirectory *parent, StatsRecord *newStats,
                                               StatsRecord *previousStats) {
	StatsRecord sr;
	sr.inodes = newStats->inodes - previousStats->inodes;
	sr.dirs = newStats->dirs - previousStats->dirs;
	sr.files = newStats->files - previousStats->files;
	sr.links = newStats->links - previousStats->links;
	sr.chunks = newStats->chunks - previousStats->chunks;
	sr.length = newStats->length - previousStats->length;
	sr.size = newStats->size - previousStats->size;
	sr.realsize = newStats->realsize - previousStats->realsize;
	addStats(parent, &sr);
}

void FilesystemNodeOperationsBase::fillAttr(FSNode *node, FSNode *parent, uint32_t uid,
                                            uint32_t gid, uint32_t auid, uint32_t agid,
                                            uint8_t sesflags, Attributes &attr) {
#ifdef METARESTORE
	mabort("Bad code path - fsnodes_fill_attr() shall not be executed in metarestore context.");
#endif /* METARESTORE */
	uint8_t *ptr;
	uint16_t mode;
	uint32_t nlink;
	(void)sesflags;
	ptr = attr.data();
	if (node->type == FSNodeType::kTrash || node->type == FSNodeType::kReserved) {
		put8bit(&ptr, FSNodeType::kFile);
	} else {
		put8bit(&ptr, node->type);
	}
	mode = node->mode & 07777;
	if (parent) {
		if (parent->mode & (EATTR_NOECACHE << 12)) {
			mode |= (MATTR_NOECACHE << 12);
		}
	}
	if ((node->mode & ((EATTR_NOOWNER | EATTR_NOACACHE) << 12)) ||
	    (sesflags & SESFLAG_MAPALL)) {
		mode |= (MATTR_NOACACHE << 12);
	}
	if ((node->mode & (EATTR_NODATACACHE << 12)) == 0) {
		mode |= (MATTR_ALLOWDATACACHE << 12);
	}
	put16bit(&ptr, mode);
	if ((node->mode & (EATTR_NOOWNER << 12)) && uid != 0) {
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
	put32bit(&ptr, node->atime);
	put32bit(&ptr, node->mtime);
	put32bit(&ptr, node->ctime);
	nlink = node->parents.size();
	switch (node->type) {
	case FSNodeType::kFile:
	case FSNodeType::kTrash:
	case FSNodeType::kReserved:
		put32bit(&ptr, nlink);
		put64bit(&ptr, static_cast<FSNodeFile*>(node)->length);
		break;
	case FSNodeType::kDirectory:
		put32bit(&ptr, static_cast<FSNodeDirectory*>(node)->nlink);
		put64bit(&ptr, static_cast<FSNodeDirectory*>(node)->stats.length >>
		                       30);  // Rescale length to GB (reduces size to 32-bit length)
		break;
	case FSNodeType::kSymlink:
		put32bit(&ptr, nlink);
		*ptr++ = 0;
		*ptr++ = 0;
		*ptr++ = 0;
		*ptr++ = 0;
		put32bit(&ptr, static_cast<FSNodeSymlink*>(node)->path_length);
		break;
	case FSNodeType::kBlockDev:
	case FSNodeType::kCharDev:
		put32bit(&ptr, nlink);
		put32bit(&ptr, static_cast<FSNodeDevice*>(node)->rdev);
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

void FilesystemNodeOperationsBase::fillAttr(const FsContext &context, FSNode *node, FSNode *parent,
                                            Attributes &attr) {
#ifdef METARESTORE
	mabort("Bad code path - fsnodes_fill_attr() shall not be executed in metarestore context.");
#endif /* METARESTORE */
	sassert(context.hasSessionData() && context.hasUidGidData());
	fillAttr(node, parent, context.uid(), context.gid(), context.auid(), context.agid(),
	         context.sesflags(), attr);
}

void FilesystemNodeOperationsBase::removeEdge(uint32_t timeStamp, FSNodeDirectory *parent,
                                              const HString &childName, FSNode *childNode) {
	assert(parent);

	auto dir_it = parent->find(childName);
	assert(dir_it != parent->end());
	assert((*dir_it).second == childNode);
	auto handlePtrToErase = dir_it->first;
	if (dir_it != parent->end()) {
		parent->entries.erase(dir_it);
		parent->entries_hash ^= childName.hash();

		if (parent->case_insensitive) {
			auto lowerCaseIt = parent->find_lowercase_container(childName);
			delete lowerCaseIt->first;
			parent->lowerCaseEntries.erase(lowerCaseIt);
			HString lowerCaseName = HString::hstringToLowerCase(childName);
			parent->lowerCaseEntriesHash ^= lowerCaseName.hash();
		}
	}

	StatsRecord sr;

	getStats(childNode, &sr);
	subStats(parent, &sr);
	parent->mtime = parent->ctime = timeStamp;
	if (childNode->type == FSNodeType::kDirectory) { parent->nlink--; }

	fsnodes_update_checksum(parent);
	HString currentName = childName;
	if (parent->case_insensitive) { currentName = HString::hstringToLowerCase(childName); }

	auto it = std::find_if(
	    childNode->parents.begin(), childNode->parents.end(),
	    [parent, currentName](const std::pair<inode_t, const hstorage::Handle *> &p) {
		    return p.first == parent->id &&
		           (parent->case_insensitive ? HString::hstringToLowerCase(p.second->get())
		                                     : p.second->get()) == currentName;
	    });

	if (it != childNode->parents.end()) { childNode->parents.erase(it); }

	// Delete the handle after the check in the parent vector in the son is done.
	delete handlePtrToErase;

	assert(childNode->type != FSNodeType::kTrash);
	childNode->ctime = timeStamp;
	fsnodes_update_checksum(childNode);

	gMetadata->edgeRemovedSignal.emit(parent->id, childNode->id);
}

void FilesystemNodeOperationsBase::link(uint32_t timeStamp, FSNodeDirectory *parent, FSNode *child,
                                        const HString &name) {
	// Needs to be freed in fsnodes_remove_edge
	auto *handlePtr = new hstorage::Handle(name);
	parent->entries.insert({handlePtr, child});
	parent->entries_hash ^= name.hash();

	if (parent->case_insensitive) {
		HString lowerCaseName = HString::hstringToLowerCase(name);
		// Needs to be freed in fsnodes_remove_edge
		auto *lowercaseHandlePtr = new hstorage::Handle(std::string(lowerCaseName.c_str()));
		parent->lowerCaseEntries.insert({lowercaseHandlePtr, child});
		parent->lowerCaseEntriesHash ^= lowerCaseName.hash();
	}

	child->parents.push_back({parent->id, handlePtr});
	gMetadata->edgeChangedSignal.emit(parent, child, handlePtr);

	if (child->type == FSNodeType::kDirectory) {
		parent->nlink++;
	}

	StatsRecord sr;
	getStats(child, &sr);
	addStats(parent, &sr);
	if (timeStamp > 0) {
		parent->mtime = parent->ctime = timeStamp;
		fsnodes_update_checksum(parent);
		assert(child->type != FSNodeType::kTrash);
		child->ctime = timeStamp;
		fsnodes_update_checksum(child);
	}
}

FSNode *FilesystemNodeOperationsBase::createNode(uint32_t timeStamp, FSNodeDirectory *parent,
                                                 const HString &name, FSNodeType type,
                                                 uint16_t mode, uint16_t umask, uint32_t uid,
                                                 uint32_t gid, uint8_t copysgid,
                                                 AclInheritance inheritAcl,
                                                 inode_t requestedINode) {
	assert(type != FSNodeType::kTrash);

	FSNode *node = FSNode::create(type);
	gMetadata->nodes++;
	if (type == FSNodeType::kDirectory) {
		gMetadata->dirNodes++;
	}
	if (type == FSNodeType::kFile) {
		gMetadata->fileNodes++;
	}
	if (type == FSNodeType::kSymlink) {
		gMetadata->linkNodes++;
	}
	/* create node */
	node->id = gInodeIdGenerator->getNextId(timeStamp, requestedINode);

	node->ctime = node->mtime = node->atime = timeStamp;
	if (type == FSNodeType::kDirectory || type == FSNodeType::kFile) {
		node->goal = parent->goal;
		node->trashtime = parent->trashtime;
	} else {
		node->goal = DEFAULT_GOAL;
		node->trashtime = kDefaultTrashTime;
	}
	if (type == FSNodeType::kDirectory) {
		node->mode = (mode & 07777) | (parent->mode & 0xF000);
	} else {
		node->mode = (mode & 07777) | (parent->mode & (0xF000 & (~(EATTR_NOECACHE << 12))));
	}
	// If desired, node inherits permissions from parent's default ACL
	const RichACL *parent_acl = (inheritAcl == AclInheritance::kInheritAcl)
	                                ? gMetadata->aclStorage.get(parent->id)
	                                : nullptr;
	if (parent_acl) {
		RichACL acl;
		uint16_t mode = node->mode;
		if (RichACL::inheritInode(*parent_acl, mode, acl, umask, type == FSNodeType::kDirectory)) {
			gMetadata->aclStorage.set(node->id, std::move(acl));
		}
		// Set effective permissions as the intersection of mode and ACL
		node->mode &= mode | ~0777;
	} else {
		// Apply umask
		node->mode &= ~(umask & 0777);  // umask must be applied manually
	}
	node->uid = uid;
	if ((parent->mode & 02000) == 02000) {  // set gid flag is set in the parent directory ?
		node->gid = parent->gid;
		if (copysgid && type == FSNodeType::kDirectory) {
			node->mode |= 02000;
		}
	} else {
		node->gid = gid;
	}

	gMetadata->addNode(node);

	fsnodes_update_checksum(node);
	link(timeStamp, parent, node, name);
	fsnodes_quota_update(node, {{QuotaResource::kInodes, +1}});

	if (type == FSNodeType::kFile) {
		fsnodes_quota_update(node, {{QuotaResource::kSize, +getSize(node)}});
	}

	return node;
}

uint32_t FilesystemNodeOperationsBase::getPathSize(FSNodeDirectory *parent, FSNode *child) {
	std::string name;
	uint32_t size;

	if (parent == nullptr || child == nullptr) {
		return 0;
	}

	name = parent->getChildName(child);
	size = name.length();

	while (parent != gMetadata->root && !parent->parents.empty()) {
		child = parent;
		assert(child->parents.size() == 1);
		parent = idToNodeVerify<FSNodeDirectory>(child->parents[0].first);
		name = parent->getChildName(child);
		size += name.length() + 1;
	}

	return size;
}

void FilesystemNodeOperationsBase::getPathData(FSNodeDirectory *parent, FSNode *child,
                                               uint8_t *path, uint32_t size) {
	std::string name;

	if (parent == nullptr || child == nullptr) {
		return;
	}

	name = parent->getChildName(child);

	if (size >= name.length()) {
		size -= name.length();
		memcpy(path + size, name.c_str(), name.length());
	} else if (size > 0) {
		memcpy(path, name.c_str() + (name.length() - size), size);
		size = 0;
	}
	if (size > 0) {
		path[--size] = '/';
	}
	while (parent != gMetadata->root && !parent->parents.empty()) {
		child = parent;
		assert(child->parents.size() == 1);
		parent = idToNodeVerify<FSNodeDirectory>(child->parents[0].first);
		name = parent->getChildName(child);
		if (size >= name.length()) {
			size -= name.length();
			memcpy(path + size, name.c_str(), name.length());
		} else if (size > 0) {
			memcpy(path, name.c_str() + (name.length() - size), size);
			size = 0;
		}
		if (size > 0) {
			path[--size] = '/';
		}
	}
}

void FilesystemNodeOperationsBase::getPath(FSNodeDirectory *parent, FSNode *child,
                                           std::string &path) {
	uint32_t size = getPathSize(parent, child);

	if (size > 65535) {
		safs_pretty_syslog(LOG_WARNING, "path too long !!! - truncate");
		size = 65535;
	}

	path.resize(size);

	getPathData(parent, child, (uint8_t *)path.data(), size);
}

#ifndef METARESTORE
constexpr uint32_t kOldPathContainerLimit = 1000000;

template<class T>
static inline uint32_t getdetachedsize(const T &data) {
	static_assert(std::is_same<T, TrashPathContainer>::value
	              || std::is_same<T, ReservedPathContainer>::value, "unsupported container");
	uint32_t result = 0;
	std::string name;
	uint32_t count = 0;
	for (const auto &entry : data) {
		if (count > kOldPathContainerLimit) {
			// See explanation in getdetacheddata
			break;
		}
		name = (std::string)entry.second;
		if (name.length() > 240) {
			result += 245;
		} else {
			result += 5 + name.length();
		}
		count++;
	}
	return result;
}

static inline inode_t getdetacheddata_getNodeId(const TrashPathContainer::key_type &key) {
	return key.id;
}

static inline inode_t getdetacheddata_getNodeId(const inode_t &key) {
	return key;
}

template<class T>
static inline void getdetacheddata(const T &data, uint8_t *dbuff) {
	static_assert(std::is_same<T, TrashPathContainer>::value
	              || std::is_same<T, ReservedPathContainer>::value, "unsupported container");

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

		if (name.length() > 240) {
			*dbuff = 240;
			dbuff++;
			memcpy(dbuff, "(...)", 5);
			dbuff += 5;
			sptr = (uint8_t*)name.c_str() + (name.length() - 235);
			for (c = 0; c < 235; c++) {
				if (*sptr == '/') {
					*dbuff = '|';
				} else {
					*dbuff = *sptr;
				}
				sptr++;
				dbuff++;
			}
		} else {
			*dbuff = name.length();
			dbuff++;
			sptr = (uint8_t*)name.c_str();
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
		putINode(&dbuff, getdetacheddata_getNodeId(entry.first));
	}
}

uint32_t FilesystemNodeOperationsBase::getDetachedSize(const TrashPathContainer &data) {
	return getdetachedsize(data);
}

void FilesystemNodeOperationsBase::getDetachedData(const TrashPathContainer &data,
                                                   uint8_t *outBuffer) {
	getdetacheddata(data, outBuffer);
}

void FilesystemNodeOperationsBase::getDetachedData(const TrashPathContainer &data, uint32_t offset,
                                                   uint32_t maxEntries,
                                                   std::vector<NamedInodeEntry> &entries) {
#if defined(SAUNAFS_HAVE_64BIT_JUDY) && !defined(DISABLE_JUDY_FOR_TRASHPATHCONTAINER)
	auto it = data.find_nth(offset);
#else
	auto it = offset < data.size() ? std::next(data.begin(), offset) : data.end();
#endif
	for (; maxEntries > 0 && it != data.end(); maxEntries--, ++it) {
		entries.emplace_back((std::string)(*it).second, (*it).first.id);
	}
}

void FilesystemNodeOperationsBase::getDetachedData(const HandleIndexContainer &data,
                                                   uint64_t handleOffset, uint32_t maxEntries,
                                                   std::vector<HandleInodeEntry> &entries,
                                                   bool fromTrash) {
	uint64_t start = (handleOffset & ~k64SignBitMask);
	auto it = data.lower_bound(HandleIndexKey(start));

	for (; maxEntries > 0 && it != data.end(); --maxEntries, ++it) {
		// Ensure we only return entries with the sign bit cleared
		// to the client to avoid sending negative offsets to fuse
		// when requesting next one
		uint64_t handleValueForClient = (*it).first.data & ~k64SignBitMask;
		std::string nameForClient = "";
		if (fromTrash) {
			FSNode *node = idToNode((*it).second);
			nameForClient = gMetadata->trash.at(TrashPathKey(node)).get().c_str();
		} else {
			nameForClient = gMetadata->reserved.at((*it).second).get().c_str();
		}
		entries.emplace_back(handleValueForClient, nameForClient, (*it).second);
	}
}

uint32_t FilesystemNodeOperationsBase::getDetachedSize(const ReservedPathContainer &data) {
	return getdetachedsize(data);
}

void FilesystemNodeOperationsBase::getDetachedData(const ReservedPathContainer &data,
                                                   uint8_t *outBuffer) {
	getdetacheddata(data, outBuffer);
}

void FilesystemNodeOperationsBase::getDetachedData(const ReservedPathContainer &data,
                                                   uint32_t offset, uint32_t maxEntries,
                                                   std::vector<NamedInodeEntry> &entries) {
#if defined(SAUNAFS_HAVE_64BIT_JUDY) && !defined(DISABLE_JUDY_FOR_RESERVEDPATHCONTAINER)
	auto it = data.find_nth(offset);
#else
	auto it = offset < data.size() ? std::next(data.begin(), offset) : data.end();
#endif
	for (; maxEntries > 0 && it != data.end(); maxEntries--, ++it) {
		entries.emplace_back((std::string)(*it).second, (*it).first);
	}
}

uint32_t FilesystemNodeOperationsBase::getDirSize(const FSNodeDirectory *nodeDir,
                                                  uint8_t withAttr) {
	uint32_t result = (((withAttr) ? 40 : 6) * 2) + 3;  // for '.' and '..'
	std::string name;
	for (const auto &entry : nodeDir->entries) {
		name = (std::string)(*entry.first);
		result += ((withAttr) ? 40 : 6) + name.length();
	}
	return result;
}

void FilesystemNodeOperationsBase::getDirData(inode_t rootINode, uint32_t uid, uint32_t gid,
                                              uint32_t auid, uint32_t agid, uint8_t sesflags,
                                              FSNodeDirectory *nodeDir, uint8_t *outBuffer,
                                              uint8_t withAttr) {
	// '.' - self
	outBuffer[0] = 1;
	outBuffer[1] = '.';
	outBuffer += 2;
	if (nodeDir->id != rootINode) {
		putINode(&outBuffer, nodeDir->id);
	} else {
		putINode(&outBuffer, SPECIAL_INODE_ROOT);
	}
	Attributes attr;
	if (withAttr) {
		fillAttr(nodeDir, nodeDir, uid, gid, auid, agid, sesflags, attr);
		::memcpy(outBuffer, attr.data(), attr.size());
		outBuffer += attr.size();
	} else {
		put8bit(&outBuffer, static_cast<uint8_t>(FSNodeType::kDirectory));
	}
	// '..' - parent
	outBuffer[0] = 2;
	outBuffer[1] = '.';
	outBuffer[2] = '.';
	outBuffer += 3;
	if (nodeDir->id == rootINode) {  // root node should returns self as its parent
		putINode(&outBuffer, SPECIAL_INODE_ROOT);
		if (withAttr) {
			fillAttr(nodeDir, nodeDir, uid, gid, auid, agid, sesflags, attr);
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
		if (withAttr) {
			if (!nodeDir->parents.empty()) {
				auto *parent = idToNodeVerify<FSNode>(nodeDir->parents[0].first);
				fillAttr(parent, nodeDir, uid, gid, auid, agid, sesflags, attr);
				::memcpy(outBuffer, attr.data(), attr.size());
			} else {
				if (rootINode == SPECIAL_INODE_ROOT) {
					fillAttr(gMetadata->root, nodeDir, uid, gid, auid, agid, sesflags, attr);
					::memcpy(outBuffer, attr.data(), attr.size());
				} else {
					FSNode *rn = idToNode(rootINode);
					if (rn) {  // it should be always true because it's checked
						   // before, but better check than sorry
						   fillAttr(rn, nodeDir, uid, gid, auid, agid, sesflags, attr);
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
		name = (std::string)(*entry.first);
		outBuffer[0] = name.size();
		outBuffer++;
		memcpy(outBuffer, name.c_str(), name.length());
		outBuffer += name.length();
		putINode(&outBuffer, entry.second->id);
		if (withAttr) {
			fillAttr(entry.second, nodeDir, uid, gid, auid, agid, sesflags, attr);
			::memcpy(outBuffer, attr.data(), attr.size());
			outBuffer += attr.size();
		} else {
			put8bit(&outBuffer, static_cast<uint8_t>(entry.second->type));
		}
	}
}

void FilesystemNodeOperationsBase::getDir(inode_t rootINode, uint32_t uid, uint32_t gid,
                                          uint32_t auid, uint32_t agid, uint8_t sesflags,
                                          FSNodeDirectory *nodeDir, uint64_t firstEntry,
                                          uint64_t numberOfEntries,
                                          std::vector<DirectoryEntry> &dirEntriesOut) {
	uint64_t const SIGN_BIT_64(1ULL << 63ULL);
	sassert(!(firstEntry & SIGN_BIT_64));
	// special entryIndex values
	static constexpr uint64_t kDotEntryIndex = 0;
	static constexpr uint64_t kDotDotEntryIndex = (static_cast<uint64_t>(1) << hstorage::Handle::kHashShift);
	static constexpr uint64_t kUnusedEntryIndex = (static_cast<uint64_t>(2) << hstorage::Handle::kHashShift);

	FSNodeDirectory *parent;
	inode_t inode;
	Attributes attr;

	if (firstEntry == kDotEntryIndex && numberOfEntries >= 1) {
		inode = nodeDir->id != rootINode ? nodeDir->id : SPECIAL_INODE_ROOT;
		parent = idToNodeVerify<FSNodeDirectory>(
		    nodeDir->parents.empty() ? SPECIAL_INODE_ROOT : nodeDir->parents[0].first);
		fillAttr(nodeDir, parent, uid, gid, auid, agid, sesflags, attr);
		dirEntriesOut.emplace_back(kDotEntryIndex, kDotDotEntryIndex, std::move(inode),
		                           std::string("."), std::move(attr));

		firstEntry = kDotDotEntryIndex;
		--numberOfEntries;
	}

	if (firstEntry == kDotDotEntryIndex && numberOfEntries >= 1) {
		if (nodeDir->id == rootINode) {
			inode = SPECIAL_INODE_ROOT;
			parent = idToNodeVerify<FSNodeDirectory>(
			    nodeDir->parents.empty() ? SPECIAL_INODE_ROOT : nodeDir->parents[0].first);
			fillAttr(nodeDir, parent, uid, gid, auid, agid, sesflags, attr);
		} else {
			if (!nodeDir->parents.empty() && nodeDir->parents[0].first != rootINode) {
				inode = nodeDir->parents[0].first;
			} else {
				inode = SPECIAL_INODE_ROOT;
			}

			FSNodeDirectory *grandparent;
			parent = idToNodeVerify<FSNodeDirectory>(
			    nodeDir->parents.empty() ? SPECIAL_INODE_ROOT : nodeDir->parents[0].first);
			grandparent = idToNodeVerify<FSNodeDirectory>(
			    parent->parents.empty() ? SPECIAL_INODE_ROOT : parent->parents[0].first);
			fillAttr(parent, grandparent, uid, gid, auid, agid, sesflags, attr);
		}

		uint64_t next_index = kUnusedEntryIndex;
		if (!nodeDir->entries.empty()) {
			auto first_dirent_it = nodeDir->find_nth(0);
			next_index = (*first_dirent_it).first->data() & ~SIGN_BIT_64;
		}
		dirEntriesOut.emplace_back(kDotDotEntryIndex, next_index, std::move(inode),
		                           std::string(".."), std::move(attr));

		firstEntry = next_index;
		--numberOfEntries;
	}

	if (numberOfEntries == 0 || nodeDir->entries.empty()) { return; }

	std::string name;
	hstorage::Handle first_index(firstEntry);

	// We're trying to find the first entry in the directory that has index
	// equal to first_entry. We don't know the second part of the pair, so we
	// use kUnknownNode as a placeholder, and it is also the minimum possible.
	auto pair_to_find = std::make_pair(&first_index, kUnknownNode);
	auto it = nodeDir->entries.lower_bound(pair_to_find);
	if (it != nodeDir->entries.end() && (*it).first->data() != firstEntry) {
		// We assume that we received hash that had its most significant bit
		// stripped so we try new find with this supposedly stripped bit set
		// again.
		first_index.unlink();  // do not try to unbind the resource under this
		                       // possibly-fake handle in destructor
		first_index = hstorage::Handle(firstEntry | SIGN_BIT_64);
		pair_to_find = std::make_pair(&first_index, kUnknownNode);
		it = nodeDir->entries.lower_bound(pair_to_find);
		if (it != nodeDir->entries.end() && (*it).first->data() != (firstEntry | SIGN_BIT_64)) {
			it = nodeDir->entries.end();
		}
	}
	pair_to_find.first->unlink();
	first_index.unlink(); // do not try to unbind the resource under this possibly-fake handle in destructor
	while (it != nodeDir->entries.end() && numberOfEntries > 0) {
		name = static_cast<std::string>(*(*it).first);
		inode = (*it).second->id;
		fillAttr((*it).second, nodeDir, uid, gid, auid, agid, sesflags, attr);

		firstEntry = (*it).first->data() & ~SIGN_BIT_64;

		uint64_t next_index = kUnusedEntryIndex;
		if (++it != nodeDir->entries.end()) { next_index = (*it).first->data() & ~SIGN_BIT_64; }

		dirEntriesOut.emplace_back(firstEntry, next_index, std::move(inode), std::move(name),
		                           std::move(attr));

		--numberOfEntries;
	}
}

void FilesystemNodeOperationsBase::checkFile(FSNodeFile *nodeFile, ChunkCountArray &chunkCount) {
	uint8_t count;

	chunkCount.fill(0);

	for (const auto &chunkid : nodeFile->chunks) {
		if (chunkid > 0) {
			chunk_get_fullcopies(chunkid, &count);
			count = std::min<unsigned>(count, CHUNK_MATRIX_SIZE - 1);
			chunkCount[count]++;
		}
	}
}
#endif

uint8_t FilesystemNodeOperationsBase::appendChunks(uint32_t timeStamp, FSNodeFile *destNodeFile,
                                                   FSNodeFile *srcNodeFile) {
	if (srcNodeFile->chunks.empty()) { return SAUNAFS_STATUS_OK; }

	uint32_t src_chunks = srcNodeFile->chunkCount();
	uint32_t dst_chunks = destNodeFile->chunkCount();

	if (((uint64_t)src_chunks + (uint64_t)dst_chunks) > ((uint64_t)MAX_INDEX + 1)) {
		return SAUNAFS_ERROR_INDEXTOOBIG;
	}

	StatsRecord psr, nsr;
	getStats(destNodeFile, &psr);

	uint32_t result_chunks = src_chunks + dst_chunks;
	destNodeFile->chunks.resize(result_chunks, 0);

	std::copy(srcNodeFile->chunks.begin(), srcNodeFile->chunks.begin() + src_chunks,
	          destNodeFile->chunks.begin() + dst_chunks);

	for(uint32_t i = 0; i < src_chunks; ++i) {
		auto chunkid = srcNodeFile->chunks[i];
		if (chunkid > 0) {
			if (chunk_add_file(chunkid, destNodeFile->goal) != SAUNAFS_STATUS_OK) {
				safs_pretty_syslog(LOG_ERR,
				                   "structure error - chunk %016" PRIX64
				                   " not found (inode: %" PRIiNode " ; index: %" PRIu32 ")",
				                   chunkid, srcNodeFile->id, i);
			}
		}
	}

	uint64_t length = (static_cast<uint64_t>(dst_chunks) << SFSCHUNKBITS) + srcNodeFile->length;
	if (destNodeFile->type == FSNodeType::kTrash) {
		gMetadata->trashSpace -= destNodeFile->length;
		gMetadata->trashSpace += length;
	} else if (destNodeFile->type == FSNodeType::kReserved) {
		gMetadata->reservedSpace -= destNodeFile->length;
		gMetadata->reservedSpace += length;
	}
	destNodeFile->length = length;
	getStats(destNodeFile, &nsr);
	fsnodes_quota_update(destNodeFile, {{QuotaResource::kSize, nsr.size - psr.size}});
	for (const auto &[parentId, _] : destNodeFile->parents) {
		auto *parent_node = idToNodeVerify<FSNodeDirectory>(parentId);
		addSubStats(parent_node, &nsr, &psr);
	}
	destNodeFile->mtime = timeStamp;
	destNodeFile->atime = timeStamp;
	srcNodeFile->atime = timeStamp;
	fsnodes_update_checksum(srcNodeFile);
	fsnodes_update_checksum(destNodeFile);
	return SAUNAFS_STATUS_OK;
}

void FilesystemNodeOperationsBase::changeFileGoal(FSNodeFile *nodeFile, uint8_t goal) {
	uint8_t old_goal = nodeFile->goal;
	StatsRecord psr, nsr;

	getStats(nodeFile, &psr);
	nodeFile->goal = goal;
	nsr = psr;
	nsr.realsize = fileRealSize(nodeFile, nsr.chunks, nsr.size);
	for (const auto &[parentId, _] : nodeFile->parents) {
		auto *parent_node = idToNodeVerify<FSNodeDirectory>(parentId);
		addSubStats(parent_node, &nsr, &psr);
	}
	for (const auto &chunkid : nodeFile->chunks) {
		if (chunkid > 0) {
			chunk_change_file(chunkid, old_goal, goal);
		}
	}
	fsnodes_update_checksum(nodeFile);
	gMetadata->nodeChangedSignal.emit(nodeFile);
}

void FilesystemNodeOperationsBase::setLength(FSNodeFile *obj, uint64_t length,
                                             bool eraseFurtherChunks) {
	uint32_t chunks;
	StatsRecord psr, nsr;
	getStats(obj, &psr);
	if (obj->type == FSNodeType::kTrash) {
		gMetadata->trashSpace -= obj->length;
		gMetadata->trashSpace += length;
	} else if (obj->type == FSNodeType::kReserved) {
		gMetadata->reservedSpace -= obj->length;
		gMetadata->reservedSpace += length;
	}
	obj->length = length;

	if (eraseFurtherChunks) {
		if (length > 0) {
			chunks = ((length - 1) >> SFSCHUNKBITS) + 1;
		} else {
			chunks = 0;
		}
		for (uint32_t i = chunks; i < obj->chunks.size(); i++) {
			uint64_t chunkid = obj->chunks[i];
			if (chunkid > 0) {
				if (chunk_delete_file(chunkid, obj->goal) != SAUNAFS_STATUS_OK) {
					safs::log_err(
					    "structure error - chunk {:#016x} not found (inode: {} ; index: {})",
					    chunkid, obj->id, i);
				}
			}
		}

		if (chunks < obj->chunks.size()) {
			obj->chunks.resize(chunks);
		}
	}

	getStats(obj, &nsr);
	fsnodes_quota_update(obj, {{QuotaResource::kSize, nsr.size - psr.size}});
	for (const auto &[parentId, _] : obj->parents) {
		auto *parent_node = idToNodeVerify<FSNodeDirectory>(parentId);
		addSubStats(parent_node, &nsr, &psr);
	}
	fsnodes_update_checksum(obj);
	gMetadata->nodeChangedSignal.emit(obj);
}

void FilesystemNodeOperationsBase::changeUidGid(FSNode *node, uint32_t uid, uint32_t gid) {
	int64_t size = 0;
	fsnodes_quota_update(node, {{QuotaResource::kInodes, -1}});
	if (node->type == FSNodeType::kFile || node->type == FSNodeType::kTrash ||
	    node->type == FSNodeType::kReserved) {
		size = getSize(node);
		fsnodes_quota_update(node, {{QuotaResource::kSize, -size}});
	}
	node->uid = uid;
	node->gid = gid;
	fsnodes_quota_update(node, {{QuotaResource::kInodes, +1}});
	if (node->type == FSNodeType::kFile || node->type == FSNodeType::kTrash ||
	    node->type == FSNodeType::kReserved) {
		fsnodes_quota_update(node, {{QuotaResource::kSize, +size}});
	}
}

void FilesystemNodeOperationsBase::removeNode(uint32_t timeStamp, FSNode *node) {
	if (!node->parents.empty()) {
		return;
	}

	if (gChecksumBackgroundUpdater.isNodeIncluded(node)) {
		removeFromChecksum(gChecksumBackgroundUpdater.fsNodesChecksum, node->checksum);
	}

	removeFromChecksum(gMetadata->fsNodesChecksum, node->checksum);

	// and free
	gMetadata->nodes--;
	gMetadata->aclStorage.erase(node->id);

	if (node->type == FSNodeType::kDirectory) {
		gMetadata->dirNodes--;
	}

	if (node->type == FSNodeType::kFile || node->type == FSNodeType::kTrash ||
	    node->type == FSNodeType::kReserved) {
		fsnodes_quota_update(node, {{QuotaResource::kSize, -getSize(node)}});
		gMetadata->fileNodes--;
		for (uint32_t i = 0; i < static_cast<FSNodeFile*>(node)->chunks.size(); ++i) {
			uint64_t chunkid = static_cast<FSNodeFile*>(node)->chunks[i];
			if (chunkid > 0) {
				if (chunk_delete_file(chunkid, node->goal) != SAUNAFS_STATUS_OK) {
					safs::log_err(
					    "structure error - chunk {:#016x} not found (inode: {} ; index: {})",
					    chunkid, node->id, i);
				}
			}
		}
	}

	if (node->type == FSNodeType::kSymlink) {
		gMetadata->linkNodes--;
	}

	gMetadata->inodePool.release(node->id, timeStamp, true);
	xattr_removeinode(node->id);
	fsnodes_quota_update(node, {{QuotaResource::kInodes, -1}});
	fsnodes_quota_remove(QuotaOwnerType::kInode, node->id);
#ifndef METARESTORE
	fsnodes_periodic_remove(node->id);
	dcm_modify(node->id, 0);
#endif

	// remove node from nodeHash
	uint32_t nodeHashIndex = NODEHASHPOS(node->id);
	auto nodeIterator = std::find(gMetadata->nodeHash[nodeHashIndex].begin(),
	                              gMetadata->nodeHash[nodeHashIndex].end(), node);

	FSNode::destroy(node);

	if (nodeIterator != gMetadata->nodeHash[nodeHashIndex].end()) {
		auto lastElement = gMetadata->nodeHash[nodeHashIndex].end() - 1;
		std::iter_swap(nodeIterator, lastElement); // Swap with last element to avoid erase: O(1)
		gMetadata->nodeHash[nodeHashIndex].pop_back(); // Remove the last element: O(1)
	}
}

void FilesystemNodeOperationsBase::unlink(uint32_t timeStamp, FSNodeDirectory *parent,
                                          const HString &childName, FSNode *childNode) {
	std::string path;

	if (childNode->parents.size() == 1) {  // last link
		// go to trash or reserved ? - get path
		if (childNode->type == FSNodeType::kFile &&
		    (childNode->trashtime > 0 ||
		     !static_cast<FSNodeFile *>(childNode)->sessionIds.empty())) {
			getPath(parent, childNode, path);
		}
	}

	removeEdge(timeStamp, parent, childName, childNode);
	if (!childNode->parents.empty()) { return; }

	// last link
	if (childNode->type == FSNodeType::kFile) {
		auto *file_node = static_cast<FSNodeFile *>(childNode);
		if (childNode->trashtime > 0) {
			childNode->type = FSNodeType::kTrash;
			childNode->ctime = timeStamp;
			fsnodes_update_checksum(childNode);

			addTrashEntry(gMetadata->trash, gMetadata->trashHandlesIndex,
			              gMetadata->trashReservedToId, childNode, path);

			gMetadata->trashSpace += file_node->length;
			gMetadata->trashNodes++;
		} else if (!file_node->sessionIds.empty()) {
			childNode->type = FSNodeType::kReserved;
			fsnodes_update_checksum(childNode);

			addReservedEntry(gMetadata->reserved, gMetadata->reservedHandlesIndex,
			                 gMetadata->trashReservedToId, childNode, path);

			gMetadata->reservedSpace += file_node->length;
			gMetadata->reservedNodes++;
		} else {
			removeNode(timeStamp, childNode);
		}
	} else {
		removeNode(timeStamp, childNode);
	}
}

int FilesystemNodeOperationsBase::purge(uint32_t timeStamp, FSNode *node) {
	if (node->type == FSNodeType::kTrash) {
		FSNodeFile *file_node = static_cast<FSNodeFile *>(node);
		gMetadata->trashSpace -= file_node->length;
		gMetadata->trashNodes--;

		if (!file_node->sessionIds.empty()) {
			file_node->type = FSNodeType::kReserved;
			fsnodes_update_checksum(file_node);
			gMetadata->reservedSpace += file_node->length;
			gMetadata->reservedNodes++;

			moveTrashToReservedEntry(gMetadata->trash, gMetadata->trashHandlesIndex,
			                         gMetadata->reserved, gMetadata->reservedHandlesIndex,
			                         gMetadata->trashReservedToId, node);

			return 0;
		} else {
			removeTrashEntry(gMetadata->trash, gMetadata->trashHandlesIndex,
			                 gMetadata->trashReservedToId, node);
			node->ctime = timeStamp;
			fsnodes_update_checksum(node);
			removeNode(timeStamp, node);

			return 1;
		}
	} else if (node->type == FSNodeType::kReserved) {
		FSNodeFile *file_node = static_cast<FSNodeFile *>(node);

		gMetadata->reservedSpace -= file_node->length;
		gMetadata->reservedNodes--;

		removeReservedEntry(gMetadata->reserved, gMetadata->reservedHandlesIndex,
		                    gMetadata->trashReservedToId, node->id);

		file_node->ctime = timeStamp;
		fsnodes_update_checksum(file_node);
		removeNode(timeStamp, file_node);
		return 1;
	}
	return -1;
}

uint8_t FilesystemNodeOperationsBase::undel(uint32_t timeStamp, FSNodeFile *node) {
	uint8_t is_new;
	uint32_t i, partleng, dots;
	/* check path */
	std::string path_str;
	if (node->type == FSNodeType::kTrash) {
		path_str = (std::string)gMetadata->trash.at(TrashPathKey(node));
	} else {
		assert(node->type == FSNodeType::kReserved);
		path_str = (std::string)gMetadata->reserved.at(node->id);
	}

	const char *path = path_str.c_str();
	unsigned pleng = path_str.length();

	if (path_str.empty()) {
		return SAUNAFS_ERROR_CANTCREATEPATH;
	}
	while (*path == '/' && pleng > 0) {
		path++;
		pleng--;
	}
	if (pleng == 0) {
		return SAUNAFS_ERROR_CANTCREATEPATH;
	}
	partleng = 0;
	dots = 0;
	for (i = 0; i < pleng; i++) {
		if (path[i] == 0) {  // incorrect name character
			return SAUNAFS_ERROR_CANTCREATEPATH;
		} else if (path[i] == '/') {
			if (partleng == 0) {  // "//" in path
				return SAUNAFS_ERROR_CANTCREATEPATH;
			}
			if (partleng == dots && partleng <= 2) {  // '.' or '..' in path
				return SAUNAFS_ERROR_CANTCREATEPATH;
			}
			partleng = 0;
			dots = 0;
		} else {
			if (path[i] == '.') {
				dots++;
			}
			partleng++;
			if (partleng > MAXFNAMELENG) {
				return SAUNAFS_ERROR_CANTCREATEPATH;
			}
		}
	}
	if (partleng == 0) {  // last part cannot be empty - it's the name of undeleted file
		return SAUNAFS_ERROR_CANTCREATEPATH;
	}
	if (partleng == dots && partleng <= 2) {  // '.' or '..' in path
		return SAUNAFS_ERROR_CANTCREATEPATH;
	}

	// create path
	FSNode *n = nullptr;
	FSNodeDirectory *p = gMetadata->root;
	is_new = 0;
	for (;;) {
		partleng = 0;
		while ((partleng < pleng) && (path[partleng] != '/')) {
			partleng++;
		}
		HString name(path, partleng);
		if (partleng == pleng) {  // last name
			if (isNameUsed(p, name)) { return SAUNAFS_ERROR_EEXIST; }
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
			link(timeStamp, p, node, name);
			gMetadata->trashSpace -= node->length;
			gMetadata->trashNodes--;
			return SAUNAFS_STATUS_OK;
		} else {
			if (is_new == 0) {
				n = lookup(p, name);
				if (n == nullptr) {
					is_new = 1;
				} else {
					if (n->type != FSNodeType::kDirectory) {
						return SAUNAFS_ERROR_CANTCREATEPATH;
					}
				}
			}
			if (is_new == 1) {
				n = createNode(timeStamp, p, name, FSNodeType::kDirectory, 0755, 0, 0, 0, 0,
				               AclInheritance::kDontInheritAcl);

#ifndef METARESTORE
				assert(metadataserver::isMaster());
#endif

				gFSOperations->changeLog(
				    timeStamp,
				    "CREATE(%" PRIiNode ",%s,%c,%d,%" PRIu32 ",%" PRIu32 ",%" PRIu32 "):%" PRIiNode,
				    p->id, escapeName(name).c_str(), static_cast<char>(FSNodeType::kDirectory),
				    n->mode & 07777, (uint32_t)0, (uint32_t)0, (uint32_t)0, n->id);
			}
			p = static_cast<FSNodeDirectory*>(n);
			assert(n->type == FSNodeType::kDirectory);
		}
		path += partleng + 1;
		pleng -= partleng + 1;
	}
}

#ifndef METARESTORE

void FilesystemNodeOperationsBase::getGoalRecursive(FSNode *node, uint8_t gmode,
                                                    GoalStatistics &fgtab, GoalStatistics &dgtab) {
	if (node->type == FSNodeType::kFile || node->type == FSNodeType::kTrash ||
	    node->type == FSNodeType::kReserved) {
		if (!GoalId::isValid(node->goal)) {
			safs_pretty_syslog(LOG_WARNING, "file inode %" PRIiNode ": unknown goal !!! - fixing",
			       node->id);
			changeFileGoal(static_cast<FSNodeFile *>(node), DEFAULT_GOAL);
		}
		fgtab[node->goal]++;
	} else if (node->type == FSNodeType::kDirectory) {
		if (!GoalId::isValid(node->goal)) {
			safs_pretty_syslog(LOG_WARNING,
			       "directory inode %" PRIiNode ": unknown goal !!! - fixing", node->id);
			node->goal = DEFAULT_GOAL;
		}
		dgtab[node->goal]++;
		if (gmode == GMODE_RECURSIVE) {
			const FSNodeDirectory *dir_node = static_cast<const FSNodeDirectory*>(node);
			for (const auto &entry : dir_node->entries) {
				getGoalRecursive(entry.second, gmode, fgtab, dgtab);
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
			const FSNodeDirectory *dir_node = static_cast<const FSNodeDirectory*>(node);
			for (const auto &entry : dir_node->entries) {
				getTrashTimeRecursive(entry.second, gmode, fileTrashtimes, dirTrashtimes);
			}
		}
	}
}

void FilesystemNodeOperationsBase::getEAttrRecursive(FSNode *node, uint8_t gmode,
                                                     ExtendedAttributesArray &fileEAttrTab,
                                                     ExtendedAttributesArray &dirEAttrTab) {
	if (node->type != FSNodeType::kDirectory) {
		fileEAttrTab[(node->mode >> 12) & (EATTR_NOOWNER | EATTR_NOACACHE | EATTR_NODATACACHE)]++;
	} else {
		dirEAttrTab[(node->mode >> 12)]++;
		if (gmode == GMODE_RECURSIVE) {
			const FSNodeDirectory *dir_node = static_cast<const FSNodeDirectory*>(node);
			for (const auto &entry : dir_node->entries) {
				getEAttrRecursive(entry.second, gmode, fileEAttrTab, dirEAttrTab);
			}
		}
	}
}

#endif  // METARESTORE

void FilesystemNodeOperationsBase::setgoalRecursive(FSNode *node, uint32_t timeStamp, uint32_t uid,
                                                    uint8_t goal, uint8_t smode,
                                                    inode_t *modifiedINodesOut,
                                                    inode_t *unchangedINodesOut,
                                                    inode_t *permissionDeniedINodesOut) {
	if (node->type == FSNodeType::kFile || node->type == FSNodeType::kDirectory ||
	    node->type == FSNodeType::kTrash || node->type == FSNodeType::kReserved) {
		if ((node->mode & (EATTR_NOOWNER << 12)) == 0 && uid != 0 && node->uid != uid) {
			(*permissionDeniedINodesOut)++;
		} else {
			if ((smode & SMODE_TMASK) == SMODE_SET && node->goal != goal) {
				if (node->type != FSNodeType::kDirectory) {
					changeFileGoal(static_cast<FSNodeFile *>(node), goal);
					(*modifiedINodesOut)++;
				} else {
					node->goal = goal;
					gMetadata->nodeChangedSignal.emit(node);
					(*modifiedINodesOut)++;
				}
				updateCTime(node, timeStamp);
				fsnodes_update_checksum(node);
			} else {
				(*unchangedINodesOut)++;
			}
		}
		if (node->type == FSNodeType::kDirectory && (smode & SMODE_RMASK)) {
			for (const auto &entry : static_cast<const FSNodeDirectory*>(node)->entries) {
				setgoalRecursive(entry.second, timeStamp, uid, goal, smode, modifiedINodesOut,
				                 unchangedINodesOut, permissionDeniedINodesOut);
			}
		}
	}
}

void FilesystemNodeOperationsBase::setTrashTimeRecursive(FSNode *node, uint32_t timeStamp,
                                                         uint32_t uid, uint32_t trashtime,
                                                         uint8_t smode, inode_t *modifiedINodesOut,
                                                         inode_t *unchangedINodesOut,
                                                         inode_t *permissionDeniedINodesOut) {
	uint8_t set;

	if (node->type == FSNodeType::kFile || node->type == FSNodeType::kDirectory ||
	    node->type == FSNodeType::kTrash || node->type == FSNodeType::kReserved) {
		if ((node->mode & (EATTR_NOOWNER << 12)) == 0 && uid != 0 && node->uid != uid) {
			(*permissionDeniedINodesOut)++;
		} else {
			set = 0;
			auto old_trash_key = TrashPathKey(node);
			switch (smode & SMODE_TMASK) {
			case SMODE_SET:
				if (node->trashtime != trashtime) {
					node->trashtime = trashtime;
					set = 1;
				}
				break;
			case SMODE_INCREASE:
				if (node->trashtime < trashtime) {
					node->trashtime = trashtime;
					set = 1;
				}
				break;
			case SMODE_DECREASE:
				if (node->trashtime > trashtime) {
					node->trashtime = trashtime;
					set = 1;
				}
				break;
			}
			if (set) {
				(*modifiedINodesOut)++;
				node->ctime = timeStamp;
				if (node->type == FSNodeType::kTrash) {
					updateTrashFromOldEntry(gMetadata->trash, node, old_trash_key);
				}
				fsnodes_update_checksum(node);
			} else {
				(*unchangedINodesOut)++;
			}
		}
		if (node->type == FSNodeType::kDirectory && (smode & SMODE_RMASK)) {
			for(const auto &entry : static_cast<const FSNodeDirectory*>(node)->entries) {
				setTrashTimeRecursive(entry.second, timeStamp, uid, trashtime, smode,
				                      modifiedINodesOut, unchangedINodesOut,
				                      permissionDeniedINodesOut);
			}
		}
	}
}

void FilesystemNodeOperationsBase::setEAttrRecursive(FSNode *node, uint32_t timeStamp, uint32_t uid,
                                                     uint8_t eattr, uint8_t smode,
                                                     inode_t *modifiedINodesOut,
                                                     inode_t *unchangedINodesOut,
                                                     inode_t *permissionDeniedINodesOut) {
	uint8_t neweattr, seattr;

	if ((node->mode & (EATTR_NOOWNER << 12)) == 0 && uid != 0 && node->uid != uid) {
		(*permissionDeniedINodesOut)++;
	} else {
		seattr = eattr;
		if (node->type != FSNodeType::kDirectory) {
			node->mode &= ~(EATTR_NOECACHE << 12);
			seattr &= ~(EATTR_NOECACHE);
		}
		neweattr = (node->mode >> 12);
		switch (smode & SMODE_TMASK) {
		case SMODE_SET:
			neweattr = seattr;
			break;
		case SMODE_INCREASE:
			neweattr |= seattr;
			break;
		case SMODE_DECREASE:
			neweattr &= ~seattr;
			break;
		}
		if (neweattr != (node->mode >> 12)) {
			node->mode = (node->mode & 0xFFF) | (((uint16_t)neweattr) << 12);
			const RichACL *node_acl = gMetadata->aclStorage.get(node->id);
			if (node_acl) {
				gMetadata->aclStorage.setMode(node->id, node->mode,
				                              node->type == FSNodeType::kDirectory);
			}
			(*modifiedINodesOut)++;
			updateCTime(node, timeStamp);
		} else {
			(*unchangedINodesOut)++;
		}
	}
	if (node->type == FSNodeType::kDirectory && (smode & SMODE_RMASK)) {
		const FSNodeDirectory *dir_node = static_cast<const FSNodeDirectory*>(node);
		for (const auto &entry : dir_node->entries) {
			setEAttrRecursive(entry.second, timeStamp, uid, eattr, smode, modifiedINodesOut,
			                  unchangedINodesOut, permissionDeniedINodesOut);
		}
	}
	fsnodes_update_checksum(node);
}

uint8_t FilesystemNodeOperationsBase::deleteAcl(FSNode *node, AclType type, uint32_t timeStamp) {
	if (type == AclType::kRichACL) {
		gMetadata->aclStorage.erase(node->id);
	} else if (type == AclType::kDefault) {
		if (node->type != FSNodeType::kDirectory) { return SAUNAFS_ERROR_ENOTSUP; }
		const RichACL *node_acl = gMetadata->aclStorage.get(node->id);
		if (node_acl) {
			RichACL new_acl = *node_acl;
			new_acl.createExplicitInheritance();
			new_acl.removeInheritOnly(true);
			if (new_acl.size() == 0) {
				gMetadata->aclStorage.erase(node->id);
			} else {
				gMetadata->aclStorage.set(node->id, std::move(new_acl));
			}
		}
	} else if (type == AclType::kAccess) {
		const RichACL *node_acl = gMetadata->aclStorage.get(node->id);
		if (node_acl) {
			RichACL new_acl = *node_acl;
			new_acl.createExplicitInheritance();
			new_acl.removeInheritOnly(false);
			if (new_acl.size() == 0) {
				gMetadata->aclStorage.erase(node->id);
			} else {
				gMetadata->aclStorage.set(node->id, std::move(new_acl));
			}
		}
	} else {
		return SAUNAFS_ERROR_EINVAL;
	}
	updateCTime(node, timeStamp);
	fsnodes_update_checksum(node);
	return SAUNAFS_STATUS_OK;
}

#ifndef METARESTORE
uint8_t FilesystemNodeOperationsBase::getAcl(FSNode *node, RichACL &acl) {
	const RichACL *richacl = gMetadata->aclStorage.get(node->id);
	if (!richacl) {
		return SAUNAFS_ERROR_ENOATTR;
	}
	acl = *richacl;
	assert((node->mode & 0777) == richacl->getMode());
	return SAUNAFS_STATUS_OK;
}
#endif

uint8_t FilesystemNodeOperationsBase::setAcl(FSNode *node, const RichACL &acl, uint32_t timeStamp) {
	if (!acl.checkInheritFlags(node->type == FSNodeType::kDirectory)) {
		return SAUNAFS_ERROR_ENOTSUP;
	}

	uint16_t mode = node->mode;
	if (RichACL::equivMode(acl, mode, node->type == FSNodeType::kDirectory)) {
		node->mode = (node->mode & ~0777) | (mode & 0777);
		gMetadata->aclStorage.erase(node->id);
	} else {
		if (!acl.isAutoSetMode()) { node->mode = (node->mode & ~0777) | (acl.getMode() & 0777); }
		RichACL new_acl = acl;
		if (acl.isAutoSetMode()) {
			new_acl.setFlags(new_acl.getFlags() & ~RichACL::kAutoSetMode);
			new_acl.setMode(node->mode, node->type == FSNodeType::kDirectory);
		}
		gMetadata->aclStorage.set(node->id, std::move(new_acl));
	}

	updateCTime(node, timeStamp);
	fsnodes_update_checksum(node);
	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemNodeOperationsBase::setAcl(FSNode *node, AclType type,
                                             const AccessControlList &acl, uint32_t timeStamp) {
	if (type != AclType::kDefault && type != AclType::kAccess) {
		return SAUNAFS_ERROR_EINVAL;
	}

	if (type == AclType::kDefault && node->type != FSNodeType::kDirectory) {
		return SAUNAFS_ERROR_ENOTSUP;
	}

	const RichACL *node_acl = gMetadata->aclStorage.get(node->id);
	RichACL new_acl;

	if (node_acl) {
		new_acl = *node_acl;
		new_acl.createExplicitInheritance();
		new_acl.removeInheritOnly(type == AclType::kDefault);
	}

	if (type == AclType::kDefault) {
		new_acl.appendDefaultPosixACL(acl);
		new_acl.setMode(node->mode, true);
	} else {
		new_acl.appendPosixACL(acl, node->type == FSNodeType::kDirectory);
		node->mode = (node->mode & ~0777) | (new_acl.getMode() & 0777);
	}
	gMetadata->aclStorage.set(node->id, std::move(new_acl));

	updateCTime(node, timeStamp);
	fsnodes_update_checksum(node);
	return SAUNAFS_STATUS_OK;
}

int FilesystemNodeOperationsBase::nameCheck(const std::string &name) {
	uint32_t i;
	if (name.length() == 0 || name.length() > MAXFNAMELENG) {
		return -1;
	}
	if (name[0] == '.') {
		if (name.length() == 1) {
			return -1;
		}
		if (name.length() == 2 && name[1] == '.') {
			return -1;
		}
	}
	for (i = 0; i < name.length(); i++) {
		if (name[i] == '\0' || name[i] == '/') {
			return -1;
		}
	}
	return 0;
}

int FilesystemNodeOperationsBase::access(const FsContext &context, FSNode *node, uint8_t modemask) {
	uint8_t nodemode;
	if ((context.sesflags() & SESFLAG_NOMASTERPERMCHECK) || context.uid() == 0) {
		return 1;
	}
	const RichACL *node_acl = gMetadata->aclStorage.get(node->id);
	if (node_acl) {
		assert((node->mode & 0777) == node_acl->getMode());

		uint32_t mask = RichACL::convertMode2Mask(modemask);
		if (node->type != FSNodeType::kDirectory) {
			mask &= ~RichACL::Ace::kDeleteChild;
		}
		return node_acl->checkPermission(mask, node->uid, node->gid, context.uid(), context.groups());
	} else {
		if (context.uid() == node->uid || (node->mode & (EATTR_NOOWNER << 12))) {
			nodemode = ((node->mode) >> 6) & 7;
		} else if (context.sesflags() & SESFLAG_IGNOREGID) {
			nodemode = (((node->mode) >> 3) | (node->mode)) & 7;
		} else if (context.hasGroup(node->gid)) {
			nodemode = ((node->mode) >> 3) & 7;
		} else {
			nodemode = (node->mode & 7);
		}
	}
	if ((nodemode & modemask) == modemask) {
		return 1;
	}
	return 0;
}

int FilesystemNodeOperationsBase::stickyAccess(FSNode *parent, FSNode *node, uint32_t uid) {
	if (uid == 0 || (parent->mode & 01000) == 0) {  // super user or sticky bit is not set
		return 1;
	}
	if (uid == parent->uid || (parent->mode & (EATTR_NOOWNER << 12)) || uid == node->uid ||
	    (node->mode & (EATTR_NOOWNER << 12))) {
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

uint8_t FilesystemNodeOperationsBase::getNodeForOperation(const FsContext &context,
                                                          ExpectedNodeType expectedNodeType,
                                                          uint8_t modemask, inode_t inode,
                                                          FSNode **nodeOut,
                                                          FSNodeDirectory **rootDirOut) {
	FSNode *p;
	FSNodeDirectory *rn;
	if (!context.hasSessionData()) {
		rn = nullptr;
		p = idToNode(inode);
		if (!p) {
			return SAUNAFS_ERROR_ENOENT;
		}
	} else if (context.rootinode() == SPECIAL_INODE_ROOT || (context.rootinode() == 0)) {
		rn = gMetadata->root;
		p = idToNode(inode);
		if (!p) {
			return SAUNAFS_ERROR_ENOENT;
		}
		if (context.rootinode() == 0 && p->type != FSNodeType::kTrash &&
		    p->type != FSNodeType::kReserved) {
			return SAUNAFS_ERROR_EPERM;
		}
	} else {
		rn = idToNode<FSNodeDirectory>(context.rootinode());
		if (!rn || rn->type != FSNodeType::kDirectory) {
			return SAUNAFS_ERROR_ENOENT;
		}
		if (inode == SPECIAL_INODE_ROOT || inode == context.rootinode()) {
			p = rn;
		} else {
			p = idToNode(inode);
			if (!p) {
				return SAUNAFS_ERROR_ENOENT;
			}
			if (!isAncestorOrNodeReservedOrTrash(rn, p)) { return SAUNAFS_ERROR_EPERM; }
		}
	}
	if ((expectedNodeType == ExpectedNodeType::kDirectory) && (p->type != FSNodeType::kDirectory)) {
		return SAUNAFS_ERROR_ENOTDIR;
	}
	if ((expectedNodeType == ExpectedNodeType::kNotDirectory) &&
	    (p->type == FSNodeType::kDirectory)) {
		return SAUNAFS_ERROR_EPERM;
	}
	if ((expectedNodeType == ExpectedNodeType::kFile) && (p->type != FSNodeType::kFile) &&
	    (p->type != FSNodeType::kReserved) && (p->type != FSNodeType::kTrash)) {
		return SAUNAFS_ERROR_EPERM;
	}
	if ((expectedNodeType == ExpectedNodeType::kFileOrDirectory) &&
	    (p->type != FSNodeType::kDirectory) && (p->type != FSNodeType::kFile) &&
	    (p->type != FSNodeType::kReserved) && (p->type != FSNodeType::kTrash)) {
		return SAUNAFS_ERROR_EPERM;
	}
	if (context.canCheckPermissions() && !access(context, p, modemask)) {
		return SAUNAFS_ERROR_EACCES;
	}
	*nodeOut = p;
	if (rootDirOut) { *rootDirOut = rn; }
	return SAUNAFS_STATUS_OK;
}
