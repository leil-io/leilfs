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

#include "master/chunk_operations_in_memory.h"

#include "master/chunks.h"

// Initial extraction: ChunkOperationsInMemory forwards to the existing chunks.cc
// free functions, which hold the real in-memory logic. No behavior change.

int ChunkOperationsInMemory::addFile(uint64_t chunkid, uint8_t goal, bool isMetadataLoading) {
	return chunk_add_file(chunkid, goal, isMetadataLoading);
}

int ChunkOperationsInMemory::deleteFile(uint64_t chunkid, uint8_t goal) {
	return chunk_delete_file(chunkid, goal);
}

int ChunkOperationsInMemory::changeFile(uint64_t chunkid, uint8_t prevGoal, uint8_t newGoal) {
	return chunk_change_file(chunkid, prevGoal, newGoal);
}

int ChunkOperationsInMemory::increaseVersion(uint64_t chunkid) {
	return chunk_increase_version(chunkid);
}

int ChunkOperationsInMemory::setVersion(uint64_t chunkid, uint32_t version) {
	return chunk_set_version(chunkid, version);
}

int ChunkOperationsInMemory::unlock(uint64_t chunkid) { return chunk_unlock(chunkid); }

uint8_t ChunkOperationsInMemory::setNextChunkId(uint64_t nextChunkIdToBeSet) {
	return chunk_set_next_chunkid(nextChunkIdToBeSet);
}

uint8_t ChunkOperationsInMemory::applyModification(uint32_t ts, uint64_t oldChunkId,
                                                   uint32_t lockid, uint8_t goal,
                                                   bool doIncreaseVersion, uint64_t *newChunkId) {
	return chunk_apply_modification(ts, oldChunkId, lockid, goal, doIncreaseVersion, newChunkId);
}

#ifndef METARESTORE
uint8_t ChunkOperationsInMemory::multiModify(uint64_t ochunkid, uint32_t *lockid, uint8_t goal,
                                             bool quotaExceeded, uint8_t *opflag,
                                             uint64_t *nchunkid, uint32_t minServerVersion) {
	return chunk_multi_modify(ochunkid, lockid, goal, quotaExceeded, opflag, nchunkid,
	                          minServerVersion);
}

uint8_t ChunkOperationsInMemory::multiTruncate(uint64_t ochunkid, uint32_t lockid, uint32_t length,
                                               uint8_t goal, bool denyTruncatingParityParts,
                                               bool quotaExceeded, uint64_t *nchunkid) {
	return chunk_multi_truncate(ochunkid, lockid, length, goal, denyTruncatingParityParts,
	                            quotaExceeded, nchunkid);
}

int ChunkOperationsInMemory::canUnlock(uint64_t chunkid, uint32_t lockid) {
	return chunk_can_unlock(chunkid, lockid);
}

int ChunkOperationsInMemory::getVersionAndLocations(
    uint64_t chunkid, uint32_t currentIp, uint32_t &version, uint32_t maxNumberOfChunkCopies,
    std::vector<ChunkTypeWithAddress> &serversList) {
	return chunk_getversionandlocations(chunkid, currentIp, version, maxNumberOfChunkCopies,
	                                    serversList);
}

int ChunkOperationsInMemory::getVersionAndLocations(
    uint64_t chunkid, uint32_t currentIp, uint32_t &version, uint32_t maxNumberOfChunkCopies,
    std::vector<ChunkPartWithAddressAndLabel> &serversList) {
	return chunk_getversionandlocations(chunkid, currentIp, version, maxNumberOfChunkCopies,
	                                    serversList);
}

int ChunkOperationsInMemory::repair(uint8_t goal, uint64_t ochunkid, uint32_t *nversion,
                                    uint8_t correctOnly) {
	return chunk_repair(goal, ochunkid, nversion, correctOnly);
}

int ChunkOperationsInMemory::getFullCopies(uint64_t chunkid, uint8_t *vcopies) {
	return chunk_get_fullcopies(chunkid, vcopies);
}

int ChunkOperationsInMemory::getPartsToModify(uint64_t chunkid, int &recover, int &remove) {
	return chunk_get_partstomodify(chunkid, recover, remove);
}

bool ChunkOperationsInMemory::hasOnlyInvalidCopies(uint64_t chunkid) {
	return chunk_has_only_invalid_copies(chunkid);
}

void ChunkOperationsInMemory::serverHasChunk(matocsserventry *ptr, uint64_t chunkid,
                                             uint32_t versionWithTodelFlag,
                                             ChunkPartType chunkType) {
	chunk_server_has_chunk(ptr, chunkid, versionWithTodelFlag, chunkType);
}

void ChunkOperationsInMemory::damaged(matocsserventry *ptr, uint64_t chunkid,
                                      ChunkPartType chunkType) {
	chunk_damaged(ptr, chunkid, chunkType);
}

void ChunkOperationsInMemory::lost(matocsserventry *ptr, uint64_t chunkid,
                                   ChunkPartType chunkType) {
	chunk_lost(ptr, chunkid, chunkType);
}

void ChunkOperationsInMemory::serverDisconnected(matocsserventry *ptr, const MediaLabel &label) {
	chunk_server_disconnected(ptr, label);
}

void ChunkOperationsInMemory::serverUnlabelledConnected() { chunk_server_unlabelled_connected(); }

void ChunkOperationsInMemory::serverLabelChanged(const MediaLabel &previousLabel,
                                                 const MediaLabel &newLabel) {
	chunk_server_label_changed(previousLabel, newLabel);
}

void ChunkOperationsInMemory::gotDeleteStatus(matocsserventry *ptr, uint64_t chunkId,
                                              ChunkPartType chunkType, uint8_t status) {
	chunk_got_delete_status(ptr, chunkId, chunkType, status);
}

void ChunkOperationsInMemory::gotReplicateStatus(matocsserventry *ptr, uint64_t chunkId,
                                                 uint32_t chunkVersion, ChunkPartType chunkType,
                                                 uint8_t status) {
	chunk_got_replicate_status(ptr, chunkId, chunkVersion, chunkType, status);
}

void ChunkOperationsInMemory::gotCreateStatus(matocsserventry *ptr, uint64_t chunkid,
                                              ChunkPartType chunkType, uint8_t status) {
	chunk_got_create_status(ptr, chunkid, chunkType, status);
}

void ChunkOperationsInMemory::gotDuplicateStatus(matocsserventry *ptr, uint64_t chunkId,
                                                 ChunkPartType chunkType, uint8_t status) {
	chunk_got_duplicate_status(ptr, chunkId, chunkType, status);
}

void ChunkOperationsInMemory::gotChunkLockStatus(matocsserventry *ptr, uint64_t chunkId,
                                                 ChunkPartType chunkType, uint8_t status) {
	chunk_got_chunklock_status(ptr, chunkId, chunkType, status);
}

void ChunkOperationsInMemory::gotWriteEndStatus(matocsserventry *ptr, uint64_t chunkId,
                                                ChunkPartType chunkType, uint8_t status) {
	chunk_got_writeend_status(ptr, chunkId, chunkType, status);
}

void ChunkOperationsInMemory::gotSetVersionStatus(matocsserventry *ptr, uint64_t chunkId,
                                                  ChunkPartType chunkType, uint8_t status) {
	chunk_got_setversion_status(ptr, chunkId, chunkType, status);
}

void ChunkOperationsInMemory::gotTruncateStatus(matocsserventry *ptr, uint64_t chunkId,
                                                ChunkPartType chunkType, uint8_t status) {
	chunk_got_truncate_status(ptr, chunkId, chunkType, status);
}

void ChunkOperationsInMemory::gotDuptruncStatus(matocsserventry *ptr, uint64_t chunkId,
                                                ChunkPartType chunkType, uint8_t status) {
	chunk_got_duptrunc_status(ptr, chunkId, chunkType, status);
}

void ChunkOperationsInMemory::stats(uint32_t *del, uint32_t *repl) { chunk_stats(del, repl); }

uint32_t ChunkOperationsInMemory::getChunkInfoSerializedSize() {
	return get_chunk_info_serialized_size();
}

void ChunkOperationsInMemory::storeChunkInfo(uint8_t *buff) { chunk_store_info(buff); }

uint32_t ChunkOperationsInMemory::getMissingCount() { return chunk_get_missing_count(); }

void ChunkOperationsInMemory::storeChunkCounters(uint8_t *buff, uint8_t matrixid) {
	chunk_store_chunkcounters(buff, matrixid);
}

uint32_t ChunkOperationsInMemory::count() { return chunk_count(); }

const ChunksReplicationState &ChunkOperationsInMemory::getReplicationState() {
	return chunk_get_replication_state();
}

const ChunksAvailabilityState &ChunkOperationsInMemory::getAvailabilityState() {
	return chunk_get_availability_state();
}

void ChunkOperationsInMemory::info(uint32_t *allChunks, uint32_t *allCopies, uint32_t *regCopies) {
	chunk_info(allChunks, allCopies, regCopies);
}

int ChunkOperationsInMemory::invalidateGoalCache() { return chunk_invalidate_goal_cache(); }
#endif  // METARESTORE

void ChunkOperationsInMemory::newfs() { chunk_newfs(); }

void ChunkOperationsInMemory::unload() { chunk_unload(); }

int ChunkOperationsInMemory::strinit() { return chunk_strinit(); }

uint64_t ChunkOperationsInMemory::checksum(ChecksumMode mode) { return chunk_checksum(mode); }

ChecksumRecalculationStatus ChunkOperationsInMemory::updateChecksumABit(uint32_t speedLimit) {
	return chunks_update_checksum_a_bit(speedLimit);
}
