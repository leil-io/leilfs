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

#include "master/metadata_edge_undo_recorder.h"

#include <cstdint>
#include <cstring>
#include <ranges>
#include <string>

#include "common/datapack.h"
#include "kv/kv_utils.h"
#include "master/kv_common_keys.h"
#include "master/metadata_backend_interface.h"
#include "master/metadata_checkpoint_helpers.h"
#include "master/metadata_edge_restore_helpers.h"
#include "slogger/slogger.h"

namespace {

bool startsWith(const kv::Key &key, std::string_view prefix) {
	return key.size() >= prefix.size() &&
	       std::memcmp(key.data(), prefix.data(), prefix.size()) == 0;
}

kv::Key edgeUndoPrefix(uint64_t checkpointVersion) {
	return kv::encodeKeyBE(kEdgeUndoKeyPrefix, checkpointVersion);
}

kv::Key edgeUndoKey(uint64_t checkpointVersion, inode_t parentId, const HString &name) {
	kv::Key key = kv::encodeKeyBE(kEdgeUndoKeyPrefix, checkpointVersion, parentId);
	kv::appendStr(key, name);
	return key;
}

// Key format: EDGEU_ + <checkpoint:u64> + <parentId:inode_t> + <name>
bool decodeEdgeUndoKey(const kv::Key &key, inode_t &parentId, std::string &name) {
	const size_t fixedSize = kEdgeUndoKeyPrefix.size() + sizeof(uint64_t) + sizeof(inode_t);
	if (!startsWith(key, kEdgeUndoKeyPrefix) || key.size() < fixedSize) { return false; }

	const uint8_t *ptr = key.data() + kEdgeUndoKeyPrefix.size() + sizeof(uint64_t);
	getINode(&ptr, parentId);

	name.assign(reinterpret_cast<const char *>(key.data()) + fixedSize, key.size() - fixedSize);
	return true;
}

}  // namespace

EdgeUndoRecorder::EdgeUndoRecorder(kv::IKVEngine *kvEngine) : kvEngine_(kvEngine) {}

void EdgeUndoRecorder::beforeMutation(const MetadataMutationContext &context,
                                      const MetadataMutation &mutation) {
	if (context.transaction == nullptr || context.checkpointVersion == 0) { return; }

	if (const auto *edgeSet = std::get_if<EdgeSetMutation>(&mutation)) {
		beforeEdgeMutation(context, edgeSet->parentId, edgeSet->name, edgeSet->liveKey);
		return;
	}

	if (const auto *edgeRemove = std::get_if<EdgeRemoveMutation>(&mutation)) {
		beforeEdgeMutation(context, edgeRemove->parentId, edgeRemove->name, edgeRemove->liveKey);
		return;
	}

	safs::log_warn("{}: received non-edge mutation for edge recorder", __func__);
}

bool EdgeUndoRecorder::restoreToCheckpointVersion(uint64_t targetVersion) {
	auto retainedCheckpointVersions = checkpoints::loadCheckpointVersions(kvEngine_);
	if (retainedCheckpointVersions.empty()) {
		safs::log_info("No retained edge checkpoints found");
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
	// Iterate checkpoints in descending order and apply those for which checkpoint >= targetVersion.
	// Please see ChunkUndoRecorder::restoreToCheckpointVersion() for rationale on this stopping
	// condition (the target interval itself must be undone).
	for (const auto checkpointVersion : std::views::reverse(retainedCheckpointVersions)) {
		if (checkpointVersion < targetVersion) { break; }

		auto [entries, success] =
		    restoreSingleCheckpoint(FilesystemOperationContext{}, checkpointVersion);

		if (!success) {
			safs::log_err("{}: failed to restore edge checkpoint version {}", __func__,
			              checkpointVersion);
			return false;
		}
		restoredEntries += entries;
	}

	safs::log_info("Restored {} edge undo entries to version {}", restoredEntries, targetVersion);
	return true;
}

std::pair<uint64_t, bool> EdgeUndoRecorder::restoreSingleCheckpoint(
    const FilesystemOperationContext &fsOpContext, uint64_t checkpointVersion) {
	kv::Key prefix = edgeUndoPrefix(checkpointVersion);
	kv::KeySelector startSelector(prefix, true, 0);
	kv::KeySelector endSelector(kv::prefixEnd(prefix), true, 0);
	uint64_t restoredEntries = 0;

	while (true) {
		auto transaction = kvEngine_->createReadOnlyTransaction();
		auto page = transaction->getRange(startSelector, endSelector, kv::kDefaultGetRangeLimit);

		for (const auto &pair : page.getPairs()) {
			inode_t parentId = 0;
			std::string name;
			if (!decodeEdgeUndoKey(pair.key, parentId, name)) { continue; }

			HString edgeName(name);
			int8_t status = kOpSuccess;
			if (pair.value.empty()) {
				// Tombstone: edge did not exist at the checkpoint, so remove it.
				status = metadata::edges::removeLoadedEdge(fsOpContext, parentId, edgeName);
			} else {
				const uint8_t *ptr = pair.value.data();
				inode_t childId{};
				getINode(&ptr, childId);
				status = metadata::edges::restoreLoadedEdge(fsOpContext, parentId, childId, edgeName);
			}

			if (status != kOpSuccess) { return {restoredEntries, false}; }
			++restoredEntries;
		}

		if (!page.hasMore() || page.getPairs().empty()) { break; }

		startSelector = kv::KeySelector(page.getPairs().back().key, false, 0);
	}
	return {restoredEntries, true};
}

int8_t EdgeUndoRecorder::dropCheckpointData(kv::IReadWriteTransaction *transaction,
                                            uint64_t droppedCheckpointVersion) {
	if (transaction == nullptr) { return kOpFailure; }

	kv::Key startKey = edgeUndoPrefix(droppedCheckpointVersion);
	transaction->removeRange(startKey, kv::prefixEnd(startKey));

	return kOpSuccess;
}

void EdgeUndoRecorder::beforeEdgeMutation(const MetadataMutationContext &context, inode_t parentId,
                                          const HString &name, const kv::Key &liveKey) {
	if (context.checkpointVersion == 0) { return; }

	// The live EDGE_ key uniquely identifies the edge (parentId, name); use it as the
	// first-touch dedup key so only the interval-start pre-image is captured.
	std::string dedupKey(liveKey.begin(), liveKey.end());
	if (touchedEdges_.contains(dedupKey)) { return; }

	recordEdgeUndo(context.transaction, context.checkpointVersion, parentId, name, liveKey);

	touchedEdges_.insert(std::move(dedupKey));
}

void EdgeUndoRecorder::recordEdgeUndo(kv::IReadWriteTransaction *transaction,
                                      uint64_t checkpointVersion, inode_t parentId,
                                      const HString &name, const kv::Key &liveKey) {
	if (transaction == nullptr || checkpointVersion == 0) { return; }

	// Undo Key: EDGEU_<checkpointVersion><parentId><name>
	kv::Key undoKey = edgeUndoKey(checkpointVersion, parentId, name);

	// If the undo row already exists, preserve the original pre-image.
	if (transaction->get(undoKey).has_value()) { return; }

	auto currentValue = transaction->get(liveKey);
	if (currentValue.has_value()) {
		transaction->set(undoKey, *currentValue);
	} else {
		// Tombstone: edge did not exist before the first mutation in this checkpoint interval.
		transaction->set(undoKey, kv::Value{});
	}
}
