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

#include "master/metadata_checkpoint_manager.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <type_traits>
#include <variant>

#include "common/datapack.h"
#include "common/serialization.h"
#include "kv/itransaction.h"
#include "kv/kv_utils.h"
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
	if (!checkpointVersionsLoaded_) { loadCheckpointVersions(); }

	auto transaction = kvEngine_->createReadWriteTransaction();

	if (!persistCheckpointDescriptor(transaction.get(), descriptor)) {
		safs::log_err(
		    "Failed to persist metadata checkpoint descriptor version {}: transaction error",
		    descriptor.metadataVersion);
		return false;
	}

	saveCheckpointVersions(transaction.get(), descriptor);
	removeDroppedCheckpointVersions(transaction.get());

	if (!transaction->commit()) {
		safs::log_err("Failed to seal checkpoint version {}: transaction commit failed",
		              descriptor.metadataVersion);
		return false;
	}

	resetIntervalState();
	pendingCheckpoint_.reset();
	activeCheckpointVersion_ = descriptor.metadataVersion;

	safs::log_info("Checkpoint version {} sealed successfully", descriptor.metadataVersion);
	return true;
}

MetadataCheckpointDescriptor MetadataCheckpointManager::loadLatestCheckpoint() {
	MetadataCheckpointDescriptor descriptor;
	auto transaction = kvEngine_->createReadOnlyTransaction();

	auto maxInodeValue = transaction->get(kv::toBytes(kMetaMaxInodeIdKey));
	if (maxInodeValue.has_value()) {
		const uint8_t *data = maxInodeValue->data();
		getINode(&data, descriptor.maxInodeId);
	}

	auto versionValue = transaction->get(kv::toBytes(kMetaVersionKey));
	if (versionValue.has_value()) {
		const uint8_t *data = versionValue->data();
		descriptor.metadataVersion = get64bit(&data);
	}

	auto nextSessionValue = transaction->get(kv::toBytes(kMetaNextSessionKey));
	if (nextSessionValue.has_value()) {
		const uint8_t *data = nextSessionValue->data();
		get32bit(&data, descriptor.nextSessionId);
	}

	auto nextChunkValue = transaction->get(kv::toBytes(kMetaNextChunkIdKey));
	if (nextChunkValue.has_value()) {
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
		    } else {
			    return MetadataSectionKind::XAttr;
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

int8_t MetadataCheckpointManager::saveCheckpointVersions(
    kv::IReadWriteTransaction *transaction, const MetadataCheckpointDescriptor &descriptor) {
	if (transaction == nullptr) { return kOpFailure; }

	if (descriptor.metadataVersion == 0) {
		safs::log_warn("{}: Active checkpoint version is 0, cannot save checkpoint versions",
		               __func__);
		return kOpFailure;
	}

	std::set<uint64_t> checkpointVersionsSet(retainedCheckpointVersions_.begin(),
	                                         retainedCheckpointVersions_.end());

	// Add the new checkpoint version to the checkpoint versions set
	checkpointVersionsSet.insert(descriptor.metadataVersion);

	const size_t maxRetainedCheckpoints =
	    std::max<size_t>(1, static_cast<size_t>(gStoredPreviousBackMetaCopies) + 1);

	// Trim the set to keep only the last 'gStoredPreviousBackMetaCopies + 1' versions
	droppedCheckpointVersions.clear();
	while (checkpointVersionsSet.size() > maxRetainedCheckpoints) {
		droppedCheckpointVersions.push_back(*checkpointVersionsSet.begin());
		checkpointVersionsSet.erase(checkpointVersionsSet.begin());
	}

	// Update the retained checkpoint versions vector and persist it in FDB
	retainedCheckpointVersions_.assign(checkpointVersionsSet.begin(), checkpointVersionsSet.end());
	checkpoints::saveCheckpointVersions(transaction, retainedCheckpointVersions_);

	return kOpSuccess;
}

int8_t MetadataCheckpointManager::removeDroppedCheckpointVersions(
    kv::IReadWriteTransaction *transaction) {
	if (transaction == nullptr) { return kOpFailure; }

	// Delete dropped checkpoints from FDB using the recorders
	for (uint64_t droppedVersion : droppedCheckpointVersions) {
		for (auto *recorder : recorders_) {
			if (recorder != nullptr) {
				if (recorder->dropCheckpointData(transaction, droppedVersion) != kOpSuccess) {
					safs::log_warn("Failed to drop checkpoint version {} in section {}",
					               droppedVersion, sectionName(recorder->sectionKind()));
				}
			}
		}
	}

	droppedCheckpointVersions.clear();
	return kOpSuccess;
}
