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

#include "master/metadata_chunk_undo_recorder.h"

#include <cstdint>
#include <cstring>
#include <ranges>

#include "common/datapack.h"
#include "kv/kv_utils.h"
#include "master/chunks.h"
#include "master/kv_common_keys.h"
#include "master/metadata_backend_interface.h"
#include "master/metadata_checkpoint_helpers.h"
#include "slogger/slogger.h"

namespace {

bool startsWith(const kv::Key &key, std::string_view prefix) {
	return key.size() >= prefix.size() &&
	       std::memcmp(key.data(), prefix.data(), prefix.size()) == 0;
}

kv::Key chunkUndoPrefix(uint64_t checkpointVersion) {
	return kv::encodeKeyBE(kChunkUndoKeyPrefix, checkpointVersion);
}

kv::Key chunkUndoKey(uint64_t checkpointVersion, uint64_t chunkId) {
	return kv::encodeKeyBE(kChunkUndoKeyPrefix, checkpointVersion, chunkId);
}

bool decodeChunkUndoKey(const kv::Key &key, uint64_t &checkpointVersion, uint64_t &chunkId) {
	// Key format: CHNU_ + <checkpoint:u64> + <chunkId:u64>
	if (!startsWith(key, kChunkUndoKeyPrefix) ||
	    key.size() != kChunkUndoKeyPrefix.size() + sizeof(uint64_t) + sizeof(uint64_t)) {
		return false;
	}

	const uint8_t *ptr = key.data() + kChunkUndoKeyPrefix.size();
	checkpointVersion = get64bit(&ptr);
	chunkId = get64bit(&ptr);
	return true;
}

}  // namespace

ChunkUndoRecorder::ChunkUndoRecorder(kv::IKVEngine *kvEngine) : kvEngine_(kvEngine) {}

void ChunkUndoRecorder::beforeMutation(const MetadataMutationContext &context,
                                       const MetadataMutation &mutation) {
	if (context.transaction == nullptr || context.checkpointVersion == 0) { return; }

	if (const auto *chunkSet = std::get_if<ChunkSetMutation>(&mutation)) {
		beforeChunkSet(context, *chunkSet);
		return;
	}

	safs::log_warn("{}: received non-chunk mutation for chunk recorder", __func__);
}

bool ChunkUndoRecorder::restoreToCheckpointVersion(uint64_t targetVersion) {
	auto retainedCheckpointVersions = checkpoints::loadCheckpointVersions(kvEngine_);
	if (retainedCheckpointVersions.empty()) {
		safs::log_info("No retained chunk checkpoints found");
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
		// Why:
		// - Chunk updates written to FDB are tagged with `activeCheckpointVersion_`.
		// - `activeCheckpointVersion_` is the *last committed* metadata dump version. It advances
		//    only after the checkpoint is successfully sealed.
		// - Therefore, all chunk modifications that happen *after* checkpoint version V is sealed
		// and
		//   *before* the next checkpoint is sealed are recorded under checkpointVersion == V.
		// - The corresponding undo record `CHNU_<V><chunkId>` stores the "before" image for the
		//   first change of that chunk within this interval.
		//
		// During shadow sync we load a metadata snapshot at version T from FDB, but
		// the forkless backend (CHNL_) contains the latest chunk state, which may include changes
		// that happened after T. Those post-T changes are still tagged with checkpointVersion == T
		// (until the next checkpoint is sealed). To restore chunk state "as-of snapshot T",
		// we must undo the interval tagged with T as well.
		//
		// Hence the stopping condition is: stop once checkpoint < targetVersion.
		if (checkpointVersion < targetVersion) { break; }

		auto [entries, success] =
		    restoreSingleCheckpoint(FilesystemOperationContext{}, checkpointVersion);
		restoredEntries += entries;
	}

	safs::log_info("Restored {} chunk undo entries to version {}", restoredEntries, targetVersion);
	return true;
}

std::pair<uint64_t, bool> ChunkUndoRecorder::restoreSingleCheckpoint(
    const FilesystemOperationContext & /*fsOpContext*/, uint64_t checkpointVersion) {
	kv::Key prefix = chunkUndoPrefix(checkpointVersion);
	kv::KeySelector startSelector(prefix, true, 0);
	kv::KeySelector endSelector(kv::prefixEnd(prefix), true, 0);
	uint64_t restoredEntries = 0;

	while (true) {
		auto transaction = kvEngine_->createReadOnlyTransaction();
		auto page = transaction->getRange(startSelector, endSelector, kv::kDefaultGetRangeLimit);

		for (const auto &pair : page.getPairs()) {
			uint64_t undoCheckpointVersion = 0;
			uint64_t chunkId = 0;
			if (!decodeChunkUndoKey(pair.key, undoCheckpointVersion, chunkId)) { continue; }

			if (pair.value.empty()) {
				chunk_restore_remove(chunkId);
			} else {
				const uint8_t *ptr = pair.value.data();
				uint32_t chunkVersion = 0;
				uint32_t lockedTo = 0;
				uint32_t lockId = 0;

				get32bit(&ptr, chunkVersion);
				get32bit(&ptr, lockedTo);
				get32bit(&ptr, lockId);

				chunk_restore_set(chunkId, chunkVersion, lockedTo, lockId);
			}

			++restoredEntries;
		}

		if (!page.hasMore() || page.getPairs().empty()) { break; }

		startSelector = kv::KeySelector(page.getPairs().back().key, false, 0);
	}
	return {restoredEntries, true};
}

int8_t ChunkUndoRecorder::dropCheckpointData(kv::IReadWriteTransaction *transaction,
                                             uint64_t droppedCheckpointVersion) {
	if (transaction == nullptr) { return kOpFailure; }

	kv::Key startKey = chunkUndoPrefix(droppedCheckpointVersion);
	transaction->removeRange(startKey, kv::prefixEnd(startKey));

	return kOpSuccess;
}

void ChunkUndoRecorder::beforeChunkSet(const MetadataMutationContext &context,
                                       const ChunkSetMutation &mutation) {
	if (context.checkpointVersion == 0) { return; }
	if (touchedChunkIds_.contains(mutation.chunkId)) { return; }

	recordChunkUndo(context.transaction, context.checkpointVersion, mutation.chunkId,
	                mutation.liveKey);

	touchedChunkIds_.insert(mutation.chunkId);
}

void ChunkUndoRecorder::recordChunkUndo(kv::IReadWriteTransaction *transaction,
                                        uint64_t checkpointVersion, uint64_t chunkId,
                                        const kv::Key &liveKey) {
	if (transaction == nullptr || checkpointVersion == 0) { return; }

	// Undo Key: CHNU_<checkpointVersion><chunkId>
	kv::Key undoKey = chunkUndoKey(checkpointVersion, chunkId);

	// If the undo row already exists, preserve the original pre-image.
	if (transaction->get(undoKey).has_value()) { return; }

	auto currentValue = transaction->get(liveKey);
	if (currentValue.has_value()) {
		transaction->set(undoKey, *currentValue);
	} else {
		// Tombstone: chunk did not exist before the first mutation in this checkpoint interval
		transaction->set(undoKey, kv::Value{});
	}
}
