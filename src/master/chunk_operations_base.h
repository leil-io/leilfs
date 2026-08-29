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

#pragma once

#include "common/platform.h"

#include "master/chunk_operations_interface.h"

/// Base for every chunk-operations backend.
///
/// Today Base forwards to the in-memory engine in chunks.{h,cc}; that engine
/// holds the real logic and owns gChunksMetadata. ChunkOperationsInMemory is an
/// empty leaf over Base, and ChunkOperationsKV overrides only the operations that
/// persist to a KV store.
///
/// Intended end-state: Base holds only the logic common to all backends, while
/// the in-memory-specific behavior moves down into ChunkOperationsInMemory (a
/// sibling of ChunkOperationsKV), so each subclass implements only its divergent
/// part.
class ChunkOperationsBase : public IChunkOperations {
public:
	int addFile(const FilesystemOperationContext &fsOpContext, uint64_t chunkid, uint8_t goal,
	            bool isMetadataLoading) override;
	int deleteFile(const FilesystemOperationContext &fsOpContext, uint64_t chunkid,
	               uint8_t goal) override;
	int changeFile(const FilesystemOperationContext &fsOpContext, uint64_t chunkid,
	               uint8_t prevGoal, uint8_t newGoal) override;
	bool defersFileReferenceMutations(
	    const FilesystemOperationContext & /*fsOpContext*/) const override {
		return false;
	}

	int increaseVersion(const FilesystemOperationContext &fsOpContext, uint64_t chunkid) override;
	int setVersion(const FilesystemOperationContext &fsOpContext, uint64_t chunkid,
	               uint32_t version) override;
	void persistRecord(const FilesystemOperationContext &fsOpContext, uint64_t chunkid) override;

	int unlock(uint64_t chunkid) override;

	uint8_t setNextChunkId(uint64_t nextChunkIdToBeSet) override;

	uint8_t applyModification(uint32_t ts, uint64_t oldChunkId, uint32_t lockid, uint8_t goal,
	                          bool doIncreaseVersion, uint64_t *newChunkId) override;

#ifndef METARESTORE
	uint8_t multiModify(const FilesystemOperationContext &fsOpContext, uint64_t ochunkid,
	                    uint32_t *lockid, uint8_t goal, bool quotaExceeded, uint8_t *opflag,
	                    uint64_t *nchunkid, uint32_t minServerVersion,
	                    uint64_t *grantGeneration = nullptr,
	                    uint64_t *grantRandom = nullptr) override;
	uint8_t multiTruncate(const FilesystemOperationContext &fsOpContext, uint64_t ochunkid,
	                      uint32_t lockid, uint32_t length, uint8_t goal,
	                      bool denyTruncatingParityParts, bool quotaExceeded,
	                      uint64_t *nchunkid) override;
	int canUnlock(uint64_t chunkid, uint32_t lockid) override;

	int getVersionAndLocations(uint64_t chunkid, uint32_t currentIp, uint32_t &version,
	                           uint32_t maxNumberOfChunkCopies,
	                           std::vector<ChunkTypeWithAddress> &serversList) override;
	int getVersionAndLocations(uint64_t chunkid, uint32_t currentIp, uint32_t &version,
	                           uint32_t maxNumberOfChunkCopies,
	                           std::vector<ChunkPartWithAddressAndLabel> &serversList) override;

	int repair(const FilesystemOperationContext &fsOpContext, uint8_t goal, uint64_t ochunkid,
	           uint32_t *nversion, uint8_t correctOnly) override;
	int getFullCopies(uint64_t chunkid, uint8_t *vcopies) override;
	int getPartsToModify(uint64_t chunkid, int &recover, int &remove) override;
	bool hasOnlyInvalidCopies(uint64_t chunkid) override;

	void serverHasChunk(matocsserventry *ptr, uint64_t chunkid, uint32_t versionWithTodelFlag,
	                    ChunkPartType chunkType) override;
	void serverHasChunks(matocsserventry *ptr,
	                     const std::vector<ChunkWithVersionAndType> &chunks) override;
	void damaged(matocsserventry *ptr, uint64_t chunkid, ChunkPartType chunkType) override;
	void lost(matocsserventry *ptr, uint64_t chunkid, ChunkPartType chunkType) override;
	void serverDisconnected(matocsserventry *ptr, const MediaLabel &label) override;
	void serverUnlabelledConnected() override;
	void serverLabelChanged(const MediaLabel &previousLabel, const MediaLabel &newLabel) override;

	void gotDeleteStatus(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType,
	                     uint8_t status) override;
	void gotReplicateStatus(matocsserventry *ptr, uint64_t chunkId, uint32_t chunkVersion,
	                        ChunkPartType chunkType, uint8_t status) override;
	void gotCreateStatus(matocsserventry *ptr, uint64_t chunkid, ChunkPartType chunkType,
	                     uint8_t status) override;
	void gotDuplicateStatus(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType,
	                        uint8_t status) override;
	void gotChunkLockStatus(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType,
	                        uint8_t status) override;
	void gotWriteEndStatus(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType,
	                       uint8_t status) override;
	void gotSetVersionStatus(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType,
	                         uint8_t status) override;
	void gotTruncateStatus(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType,
	                       uint8_t status) override;
	void gotDuptruncStatus(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType,
	                       uint8_t status) override;

	void stats(uint32_t *del, uint32_t *repl) override;
	uint32_t getChunkInfoSerializedSize() override;
	void storeChunkInfo(uint8_t *buff) override;
	uint32_t getMissingCount() override;
	void storeChunkCounters(uint8_t *buff, uint8_t matrixid) override;
	uint32_t count() override;
	const ChunksReplicationState &getReplicationState() override;
	const ChunksAvailabilityState &getAvailabilityState() override;
	void info(uint32_t *allChunks, uint32_t *allCopies, uint32_t *regCopies) override;
	int invalidateGoalCache() override;
#endif  // METARESTORE

	void newfs() override;
	void unload() override;
	int strinit() override;

	uint64_t checksum(ChecksumMode mode) override;
	ChecksumRecalculationStatus updateChecksumABit(uint32_t speedLimit) override;
};
