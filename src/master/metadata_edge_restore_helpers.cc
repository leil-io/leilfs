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
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
*/

#include "master/metadata_edge_restore_helpers.h"

#include "master/filesystem_node_types.h"
#include "master/filesystem_operations_interface.h"
#include "master/metadata_backend_interface.h"
#include "slogger/slogger.h"

namespace {

/// Resolves an inode to a directory node, or nullptr when it is absent or not a directory.
/// @param[out] missing Set to true when the inode simply does not exist (vs. exists but is not a
///                     directory), so callers can treat "parent gone" as idempotent success.
FSNodeDirectory *resolveDirectory(const FilesystemOperationContext &fsOpContext, inode_t parentId,
                                  bool &missing) {
	missing = false;
	FSNode *parentNode = gFSOperations->nodeOperations()->idToNode(fsOpContext, parentId);
	if (parentNode == nullptr) {
		missing = true;
		return nullptr;
	}
	if (parentNode->type != FSNodeType::kDirectory) { return nullptr; }
	return static_cast<FSNodeDirectory *>(parentNode);
}

/// Detaches one directory entry in place: erases it from the parent map, subtracts the child
/// stats, decrements the directory link count for directory children, drops the matching parent
/// back-pointer, and frees the handle. Signal-free; node bodies are left untouched.
void detachEntry(const FilesystemOperationContext &fsOpContext, FSNodeDirectory *parent,
                 FSNodeDirectory::iterator entry) {
	auto *handlePtr = entry->first;
	FSNode *child = entry->second;

	parent->entries.erase(entry);
	parent->entries_hash ^= handlePtr->hash();

	StatsRecord childStats{};
	gFSOperations->nodeOperations()->getStats(fsOpContext, child, &childStats);
	gFSOperations->nodeOperations()->subStats(fsOpContext, parent, &childStats);

	if (child->type == FSNodeType::kDirectory) { parent->nlink--; }

	// Identity match on the handle pointer: the same handle is stored in the parent's entry map
	// and in the child's parents vector, so this drops exactly the back-pointer for this edge
	// (important when the child is a hard-linked file with several parents).
	for (auto parentIter = child->parents.begin(); parentIter != child->parents.end();
	     ++parentIter) {
		if (parentIter->second == handlePtr) {
			child->parents.erase(parentIter);
			break;
		}
	}

	delete handlePtr;
}

}  // namespace

namespace metadata::edges {

int8_t restoreLoadedEdge(const FilesystemOperationContext &fsOpContext, inode_t parentId,
                         inode_t childId, const HString &name) {
	bool parentMissing = false;
	FSNodeDirectory *parent = resolveDirectory(fsOpContext, parentId, parentMissing);
	if (parent == nullptr) {
		safs::log_err("{}: parent inode {} missing or not a directory", __func__, parentId);
		return kOpFailure;
	}

	FSNode *child = gFSOperations->nodeOperations()->idToNode(fsOpContext, childId);
	if (child == nullptr) {
		safs::log_err("edge restore: child inode {} not found for edge under parent {}", childId,
		              parentId);
		return kOpFailure;
	}

	auto existing = parent->find(name);
	if (existing != parent->entries.end()) {
		if (existing->second == child) { return kOpSuccess; }  // edge already correct
		// A different child currently occupies this name; detach it before re-attaching.
		detachEntry(fsOpContext, parent, existing);
	}

	// Signal-free attach, mirroring MetadataBackendForkless::loadEdge() (case-insensitive
	// lower-case entries are intentionally not maintained here, matching the loader).
	auto *handlePtr = new hstorage::Handle(name);
	if (!parent->entries.insert({handlePtr, child}).second) {
		delete handlePtr;
		safs::log_err("{}: duplicate entry {}->{} under directory {}", __func__, name.c_str(),
		              childId, parentId);
		return kOpFailure;
	}
	parent->entries_hash ^= handlePtr->hash();
	child->parents.push_back({parent->id, handlePtr});

	if (child->type == FSNodeType::kDirectory) { parent->nlink++; }

	StatsRecord childStats{};
	gFSOperations->nodeOperations()->getStats(fsOpContext, child, &childStats);
	gFSOperations->nodeOperations()->addStats(fsOpContext, parent, &childStats);

	return kOpSuccess;
}

int8_t removeLoadedEdge(const FilesystemOperationContext &fsOpContext, inode_t parentId,
                        const HString &name) {
	bool parentMissing = false;
	FSNodeDirectory *parent = resolveDirectory(fsOpContext, parentId, parentMissing);
	if (parent == nullptr) {
		// Parent gone: the edge cannot exist, so removal is already satisfied. A present-but-
		// non-directory parent is a real inconsistency.
		if (parentMissing) { return kOpSuccess; }
		safs::log_err("{}: parent inode {} is not a directory during edge removal", __func__,
		              parentId);
		return kOpFailure;
	}

	auto entry = parent->find(name);
	if (entry == parent->entries.end()) { return kOpSuccess; }  // already absent

	detachEntry(fsOpContext, parent, entry);
	return kOpSuccess;
}

}  // namespace metadata::edges
