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

#include "common/platform.h"

#include "chunkserver-common/hdd_utils.h"

#include <cinttypes>
#include <fcntl.h>
#include <sys/time.h>

#include "chunkserver-common/global_shared_resources.h"
#include "chunkserver-common/hdd_stats.h"
#include "common/event_loop.h"
#include "devtools/TracePrinter.h"
#include "devtools/request_log.h"

void hddAddErrorAndPreserveErrno(IChunk *chunk) {
	TRACETHIS();
	assert(chunk);
	uint32_t index;
	struct timeval tv;
	int errmem = errno;

	{
		std::lock_guard disksLockGuard(gDisksMutex);

		gettimeofday(&tv, nullptr);

		auto *disk = chunk->owner();
		index = disk->lastErrorIndex();
		disk->lastErrorTab()[index].chunkid = chunk->id();
		disk->lastErrorTab()[index].errornumber = errmem;
		disk->lastErrorTab()[index].timestamp = tv.tv_sec;
		index = (index + 1) % disk::kLastErrorSize;
		disk->setLastErrorIndex(index);
	}

	++gErrorCounter;

	errno = errmem;
}

void hddReportDamagedChunk(uint64_t chunkId, ChunkPartType chunkType) {
	TRACETHIS1(chunkId);
	std::lock_guard lockGuard(gMasterReportsLock);
	gDamagedChunks.push_back({chunkId, chunkType});
}

bool hddChunkTryLock(IChunk *chunk) {
	assert(gChunksMapMutex.try_lock() == false);
	assert(chunk);
	TRACETHIS1(chunk->id());
	bool ret = false;

	if (chunk != nullptr && chunk->state() == ChunkState::Available) {
		chunk->setState(ChunkState::Locked);
		ret = true;
	}

	return ret;
}

void hddChunkRelease(IChunk *chunk) {
	TRACETHIS();
	assert(chunk);

	std::unique_lock chunksMapUniqueLock(gChunksMapMutex);

	if (chunk->state() == ChunkState::Locked) {
		chunk->setState(ChunkState::Available);
		if (chunk->condVar()) {
			chunk->condVar()->condVar.notify_one();
		}
	} else if (chunk->state() == ChunkState::ToBeDeleted) {
		if (chunk->condVar()) {
			chunk->setState(ChunkState::Deleted);
			chunk->condVar()->condVar.notify_one();
		} else {
			hddRemoveChunkFromContainers(chunk);
		}
	}
}

void hddRemoveChunkFromContainers(IChunk *chunk) {
	TRACETHIS();
	assert(chunk);

	auto chunkIter = gChunksMap.find(chunkToKey(*chunk));

	if (chunkIter == gChunksMap.end()) {
		safs::log_warn(
		    "Chunk to be removed wasn't found on the chunkserver. "
		    "(chunkid: {:#04x}, chunktype: {})",
		    chunk->id(), chunk->type().toString());
		return;
	}

	const auto *chunkSmartPointer = chunkIter->second.get();
	gOpenChunks.purge(chunkSmartPointer->metaFD());

	auto *disk = chunkSmartPointer->owner();

	if (disk != nullptr) {
		// remove this chunk from its disk's testlist
		const std::lock_guard testsLockGuard(gTestsMutex);
		disk->chunks().remove(chunk);
		disk->setNeedRefresh(true);
	}

	gChunksMap.erase(chunkIter);
}

int chunkWriteCrc(IChunk *chunk) {
	TRACETHIS();
	assert(chunk);

	chunk->owner()->setNeedRefresh(true);

	uint8_t *crcData = gOpenChunks.getResource(chunk->metaFD()).crcData();

	{
		DiskWriteStatsUpdater updater(chunk->owner(), chunk->getCrcBlockSize());
		ssize_t ret = chunk->owner()->writeCrc(chunk, crcData);

		if (ret != static_cast<ssize_t>(chunk->getCrcBlockSize())) {
			int errmem = errno;
			safs_silent_errlog(LOG_WARNING,
			                   "chunk_writecrc: file: %s - write error",
			                   chunk->fullMetaFilename().c_str());
			errno = errmem;
			updater.markWriteAsFailed();
			return SAUNAFS_ERROR_IO;
		}
	}

	HddStats::overheadWrite(chunk->getCrcBlockSize());
	return SAUNAFS_STATUS_OK;
}

void hddAdviseNoCache(IChunk *chunk) {
	TRACETHIS();
	assert(chunk);

	::posix_fadvise(chunk->metaFD(), 0, 0, POSIX_FADV_DONTNEED);

	if (chunk->dataFD() >= 0 && !chunk->owner()->isZonedDevice()) {
		::posix_fadvise(chunk->dataFD(), 0, 0, POSIX_FADV_DONTNEED);
	}
}

int hddIOEnd(IChunk *chunk) {
	assert(chunk);
	TRACETHIS1(chunk->id());

	if (chunk->wasChanged()) {
		int status = chunkWriteCrc(chunk);
		PRINTTHIS(status);

		if (status != SAUNAFS_STATUS_OK) {
			// FIXME(hazeman): We are probably leaking fd here.
			int errmem = errno;
			safs_silent_errlog(LOG_WARNING, "hddIOEnd: file:%s - write error",
			                   chunk->fullMetaFilename().c_str());
			errno = errmem;
			return status;
		}

		if (gPerformFsync) {
			uint64_t startTime = getMicroSecsTime();
			status = chunk->owner()->fsyncChunk(chunk);

			if (status != SAUNAFS_STATUS_OK) {
				int errmem = errno;
				safs_silent_errlog(LOG_WARNING,
				                   "hddIOEnd: file:%s - fsync error",
				                   chunk->fullMetaFilename().c_str());
				errno = errmem;
				return status;
			}

			HddStats::dataFSync(chunk->owner(), getMicroSecsTime() - startTime);
		}

		chunk->setWasChanged(false);
	}

	if (chunk->refCount() <= 0) {
		safs::log_warn("({}) refcount = 0 - This should never happen! (chunkId: {})", __func__,
		               chunk->id());
		errno = 0;

		return SAUNAFS_STATUS_OK;
	}

	chunk->setRefCount(chunk->refCount() - 1);

	if (chunk->refCount() == 0) {
#ifdef SAUNAFS_HAVE_POSIX_FADVISE
	if (gAdviseNoCache) {
		hddAdviseNoCache(chunk);
	}
#endif /* SAUNAFS_HAVE_POSIX_FADVISE */

		gOpenChunks.release(chunk->metaFD(), eventloop_time());
	}

	errno = 0;
	chunk->setValidAttr(0);

	return SAUNAFS_STATUS_OK;
}

int hddIOBegin(IChunk *chunk, int newFlag, uint32_t chunkVersion) {
	LOG_AVG_TILL_END_OF_SCOPE0("hddIOBegin");
	TRACETHIS();
	assert(chunk);
	int status;

	{  // We can move this chunk as last one to be tested
		std::lock_guard testsLockGuard(gTestsMutex);
		chunk->owner()->chunks().markAsTested(chunk);
	}

	if (chunk->refCount() == 0) {
		bool add = (chunk->metaFD() < 0);

		assert(!(newFlag && chunk->metaFD() >= 0));

		gOpenChunks.acquire(chunk->metaFD());  // Ignored if c->fd < 0

		if (chunk->metaFD() < 0) {
			// Try to free some long unused descriptors
			gOpenChunks.freeUnused(eventloop_time(), gChunksMapMutex);
			for (int i = 0; i < kOpenRetryCount; ++i) {
				if (newFlag) {
					chunk->owner()->creat(chunk);
				} else {
					chunk->owner()->open(chunk);
				}
				if (chunk->metaFD() < 0 && errno != ENFILE) {
					safs_silent_errlog(LOG_WARNING,
					                   "hddIOBegin: file:%s - open error",
					                   chunk->fullMetaFilename().c_str());
					return SAUNAFS_ERROR_IO;
				} else if (chunk->metaFD() >= 0) {
					gOpenChunks.acquire(chunk->metaFD(), OpenChunk(chunk));
					break;
				} else {  // chunk->fd < 0 && errno == ENFILE
					usleep((kOpenRetry_ms * 1000) << i);
					// Force free unused descriptors
					auto freed = gOpenChunks.freeUnused(disk::kMaxUInt32Number,
					                                    gChunksMapMutex, 4);
					safs_pretty_syslog(LOG_NOTICE,
					                   "hddIOBegin: freed unused: %d", freed);
				}
			}
			if (chunk->metaFD() < 0) {
				safs_silent_errlog(LOG_WARNING,
				                   "hddIOBegin: file: %s - open error",
				                   chunk->fullMetaFilename().c_str());
				return SAUNAFS_ERROR_IO;
			}
		}

		if (newFlag) {
			uint8_t *crcData =
			    gOpenChunks.getResource(chunk->metaFD()).crcData();
			memset(crcData, 0, chunk->getCrcBlockSize());
		} else if (add) {
			chunk->readaheadHeader();
			uint8_t *crcData =
			    gOpenChunks.getResource(chunk->metaFD()).crcData();
			status = chunk->owner()->readChunkCrc(chunk, chunkVersion, crcData);
			if (status != SAUNAFS_STATUS_OK) {
				int errmem = errno;
				gOpenChunks.release(chunk->metaFD(), eventloop_time());
				safs_silent_errlog(LOG_WARNING,
				                   "hddIOBegin: file:%s - read error",
				                   chunk->fullMetaFilename().c_str());
				errno = errmem;
				return status;
			}
		}
	}

	chunk->setRefCount(chunk->refCount() + 1);
	errno = 0;

	return SAUNAFS_STATUS_OK;
}

bool hddScansInProgress() { return gScansInProgress != 0; }

int hddUpdateChunkAttributesWithRetry(IDisk *disk, IChunk *chunk, bool isFromScan) {
	int status = SAUNAFS_ERROR_IO;
	for (int i = 0; i < kOpenRetryCount && status == SAUNAFS_ERROR_IO; ++i) {
		if (i > 0) { usleep(kOpenRetry_ms * 1000); }
		status = disk->updateChunkAttributes(chunk, isFromScan);
	}
	return status;
}

IChunk *hddChunkFindAndLock(uint64_t chunkId, ChunkPartType chunkType) {
	LOG_AVG_TILL_END_OF_SCOPE0("chunk_find");

	return hddChunkFindOrCreatePlusLock(nullptr, chunkId, chunkType,
	                                    disk::ChunkGetMode::kFindOnly);
}

IChunk *hddChunkFindOrCreatePlusLock(IDisk *disk, uint64_t chunkid,
                                     ChunkPartType chunkType,
                                     disk::ChunkGetMode creationMode) {
	TRACETHIS2(chunkid, (unsigned)creationMode);
	IChunk *chunk = nullptr;
	IDisk *effectiveDisk = disk;

	std::unique_lock chunksMapLock(gChunksMapMutex);
	auto chunkIter = gChunksMap.find(makeChunkKey(chunkid, chunkType));

	if (chunkIter == gChunksMap.end()) {  // The chunk does not exists
		if (creationMode !=
		    disk::ChunkGetMode::kFindOnly) {  // Create it if requested
			chunk =
			    hddRecreateChunk(effectiveDisk, nullptr, chunkid, chunkType);
		}

		return chunk;
	}

	chunk = chunkIter->second.get();
	effectiveDisk = chunk->owner();

	if (creationMode == disk::ChunkGetMode::kCreateOnly) {
		if (chunk->state() == ChunkState::Available ||
		    chunk->state() == ChunkState::Locked) {
			return nullptr;
		}
	}

	while (true) {
		switch (chunk->state()) {
		case ChunkState::Available:
			chunk->setState(ChunkState::Locked);
			chunksMapLock.unlock();
			if (chunk->validAttr() == 0) {
				int attrStatus =
				    hddUpdateChunkAttributesWithRetry(effectiveDisk, chunk, false);
				if (attrStatus != SAUNAFS_STATUS_OK) {
					// The chunk was found as available, but we can not
					// update its attributes, let's recreate it only if
					// requested
					if (creationMode != disk::ChunkGetMode::kFindOnly) {
						effectiveDisk->unlinkChunk(chunk);
						chunksMapLock.lock();
						chunk = hddRecreateChunk(effectiveDisk, chunk, chunkid,
						                         chunkType);
						return chunk;
					}

					// The Chunk is damaged, remove it from disk and from
					// memory
					hddReportDamagedChunk(chunk->id(), chunk->type());
					effectiveDisk->unlinkChunk(chunk);
					hddDeleteChunkFromRegistry(chunk);
					return nullptr;
				}
			}
			return chunk;
		case ChunkState::Deleted:
			if (creationMode != disk::ChunkGetMode::kFindOnly) {  // Reuse it
				chunk =
				    hddRecreateChunk(effectiveDisk, chunk, chunkid, chunkType);
				return chunk;
			}
			if (chunk->condVar() != nullptr) {  // waiting threads - wake them up
				chunk->condVar()->condVar.notify_one();
			} else {  // no more waiting threads - remove
				hddRemoveChunkFromContainers(chunk);
			}
			return nullptr;
		case ChunkState::ToBeDeleted:
		case ChunkState::Locked:
			if (chunk->condVar() == nullptr) {
				// Try to reuse one if possible.
				if (!gFreeCondVars.empty()) {
					chunk->setCondVar(std::move(gFreeCondVars.back()));
					gFreeCondVars.pop_back();
				} else {
					chunk->setCondVar(std::make_unique<CondVarWithWaitCount>());
				}
			}
			chunk->condVar()->numberOfWaitingThreads++;
			auto status = chunk->condVar()->condVar.wait_for(
			    chunksMapLock,
			    std::chrono::seconds(kSecondsToWaitForLockedChunk_));
			chunk->condVar()->numberOfWaitingThreads--;
			uint32_t waitingThreads = chunk->condVar()->numberOfWaitingThreads;
			if (chunk->condVar()->numberOfWaitingThreads == 0) {
				// No more waiting threads, store it to be reused
				gFreeCondVars.emplace_back(std::move(chunk->condVar()));
			}
			if (status == std::cv_status::timeout) {
				safs::log_warn("Chunk {} locked for {} seconds, waiting threads {}", chunkid,
				               kSecondsToWaitForLockedChunk_, waitingThreads);
			}
		}
	}
}

IChunk *hddRecreateChunk(IDisk *disk, IChunk *chunk, uint64_t chunkId,
                         ChunkPartType type) {
	std::unique_ptr<CondVarWithWaitCount> waiting;

	if (chunk != ChunkNotFound) {
		assert(chunk->id() == chunkId);

		if (chunk->state() != ChunkState::Deleted &&
		    chunk->owner() != nullptr) {
			const std::lock_guard lock(gTestsMutex);
			disk->chunks().remove(chunk);
			disk->setNeedRefresh(true);
		}

		waiting = std::move(chunk->condVar());

		// It's possible to reuse object chunk if the format is the same,
		// but it doesn't happen often enough to justify adding extra code.
		hddRemoveChunkFromContainers(chunk);
	}

	if (disk == DiskNotFound) { return ChunkNotFound; }

	chunk = disk->instantiateNewConcreteChunk(chunkId, type);
	passert(chunk);

	bool success = gChunksMap
	                   .insert({makeChunkKey(chunkId, type),
	                            std::unique_ptr<IChunk>(chunk)})
	                   .second;
	massert(success,
	        "Cannot insert new chunk to the map as a chunk with "
	        "its chunkId and chunkPartType already exists");

	chunk->setCondVar(std::move(waiting));

	return chunk;
}

/* chunk operations */
void hddDeleteChunkFromRegistry(IChunk *chunk) {
	TRACETHIS();
	assert(chunk);

	const std::lock_guard chunksMapLockGuard(gChunksMapMutex);

	if (chunk->condVar()) {
		chunk->setState(ChunkState::Deleted);
		chunk->condVar()->condVar.notify_one();
	} else {
		hddRemoveChunkFromContainers(chunk);
	}
}
