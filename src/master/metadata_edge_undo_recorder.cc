/*
   Copyright 2026      Leil Storage OÜ

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

#include "common/platform.h"

#include "master/metadata_edge_undo_recorder.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

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

kv::Key detachedPathUndoKey(uint64_t checkpointVersion, inode_t inode) {
	return kv::encodeKeyBE(kEdgeUndoKeyPrefix, checkpointVersion, inode_t{0}, inode);
}

struct EdgeUndoEntry {
	inode_t parentId = 0;
	std::string name;
	std::optional<inode_t> childId;
};

struct DetachedPathUndoEntry {
	inode_t inode = 0;
	std::optional<std::pair<FSNodeType, HString>> preimage;
};

// Key format: EDGEU_ + <checkpoint:u64> + <parentId:inode_t> + <name>
bool decodeEdgeUndoKey(const kv::Key &key, inode_t &parentId, std::string &name) {
	const size_t fixedSize = kEdgeUndoKeyPrefix.size() + sizeof(uint64_t) + sizeof(inode_t);
	if (!startsWith(key, kEdgeUndoKeyPrefix) || key.size() < fixedSize) { return false; }

	const uint8_t *ptr = key.data() + kEdgeUndoKeyPrefix.size() + sizeof(uint64_t);
	getINode(&ptr, parentId);

	name.assign(reinterpret_cast<const char *>(key.data()) + fixedSize, key.size() - fixedSize);
	return true;
}

bool decodeDetachedPathUndoKey(const kv::Key &key, inode_t &inode) {
	const size_t expectedSize = kEdgeUndoKeyPrefix.size() + sizeof(uint64_t) + (sizeof(inode_t) * 2);
	if (key.size() != expectedSize) { return false; }

	const uint8_t *ptr = key.data() + kEdgeUndoKeyPrefix.size() + sizeof(uint64_t);
	inode_t parentId = 0;
	getINode(&ptr, parentId);
	if (parentId != 0) { return false; }
	getINode(&ptr, inode);
	return true;
}

kv::Value detachedPathUndoValue(FSNodeType nodeType, const kv::Value &path) {
	kv::Value value;
	value.reserve(path.size() + 1);
	value.push_back(static_cast<uint8_t>(nodeType));
	value.insert(value.end(), path.begin(), path.end());
	return value;
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

	if (const auto *detachedSet = std::get_if<DetachedPathSetMutation>(&mutation)) {
		beforeDetachedPathMutation(context, detachedSet->inode);
		return;
	}

	if (const auto *detachedRemove = std::get_if<DetachedPathRemoveMutation>(&mutation)) {
		beforeDetachedPathMutation(context, detachedRemove->inode);
		return;
	}

	safs::log_warn("{}: received non-edge mutation for edge recorder", __func__);
}

bool EdgeUndoRecorder::restoreToCheckpointVersion(uint64_t targetVersion) {
	detachedPathsTouchedDuringRestore_.clear();

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
	std::vector<EdgeUndoEntry> undoEntries;
	std::vector<DetachedPathUndoEntry> detachedUndoEntries;

	while (true) {
		auto transaction = kvEngine_->createReadOnlyTransaction();
		auto page = transaction->getRange(startSelector, endSelector, kv::kDefaultGetRangeLimit);

		for (const auto &pair : page.getPairs()) {
			inode_t parentId = 0;
			std::string name;
			if (!decodeEdgeUndoKey(pair.key, parentId, name)) { continue; }

			if (parentId == 0) {
				inode_t inode = 0;
				if (!decodeDetachedPathUndoKey(pair.key, inode)) {
					safs::log_err("{}: malformed detached-path undo key", __func__);
					return {0, false};
				}

				if (pair.value.empty()) {
					detachedUndoEntries.push_back({.inode = inode, .preimage = std::nullopt});
				} else {
					const auto nodeType = static_cast<FSNodeType>(pair.value.front());
					if ((nodeType != FSNodeType::kTrash && nodeType != FSNodeType::kReserved) ||
					    pair.value.size() == 1) {
						safs::log_err("{}: malformed detached-path undo value of size {}", __func__,
						              pair.value.size());
						return {0, false};
					}
					detachedUndoEntries.push_back(
					    {.inode = inode,
					     .preimage = std::pair{
					         nodeType,
					         HString(pair.value.begin() + 1, pair.value.end()),
					     }});
				}
				detachedPathsTouchedDuringRestore_.insert(inode);
				continue;
			}

			if (pair.value.empty()) {
				undoEntries.push_back(
				    {.parentId = parentId, .name = name, .childId = std::nullopt});
			} else {
				if (pair.value.size() != sizeof(inode_t)) {
					safs::log_err("{}: malformed edge undo value of size {}", __func__,
					              pair.value.size());
					return {0, false};
				}
				const uint8_t *ptr = pair.value.data();
				inode_t childId{};
				getINode(&ptr, childId);
				undoEntries.push_back({.parentId = parentId, .name = name, .childId = childId});
			}
		}

		if (!page.hasMore() || page.getPairs().empty()) { break; }

		startSelector = kv::KeySelector(page.getPairs().back().key, false, 0);
	}

	// Detach every affected live edge before attaching any checkpoint pre-image. Applying the
	// rows independently can attach one side of a directory hierarchy inversion while its live
	// inverse still exists, temporarily creating a parent cycle during recursive stats updates.
	for (const auto &entry : undoEntries) {
		if (metadata::edges::removeLoadedEdge(fsOpContext, entry.parentId, HString(entry.name)) !=
		    kOpSuccess) {
			return {0, false};
		}
	}
	for (const auto &entry : detachedUndoEntries) {
		if (metadata::edges::removeLoadedDetachedPath(fsOpContext, entry.inode) != kOpSuccess) {
			return {0, false};
		}
	}

	for (const auto &entry : undoEntries) {
		if (!entry.childId.has_value()) { continue; }
		if (metadata::edges::restoreLoadedEdge(fsOpContext, entry.parentId, *entry.childId,
		                                       HString(entry.name)) != kOpSuccess) {
			return {0, false};
		}
	}
	for (const auto &entry : detachedUndoEntries) {
		if (!entry.preimage.has_value()) { continue; }
		if (metadata::edges::restoreLoadedDetachedPath(fsOpContext, entry.inode,
		                                               entry.preimage->first,
		                                               entry.preimage->second) != kOpSuccess) {
			return {0, false};
		}
	}

	return {static_cast<uint64_t>(undoEntries.size() + detachedUndoEntries.size()), true};
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

	recordEdgeUndo(context.transaction, context.checkpointVersion, parentId, name, liveKey);
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

void EdgeUndoRecorder::beforeDetachedPathMutation(const MetadataMutationContext &context,
                                                  inode_t inode) {
	if (context.checkpointVersion == 0) { return; }

	recordDetachedPathUndo(context.transaction, context.checkpointVersion, inode);
}

void EdgeUndoRecorder::recordDetachedPathUndo(kv::IReadWriteTransaction *transaction,
                                              uint64_t checkpointVersion, inode_t inode) {
	if (transaction == nullptr || checkpointVersion == 0) { return; }

	// Undo Key: EDGEU_<checkpointVersion><0><inode>
	kv::Key undoKey = detachedPathUndoKey(checkpointVersion, inode);
	if (transaction->get(undoKey).has_value()) { return; }

	const auto trashPath = transaction->get(kv::encodeKeyBE(kTrashPathKeyPrefix, inode));
	const auto reservedPath = transaction->get(kv::encodeKeyBE(kReservedPathKeyPrefix, inode));
	if (trashPath.has_value() && reservedPath.has_value()) {
		safs::log_err("{}: inode {} has both trash and reserved path rows", __func__, inode);
		// Preserve evidence that the interval-start state violated the live-key invariant. A
		// malformed tagged value makes a later rollback fail closed instead of treating the
		// mutation as if no detached path had existed.
		transaction->set(undoKey, kv::Value{static_cast<uint8_t>(FSNodeType::kFile)});
		return;
	}

	if (trashPath.has_value()) {
		transaction->set(undoKey, detachedPathUndoValue(FSNodeType::kTrash, *trashPath));
	} else if (reservedPath.has_value()) {
		transaction->set(undoKey, detachedPathUndoValue(FSNodeType::kReserved, *reservedPath));
	} else {
		transaction->set(undoKey, kv::Value{});
	}
}
