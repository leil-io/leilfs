/*
   Copyright 2023 Leil Storage

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include <boost/shared_ptr.hpp>
#include <deque>

#include "chunkserver-common/chunk_interface.h"
#include "common/chunk_with_version_and_type.h"
#include "protocol/chunks_with_type.h"

#define ChunkNotFound nullptr

// master reports
inline std::deque<ChunkWithType> gDamagedChunks;
inline std::deque<ChunkWithType> gLostChunks;
inline std::deque<ChunkWithVersionAndType> gNewChunks;
inline std::atomic<uint32_t> gErrorCounter = 0;
inline std::atomic_bool gHddSpaceChanged = false;

inline std::thread gDisksThread, gDelayedThread, gTesterThread;
inline std::thread gChunkTesterThread;

inline std::atomic_bool gTerminate = false;
inline uint8_t gDiskActions = 0;  // no need for atomic; guarded by gDisksMutex
inline std::atomic_bool gResetTester = false;

// master reports = damaged chunks, lost chunks, new chunks
inline std::mutex gMasterReportsLock;

/// Avoid and point out possible deadlocks, by waiting for the specified time to
/// release the condition variable waiting on a Chunk. The number is small
/// enough not to stop the operation (mount retries) and big enough to detect
/// unusual/unexpected behavior.
static constexpr uint8_t kSecondsToWaitForLockedChunk_ = 30;

/// Adds the error to the Disk owner of chunk in a thread sage way and preserves
/// the errno to be used in later checks.
void hddAddErrorAndPreserveErrno(IChunk *chunk);

/// Adds the chunk to the damaged chunks container.
/// The damaged chunks are reported periodically to master in the event loop.
void hddReportDamagedChunk(uint64_t chunkId, ChunkPartType chunkType);

bool hddChunkTryLock(IChunk *chunk);

/// Removes the Chunk from the registry and from the disk's testlist.
void hddRemoveChunkFromContainers(IChunk *chunk);

/// Sets the chunk's state to Available or Deleted and notifies other waiting
/// threads. If the chunk was deleted, and there are no threads waiting, it also
/// removes it from the registry and from the disk's testlist.
void hddChunkRelease(IChunk *chunk);

/// Advises the kernel that the chunk should not be cached (meta and data files)
void hddAdviseNoCache(IChunk *chunk);

int hddIOEnd(IChunk *chunk);

int hddIOBegin(IChunk *chunk, int newFlag,
               uint32_t chunkVersion = disk::kMaxUInt32Number);

bool hddScansInProgress();

IChunk *hddChunkFindAndLock(uint64_t chunkId, ChunkPartType chunkType);

int chunkWriteCrc(IChunk *chunk);

/// Calls disk->updateChunkAttributes(), retrying a few times (with backoff)
/// while it keeps returning SAUNAFS_ERROR_IO, since that's a transient read
/// failure rather than a confirmed-absent chunk.
int hddUpdateChunkAttributesWithRetry(IDisk *disk, IChunk *chunk, bool isFromScan);

/// Finds an existing chunk or creates a new one, and locks it anyway.
///
/// The function locks the returned chunk (may block if the chunk is already
/// locked). To unlock it, use \ref hddChunkRelease.
IChunk *hddChunkFindOrCreatePlusLock(IDisk *disk, uint64_t chunkid,
                                     ChunkPartType chunkType,
                                     disk::ChunkGetMode creationMode);

/// Remove old chunk and create new one in its place.
///
/// \param disk pointer to disk object which will be the owner if new chunk
/// \param chunk pointer to old object
/// \param chunkId chunk id that will be reused
/// \param type type of new chunk object
/// \return address of the new object or ChunkNotFound
///
/// \note ChunkId and threads waiting for this object are preserved.
IChunk *hddRecreateChunk(IDisk *disk, IChunk *chunk, uint64_t chunkId,
                         ChunkPartType type);

void hddDeleteChunkFromRegistry(IChunk *chunk);
