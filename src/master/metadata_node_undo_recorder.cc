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

#include "master/metadata_node_undo_recorder.h"

#include <cstdint>
#include <cstring>
#include <ranges>

#include "kv/kv_utils.h"
#include "master/kv_common_keys.h"
#include "master/metadata_backend_interface.h"
#include "master/metadata_checkpoint_helpers.h"
#include "master/metadata_node_restore_helpers.h"
#include "slogger/slogger.h"

namespace {

bool startsWith(const kv::Key &key, std::string_view prefix) {
	return key.size() >= prefix.size() &&
	       std::memcmp(key.data(), prefix.data(), prefix.size()) == 0;
}

kv::Key nodeUndoPrefix(uint64_t checkpointVersion) {
	return kv::encodeKeyBE(kNodeUndoKeyPrefix, checkpointVersion);
}

kv::Key nodeUndoKey(uint64_t checkpointVersion, inode_t inode) {
	return kv::encodeKeyBE(kNodeUndoKeyPrefix, checkpointVersion, inode);
}

inode_t decodeNodeUndoKey(const kv::Key &key) {
	// Key format: NODEU_ + <checkpoint:u64> + <nodeId:inode_t>
	if (!startsWith(key, kNodeUndoKeyPrefix) ||
	    key.size() != kNodeUndoKeyPrefix.size() + sizeof(uint64_t) + sizeof(inode_t)) {
		return 0U;  // Invalid key format
	}

	// Skip prefix and checkpointVersion to get nodeId
	const uint8_t *ptr = key.data() + kNodeUndoKeyPrefix.size() + sizeof(uint64_t);
	inode_t nodeId{};
	getINode(&ptr, nodeId);
	return nodeId;
}
}  // namespace

NodeUndoRecorder::NodeUndoRecorder(kv::IKVEngine *kvEngine) : kvEngine_(kvEngine) {}

void NodeUndoRecorder::beforeMutation(const MetadataMutationContext &context,
                                      const MetadataMutation &mutation) {
	if (context.transaction == nullptr || context.checkpointVersion == 0) { return; }

	const bool handled = std::visit(
	    [&](const auto &typedMutation) -> bool {
		    using T = std::decay_t<decltype(typedMutation)>;
		    if constexpr (std::is_same_v<T, NodeSetMutation>) {
			    beforeNodeSet(context, typedMutation);
			    return true;
		    } else if constexpr (std::is_same_v<T, NodeRemoveMutation>) {
			    beforeNodeRemove(context, typedMutation);
			    return true;
		    } else {
			    return false;
		    }
	    },
	    mutation);

	if (!handled) {
		safs::log_warn("{}: received non-node mutation for node recorder", __func__);
	}
}

bool NodeUndoRecorder::restoreToCheckpointVersion(uint64_t targetVersion) {
	// Fresh per-load record of inodes this rollback deletes (consumed by the forkless edge load).
	removedDuringRestore_.clear();

	auto retainedCheckpointVersions = checkpoints::loadCheckpointVersions(kvEngine_);
	if (retainedCheckpointVersions.empty()) {
		safs::log_info("No retained node checkpoints found");
		return true;
	}

	// sort checkpoint versions in ascending order
	std::ranges::sort(retainedCheckpointVersions);

	if (targetVersion > retainedCheckpointVersions.back()) {
		safs::log_warn("Target version {} is newer than retained latest {}", targetVersion,
		               retainedCheckpointVersions.back());
		return false;
	}

	if (targetVersion < retainedCheckpointVersions.front()) {
		safs::log_warn("Target version {} is older than retained earliest {}", targetVersion,
		               retainedCheckpointVersions.front());
		return false;
	}

	uint64_t restoredEntries = 0;
	// Iterate checkpoints in descending order and apply those for which checkpoint >= targetVersion
	for (const auto checkpointVersion : std::views::reverse(retainedCheckpointVersions)) {
		// Important: we must also apply rollback when checkpoint == targetVersion.
		// Please see ChunkUndoRecorder::restoreToCheckpointVersion() for rationale on this stopping condition.
		if (checkpointVersion < targetVersion) { break; }

		auto [entries, success] =
		    restoreSingleCheckpoint(FilesystemOperationContext{}, checkpointVersion);

		if (!success) {
			safs::log_err("{}: failed to restore node checkpoint version {}", __func__,
			              checkpointVersion);
			return false;
		}
		restoredEntries += entries;
	}

	safs::log_info("Restored {} node undo entries to version {}", restoredEntries, targetVersion);
	return true;
}

std::pair<uint64_t, bool> NodeUndoRecorder::restoreSingleCheckpoint(
    const FilesystemOperationContext &fsOpContext, uint64_t checkpointVersion) {
	uint64_t restoredEntries = 0;

	kv::Key startKey = nodeUndoPrefix(checkpointVersion);
	kv::Key endKey = kv::prefixEnd(startKey);
	kv::KeySelector startSelector(startKey, true, 0);
	kv::KeySelector endSelector(endKey, true, 0);

	while (true) {
		auto transaction = kvEngine_->createReadOnlyTransaction();
		auto page = transaction->getRange(startSelector, endSelector, kv::kDefaultGetRangeLimit);

		for (const auto &pair : page.getPairs()) {
			inode_t nodeId = decodeNodeUndoKey(pair.key);

			if (!applyNodeUndoEntry(fsOpContext, nodeId, pair.value)) { return {0, false}; }
			restoredEntries++;
		}

		if (!page.hasMore() || page.getPairs().empty()) { break; }

		startSelector = kv::KeySelector(page.getPairs().back().key, false, 0);
	}
	return {restoredEntries, true};
}

int8_t NodeUndoRecorder::dropCheckpointData(kv::IReadWriteTransaction *transaction,
                                            uint64_t droppedCheckpointVersion) {
	if (transaction == nullptr) { return kOpFailure; }

	kv::Key startKey = nodeUndoPrefix(droppedCheckpointVersion);
	transaction->removeRange(startKey, kv::prefixEnd(startKey));

	return kOpSuccess;
}

void NodeUndoRecorder::beforeNodeSet(const MetadataMutationContext &context,
                                     const NodeSetMutation &mutation) {
	if (context.checkpointVersion == 0) { return; }
	if (touchedNodeIds_.contains(mutation.inode)) { return; }

	// currentValue will be the value of NODE_<inode> key if it exists, or std::nullopt
	// if the key does not exist (i.e. node is being created)
	auto currentValue = context.transaction->get(mutation.liveKey);

	if (currentValue == std::nullopt) {
		recordNodeUndoRemove(context.transaction, context.checkpointVersion, mutation.inode);
		return;
	}

	recordNodeUndoSet(context.transaction, context.checkpointVersion, mutation.inode,
	                  currentValue.value());
}

void NodeUndoRecorder::beforeNodeRemove(const MetadataMutationContext &context,
                                        const NodeRemoveMutation &mutation) {
	if (context.checkpointVersion == 0) { return; }
	if (touchedNodeIds_.contains(mutation.inode)) { return; }

	// currentValue will be the value of NODE_<inode> key if it exists, or std::nullopt
	// if the key does not exist (i.e. node is being removed)
	auto currentValue = context.transaction->get(mutation.liveKey);

	if (currentValue == std::nullopt) { return; }

	recordNodeUndoSet(context.transaction, context.checkpointVersion, mutation.inode,
	                  currentValue.value());
}

void NodeUndoRecorder::recordNodeUndoSet(kv::IReadWriteTransaction *transaction,
                                         uint64_t checkpointVersion, inode_t inode,
                                         const kv::Value &serializedNode) {
	if (transaction == nullptr || checkpointVersion == 0) { return; }

	// Undo Key: NODEU_<checkpointVersion><nodeId>
	kv::Key undoKey = nodeUndoKey(checkpointVersion, inode);

	// Preserve the original pre-image if already recorded.
	if (transaction->get(undoKey).has_value()) {
		touchedNodeIds_.insert(inode);
		return;
	}

	transaction->set(undoKey, serializedNode);
	touchedNodeIds_.insert(inode);
}

void NodeUndoRecorder::recordNodeUndoRemove(kv::IReadWriteTransaction *transaction,
                                            uint64_t checkpointVersion, inode_t inode) {
	if (transaction == nullptr || checkpointVersion == 0) { return; }

	// Undo Key: NODEU_<checkpointVersion><nodeId>
	kv::Key undoKey = nodeUndoKey(checkpointVersion, inode);

	// Preserve the original pre-image if already recorded.
	if (transaction->get(undoKey).has_value()) {
		touchedNodeIds_.insert(inode);
		return;
	}

	// Tombstone: node did not exist before the first mutation in this checkpoint interval
	transaction->set(undoKey, kv::Value{});
	touchedNodeIds_.insert(inode);
}

bool NodeUndoRecorder::applyNodeUndoEntry(const FilesystemOperationContext &fsOpContext, inode_t nodeId,
	                        const kv::Value &undoValue) {
	if (undoValue.empty()) {
		if (metadata::nodes::removeLoadedNode(fsOpContext, nodeId) != kOpSuccess) { return false; }
		// Track the deletion so the forkless edge load can skip live edges that point at this
		// rolled-back inode instead of failing the EDGE section load.
		removedDuringRestore_.insert(nodeId);
		return true;
	}

	const uint8_t *source = undoValue.data();
	auto type = static_cast<FSNodeType>(source[0]);
	FSNode *node = FSNode::create(type);
	if (node == nullptr) {
		safs::log_err("{}: failed to create node for inode {}", __func__, nodeId);
		return false;
	}

	node->deserialize(&source);
	return metadata::nodes::restoreLoadedNode(fsOpContext, node) == kOpSuccess;
}
