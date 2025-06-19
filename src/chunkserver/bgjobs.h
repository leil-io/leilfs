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

#include "chunkserver-common/jobpool.h"
#include "chunkserver/output_buffer.h"

#include <cstdint>
#include <list>

constexpr auto kEmptyCallback = nullptr;
constexpr auto kEmptyExtra = nullptr;

void disableJob(uint32_t jobId, uint32_t listenerId, uint64_t chunkId, ChunkPartType chunkType);

void disableJobs(std::list<uint32_t> &jobIds, uint32_t listenerId, uint64_t chunkId,
                 ChunkPartType chunkType);

void changeCallback(std::list<uint32_t> &jobIds, JobPool::JobCallback callback, void *extra,
                    uint32_t listenerId, uint64_t chunkId, ChunkPartType chunkType);

void changeCallback(uint32_t jobId, JobPool::JobCallback callback, void *extra, uint32_t listenerId,
                    uint64_t chunkId, ChunkPartType chunkType);

/// @brief Adds an open job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param chunkId The ID of the chunk.
/// @param chunkType The type of the chunk.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_open(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                  ChunkPartType chunkType, uint32_t listenerId = 0);
uint32_t job_open(uint32_t listenerId, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                  ChunkPartType chunkType);

/// @brief Adds a close job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param chunkId The ID of the chunk.
/// @param chunkType The type of the chunk.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_close(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                   ChunkPartType chunkType, uint32_t listenerId = 0);
uint32_t job_close(uint32_t sourceId, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                   ChunkPartType chunkType);

/// @brief Adds a read job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param chunkId The ID of the chunk.
/// @param version The version of the chunk.
/// @param chunkType The type of the chunk.
/// @param offset The offset to read from.
/// @param size The size to read.
/// @param maxBlocksToBeReadBehind The maximum blocks to be read behind.
/// @param blocksToBeReadAhead The blocks to be read ahead.
/// @param outputBuffer The output buffer for the read data.
/// @param performHddOpen Whether to perform HDD open.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_read(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                  uint32_t version, ChunkPartType chunkType, uint32_t offset, uint32_t size,
                  uint32_t maxBlocksToBeReadBehind, uint32_t blocksToBeReadAhead,
                  OutputBuffer *outputBuffer, bool performHddOpen, uint32_t listenerId = 0);
uint32_t job_read(uint32_t listenerId, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                  uint32_t version, ChunkPartType chunkType, uint32_t offset, uint32_t size,
                  uint32_t maxBlocksToBeReadBehind, uint32_t blocksToBeReadAhead,
                  OutputBuffer *outputBuffer, bool performHddOpen);

/// @brief Adds a prefetch job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param chunkId The ID of the chunk.
/// @param chunkType The type of the chunk.
/// @param firstBlockToBePrefetched The first block to be prefetched.
/// @param blocksToBePrefetched The number of blocks to be prefetched.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_prefetch(JobPool &jobPool, uint64_t chunkId, ChunkPartType chunkType,
                      uint32_t firstBlockToBePrefetched, uint32_t blocksToBePrefetched,
                      uint32_t listenerId = 0);

uint32_t job_prefetch(uint32_t listenerId, uint64_t chunkId, ChunkPartType chunkType,
                      uint32_t firstBlockToBePrefetched, uint32_t blocksToBePrefetched);

/// @brief Adds a write job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param chunkId The ID of the chunk.
/// @param chunkVersion The version of the chunk.
/// @param chunkType The type of the chunk.
/// @param blockNum The block number to write.
/// @param offset The offset to write to.
/// @param size The size to write.
/// @param crc The CRC of the data.
/// @param buffer The data buffer to write.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_write(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                   uint32_t chunkVersion, ChunkPartType chunkType, uint16_t blockNum,
                   uint32_t offset, uint32_t size, uint32_t crc, const uint8_t *buffer,
                   uint32_t listenerId = 0);
uint32_t job_write(uint32_t listenerId, JobPool::JobCallback callback, void *extra,
                   uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                   uint16_t blockNum, uint32_t offset, uint32_t size, uint32_t crc,
                   const uint8_t *buffer);

/// @brief Adds a get blocks job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param chunkId The ID of the chunk.
/// @param version The version of the chunk.
/// @param chunkType The type of the chunk.
/// @param blocks The blocks to get.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_get_blocks(JobPool &jobPool, JobPool::JobCallback callback, void *extra,
                        uint64_t chunkId, uint32_t version, ChunkPartType chunkType,
                        uint16_t *blocks, uint32_t listenerId = 0);
uint32_t job_get_blocks(uint32_t listenerId, JobPool::JobCallback callback, void *extra,
                        uint64_t chunkId, uint32_t version, ChunkPartType chunkType,
                        uint16_t *blocks);

/// @brief Adds a replicate job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param chunkId The ID of the chunk.
/// @param chunkVersion The version of the chunk.
/// @param chunkType The type of the chunk.
/// @param sourcesBufferSize The size of the sources buffer.
/// @param sourcesBuffer The sources buffer.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_replicate(JobPool &jobPool, JobPool::JobCallback callback, void *extra,
                       uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                       uint32_t sourcesBufferSize, const uint8_t *sourcesBuffer,
                       uint32_t listenerId = 0);

/// @brief Adds an invalid job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_invalid(JobPool &jobPool, JobPool::JobCallback callback, void *extra,
                     uint32_t listenerId = 0);

/// @brief Adds a delete job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param chunkId The ID of the chunk.
/// @param chunkVersion The version of the chunk.
/// @param chunkType The type of the chunk.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_delete(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                    uint32_t chunkVersion, ChunkPartType chunkType, uint32_t listenerId = 0);

/// @brief Adds a create job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param chunkId The ID of the chunk.
/// @param chunkVersion The version of the chunk.
/// @param chunkType The type of the chunk.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_create(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                    uint32_t chunkVersion, ChunkPartType chunkType, uint32_t listenerId = 0);

/// @brief Adds a change version job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param chunkId The ID of the chunk.
/// @param chunkVersion The version of the chunk.
/// @param chunkType The type of the chunk.
/// @param newChunkVersion The new version of the chunk.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_version(JobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                     uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                     uint32_t newChunkVersion, uint32_t listenerId = 0);

/// @brief Adds a truncate job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param chunkId The ID of the chunk.
/// @param chunkType The type of the chunk.
/// @param chunkVersion The version of the chunk.
/// @param newChunkVersion The new version of the chunk.
/// @param length The length to truncate to.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_truncate(JobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                      uint64_t chunkId, ChunkPartType chunkType, uint32_t chunkVersion,
                      uint32_t newChunkVersion, uint32_t length, uint32_t listenerId = 0);

/// @brief Adds a duplicate job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param chunkId The ID of the chunk.
/// @param chunkVersion The version of the chunk.
/// @param newChunkVersion The new version of the chunk.
/// @param chunkType The type of the chunk.
/// @param chunkIdCopy The ID of the chunk to copy.
/// @param chunkVersionCopy The version of the chunk to copy.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_duplicate(JobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                       uint64_t chunkId, uint32_t chunkVersion, uint32_t newChunkVersion,
                       ChunkPartType chunkType, uint64_t chunkIdCopy, uint32_t chunkVersionCopy,
                       uint32_t listenerId = 0);

/// @brief Adds a duplicate and truncate job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param chunkId The ID of the chunk.
/// @param chunkVersion The version of the chunk.
/// @param newChunkVersion The new version of the chunk.
/// @param chunkType The type of the chunk.
/// @param chunkIdCopy The ID of the chunk to copy.
/// @param chunkVersionCopy The version of the chunk to copy.
/// @param length The length to truncate to.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_duptrunc(JobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                      uint64_t chunkId, uint32_t chunkVersion, uint32_t newChunkVersion,
                      ChunkPartType chunkType, uint64_t chunkIdCopy, uint32_t chunkVersionCopy,
                      uint32_t length, uint32_t listenerId = 0);
