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
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
*/

#include "master/chunk_operations_base.h"

#include "errors/sfserr.h"
#include "master/chunks.h"
#include "master/filesystem_operation_context.h"

// Default behavior forwards to the existing chunks.cc free functions, which hold
// the real in-memory logic. leil-master uses these as-is; ChunkOperationsKV (or similars)
// can now override the subset that must persist to a KV store.

int ChunkOperationsBase::addFile([[maybe_unused]] const FilesystemOperationContext &fsOpContext,
                                 uint64_t chunkid, uint8_t goal, bool isMetadataLoading) {
	return chunk_add_file(chunkid, goal, isMetadataLoading);
}

int ChunkOperationsBase::deleteFile([[maybe_unused]] const FilesystemOperationContext &fsOpContext,
                                    uint64_t chunkid, uint8_t goal) {
	return chunk_delete_file(chunkid, goal);
}

int ChunkOperationsBase::changeFile([[maybe_unused]] const FilesystemOperationContext &fsOpContext,
                                    uint64_t chunkid, uint8_t prevGoal, uint8_t newGoal) {
	return chunk_change_file(chunkid, prevGoal, newGoal);
}

int ChunkOperationsBase::increaseVersion(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, uint64_t chunkid) {
	return chunk_increase_version(chunkid);
}

int ChunkOperationsBase::setVersion([[maybe_unused]] const FilesystemOperationContext &fsOpContext,
                                    uint64_t chunkid, uint32_t version) {
	return chunk_set_version(chunkid, version);
}

void ChunkOperationsBase::persistRecord(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext,
    [[maybe_unused]] uint64_t chunkid) {}

int ChunkOperationsBase::unlock(uint64_t chunkid) { return chunk_unlock(chunkid); }

uint8_t ChunkOperationsBase::setNextChunkId(uint64_t nextChunkIdToBeSet) {
	return chunk_set_next_chunkid(nextChunkIdToBeSet);
}

uint8_t ChunkOperationsBase::applyModification(uint32_t ts, uint64_t oldChunkId, uint32_t lockid,
                                               uint8_t goal, bool doIncreaseVersion,
                                               uint64_t *newChunkId) {
	return chunk_apply_modification(ts, oldChunkId, lockid, goal, doIncreaseVersion, newChunkId);
}

#ifndef METARESTORE
uint8_t ChunkOperationsBase::multiModify(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, uint64_t ochunkid,
    uint32_t *lockid, uint8_t goal, bool quotaExceeded, uint8_t *opflag, uint64_t *nchunkid,
    uint32_t minServerVersion, uint64_t * /*grantGeneration*/, uint64_t * /*grantRandom*/) {
	// Legacy backends issue no write grants; the out-params stay at the caller's zeros.
	return chunk_multi_modify(ochunkid, lockid, goal, quotaExceeded, opflag, nchunkid,
	                          minServerVersion);
}

uint8_t ChunkOperationsBase::multiTruncate(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, uint64_t ochunkid,
    uint32_t lockid, uint32_t length, uint8_t goal, bool denyTruncatingParityParts,
    bool quotaExceeded, uint64_t *nchunkid) {
	return chunk_multi_truncate(ochunkid, lockid, length, goal, denyTruncatingParityParts,
	                            quotaExceeded, nchunkid);
}

int ChunkOperationsBase::canUnlock(uint64_t chunkid, uint32_t lockid) {
	return chunk_can_unlock(chunkid, lockid);
}

int ChunkOperationsBase::getVersionAndLocations(uint64_t chunkid, uint32_t currentIp,
                                                uint32_t &version, uint32_t maxNumberOfChunkCopies,
                                                std::vector<ChunkTypeWithAddress> &serversList) {
	return chunk_getversionandlocations(chunkid, currentIp, version, maxNumberOfChunkCopies,
	                                    serversList);
}

int ChunkOperationsBase::getVersionAndLocations(
    uint64_t chunkid, uint32_t currentIp, uint32_t &version, uint32_t maxNumberOfChunkCopies,
    std::vector<ChunkPartWithAddressAndLabel> &serversList) {
	return chunk_getversionandlocations(chunkid, currentIp, version, maxNumberOfChunkCopies,
	                                    serversList);
}

int ChunkOperationsBase::repair([[maybe_unused]] const FilesystemOperationContext &fsOpContext,
                                uint8_t goal, uint64_t ochunkid, uint32_t *nversion,
                                uint8_t correctOnly) {
	return chunk_repair(goal, ochunkid, nversion, correctOnly);
}

int ChunkOperationsBase::getFullCopies(uint64_t chunkid, uint8_t *vcopies) {
	return chunk_get_fullcopies(chunkid, vcopies);
}

int ChunkOperationsBase::getPartsToModify(uint64_t chunkid, int &recover, int &remove) {
	return chunk_get_partstomodify(chunkid, recover, remove);
}

bool ChunkOperationsBase::hasOnlyInvalidCopies(uint64_t chunkid) {
	return chunk_has_only_invalid_copies(chunkid);
}

void ChunkOperationsBase::serverHasChunk(matocsserventry *ptr, uint64_t chunkid,
                                         uint32_t versionWithTodelFlag, ChunkPartType chunkType) {
	chunk_server_has_chunk(ptr, chunkid, versionWithTodelFlag, chunkType);
}

void ChunkOperationsBase::serverHasChunks(matocsserventry *ptr,
                                          const std::vector<ChunkWithVersionAndType> &chunks) {
	for (const auto &chunk : chunks) { serverHasChunk(ptr, chunk.id, chunk.version, chunk.type); }
}

void ChunkOperationsBase::damaged(matocsserventry *ptr, uint64_t chunkid, ChunkPartType chunkType) {
	chunk_damaged(ptr, chunkid, chunkType);
}

void ChunkOperationsBase::lost(matocsserventry *ptr, uint64_t chunkid, ChunkPartType chunkType) {
	chunk_lost(ptr, chunkid, chunkType);
}

void ChunkOperationsBase::serverDisconnected(matocsserventry *ptr, const MediaLabel &label) {
	chunk_server_disconnected(ptr, label);
}

void ChunkOperationsBase::serverUnlabelledConnected() { chunk_server_unlabelled_connected(); }

void ChunkOperationsBase::serverLabelChanged(const MediaLabel &previousLabel,
                                             const MediaLabel &newLabel) {
	chunk_server_label_changed(previousLabel, newLabel);
}

void ChunkOperationsBase::gotDeleteStatus(matocsserventry *ptr, uint64_t chunkId,
                                          ChunkPartType chunkType, uint8_t status) {
	chunk_got_delete_status(ptr, chunkId, chunkType, status);
}

void ChunkOperationsBase::gotReplicateStatus(matocsserventry *ptr, uint64_t chunkId,
                                             uint32_t chunkVersion, ChunkPartType chunkType,
                                             uint8_t status) {
	chunk_got_replicate_status(ptr, chunkId, chunkVersion, chunkType, status);
}

void ChunkOperationsBase::gotCreateStatus(matocsserventry *ptr, uint64_t chunkid,
                                          ChunkPartType chunkType, uint8_t status) {
	chunk_got_create_status(ptr, chunkid, chunkType, status);
}

void ChunkOperationsBase::gotDuplicateStatus(matocsserventry *ptr, uint64_t chunkId,
                                             ChunkPartType chunkType, uint8_t status) {
	chunk_got_duplicate_status(ptr, chunkId, chunkType, status);
}

void ChunkOperationsBase::gotChunkLockStatus(matocsserventry *ptr, uint64_t chunkId,
                                             ChunkPartType chunkType, uint8_t status) {
	chunk_got_chunklock_status(ptr, chunkId, chunkType, status);
}

void ChunkOperationsBase::gotWriteEndStatus(matocsserventry *ptr, uint64_t chunkId,
                                            ChunkPartType chunkType, uint8_t status) {
	chunk_got_writeend_status(ptr, chunkId, chunkType, status);
}

void ChunkOperationsBase::gotSetVersionStatus(matocsserventry *ptr, uint64_t chunkId,
                                              ChunkPartType chunkType, uint8_t status) {
	chunk_got_setversion_status(ptr, chunkId, chunkType, status);
}

void ChunkOperationsBase::gotTruncateStatus(matocsserventry *ptr, uint64_t chunkId,
                                            ChunkPartType chunkType, uint8_t status) {
	chunk_got_truncate_status(ptr, chunkId, chunkType, status);
}

void ChunkOperationsBase::gotDuptruncStatus(matocsserventry *ptr, uint64_t chunkId,
                                            ChunkPartType chunkType, uint8_t status) {
	chunk_got_duptrunc_status(ptr, chunkId, chunkType, status);
}

void ChunkOperationsBase::stats(uint32_t *del, uint32_t *repl) { chunk_stats(del, repl); }

uint32_t ChunkOperationsBase::getChunkInfoSerializedSize() {
	return get_chunk_info_serialized_size();
}

void ChunkOperationsBase::storeChunkInfo(uint8_t *buff) { chunk_store_info(buff); }

uint32_t ChunkOperationsBase::getMissingCount() { return chunk_get_missing_count(); }

void ChunkOperationsBase::storeChunkCounters(uint8_t *buff, uint8_t matrixid) {
	chunk_store_chunkcounters(buff, matrixid);
}

uint32_t ChunkOperationsBase::count() { return chunk_count(); }

const ChunksReplicationState &ChunkOperationsBase::getReplicationState() {
	return chunk_get_replication_state();
}

const ChunksAvailabilityState &ChunkOperationsBase::getAvailabilityState() {
	return chunk_get_availability_state();
}

void ChunkOperationsBase::info(uint32_t *allChunks, uint32_t *allCopies, uint32_t *regCopies) {
	chunk_info(allChunks, allCopies, regCopies);
}

int ChunkOperationsBase::invalidateGoalCache() { return chunk_invalidate_goal_cache(); }
#endif  // METARESTORE

void ChunkOperationsBase::newfs() { chunk_newfs(); }

void ChunkOperationsBase::unload() { chunk_unload(); }

int ChunkOperationsBase::strinit() { return chunk_strinit(); }

uint64_t ChunkOperationsBase::checksum(ChecksumMode mode) { return chunk_checksum(mode); }

ChecksumRecalculationStatus ChunkOperationsBase::updateChecksumABit(uint32_t speedLimit) {
	return chunks_update_checksum_a_bit(speedLimit);
}
