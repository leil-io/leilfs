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

#include "master/metadata_checkpoint_manager.h"

#include <cstring>
#include <set>
#include <string>
#include <type_traits>
#include <variant>

#include "common/datapack.h"
#include "common/serialization.h"
#include "kv/itransaction.h"
#include "kv/kv_utils.h"
#include "master/exceptions.h"
#include "master/kv_common_keys.h"
#include "master/metadata_backend_interface.h"
#include "master/metadata_checkpoint_helpers.h"
#include "master/metadata_section_undo_recorder.h"
#include "slogger/slogger.h"

MetadataCheckpointManager::MetadataCheckpointManager(kv::IKVEngine *kvEngine)
    : kvEngine_(kvEngine) {
	initializeRecorders();
}

bool MetadataCheckpointManager::beginCheckpoint(const MetadataCheckpointDescriptor &descriptor) {
	if (pendingCheckpoint_.has_value()) {
		safs::log_warn("Replacing pending metadata checkpoint at version {} with version {}",
		               pendingCheckpoint_->metadataVersion, descriptor.metadataVersion);
	}

	pendingCheckpoint_ = descriptor;
	return true;
}

bool MetadataCheckpointManager::sealCheckpoint(const MetadataCheckpointDescriptor &descriptor) {
	if (descriptor.metadataVersion == 0) {
		safs::log_warn("{}: cannot seal a checkpoint with version 0", __func__);
		return false;
	}

	if (!checkpointVersionsLoaded_) { loadCheckpointVersions(); }

	auto transaction = kvEngine_->createReadWriteTransaction();

	if (!persistCheckpointDescriptor(transaction.get(), descriptor)) {
		safs::log_err(
		    "Failed to persist metadata checkpoint descriptor version {}: transaction error",
		    descriptor.metadataVersion);
		return false;
	}

	// Compute the next catalog and the dropped versions on local copies so the manager's
	// in-memory state is mutated only after the transaction commits successfully.
	std::vector<uint64_t> nextCheckpointVersions;
	std::vector<uint64_t> droppedVersions;
	computeRetainedCheckpointVersions(descriptor.metadataVersion, nextCheckpointVersions,
	                                  droppedVersions);

	if (checkpoints::saveCheckpointVersions(transaction.get(), nextCheckpointVersions) !=
	    kOpSuccess) {
		safs::log_err("Failed to persist checkpoint catalog for version {}",
		              descriptor.metadataVersion);
		return false;
	}

	removeDroppedCheckpointVersions(transaction.get(), droppedVersions);

	if (!transaction->commit()) {
		safs::log_err("Failed to seal checkpoint version {}: transaction commit failed",
		              descriptor.metadataVersion);
		return false;
	}

	// Commit succeeded: now it is safe to update the in-memory state.
	retainedCheckpointVersions_ = std::move(nextCheckpointVersions);
	resetIntervalState();
	pendingCheckpoint_.reset();
	activeCheckpointVersion_ = descriptor.metadataVersion;

	safs::log_info("Checkpoint version {} sealed successfully", descriptor.metadataVersion);
	return true;
}

MetadataCheckpointDescriptor MetadataCheckpointManager::loadLatestCheckpoint() {
	MetadataCheckpointDescriptor descriptor;
	auto transaction = kvEngine_->createReadOnlyTransaction();

	// A present-but-undersized key means corrupted restore state; fail fast instead of
	// silently falling back to defaults (which could reuse inode/chunk/session ids).
	auto maxInodeValue = transaction->get(kv::toBytes(kMetaMaxInodeIdKey));
	if (maxInodeValue.has_value()) {
		if (maxInodeValue->size() < sizeof(inode_t)) {
			throw MetadataConsistencyException("Invalid size for META_MAX_INODE_ID key");
		}
		const uint8_t *data = maxInodeValue->data();
		getINode(&data, descriptor.maxInodeId);
	}

	auto versionValue = transaction->get(kv::toBytes(kMetaVersionKey));
	if (versionValue.has_value()) {
		if (versionValue->size() < sizeof(uint64_t)) {
			throw MetadataConsistencyException("Invalid size for META_VERSION key");
		}
		const uint8_t *data = versionValue->data();
		descriptor.metadataVersion = get64bit(&data);
	}

	auto nextSessionValue = transaction->get(kv::toBytes(kMetaNextSessionKey));
	if (nextSessionValue.has_value()) {
		if (nextSessionValue->size() < sizeof(uint32_t)) {
			throw MetadataConsistencyException("Invalid size for META_NEXT_SESSION key");
		}
		const uint8_t *data = nextSessionValue->data();
		get32bit(&data, descriptor.nextSessionId);
	}

	auto nextChunkValue = transaction->get(kv::toBytes(kMetaNextChunkIdKey));
	if (nextChunkValue.has_value()) {
		if (nextChunkValue->size() < sizeof(uint64_t)) {
			throw MetadataConsistencyException("Invalid size for META_NEXT_CHUNK_ID key");
		}
		const uint8_t *data = nextChunkValue->data();
		descriptor.nextChunkId = get64bit(&data);
	}

	retainedCheckpointVersions_ = checkpoints::loadCheckpointVersions(kvEngine_);
	activeCheckpointVersion_ = retainedCheckpointVersions_.empty() ? 0 : retainedCheckpointVersions_.back();

	resetIntervalState();
	pendingCheckpoint_.reset();
	checkpointVersionsLoaded_ = true;

	return descriptor;
}

void MetadataCheckpointManager::reloadDurableCheckpointState() {
	loadCheckpointVersions();
	resetIntervalState();
	pendingCheckpoint_.reset();
}

void MetadataCheckpointManager::recordPreMutation(const MetadataMutationContext &context,
	                       const MetadataMutation &mutation) {
	if (context.transaction == nullptr || context.checkpointVersion == 0) { return; }

	MetadataSectionKind section = std::visit(
	    [](const auto &typedMutation) -> MetadataSectionKind {
		    using T = std::decay_t<decltype(typedMutation)>;

		    if constexpr (std::is_same_v<T, ChunkSetMutation>) {
			    return MetadataSectionKind::Chunk;
		    } else if constexpr (std::is_same_v<T, NodeSetMutation> ||
		                         std::is_same_v<T, NodeRemoveMutation>) {
			    return MetadataSectionKind::Node;
		    } else if constexpr (std::is_same_v<T, FreeNodeSetMutation> ||
		                         std::is_same_v<T, FreeNodeRemoveMutation>) {
			    return MetadataSectionKind::FreeNode;
		    } else if constexpr (std::is_same_v<T, EdgeSetMutation> ||
		                         std::is_same_v<T, EdgeRemoveMutation>) {
			    return MetadataSectionKind::Edge;
		    } else if constexpr (std::is_same_v<T, XAttrSetMutation> ||
		                         std::is_same_v<T, XAttrRemoveMutation> ||
		                         std::is_same_v<T, XAttrRangeRemoveMutation>) {
			    return MetadataSectionKind::XAttr;
		    } else {
			    // Force a compile error if a new MetadataMutation alternative is added
			    // without being mapped to a section here.
			    static_assert(!sizeof(T *), "Unhandled mutation type in std::visit");
		    }
	    },
	    mutation);

	if (auto *recorder = recorderFor(section)) { recorder->beforeMutation(context, mutation); }
}

bool MetadataCheckpointManager::restoreSectionToCheckpointVersion(MetadataSectionKind section,
                                                                  uint64_t targetVersion) {
	auto section_ = sectionName(section);
	safs::log_info("Restoring section {} to checkpoint version {}", section_, targetVersion);

	if (targetVersion == 0 || section_ == "Unknown") {
		safs::log_info(
		    "Invalid target checkpoint version {} or section {}, skipping restore for section {}",
		    targetVersion, section_, section_);
		return false;
	}

	if (auto *recorder = recorderFor(section)) {
		return recorder->restoreToCheckpointVersion(targetVersion);
	}

	return false;
}

void MetadataCheckpointManager::initializeRecorders() {
	// Initialize recorders for each metadata section.
	// Each recorder is responsible for tracking mutations and restoring data for its respective
	// section.
}

ISectionUndoRecorder *MetadataCheckpointManager::recorderFor(MetadataSectionKind section) {
	for (auto *recorder : recorders_) {
		if (recorder != nullptr && recorder->sectionKind() == section) { return recorder; }
	}

	return nullptr;
}

void MetadataCheckpointManager::resetIntervalState() {
	for (auto *recorder : recorders_) {
		if (recorder != nullptr) { recorder->resetIntervalState(); }
	}
}

bool MetadataCheckpointManager::persistCheckpointDescriptor(
    kv::IReadWriteTransaction *transaction, const MetadataCheckpointDescriptor &descriptor) const {
	if (transaction == nullptr) { return false; }

	kv::Value maxInodeIdValue;
	serialize(maxInodeIdValue, descriptor.maxInodeId);
	transaction->set(kv::toBytes(kMetaMaxInodeIdKey), maxInodeIdValue);

	kv::Value metadataVersionValue;
	serialize(metadataVersionValue, descriptor.metadataVersion);
	transaction->set(kv::toBytes(kMetaVersionKey), metadataVersionValue);

	kv::Value nextSessionIdValue;
	serialize(nextSessionIdValue, descriptor.nextSessionId);
	transaction->set(kv::toBytes(kMetaNextSessionKey), nextSessionIdValue);

	kv::Value nextChunkIdValue;
	serialize(nextChunkIdValue, descriptor.nextChunkId);
	transaction->set(kv::toBytes(kMetaNextChunkIdKey), nextChunkIdValue);

	return true;
}

void MetadataCheckpointManager::loadCheckpointVersions() {
	retainedCheckpointVersions_ = checkpoints::loadCheckpointVersions(kvEngine_);

	std::string checkpoints;
	for (const auto &checkpoint : retainedCheckpointVersions_) {
		checkpoints += std::to_string(checkpoint) + " ";
	}
	safs::log_info("Loaded {} checkpoint versions from FDB: [ {}]",
	               retainedCheckpointVersions_.size(), checkpoints);

	activeCheckpointVersion_ =
	    retainedCheckpointVersions_.empty() ? 0 : retainedCheckpointVersions_.back();
	checkpointVersionsLoaded_ = true;
}

void MetadataCheckpointManager::computeRetainedCheckpointVersions(
    uint64_t newVersion, std::vector<uint64_t> &retained, std::vector<uint64_t> &dropped) const {
	std::set<uint64_t> checkpointVersionsSet(retainedCheckpointVersions_.begin(),
	                                         retainedCheckpointVersions_.end());

	// Add the new checkpoint version to the checkpoint versions set
	checkpointVersionsSet.insert(newVersion);

	// gStoredPreviousBackMetaCopies is uint32_t (never negative); the +1 keeps the active
	// checkpoint, and on 64-bit size_t the addition cannot overflow.
	const size_t maxRetainedCheckpoints = static_cast<size_t>(gStoredPreviousBackMetaCopies) + 1;

	// Trim the set to keep only the last 'gStoredPreviousBackMetaCopies + 1' versions
	dropped.clear();
	while (checkpointVersionsSet.size() > maxRetainedCheckpoints) {
		dropped.push_back(*checkpointVersionsSet.begin());
		checkpointVersionsSet.erase(checkpointVersionsSet.begin());
	}

	retained.assign(checkpointVersionsSet.begin(), checkpointVersionsSet.end());
}

int8_t MetadataCheckpointManager::removeDroppedCheckpointVersions(
    kv::IReadWriteTransaction *transaction, const std::vector<uint64_t> &droppedVersions) {
	if (transaction == nullptr) { return kOpFailure; }

	// Best-effort cleanup of per-checkpoint data for the dropped versions using the recorders.
	for (uint64_t droppedVersion : droppedVersions) {
		for (auto *recorder : recorders_) {
			if (recorder != nullptr) {
				if (recorder->dropCheckpointData(transaction, droppedVersion) != kOpSuccess) {
					safs::log_warn("Failed to drop checkpoint version {} in section {}",
					               droppedVersion, sectionName(recorder->sectionKind()));
				}
			}
		}
	}

	return kOpSuccess;
}
