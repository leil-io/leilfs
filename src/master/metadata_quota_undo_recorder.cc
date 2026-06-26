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

#include "master/metadata_quota_undo_recorder.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <ranges>
#include <string>

#include "common/datapack.h"
#include "common/quota_database.h"
#include "kv/kv_utils.h"
#include "master/filesystem_metadata.h"
#include "master/kv_common_keys.h"
#include "master/metadata_backend_interface.h"
#include "master/metadata_checkpoint_helpers.h"
#include "protocol/quota.h"
#include "slogger/slogger.h"

namespace {

constexpr uint8_t kQuotaTombstone = 0;
constexpr uint8_t kQuotaPresent = 1;
// Number of persisted limit slots per owner: rigor {soft, hard} x resource {inodes, size}.
constexpr size_t kQuotaLimitSlots = 4;

// Slot index in the recorded pre-image for a (rigor, resource) pair.
constexpr size_t quotaSlot(QuotaRigor rigor, QuotaResource resource) {
	return (static_cast<size_t>(rigor) * 2) + static_cast<size_t>(resource);
}

bool startsWith(const kv::Key &key, std::string_view prefix) {
	return key.size() >= prefix.size() &&
	       std::memcmp(key.data(), prefix.data(), prefix.size()) == 0;
}

kv::Key quotaUndoPrefix(uint64_t checkpointVersion) {
	return kv::encodeKeyBE(kQuotaUndoKeyPrefix, checkpointVersion);
}

kv::Key quotaUndoKey(uint64_t checkpointVersion, QuotaOwnerType ownerType, inode_t ownerId) {
	kv::Key key = kv::encodeKeyBE(kQuotaUndoKeyPrefix, checkpointVersion);
	key.push_back(static_cast<uint8_t>(ownerType));
	kv::Value idBytes = kv::toBytesBE(ownerId);
	key.insert(key.end(), idBytes.begin(), idBytes.end());
	return key;
}

// Undo key format: QUOTU_ + <checkpoint:u64> + <ownerType:u8> + <ownerId:inode_t>
bool decodeQuotaUndoKey(const kv::Key &key, QuotaOwnerType &ownerType, inode_t &ownerId) {
	const size_t fixedSize =
	    kQuotaUndoKeyPrefix.size() + sizeof(uint64_t) + sizeof(uint8_t) + sizeof(inode_t);
	if (!startsWith(key, kQuotaUndoKeyPrefix) || key.size() != fixedSize) { return false; }

	const uint8_t *ptr = key.data() + kQuotaUndoKeyPrefix.size() + sizeof(uint64_t);
	ownerType = static_cast<QuotaOwnerType>(*ptr);
	ptr++;
	getINode(&ptr, ownerId);
	return true;
}

}  // namespace

QuotaUndoRecorder::QuotaUndoRecorder(kv::IKVEngine *kvEngine) : kvEngine_(kvEngine) {}

void QuotaUndoRecorder::beforeMutation(const MetadataMutationContext &context,
                                       const MetadataMutation &mutation) {
	if (context.transaction == nullptr || context.checkpointVersion == 0) { return; }

	if (const auto *quotaSet = std::get_if<QuotaSetMutation>(&mutation)) {
		beforeOwnerMutation(context, quotaSet->ownerType, quotaSet->ownerId, quotaSet->rangeBegin,
		                    quotaSet->rangeEnd);
		return;
	}

	if (const auto *quotaRemove = std::get_if<QuotaRemoveMutation>(&mutation)) {
		beforeOwnerMutation(context, quotaRemove->ownerType, quotaRemove->ownerId,
		                    quotaRemove->rangeBegin, quotaRemove->rangeEnd);
		return;
	}

	safs::log_warn("{}: received non-quota mutation for quota recorder", __func__);
}

bool QuotaUndoRecorder::restoreToCheckpointVersion(uint64_t targetVersion) {
	auto retainedCheckpointVersions = checkpoints::loadCheckpointVersions(kvEngine_);
	if (retainedCheckpointVersions.empty()) {
		safs::log_info("No retained quota checkpoints found");
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
			safs::log_err("{}: failed to restore quota checkpoint version {}", __func__,
			              checkpointVersion);
			return false;
		}
		restoredEntries += entries;
	}

	// quotaDatabase.set()/remove() do not maintain the checksum; recompute it once after rollback
	// (the final metadata checksum recalculation in loadall would also cover this).
	gMetadata->quotaChecksum = gMetadata->quotaDatabase.checksum();

	safs::log_info("Restored {} quota undo entries to version {}", restoredEntries, targetVersion);
	return true;
}

std::pair<uint64_t, bool> QuotaUndoRecorder::restoreSingleCheckpoint(
    const FilesystemOperationContext & /*fsOpContext*/, uint64_t checkpointVersion) {
	kv::Key prefix = quotaUndoPrefix(checkpointVersion);
	kv::KeySelector startSelector(prefix, true, 0);
	kv::KeySelector endSelector(kv::prefixEnd(prefix), true, 0);
	uint64_t restoredEntries = 0;

	while (true) {
		auto transaction = kvEngine_->createReadOnlyTransaction();
		auto page = transaction->getRange(startSelector, endSelector, kv::kDefaultGetRangeLimit);

		for (const auto &pair : page.getPairs()) {
			QuotaOwnerType ownerType{};
			inode_t ownerId = 0;
			if (!decodeQuotaUndoKey(pair.key, ownerType, ownerId) || pair.value.empty()) { continue; }

			if (pair.value[0] == kQuotaTombstone) {
				// Owner had no limits at the checkpoint: remove it.
				gMetadata->quotaDatabase.remove(ownerType, ownerId);
			} else if (pair.value.size() >= 1 + (kQuotaLimitSlots * sizeof(uint64_t))) {
				const uint8_t *valuePtr = pair.value.data() + 1;
				std::array<uint64_t, kQuotaLimitSlots> limits{};
				for (auto &limit : limits) { limit = get64bit(&valuePtr); }

				for (const auto rigor : {QuotaRigor::kSoft, QuotaRigor::kHard}) {
					for (const auto resource : {QuotaResource::kInodes, QuotaResource::kSize}) {
						gMetadata->quotaDatabase.set(ownerType, ownerId, rigor, resource,
						                             limits[quotaSlot(rigor, resource)]);
					}
				}
				// Drop the owner if the restored limits are all zero, matching setquota semantics.
				gMetadata->quotaDatabase.removeEmpty(ownerType, ownerId);
			} else {
				safs::log_warn("{}: skipping malformed quota undo value of size {}", __func__,
				               pair.value.size());
				continue;
			}

			++restoredEntries;
		}

		if (!page.hasMore() || page.getPairs().empty()) { break; }

		startSelector = kv::KeySelector(page.getPairs().back().key, false, 0);
	}
	return {restoredEntries, true};
}

int8_t QuotaUndoRecorder::dropCheckpointData(kv::IReadWriteTransaction *transaction,
                                             uint64_t droppedCheckpointVersion) {
	if (transaction == nullptr) { return kOpFailure; }

	kv::Key startKey = quotaUndoPrefix(droppedCheckpointVersion);
	transaction->removeRange(startKey, kv::prefixEnd(startKey));

	return kOpSuccess;
}

void QuotaUndoRecorder::beforeOwnerMutation(const MetadataMutationContext &context,
                                            QuotaOwnerType ownerType, inode_t ownerId,
                                            const kv::Key &rangeBegin, const kv::Key &rangeEnd) {
	if (context.checkpointVersion == 0) { return; }

	// The owner prefix (rangeBegin) uniquely identifies the owner; use it as the first-touch dedup
	// key so only the interval-start pre-image is captured.
	std::string dedupKey(rangeBegin.begin(), rangeBegin.end());
	if (touchedOwners_.contains(dedupKey)) { return; }

	recordOwnerUndo(context.transaction, context.checkpointVersion, ownerType, ownerId, rangeBegin,
	                rangeEnd);

	touchedOwners_.insert(std::move(dedupKey));
}

void QuotaUndoRecorder::recordOwnerUndo(kv::IReadWriteTransaction *transaction,
                                        uint64_t checkpointVersion, QuotaOwnerType ownerType,
                                        inode_t ownerId, const kv::Key &rangeBegin,
                                        const kv::Key &rangeEnd) {
	if (transaction == nullptr || checkpointVersion == 0) { return; }

	// Undo Key: QUOTU_<checkpointVersion><ownerType><ownerId>
	kv::Key undoKey = quotaUndoKey(checkpointVersion, ownerType, ownerId);

	// If the undo row already exists, preserve the original pre-image.
	if (transaction->get(undoKey).has_value()) { return; }

	// Reconstruct the owner's current soft/hard limits from the live QUOT_ range.
	std::array<uint64_t, kQuotaLimitSlots> limits{};
	bool present = false;

	kv::KeySelector startSelector(rangeBegin, true, 0);
	kv::KeySelector endSelector(rangeEnd, true, 0);
	while (true) {
		auto page = transaction->getRange(startSelector, endSelector, kv::kDefaultGetRangeLimit);

		for (const auto &pair : page.getPairs()) {
			// Live key: QUOT_<ownerType><ownerId><rigor:u8><resource:u8>
			if (pair.key.size() < 2 || pair.value.size() < sizeof(uint64_t)) { continue; }
			const uint8_t rigor = pair.key[pair.key.size() - 2];
			const uint8_t resource = pair.key[pair.key.size() - 1];
			if (rigor > static_cast<uint8_t>(QuotaRigor::kHard) ||
			    resource > static_cast<uint8_t>(QuotaResource::kSize)) {
				continue;  // not a persisted soft/hard limit row
			}

			const uint8_t *valuePtr = pair.value.data();
			limits[(static_cast<size_t>(rigor) * 2) + resource] = get64bit(&valuePtr);
			present = true;
		}

		if (!page.hasMore() || page.getPairs().empty()) { break; }

		startSelector = kv::KeySelector(page.getPairs().back().key, false, 0);
	}

	kv::Value undoValue;
	if (present) {
		undoValue.reserve(1 + (kQuotaLimitSlots * sizeof(uint64_t)));
		undoValue.push_back(kQuotaPresent);
		for (const uint64_t limit : limits) {
			kv::Value limitBytes = kv::toBytesBE(limit);
			undoValue.insert(undoValue.end(), limitBytes.begin(), limitBytes.end());
		}
	} else {
		// Tombstone: the owner had no limits before the first mutation in this checkpoint interval.
		undoValue.push_back(kQuotaTombstone);
	}

	transaction->set(undoKey, undoValue);
}
