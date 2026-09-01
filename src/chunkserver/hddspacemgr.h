/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


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

#pragma once

#include "common/platform.h"

#include <atomic>
#include <vector>

#include "chunkserver-common/chunk_interface.h"
#include "chunkserver/io_buffers.h"
#include "common/chunk_part_type.h"
#include "common/chunk_with_version_and_type.h"
#include "protocol/chunks_with_type.h"

uint32_t hddGetAndResetErrorCounter();

void hddGetDamagedChunks(std::vector<ChunkWithType>& chunks, std::size_t limit);
void hddGetLostChunks(std::vector<ChunkWithType>& chunks, std::size_t limit);
void hddReportLostChunk(uint64_t chunkid, ChunkPartType chunk_type);
void hddGetNewChunks(std::vector<ChunkWithVersionAndType>& chunks,
                     std::size_t limit);

/* Both must be called with the disks mutex locked */
uint32_t hddGetSerializedSizeOfAllDiskInfosV2();
void hddSerializeAllDiskInfosV2(uint8_t *buff);

std::string hddGetDiskGroups();

// Controls the number of chunks processed per bulk operation when reporting
// chunks to the master (e.g. registration, damaged, lost, new chunks).
inline constexpr uint32_t kDefaultChunkBulkSize = 1'000;
inline constexpr uint32_t kMinChunkBulkSize = 1;
inline constexpr uint32_t kMaxChunkBulkSize = 100'000;
inline std::atomic_uint32_t gChunkBulkSize{kDefaultChunkBulkSize};

using BulkFunction = std::function<void(std::vector<ChunkWithVersionAndType>&)>;
/// Executes the callback for each bulk of at most \p bulkSize chunks.
void hddForeachChunkInBulks(BulkFunction bulkCallback, std::size_t bulkSize);

/// Master-driven (pull) chunk registration sweep.
/// Unlike hddForeachChunkInBulks, which enumerates everything in one call,
/// this sweep is resumable: the master releases bulks one credit at a time
/// (see SAU_MATOCS_REGISTER_CHUNKS_START), so enumeration must survive
/// concurrent registry mutation between calls.

/// Starts a new registration session over the chunk registry, invalidating the
/// marks of the previous one. Full registry registration makes previously
/// queued reports redundant; a pull sweep also covers new records until it
/// completes.
///
/// \p mode distinguishes the push path, which only retires old marks, from the
/// pull path, which suppresses redundant CHUNK_NEW reports while it sweeps.
enum class RegistrationSweepMode {
	kPush,
	kPull,
};
void hddRegistrationSweepBegin(RegistrationSweepMode mode);

/// Result of advancing a master-driven registration sweep.
enum class RegistrationSweepResult {
	kBulkReady,  ///< \p bulk contains records ready to send to the master.
	kRetry,      ///< Locked records or pending disk scans remain; retry without consuming a credit.
	kComplete,   ///< Every reportable record has been included in the sweep.
};

/// Collects at most \p bulkSize chunks not yet reported in
/// this sweep session and marks them as reported. Locked chunks are not
/// waited on: callers receive \ref RegistrationSweepResult::kRetry and must
/// try again later without consuming a master credit.
RegistrationSweepResult hddRegistrationSweepNext(std::vector<ChunkWithVersionAndType> &bulk,
                                                 std::size_t bulkSize);

/// Number of buckets currently backing the chunk registry. Reported alongside
/// the registration progress messages so a test can tell whether the registry
/// rehashed while the sweep was walking it: a rehash moves chunks across
/// buckets behind the sweep's cursor, and the termination pass is what has to
/// catch them. Carries no meaning for the sweep itself.
uint64_t hddGetChunkRegistryBucketCount();

int hddGetAndResetSpaceChanged();
void hddGetTotalSpace(uint64_t *usedSpace, uint64_t *totalSpace,
                      uint32_t *chunkCount, uint64_t *toDelUsedSpace,
                      uint64_t *toDelTotalSpace, uint32_t *toDelChunkCount);
int hddGetLoadFactor();

/* I/O operations */
int hddOpen(IChunk *chunk);
int hddOpen(uint64_t chunkId, ChunkPartType chunkType);
int hddClose(IChunk *chunk);
int hddClose(uint64_t chunkId, ChunkPartType chunkType);
int hddPrefetchBlocks(uint64_t chunkId, ChunkPartType chunkType,
                      uint32_t firstBlock, uint16_t numberOfBlocks);
int hddRead(uint64_t chunkId, uint32_t version, ChunkPartType chunkType,
            uint32_t offset, uint32_t size,
            [[maybe_unused]] uint32_t maxBlocksToBeReadBehind,
            [[maybe_unused]] uint32_t blocksToBeReadAhead,
            OutputBuffer *outputBuffer);
int hddChunkWriteBlock(uint64_t chunkId, uint32_t version,
                       ChunkPartType chunkType, uint16_t blocknum,
                       uint32_t offset, uint32_t size, uint32_t crc,
                       const uint8_t *buffer);
int hddChunkWriteFullBlocks(uint64_t chunkId, uint32_t version, ChunkPartType chunkType,
                            uint16_t startBlock, uint16_t numBlocks, std::vector<uint32_t> &crcList,
                            std::vector<uint16_t> &blocksPerBuffer,
                            std::vector<const uint8_t *> &buffers);

/* chunk info */
int hddChunkGetNumberOfBlocks(uint64_t chunkId, ChunkPartType chunkType,
                              uint32_t version, uint16_t *blocks);

/* chunk operations */
int hddTruncate(uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                uint32_t chunkNewVersion, uint32_t length);

int hddDuplicate(uint64_t chunkId, uint32_t chunkVersion, uint32_t chunkNewVersion,
                 ChunkPartType chunkType, uint64_t copyChunkId, uint32_t copyChunkVersion);

int hddDuplicateTruncate(uint64_t chunkId, uint32_t chunkVersion, uint32_t chunkNewVersion,
                         ChunkPartType chunkType, uint64_t copyChunkId, uint32_t copyChunkVersion,
                         uint32_t length);

/* chunk testing */
void hddAddChunkToTestQueue(ChunkWithVersionAndType chunk);

/* initialization */
int initDiskManager();
int loadPlugins();
int hddLateInit();
int hddInit();

// Chunk low-level operations
// The following functions shouldn't be used, unless for specific implementation
// i.e. \see ChunkFileCreator
// In most cases functions above are preferred.

/// Deletes the chunk from the registry and from the disk's testlist in a
/// thread-safe way.
void hddDeleteChunkFromRegistry(IChunk *chunk);

/** \brief Creates a new chunk on disk
 *
 * \param chunkid - id of created chunk
 * \param version - version of created chunk
 * \param chunkType - type of created chunk
 * \return On success returns pair of SAUNAFS_STATUS_OK and created chunk in
 *         locked state. On failure, returns pair of error code and nullptr.
 */
std::pair<int, IChunk *> hddInternalCreateChunk(uint64_t chunkId,
                                                uint32_t version,
                                                ChunkPartType chunkType);
int hddInternalCreate(uint64_t chunkId, uint32_t version,
                      ChunkPartType chunkType);
int hddInternalDelete(IChunk *chunk, uint32_t version);
int hddInternalDelete(uint64_t chunkId, uint32_t version,
                      ChunkPartType chunkType);
int hddInternalUpdateVersion(IChunk *chunk, uint32_t version,
                             uint32_t newversion);
int hddInternalUpdateVersion(uint64_t chunkId, uint32_t version,
                             uint32_t newversion, ChunkPartType chunkType);

/** \brief Inserts the input buffer of already replied write operation to a container, so that it is
 * used to patch some of the read requests coming after the write operation, but before the chunk
 * file is updated on disk. Thread safe.
 */
void hddInsertAlreadyRepliedInputBuffer(uint64_t chunkId, ChunkPartType chunkType,
                                        std::shared_ptr<InputBuffer> inputBuffer,
                                        bool isFirstReply);

/** \brief Removes the input buffer of already replied write operation from the container, so that
 * it is not used to patch read requests anymore. Thread safe.
 */
void hddRemoveAlreadyRepliedInputBuffer(uint64_t chunkId, ChunkPartType chunkType,
                                        std::shared_ptr<InputBuffer> inputBuffer);
