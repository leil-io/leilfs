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

#include "master/metadata_node_restore_helpers.h"

#include <algorithm>
#include <cstring>

#include "master/filesystem_checksum.h"
#include "master/filesystem_metadata.h"
#include "master/filesystem_operations_interface.h"
#include "master/filesystem_quota.h"
#include "master/matoclserv_sessions.h"
#include "master/metadata_backend_interface.h"

namespace {
/// File-like node types share open-file/quota handling: regular files plus their trash and
/// reserved variants.
bool isFileLikeNode(FSNodeType type) {
	return type == FSNodeType::kFile || type == FSNodeType::kTrash || type == FSNodeType::kReserved;
}

/// Whether a current node may be replaced by a restored node of a given type. Types must match
/// exactly, or both be file-like (a file may move between regular/trash/reserved across a
/// rollback boundary).
bool areCompatibleForRestore(FSNodeType currentType, FSNodeType restoredType) {
	auto areBothFileLike = isFileLikeNode(currentType) && isFileLikeNode(restoredType);
	return currentType == restoredType || areBothFileLike;
}

/// Enforces the node-only replacement contract and carries over edge-owned directory state.
///
/// Rejects the replacement when the current node still has parent links, or (for a
/// directory->directory replacement) when the current directory still has entries: both are
/// EDGE-section state that node restore must not touch. On success the restored directory
/// inherits the current directory's caseInsensitive flag.
///
/// @return kOpSuccess when replacement is allowed, kOpFailure when edge-owned state is attached.
int8_t restoreParentsAndSectionLocalState(FSNode *currentNode, FSNode *restoredNode) {
	if (!currentNode->parents.empty()) {
		safs::log_err("node restore: inode {} still has parent links during node-only replacement",
		              currentNode->id);
		return kOpFailure;
	}

	if (currentNode->type == FSNodeType::kDirectory &&
	    restoredNode->type == FSNodeType::kDirectory) {
		auto *currentDir = static_cast<FSNodeDirectory *>(currentNode);
		auto *restoredDir = static_cast<FSNodeDirectory *>(restoredNode);

		if (!currentDir->entries.empty() || !currentDir->lowerCaseEntries.empty()) {
			safs::log_err(
			    "node restore: directory inode {} still has edge-owned state during node-only replacement",
			    currentNode->id);
			return kOpFailure;
		}

		restoredDir->caseInsensitive = currentDir->caseInsensitive;
	}

	return kOpSuccess;
}

/// Applies all node-local accounting for a node being added to the live image.
///
/// Bumps per-type counters (dirNodes/linkNodes/fileNodes), registers open-file sessions for
/// file-like nodes, charges size and inode quota, increments the global node count, and updates
/// the node checksum. Inverse of detachLoadedNodeAccounting().
///
/// @return kOpSuccess on success, kOpFailure on an unrecognized node type.
int8_t attachLoadedNodeAccounting(const FilesystemOperationContext &fsOpContext, FSNode *node) {
#ifndef METARESTORE
	auto *nodeFile = static_cast<FSNodeFile *>(node);
#endif

	switch (node->type) {
	case FSNodeType::kDirectory:
		gMetadata->dirNodes++;
		break;
	case FSNodeType::kSocket:
	case FSNodeType::kFifo:
	case FSNodeType::kBlockDev:
	case FSNodeType::kCharDev:
		// Nothing extra to do
		break;
	case FSNodeType::kSymlink:
		gMetadata->linkNodes++;
		break;
	case FSNodeType::kFile:
	case FSNodeType::kTrash:
	case FSNodeType::kReserved:
#ifndef METARESTORE
		for (const auto &sessionId : nodeFile->sessionIds) {
			matoclserv_add_open_file(sessionId, node->id);
		}
#endif
		fsnodes_quota_update(
		    node,
		    {{QuotaResource::kSize, +gFSOperations->nodeOperations()->getSize(fsOpContext, node)}});
		gMetadata->fileNodes++;
		break;
	default:
		safs::log_err("Attaching loaded node: unrecognized node type: {}",
		              static_cast<char>(node->type));
		fsnodes_quota_update(node, {{QuotaResource::kInodes, +1}});
		return kOpFailure;
	}

	gMetadata->nodes++;
	fsnodes_quota_update(node, {{QuotaResource::kInodes, +1}});

	fsnodes_update_checksum(node);
	return kOpSuccess;
}

/// Reverses node-local accounting for a node being removed from the live image.
///
/// Removes the node checksum from both the background updater and the metadata checksum,
/// decrements per-type counters, deregisters open-file sessions for file-like nodes, refunds
/// size and inode quota, and decrements the global node count. Inverse of
/// attachLoadedNodeAccounting().
void detachLoadedNodeAccounting(const FilesystemOperationContext &fsOpContext, FSNode *node) {
#ifndef METARESTORE
	auto *nodeFile = static_cast<FSNodeFile *>(node);
#endif

	// Remove node checksum from background updater checksum
	if (gChecksumBackgroundUpdater.isNodeIncluded(node)) {
		removeFromChecksum(gChecksumBackgroundUpdater.fsNodesChecksum, node->checksum);
	}

	// Remove node checksum from metadata checksum
	removeFromChecksum(gMetadata->fsNodesChecksum, node->checksum);

	switch (node->type) {
	case FSNodeType::kDirectory:
		gMetadata->dirNodes--;
		break;
	case FSNodeType::kSocket:
	case FSNodeType::kFifo:
	case FSNodeType::kBlockDev:
	case FSNodeType::kCharDev:
		// Nothing extra to do
		break;
	case FSNodeType::kSymlink:
		gMetadata->linkNodes--;
		break;
	case FSNodeType::kFile:
	case FSNodeType::kTrash:
	case FSNodeType::kReserved:
#ifndef METARESTORE
		for (const auto &sessionId : nodeFile->sessionIds) {
			matoclserv_remove_open_file(sessionId, node->id);
		}
#endif
		fsnodes_quota_update(
		    node,
		    {{QuotaResource::kSize, -gFSOperations->nodeOperations()->getSize(fsOpContext, node)}});
		gMetadata->fileNodes--;
		break;
	default:
		safs::log_err("{}: unrecognized node type: {}", __func__, static_cast<char>(node->type));
		fsnodes_quota_update(node, {{QuotaResource::kInodes, -1}});
		return;
	}

	gMetadata->nodes--;
	fsnodes_quota_update(node, {{QuotaResource::kInodes, -1}});
}

/// Inserts a node into the global node hash table.
void insertNodeIntoHash(FSNode *node) { gMetadata->addNode(node, true); }

/// Removes a node from its node-hash bucket via swap-and-pop.
/// @return true if the node was found and removed, false otherwise.
bool removeNodeFromHash(FSNode *node) {
	uint32_t nodeHashIndex = NODEHASHPOS(node->id);
	auto &bucket = gMetadata->nodeHash[nodeHashIndex];
	auto foundNodeIterator = std::ranges::find(bucket, node);
	if (foundNodeIterator != bucket.end()) {
		auto lastElement = bucket.end() - 1;
		std::iter_swap(foundNodeIterator, lastElement);
		bucket.pop_back();
		return true;
	}
	return false;
}
}  // namespace

namespace metadata::nodes {
int8_t insertLoadedNode(const FilesystemOperationContext &fsOpContext, FSNode *node) {
	int8_t status = attachLoadedNodeAccounting(fsOpContext, node);
	if (status != kOpSuccess) { return status; }

	insertNodeIntoHash(node);
	gMetadata->inodePool.markAsAcquired(node->id);
	return kOpSuccess;
}

int8_t restoreLoadedNode(const FilesystemOperationContext &fsOpContext, FSNode *restoredNode) {
	FSNode *currentNode = gFSOperations->nodeOperations()->idToNode(fsOpContext, restoredNode->id);

	if (currentNode == nullptr) { return insertLoadedNode(fsOpContext, restoredNode); }

	if (!areCompatibleForRestore(currentNode->type, restoredNode->type)) {
		safs::log_err("{}: incompatible type replacement for inode {}", __func__, restoredNode->id);
		FSNode::destroy(restoredNode);
		return kOpFailure;
	}

	auto stateStatus = restoreParentsAndSectionLocalState(currentNode, restoredNode);

	 if (stateStatus != kOpSuccess) {
        FSNode::destroy(restoredNode);
        return stateStatus;
    }

	detachLoadedNodeAccounting(fsOpContext, currentNode);
	removeNodeFromHash(currentNode);
	FSNode::destroy(currentNode);

	auto status = attachLoadedNodeAccounting(fsOpContext, restoredNode);
	if (status != kOpSuccess) {
		FSNode::destroy(restoredNode);
		return status;
	}

	insertNodeIntoHash(restoredNode);
	gMetadata->inodePool.markAsAcquired(restoredNode->id);

	return kOpSuccess;
}

int8_t removeLoadedNode(const FilesystemOperationContext &fsOpContext, inode_t nodeId) {
	if (nodeId == SPECIAL_INODE_ROOT) {
		safs::log_err("{}: refusing to remove root inode", __func__);
		return kOpFailure;
	}

	FSNode *node = gFSOperations->nodeOperations()->idToNode(fsOpContext, nodeId);
	if (node == nullptr) { return kOpSuccess; }

	if (!node->parents.empty()) {
		safs::log_err("{}: inode {} still has parent links during section-local removal", __func__,
		              nodeId);
		return kOpFailure;
	}

	if (node->type == FSNodeType::kDirectory) {
		auto *directory = static_cast<FSNodeDirectory *>(node);
		if (directory != nullptr && !directory->entries.empty()) {
			safs::log_err("{}: directory inode {} still has entries during section-local removal",
			              __func__, nodeId);
			return kOpFailure;
		}
	}

	detachLoadedNodeAccounting(fsOpContext, node);
	removeNodeFromHash(node);
	gMetadata->inodePool.release(nodeId, node->ctime, true);
	FSNode::destroy(node);

	return kOpSuccess;
}
}  // namespace metadata::nodes
