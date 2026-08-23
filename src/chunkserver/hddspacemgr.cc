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

#include "common/platform.h"

#include "hddspacemgr.h"
#include <cstdint>

#ifdef SAUNAFS_HAVE_FALLOC_FL_PUNCH_HOLE_IN_LINUX_FALLOC_H
#  define SAUNAFS_HAVE_FALLOC_FL_PUNCH_HOLE
#endif

#if defined(SAUNAFS_HAVE_FALLOCATE)
#if defined(SAUNAFS_HAVE_FALLOC_FL_PUNCH_HOLE) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#endif

#include <dirent.h>
#include <fcntl.h>
#include <cerrno>
#ifdef SAUNAFS_HAVE_FALLOC_FL_PUNCH_HOLE_IN_LINUX_FALLOC_H
#  include <linux/falloc.h>
#endif
#include <cinttypes>
#include <cmath>
#ifndef SAUNAFS_HAVE_THREAD_LOCAL
#include <pthread.h>
#endif // SAUNAFS_HAVE_THREAD_LOCAL
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#ifdef SAUNAFS_HAVE_THREAD_LOCAL
#include <array>
#endif // SAUNAFS_HAVE_THREAD_LOCAL
#include <algorithm>
#include <atomic>
#include <deque>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "chunkserver-common/chunk_interface.h"
#include "chunkserver-common/chunk_trash_manager.h"
#include "chunkserver-common/chunk_with_fd.h"
#include "chunkserver-common/cmr_disk.h"
#include "chunkserver-common/default_disk_manager.h"
#include "chunkserver-common/global_shared_resources.h"
#include "chunkserver-common/hdd_stats.h"
#include "chunkserver-common/hdd_utils.h"
#include "chunkserver-common/iostat.h"
#include "chunkserver-common/plugin_manager.h"
#include "chunkserver-common/subfolder.h"
#include "chunkserver/chartsdata.h"
#include "chunkserver/chunk_filename_parser.h"
#include "chunkserver/io_buffers.h"
#include "common/chunk_version_with_todel_flag.h"
#include "common/crc.h"
#include "common/datapack.h"
#include "common/disk_info.h"
#include "common/event_loop.h"
#include "common/legacy_vector.h"
#include "common/massert.h"
#include "common/serialization.h"
#include "common/slice_traits.h"
#include "common/time_utils.h"
#include "common/unique_queue.h"
#include "config/cfg.h"
#include "devtools/TracePrinter.h"
#include "devtools/request_log.h"
#include "errors/saunafs_error_codes.h"
#include "protocol/SFSCommunication.h"
#include "slogger/slogger.h"

constexpr int kErrorLimit = 2;
constexpr int kLastErrorTime = 60;
constexpr size_t kIgnoreHeaderSize = 1;
constexpr uint32_t startingOffsetOfBlock(uint32_t blocknum) { return blocknum * SFSBLOCKSIZE; }

inline std::atomic_bool gCheckCrcWhenReading{true};

static std::mutex gDisksToBeDeletedWithPendingChunksMutex;
static std::vector<std::pair<std::unique_ptr<IDisk>, std::vector<ChunkWithType>>>
    gDisksToBeDeletedWithPendingChunks;
static std::vector<std::pair<IDisk *, std::vector<ChunkWithType>>>
    gNewDisksToBeDeletedWithPendingChunks;

static std::mutex gAlreadyRepliedInputBuffersMutex;
using InputBufferWithWriteInfo = std::pair<std::shared_ptr<InputBuffer>, std::vector<WriteInfo>>;
static std::unordered_map<ChunkWithType, std::list<InputBufferWithWriteInfo>, KeyOperations,
                          KeyOperations>
    gAlreadyRepliedInputBuffers;

/// @enum SendDataToMasterMode
/// @brief Represents the reason of sending data to the master server.
enum class SendDataToMasterMode : uint8_t {
	ForDiskRemoval,
	ForDamagedDisk,
	ForNewChunk
};

void hddGetDamagedChunks(std::vector<ChunkWithType>& chunks,
                         std::size_t limit) {
	TRACETHIS();
	std::lock_guard lockGuard(gMasterReportsLock);
	std::size_t size = std::min(gDamagedChunks.size(), limit);
	chunks.assign(gDamagedChunks.begin(), gDamagedChunks.begin() + size);
	gDamagedChunks.erase(gDamagedChunks.begin(), gDamagedChunks.begin() + size);
}

void hddReportLostChunk(uint64_t chunkid, ChunkPartType chunk_type) {
	TRACETHIS1(chunkid);
	std::lock_guard lockGuard(gMasterReportsLock);
	gLostChunks.push_back({chunkid, chunk_type});
}

void hddGetLostChunks(std::vector<ChunkWithType> &chunks, std::size_t limit) {
	TRACETHIS();
	std::lock_guard lockGuard(gMasterReportsLock);
	std::size_t size = std::min(gLostChunks.size(), limit);
	chunks.assign(gLostChunks.begin(), gLostChunks.begin() + size);
	gLostChunks.erase(gLostChunks.begin(), gLostChunks.begin() + size);
}

void hddReportNewChunkToMaster(uint64_t id, uint32_t version, bool todel,
                               ChunkPartType type) {
	TRACETHIS();
	uint32_t versionWithTodelFlag =
	    common::combineVersionWithTodelFlag(version, todel);
	std::lock_guard lockGuard(gMasterReportsLock);
	gNewChunks.push_back(
	    ChunkWithVersionAndType(id, versionWithTodelFlag, type));
}

/// Drops the entries the master already learned in the current registration
/// session. Defined with the rest of the sweep state below.
static void hddRegistrationSweepDropReported(std::vector<ChunkWithVersionAndType> &chunks);

void hddGetNewChunks(std::vector<ChunkWithVersionAndType> &chunks,
                     std::size_t limit) {
	TRACETHIS();
	{
		std::lock_guard lockGuard(gMasterReportsLock);
		std::size_t size = std::min(gNewChunks.size(), limit);
		chunks.assign(gNewChunks.begin(), gNewChunks.begin() + size);
		gNewChunks.erase(gNewChunks.begin(), gNewChunks.begin() + size);
	}

	// Announcements queued while registration was running are redundant: the
	// sweep reports the whole registry anyway, and marks every entry it sends.
	// Worse than redundant: an entry carries the
	// version the disk scan happened to see, so a version change applied
	// before the queue drains puts a superseded claim on the wire.
	//
	// Deliberately outside the gMasterReportsLock scope above: the filter takes
	// gChunksMapMutex, and hddSendDataToMaster already holds that while queueing
	// a report, so nesting the two would invert the established order.
	hddRegistrationSweepDropReported(chunks);
}

uint32_t hddGetAndResetErrorCounter() {
	TRACETHIS();
	return gErrorCounter.exchange(0);
}

int hddGetAndResetSpaceChanged() {
	TRACETHIS();
	return gHddSpaceChanged.exchange(false);
}

static void hddReloadChunkBulkSize(bool isReload) {
	const auto previousChunkBulkSize = gChunkBulkSize.load(std::memory_order_relaxed);
	const auto configuredChunkBulkSize = cfg_get_minmaxvalue<uint32_t>(
	    "HDD_CHUNK_BULK_SIZE", kDefaultChunkBulkSize, kMinChunkBulkSize, kMaxChunkBulkSize);

	gChunkBulkSize.store(configuredChunkBulkSize, std::memory_order_relaxed);

	if (isReload && previousChunkBulkSize != configuredChunkBulkSize) {
		safs::log_info("hdd space manager: HDD_CHUNK_BULK_SIZE changed from {} to {}",
		               previousChunkBulkSize, configuredChunkBulkSize);
	}

	safs::log_info("hdd space manager: Effective HDD_CHUNK_BULK_SIZE: {}", configuredChunkBulkSize);
}

uint32_t hddGetSerializedSizeOfAllDiskInfosV2() {
	TRACETHIS();
	uint32_t serializedSizeOfAllDisks = 0;
	static constexpr uint32_t kMaxDiskInfoSerializedSizeWithoutPath = (2 + 226);
	static constexpr size_t kMaxDiskPathSize = 255;

	for (const auto &disk : gDisks) {
		serializedSizeOfAllDisks += kMaxDiskInfoSerializedSizeWithoutPath +
		                            std::min(disk->dataPath().size(), kMaxDiskPathSize);
	}

	return serializedSizeOfAllDisks;
}

void hddSerializeAllDiskInfosV2(uint8_t *buff) {
	TRACETHIS();

	if (buff != nullptr) {
		LegacyVector<DiskInfo> diskInfoVector;

		for (const auto &disk : gDisks) { diskInfoVector.emplace_back(disk->toDiskInfo()); }

		serialize(&buff, diskInfoVector);
	}
}

std::string hddGetDiskGroups() {
	TRACETHIS();

	return gDiskManager->getDiskGroupsInfo();
}

void hddDiskInfoRotateStats() {
	TRACETHIS();

	std::lock_guard diskLockGuard(gDisksMutex);

	for (auto &disk : gDisks) {
		auto &diskStats = disk->getCurrentStats();
		if (disk->statsPos() == 0) {
			disk->setStatsPos(disk::kStatsHistoryIn24Hours - 1);
		} else {
			disk->setStatsPos(disk->statsPos() - 1);
		}
		disk->stats()[disk->statsPos()] = diskStats;
		diskStats.clear();
	}
}

static IChunk *hddChunkCreate(IDisk *disk, uint64_t chunkId,
                              ChunkPartType chunkType, uint32_t version) {
	TRACETHIS();

	auto *chunk = hddChunkFindOrCreatePlusLock(disk, chunkId, chunkType,
	                                           disk::ChunkGetMode::kCreateOnly);
	if (chunk == ChunkNotFound) {
		return ChunkNotFound;
	}

	chunk->setVersion(version);
	disk->setNeedRefresh(true);

	std::lock_guard testsLockGuard(gTestsMutex);
	disk->chunks().insert(chunk);

	return chunk;
}

void hddReleaseDisksToBeDeleted() {
	// Owning pointers of the disks selected for destruction. Declared before
	// the lock so it is destroyed after the lock is released: the disk
	// destructor joins per-disk worker threads (e.g. plugin garbage
	// collectors), and destroying a disk while holding
	// gDisksToBeDeletedWithPendingChunksMutex can deadlock with
	// hddCheckDisks, which acquires that mutex while holding gDisksMutex.
	std::vector<std::unique_ptr<IDisk>> disksToDestroy;

	std::lock_guard diskToBeDeletedLock(gDisksToBeDeletedWithPendingChunksMutex);
	std::unique_lock chunksMapLock(gChunksMapMutex, std::defer_lock);

	std::vector<IDisk *> disksToDelete;
	for (auto &diskToDelWithPendingChunks : gDisksToBeDeletedWithPendingChunks) {
		auto *disk = diskToDelWithPendingChunks.first.get();
		auto &pendingChunks = diskToDelWithPendingChunks.second;

		while (!pendingChunks.empty()) {
			auto &chunkIdentifier = pendingChunks.back();
			bool isChunkPending = true;

			chunksMapLock.lock();
			auto chunkIter =
			    gChunksMap.find(makeChunkKey(chunkIdentifier.id, chunkIdentifier.type));
			if (chunkIter == gChunksMap.end() || chunkIter->second->owner() != disk) {
				// Chunk not found or not owned by this disk
				isChunkPending = false;
			}
			chunksMapLock.unlock();

			if (!isChunkPending) {
				// Remove the chunk from the disk's pending chunks
				pendingChunks.pop_back();
			} else {
				// Stop if we found a chunk that is still present
				break;
			}
		}

		if (pendingChunks.empty()) {
			// No pending chunks, we can delete the disk
			disksToDelete.push_back(disk);
		}
	}

	disksToDestroy.reserve(disksToDelete.size());

	for (auto *disk : disksToDelete) {
		safs::log_info("({}) Deleting disk {} with no pending chunks", __func__,
		               disk->getPaths().c_str());
		auto diskIter =
		    std::find_if(gDisksToBeDeletedWithPendingChunks.begin(),
		                 gDisksToBeDeletedWithPendingChunks.end(),
		                 [disk](const auto &pair) { return pair.first.get() == disk; });
		if (diskIter != gDisksToBeDeletedWithPendingChunks.end()) {
			// Take ownership; the disk is destroyed by disksToDestroy after
			// the mutex is released.
			disksToDestroy.push_back(std::move(diskIter->first));
			gDisksToBeDeletedWithPendingChunks.erase(diskIter);
		}
	}
}

void hddSendDataToMaster(IDisk *disk, SendDataToMasterMode mode) {
	TRACETHIS();
	bool markedForDeletion = disk->isMarkedForDeletion();

	std::scoped_lock lock(gChunksMapMutex, gTestsMutex);

	// Until C++14 the order of the elements that are not erased is not
	// guaranteed to be preserved in std::unordered_map. Thus, to be truly
	// portable, all elements to be removed from gChunksMap are first stored
	// in an auxiliary container and then each is erased from gChunksMap
	// outside the loop.
	std::vector<IChunk *> chunksToRemove;

	if (mode != SendDataToMasterMode::ForNewChunk) {
		chunksToRemove.reserve(disk->chunks().size());
	}

	for (const auto &chunkEntry : gChunksMap) {
		IChunk *chunk = chunkEntry.second.get();

		if (chunk->owner() == disk) {
			if (mode != SendDataToMasterMode::ForNewChunk) {
				chunksToRemove.push_back(chunk);
			} else {
				hddReportNewChunkToMaster(chunk->id(), chunk->version(),
				                          markedForDeletion, chunk->type());
			}
		}
	}

	std::vector<ChunkWithType> pendingChunks;
	for (auto chunk : chunksToRemove) {
		hddReportLostChunk(chunk->id(), chunk->type());

		if (chunk->state() == ChunkState::Available) {
			gOpenChunks.purge(chunk->metaFD());
			chunk->owner()->chunks().remove(chunk);
			hddForgetPresentChunkType(chunk->type());
			gChunksMap.erase(chunkToKey(*chunk));
		} else if (chunk->state() == ChunkState::Locked) {
			// Some hdd worker is doing something with this chunk,
			// so we just mark it for deletion.
			chunk->setState(ChunkState::ToBeDeleted);
			if (mode == SendDataToMasterMode::ForDiskRemoval) {
				pendingChunks.emplace_back(chunk->id(), chunk->type());
			}
		}
	}

	if (mode == SendDataToMasterMode::ForDiskRemoval) {
		// Only for disk removal we need to store pending chunks: those disks are supposed to be
		// deleted after all pending chunks are removed.
		gNewDisksToBeDeletedWithPendingChunks.emplace_back(disk, std::move(pendingChunks));
	}
}

/// Assigned to each Disk
void hddDiskScanThread(IDisk *disk);

// Run every second
void hddCheckDisks() {
	TRACETHIS();
	uint32_t i;
	uint32_t now;
	int changed, err;
	struct timeval tv;

	gettimeofday(&tv, NULL);
	now = tv.tv_sec;

	changed = 0;

	// Owning pointers of removed disks with no pending chunks. Declared
	// before the lock so it is destroyed after the lock is released: the
	// disk destructor joins per-disk worker threads (e.g. plugin garbage
	// collectors) which may take gDisksMutex, and destroying promptly here
	// (instead of deferring to hddReleaseDisksToBeDeleted) minimizes the
	// window in which a re-added disk coexists with its old instance
	// (stale lock-file descriptors, still-running workers).
	std::vector<std::unique_ptr<IDisk>> disksToDestroy;

	std::unique_lock disksUniqueLock(gDisksMutex);

	if (gDiskActions == 0) {
		return;
	}

	for (auto &disk : gDisks) {
		if (disk->wasRemovedFromConfig()) {
			switch (disk->scanState()) {
			case IDisk::ScanState::kInProgress:
				disk->setScanState(IDisk::ScanState::kTerminate);
				break;
			case IDisk::ScanState::kThreadFinished:
				disk->scanThread().join();
				/* fallthrough */
			case IDisk::ScanState::kSendNeeded:
			case IDisk::ScanState::kNeeded:
				disk->setScanState(IDisk::ScanState::kWorking);
				/* fallthrough */
			case IDisk::ScanState::kWorking:
				hddSendDataToMaster(disk.get(), SendDataToMasterMode::ForDiskRemoval);
				changed = 1;
				disk->setWasRemovedFromConfig(false);
				break;
			case IDisk::ScanState::kTerminate:
				break;
			}
			// At this point, this is only true if it was already sent to master
			if (!disk->wasRemovedFromConfig()) {
				// Delay the deletion after the loop
				safs_pretty_syslog(LOG_NOTICE, "Disk %s successfully removed",
				                   disk->getPaths().c_str());

				gResetTester = true;
			}
		}
	}

	std::unique_lock diskToBeDeletedLock(gDisksToBeDeletedWithPendingChunksMutex);
	for (auto &diskToDelWithPendingChunks : gNewDisksToBeDeletedWithPendingChunks) {
		for (auto it = gDisks.begin(); it != gDisks.end(); ++it) {
			if (diskToDelWithPendingChunks.first == it->get()) {
				ChunkTrashManager::eraseDisk((*it)->metaPath());
				if (!(*it)->isZonedDevice() && (*it)->metaPath() != (*it)->dataPath()) {
					ChunkTrashManager::eraseDisk((*it)->dataPath());
				}
				// Never destroy the disk here: the destructor joins worker
				// threads (e.g. a plugin garbage collector) that may take
				// gDisksMutex, which is held now — that join would deadlock
				// the whole chunkserver. Disks with pending chunks are
				// deferred to hddReleaseDisksToBeDeleted; disks without
				// pending chunks are destroyed by disksToDestroy right after
				// this function releases gDisksMutex, keeping the overlap
				// window with a possible fast re-add minimal. Moving *it
				// leaves a null unique_ptr in gDisks, so it must stay the
				// last use of the disk.
				if (!diskToDelWithPendingChunks.second.empty()) {
					gDisksToBeDeletedWithPendingChunks.emplace_back(
					    std::move(*it), std::move(diskToDelWithPendingChunks.second));
				} else {
					disksToDestroy.push_back(std::move(*it));
				}
				gDisks.erase(it);
				break;
			}
		}
	}
	diskToBeDeletedLock.unlock();

	gNewDisksToBeDeletedWithPendingChunks.clear();

	for (auto &disk : gDisks) {
		if (disk->isDamaged() || disk->wasRemovedFromConfig()) {
			continue;
		}
		switch (disk->scanState()) {
		case IDisk::ScanState::kNeeded:
			disk->setScanState(IDisk::ScanState::kInProgress);
			disk->setScanThread(std::thread(hddDiskScanThread, disk.get()));
			break;
		case IDisk::ScanState::kThreadFinished:
			disk->scanThread().join();
			disk->setScanState(IDisk::ScanState::kWorking);
			disk->refreshDataDiskUsage();
			disk->setNeedRefresh(false);
			disk->setLastRefresh(now);
			changed = 1;
			break;
		case IDisk::ScanState::kSendNeeded:
			hddSendDataToMaster(disk.get(), SendDataToMasterMode::ForNewChunk);
			disk->setScanState(IDisk::ScanState::kWorking);
			disk->refreshDataDiskUsage();
			disk->setNeedRefresh(false);
			disk->setLastRefresh(now);
			changed = 1;
			break;
		case IDisk::ScanState::kWorking:
			err = 0;
			for (i = 0; i < disk::kLastErrorSize; ++i) {
				if (disk->lastErrorTab()[i].timestamp + kLastErrorTime >= now
				    && (disk->lastErrorTab()[i].errornumber == EIO
				        || disk->lastErrorTab()[i].errornumber == EROFS)) {
					err++;
				}
			}
			if (err >= kErrorLimit &&
			    !(disk->isMarkedForRemoval() && disk->isReadOnly())) {
				safs_pretty_syslog(
				    LOG_WARNING, "%u errors occurred in %u seconds on disk: %s",
				    err, kLastErrorTime, disk->getPaths().c_str());
				hddSendDataToMaster(disk.get(), SendDataToMasterMode::ForDamagedDisk);
				disk->setIsDamaged(true);
				changed = 1;
			} else {
				if (disk->needRefresh() ||
				    disk->lastRefresh() + disk::kSecondsInOneMinute < now) {
					disk->refreshDataDiskUsage();
					disk->setNeedRefresh(false);
					disk->setLastRefresh(now);
					changed = 1;
				}
			}
		case IDisk::ScanState::kInProgress:
		case IDisk::ScanState::kTerminate:
			break;
		}
	}

	gDiskManager->updateSpaceUsage();

	disksUniqueLock.unlock();

	if (changed) {
		gHddSpaceChanged = true;
	}
}

void hddForeachChunkInBulks(BulkFunction bulkCallback, std::size_t bulkSize) {
	TRACETHIS();

	std::vector<ChunkWithVersionAndType> bulk;
	std::vector<ChunkWithType> recheckList;
	bulk.reserve(bulkSize);

	enum class BulkReadyWhen { FULL, NONEMPTY };
	auto handleBulkIfReady = [&bulk, &bulkCallback,
	                          bulkSize](BulkReadyWhen whatIsReady) {
		if ((whatIsReady == BulkReadyWhen::FULL && bulk.size() >= bulkSize) ||
		    (whatIsReady == BulkReadyWhen::NONEMPTY && !bulk.empty())) {
			bulkCallback(bulk);
			bulk.clear();
		}
	};
	auto addChunkToBulk = [&bulk](const IChunk *chunk) {
		common::chunk_version_t versionWithTodelFlag =
		    common::combineVersionWithTodelFlag(
		    chunk->version(), chunk->owner()->isMarkedForDeletion());
		bulk.push_back(ChunkWithVersionAndType(
		    chunk->id(), versionWithTodelFlag, chunk->type()));
	};

	{
		// do the operation for all immediately available (not-locked) chunks
		// add all other chunks to recheckList
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);

		for (const auto &chunkEntry : gChunksMap) {
			const IChunk *chunk = chunkEntry.second.get();
			if (chunk->state() != ChunkState::Available) {
				recheckList.push_back(
				    ChunkWithType(chunk->id(), chunk->type()));
				continue;
			}
			handleBulkIfReady(BulkReadyWhen::FULL);
			addChunkToBulk(chunk);
		}
		handleBulkIfReady(BulkReadyWhen::NONEMPTY);
	}

	// wait till each chunk from recheckList becomes available,
	// lock (acquire) it and then do the operation
	for (const auto &chunkWithType : recheckList) {
		handleBulkIfReady(BulkReadyWhen::FULL);
		auto *chunk =
		    hddChunkFindAndLock(chunkWithType.id, chunkWithType.type);
		if (chunk) {
			addChunkToBulk(chunk);
			hddChunkRelease(chunk);
		}
	}
	handleBulkIfReady(BulkReadyWhen::NONEMPTY);
}

/// Master-driven (pull) chunk registration sweep.
/// All sweep state is guarded by gChunksMapMutex, like the registry itself.
/// Chunks are marked with the session epoch when reported; the termination
/// pass rescans the whole registry until no unmarked chunk remains, which
/// covers chunks skipped while locked, chunks moved behind the bucket cursor
/// by a rehash, and chunks inserted during the sweep.

static uint8_t gRegistrationSweepEpoch = 0;
static std::size_t gRegistrationSweepBucket = 0;
/// Bucket array the cursor above refers to. A different value means the
/// registry rehashed between calls and the cursor lost its meaning.
static std::size_t gRegistrationSweepBucketCount = 0;
static bool gRegistrationSweepCursorDone = false;

void hddRegistrationSweepBegin() {
	std::lock_guard chunksMapLockGuard(gChunksMapMutex);

	++gRegistrationSweepEpoch;
	if (gRegistrationSweepEpoch == 0) {
		// The epoch wrapped: clear every stale mark, or a chunk still
		// carrying this epoch value from 256 sessions ago would be skipped.
		for (const auto &chunkEntry : gChunksMap) { chunkEntry.second->setRegistrationEpoch(0); }
		gRegistrationSweepEpoch = 1;
	}

	gRegistrationSweepBucket = 0;
	gRegistrationSweepBucketCount = gChunksMap.bucket_count();
	gRegistrationSweepCursorDone = false;
}

bool hddRegistrationSweepNext(std::vector<ChunkWithVersionAndType> &bulk, std::size_t bulkSize) {
	bulk.clear();
	bulk.reserve(bulkSize);

	auto addChunkToBulk = [&bulk](IChunk *chunk, uint8_t epoch) {
		chunk->setRegistrationEpoch(epoch);
		bulk.emplace_back(chunk->id(),
		                  common::combineVersionWithTodelFlag(
		                      chunk->version(), chunk->owner()->isMarkedForDeletion()),
		                  chunk->type());
	};

	std::vector<ChunkWithType> recheckList;
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);

		// Phase 1: advance the bucket cursor. Buckets are consumed whole, so
		// a bulk can slightly exceed bulkSize (bucket sizes are ~1).
		const std::size_t bucketCount = gChunksMap.bucket_count();

		// A rehash redistributed every entry, so the cursor no longer separates
		// swept buckets from unswept ones and chunks can now sit behind it.
		// Walk again from the start rather than leaving them to the termination
		// pass: chunks already reported cost one epoch compare each, and a
		// table only doubles a logarithmic number of times, so restarting
		// cannot happen often. The termination pass would find them too, but it
		// rescans the whole registry once per bulk, which is at its worst
		// exactly when a rehash displaced many chunks at once.
		if (bucketCount != gRegistrationSweepBucketCount) {
			gRegistrationSweepBucketCount = bucketCount;
			gRegistrationSweepBucket = 0;
			gRegistrationSweepCursorDone = false;
		}

		while (!gRegistrationSweepCursorDone && bulk.size() < bulkSize) {
			if (gRegistrationSweepBucket >= bucketCount) {
				gRegistrationSweepCursorDone = true;
				break;
			}
			for (auto bucketIter = gChunksMap.begin(gRegistrationSweepBucket);
			     bucketIter != gChunksMap.end(gRegistrationSweepBucket); ++bucketIter) {
				IChunk *chunk = bucketIter->second.get();
				if (chunk->registrationEpoch() == gRegistrationSweepEpoch ||
				    chunk->state() != ChunkState::Available) {
					// Marked already, or picked up by a later pass
					continue;
				}
				addChunkToBulk(chunk, gRegistrationSweepEpoch);
			}
			++gRegistrationSweepBucket;
		}

		if (!bulk.empty()) { return true; }

		// Phase 2 (termination): collect the stragglers
		for (const auto &chunkEntry : gChunksMap) {
			IChunk *chunk = chunkEntry.second.get();
			if (chunk->registrationEpoch() == gRegistrationSweepEpoch ||
			    chunk->state() == ChunkState::Deleted ||
			    chunk->state() == ChunkState::ToBeDeleted) {
				continue;
			}
			if (chunk->state() == ChunkState::Available) {
				addChunkToBulk(chunk, gRegistrationSweepEpoch);
			} else {
				recheckList.emplace_back(chunk->id(), chunk->type());
			}
			if (bulk.size() + recheckList.size() >= bulkSize) { break; }
		}
	}

	// Wait for the locked stragglers, mirroring hddForeachChunkInBulks
	for (const auto &chunkWithType : recheckList) {
		auto *chunk = hddChunkFindAndLock(chunkWithType.id, chunkWithType.type);
		if (chunk == ChunkNotFound) { continue; }
		{
			std::lock_guard chunksMapLockGuard(gChunksMapMutex);
			if (chunk->registrationEpoch() != gRegistrationSweepEpoch) {
				addChunkToBulk(chunk, gRegistrationSweepEpoch);
			}
		}
		hddChunkRelease(chunk);
	}

	return !bulk.empty();
}

uint64_t hddGetChunkRegistryBucketCount() {
	std::lock_guard chunksMapLockGuard(gChunksMapMutex);
	return gChunksMap.bucket_count();
}

static void hddRegistrationSweepDropReported(std::vector<ChunkWithVersionAndType> &chunks) {
	if (chunks.empty()) { return; }

	std::lock_guard chunksMapLockGuard(gChunksMapMutex);

	// Epoch 0 is also the unmarked value, so comparing against it would discard
	// every announcement. Both registration paths open a session before the
	// drain can run, and hddRegistrationSweepBegin never leaves the epoch at 0,
	// so this is not reachable today. It is kept because a future registration
	// path that neglects to open one would otherwise silently drop every chunk
	// this server owns.
	if (gRegistrationSweepEpoch == 0) { return; }

	// registrationEpoch is guarded by gChunksMapMutex, so the mark is readable
	// without locking each chunk: no attribute refresh, and no waiting behind
	// an operation already holding the chunk.
	auto alreadyReported = [](const ChunkWithVersionAndType &entry) {
		auto chunkIter = gChunksMap.find(makeChunkKey(entry.id, entry.type));
		if (chunkIter == gChunksMap.end()) {
			// Removed since it was queued: deleted on the master's order, found damaged,
			// or carried away with its disk. Whatever removed it already told the master,
			// and the lost report is drained ahead of this one, so announcing it now would
			// add back a copy that no longer exists.
			return true;
		}
		return chunkIter->second->registrationEpoch() == gRegistrationSweepEpoch;
	};

	std::erase_if(chunks, alreadyReported);
}

void hddGetTotalSpace(uint64_t *usedSpace, uint64_t *totalSpace,
                      uint32_t *chunkCount, uint64_t *toDelUsedSpace,
                      uint64_t *toDelTotalSpace, uint32_t *toDelChunkCount) {
	TRACETHIS();
	uint64_t available = 0, total = 0;
	uint64_t toDelAvailable = 0, toDelTotal = 0;
	uint32_t chunks = 0, toDelChunks = 0;

	{
		std::lock_guard disksLockGuard(gDisksMutex);

		for (const auto &disk : gDisks) {
			if (disk->isDamaged() || disk->wasRemovedFromConfig()) {
				continue;
			}
			if (!disk->isMarkedForDeletion()) {
				if (disk->scanState() == IDisk::ScanState::kWorking) {
					available += disk->availableSpace();
					total += disk->totalSpace();
				}

				std::lock_guard testsLockGuard(gTestsMutex);
				chunks += disk->chunks().size();
			} else {
				if (disk->scanState() == IDisk::ScanState::kWorking) {
					toDelAvailable += disk->availableSpace();
					toDelTotal += disk->totalSpace();
				}

				std::lock_guard testsLockGuard(gTestsMutex);
				toDelChunks += disk->chunks().size();
			}
		}
	}

	*usedSpace = total - available;
	*totalSpace = total;
	*chunkCount = chunks;
	*toDelUsedSpace = toDelTotal - toDelAvailable;
	*toDelTotalSpace = toDelTotal;
	*toDelChunkCount = toDelChunks;

	HddStats::gStatsLastReadUsedSpace = *usedSpace;
}

int hddGetLoadFactor() {
	return gIoStat.getLoadFactor();
}

/* I/O operations */
int hddOpen(IChunk *chunk) {
	assert(chunk);
	LOG_AVG_TILL_END_OF_SCOPE0("hddOpen");
	TRACETHIS1(chunk->id());

	int status = hddIOBegin(chunk, 0);
	PRINTTHIS(status);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
		hddReportDamagedChunk(chunk->id(), chunk->type());
	}

	return status;
}

int hddOpen(uint64_t chunkId, ChunkPartType chunkType) {
	auto *chunk = hddChunkFindAndLock(chunkId, chunkType);
	if (chunk == ChunkNotFound) {
		safs::log_err("hddOpen: could not find chunkid {}", chunkId);
		return SAUNAFS_ERROR_NOCHUNK;
	}

	int status = hddOpen(chunk);
	hddChunkRelease(chunk);

	return status;
}

int hddClose(IChunk *chunk) {
	assert(chunk);
	TRACETHIS1(chunk->id());
	int status = hddIOEnd(chunk);
	PRINTTHIS(status);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
		hddReportDamagedChunk(chunk->id(), chunk->type());
	}
	return status;
}

void hddInsertAlreadyRepliedInputBuffer(uint64_t chunkId, ChunkPartType chunkType,
                                        std::shared_ptr<InputBuffer> inputBuffer,
                                        bool isFirstReply) {
	TRACETHIS2(chunkId, chunkType.toString());

	std::lock_guard lock(gAlreadyRepliedInputBuffersMutex);
	if (isFirstReply) {
		auto writeInfoVec = inputBuffer->getWriteInfoVector();
		gAlreadyRepliedInputBuffers[{chunkId, chunkType}].emplace_back(std::move(inputBuffer),
		                                                               std::move(writeInfoVec));
	} else {
		// For subsequent replies, we need to find the existing input buffer and update its write
		// info vector
		auto it = gAlreadyRepliedInputBuffers.find({chunkId, chunkType});
		if (it == gAlreadyRepliedInputBuffers.end()) {
			// If the entry for this chunk is not found, we just skip the update.
			return;
		}
		auto &inputBufferList = it->second;
		// We assume that the input buffer is already in the list, since it should have been added
		// when the first reply was done. If it's not found, we just skip the update.
		for (auto &inputBufferWithWriteInfo : inputBufferList) {
			if (inputBufferWithWriteInfo.first == inputBuffer) {
				inputBufferWithWriteInfo.second = inputBuffer->getWriteInfoVector();
				break;
			}
		}
	}
}

void hddRemoveAlreadyRepliedInputBuffer(uint64_t chunkId, ChunkPartType chunkType,
                                        std::shared_ptr<InputBuffer> inputBuffer) {
	TRACETHIS2(chunkId, chunkType.toString());

	std::lock_guard lock(gAlreadyRepliedInputBuffersMutex);
	if (gAlreadyRepliedInputBuffers.contains({chunkId, chunkType})) {
		auto &inputBufferList = gAlreadyRepliedInputBuffers[{chunkId, chunkType}];
		inputBufferList.remove_if([&inputBuffer](const InputBufferWithWriteInfo &item) {
			return item.first == inputBuffer;
		});
		if (inputBufferList.empty()) { gAlreadyRepliedInputBuffers.erase({chunkId, chunkType}); }
	}
}

// This function is called when we have a read request and we want to update the output buffer with
// data from already replied input buffers that overlap with the requested range. This allows us to
// patch the output buffer with the data that should be already in the disk.
// @param chunkId the id of the chunk being read
// @param chunkType the type of the chunk being read
// @param offset the offset of the read request
// @param size the size of the read request
// @param outputBuffer the output buffer to be updated
void hddUpdateOutputBufferWithAlreadyRepliedInputBuffers(uint64_t chunkId, ChunkPartType chunkType,
                                                         uint32_t offset, uint32_t size,
                                                         OutputBuffer *outputBuffer) {
	TRACETHIS2(chunkId, chunkType.toString());

	uint32_t endOffset = offset + size;
	// We need to check all already replied input buffers for this chunk and see if there is any
	// overlap. It is important to keep the lock while we are checking and updating the output
	// buffer, to make sure that the input buffers are not modified by other threads while we are
	// using them.
	// TODO (dave): consider creating multiple locks for different chunks to reduce contention, if
	// this becomes a bottleneck.
	std::lock_guard lock(gAlreadyRepliedInputBuffersMutex);
	auto it = gAlreadyRepliedInputBuffers.find({chunkId, chunkType});
	if (it == gAlreadyRepliedInputBuffers.end()) { return; }
	auto &inputBufferList = it->second;

	for (const auto &[inputBuffer, writeInfoVec] : inputBufferList) {
		uint16_t repliedBlocks = inputBuffer->repliedBlocks;

		// For each block that was replied to the client, check if it overlaps with the
		// requested range
		for (uint16_t blockNum = 0; blockNum < repliedBlocks; ++blockNum) {
			auto &writeInfo = writeInfoVec[blockNum];
			uint32_t opOffset = writeInfo.offset + writeInfo.blockNum * SFSBLOCKSIZE;
			uint32_t opEndOffset = opOffset + writeInfo.size;

			uint32_t commonStart = std::max(offset, opOffset);
			uint32_t commonEnd = std::min(endOffset, opEndOffset);

			if (commonStart >= commonEnd) {
				// No overlap
				continue;
			}
			// There is an overlap in [commonStart, commonEnd) range, we need to copy data from
			// input buffer to output buffer

			// Calculate the offsets and sizes for copying
			// The start offset in the input buffer blocks is already in the block
			uint32_t startOffsetInInputBufferBlock = commonStart - opOffset;
			// If the output buffer is from a read in a single block, the this start offset is ok
			uint32_t startOffsetInOutputBufferBlock = commonStart - offset;
			uint32_t blockIndexInOutputBuffer = 0;
			uint32_t blockSizeInOutputBuffer = size;
			if (offset % SFSBLOCKSIZE == 0) {
				// Aligned case, could be multiblock read
				if (startOffsetInOutputBufferBlock / SFSBLOCKSIZE == size / SFSBLOCKSIZE) {
					// Last block
					blockSizeInOutputBuffer = (size - 1) % SFSBLOCKSIZE + 1;
				} else {
					// Full blocks
					blockSizeInOutputBuffer = SFSBLOCKSIZE;
				}
				blockIndexInOutputBuffer = startOffsetInOutputBufferBlock / SFSBLOCKSIZE;
				startOffsetInOutputBufferBlock %= SFSBLOCKSIZE;
			}  // Unaligned case, single block read -> everything is correct
			uint32_t bytesToCopy = commonEnd - commonStart;

			outputBuffer->updateIntervalBlockData(
			    blockIndexInOutputBuffer, startOffsetInOutputBufferBlock, bytesToCopy,
			    inputBuffer->getBlockBufferData(blockNum, startOffsetInInputBufferBlock));

			outputBuffer->updateBlockCRC(blockIndexInOutputBuffer, blockSizeInOutputBuffer);
		}
	}
}

int hddClose(uint64_t chunkId, ChunkPartType chunkType) {
	auto *chunk = hddChunkFindAndLock(chunkId, chunkType);
	if (chunk == NULL) {
		safs::log_err("hddClose: could not find chunkid {}", chunkId);
		return SAUNAFS_ERROR_NOCHUNK;
	}
	int status = hddClose(chunk);
	hddChunkRelease(chunk);
	return status;
}

int hddReadCrcAndBlock(IChunk *chunk, uint16_t blockNumber, OutputBuffer *outputBuffer,
                       bool isDataPreviouslyRead) {
	LOG_AVG_TILL_END_OF_SCOPE0("hddReadCrcAndBlock");
	assert(chunk);
	TRACETHIS2(chunk->id(), blockNumber);

	uint32_t bytesRead = 0;

	if (blockNumber >= SFSBLOCKSINCHUNK) {
		return SAUNAFS_ERROR_BNUMTOOBIG;
	}

	if (blockNumber >= chunk->blocks()) {
		bytesRead =
		    outputBuffer->copyIntoBuffer(OutputBuffer::BufferType::CRC, &gEmptyBlockCrc, kCrcSize);
		// Put a block of zeros into the buffer
		bytesRead +=
		    outputBuffer->copyValueIntoBuffer(OutputBuffer::BufferType::Block, 0, SFSBLOCKSIZE);

		if (bytesRead != kHddBlockSize) {
			safs::log_warn(
			    "hddReadCrcAndBlock: read error on block: {}, chunk: {}, current block count: {}",
			    blockNumber, chunk->id(), chunk->blocks());
			return SAUNAFS_ERROR_IO;
		}
	} else {
		const uint8_t *crcData =
		    gOpenChunks.getResource(chunk->metaFD()).crcData() + blockNumber * kCrcSize;
		bytesRead = outputBuffer->copyIntoBuffer(OutputBuffer::BufferType::CRC, crcData, kCrcSize);

		if (!isDataPreviouslyRead) {
			bytesRead +=
			    outputBuffer->copyIntoBuffer(OutputBuffer::BufferType::Block, chunk, SFSBLOCKSIZE,
			                                 startingOffsetOfBlock(blockNumber));

			if (bytesRead != kHddBlockSize) {
				hddAddErrorAndPreserveErrno(chunk);
				safs::log_warn("hddReadCrcAndBlock: file: {} - read error on block: {}",
				               chunk->fullDataFilename().c_str(), blockNumber);
				hddReportDamagedChunk(chunk->id(), chunk->type());
				return SAUNAFS_ERROR_IO;
			}
		}
	}

	return SAUNAFS_STATUS_OK;
}

int hddPrefetchBlocks(uint64_t chunkId, ChunkPartType chunkType,
                      uint32_t firstBlock, uint16_t numberOfBlocks) {
	LOG_AVG_TILL_END_OF_SCOPE0("hddPrefetchBlocks");

	auto *chunk = hddChunkFindAndLock(chunkId, chunkType);
	if (chunk == ChunkNotFound) {
		safs::log_err("Couldn't find chunkID {} for prefetching", chunkId);
		return SAUNAFS_ERROR_NOCHUNK;
	}

	int status = hddOpen(chunk);
	if (status != SAUNAFS_STATUS_OK) {
		safs_pretty_syslog(LOG_WARNING, "error opening chunk for prefetching: %"
		                   PRIu64 " - %s",
				chunkId, saunafs_error_string(status));
		hddChunkRelease(chunk);
		return status;
	}

	chunk->owner()->prefetchChunkBlocks(*chunk, firstBlock, numberOfBlocks);

	safs_silent_syslog(LOG_DEBUG, "chunkserver.hddPrefetchBlocks chunk: %"
	                   PRIu64 "status: %u firstBlock: %u nrOfBlocks: %u",
	                   chunkId, status, firstBlock, numberOfBlocks);

	status = hddClose(chunk);
	if (status != SAUNAFS_STATUS_OK) {
		safs_pretty_syslog(LOG_WARNING,
		                   "error closing prefetched chunk: %" PRIu64 " - %s",
		                   chunkId, saunafs_error_string(status));
	}

	hddChunkRelease(chunk);

	return status;
}

static void hddReadAheadAndBehind(IChunk *chunk, uint16_t block,
                                  uint32_t maxBlocksToBeReadBehind,
                                  uint32_t blocksToBeReadAhead) {
	// Ask OS for an appropriate read ahead and (if requested and needed)
	// read some blocks that were possibly skipped in a sequential file read
	if (chunk->blockExpectedToBeReadNext() < block &&
	    maxBlocksToBeReadBehind > 0) {
		// We were asked to read some possibly skipped blocks.
		uint16_t firstBlockToRead = chunk->blockExpectedToBeReadNext();
		// Try to prevent all possible overflows:
		if (firstBlockToRead + maxBlocksToBeReadBehind < block) {
			firstBlockToRead = block - maxBlocksToBeReadBehind;
		}
		sassert(firstBlockToRead < block);
		chunk->owner()->prefetchChunkBlocks(
		    *chunk, firstBlockToRead,
		    blocksToBeReadAhead + block - firstBlockToRead);
		auto buffer = getReadOutputBufferPool().get(kIgnoreHeaderSize, block - firstBlockToRead);
		for (uint16_t b = firstBlockToRead; b < block; ++b) {
			hddReadCrcAndBlock(chunk, b, buffer.get(), false);
		}
		getReadOutputBufferPool().put(std::move(buffer));
	} else {
		chunk->owner()->prefetchChunkBlocks(*chunk, block, blocksToBeReadAhead);
	}

	chunk->setBlockExpectedToBeReadNext(
	    std::max<uint16_t>(block + 1, chunk->blockExpectedToBeReadNext()));
}

/**
* Checks the CRC for the requested full block.
* The check may be skipped if the forceCheck is false and the configuration
* option HDD_CHECK_CRC_WHEN_READING is set to 0 (false).
* @param chunk Chunk to read from.
* @param block Block to check.
* @param outputBuffer Assumes the outputBuffer is already filled with data.
* @param offsetInBlockBuffer Starting index of the block in the outputBuffer.
* @param forceCheck If true, the CRC is checked even if the option is disabled
                    from the configuration. This is needed to keep integrity of
                    partial reads.
*/
int hddCheckCrcForFullBlock(IChunk *chunk, uint16_t block, OutputBuffer *outputBuffer,
                            uint32_t offsetInBlockBuffer, bool forceCheck) {
	if (!forceCheck && (!gCheckCrcWhenReading || block >= chunk->blocks())) {
		return SAUNAFS_STATUS_OK;
	}

	const uint8_t *crcData =
	    gOpenChunks.getResource(chunk->metaFD()).crcData() + block * kCrcSize;
	uint32_t crcValue;
	get32bit(&crcData, crcValue);

	if (!outputBuffer->checkCRC(SFSBLOCKSIZE, crcValue, offsetInBlockBuffer)) {
		hddAddChunkToTestQueue(ChunkWithVersionAndType{
		    chunk->id(), chunk->version(), chunk->type()});
		return SAUNAFS_ERROR_CRC;
	}

	return SAUNAFS_STATUS_OK;
};

int hddRead(uint64_t chunkId, uint32_t version, ChunkPartType chunkType,
            uint32_t offset, uint32_t size,
            [[maybe_unused]] uint32_t maxBlocksToBeReadBehind,
            [[maybe_unused]] uint32_t blocksToBeReadAhead,
            OutputBuffer *outputBuffer) {
	LOG_AVG_TILL_END_OF_SCOPE0("hddRead");
	TRACETHIS3(chunkId, offset, size);

	auto originalOffset = offset;
	auto originalSize = size;
	uint16_t block = offset / SFSBLOCKSIZE;

	safs::log_debug("hddRead: chunkId: {}, block: {}, offset: {}, size: {}",
	                chunkId, block, offset, size);

	uint32_t offsetWithinBlock = offset % SFSBLOCKSIZE;

	auto* chunk = hddChunkFindAndLock(chunkId, chunkType);

	if (chunk == ChunkNotFound) {
		safs::log_err("hddRead: Couldn't find chunkID {}", chunkId);
		return SAUNAFS_ERROR_NOCHUNK;
	}

	if (chunk->version() != version && version > 0) {
		hddChunkRelease(chunk);
		return SAUNAFS_ERROR_WRONGVERSION;
	}

	// Zoned devices use direct_io, so prefetched data is not cached
	if (!chunk->owner()->isZonedDevice()) {
		hddReadAheadAndBehind(chunk, block, maxBlocksToBeReadBehind,
		                      blocksToBeReadAhead);
	}

	// Put CRC and the data requested into buffer.
	// If possible (in case when whole block is read) try to put data directly
	// into passed outputBuffer, otherwise use temporary buffer to recompute
	// the checksum

	int status = SAUNAFS_STATUS_OK;
	if (size >= SFSBLOCKSIZE) {
		// We're are considering only reads of full blocks here
		if (offsetWithinBlock != 0) {
			safs::log_warn(
			    "hddRead: offset is not aligned to block size (chunkId: {}, offset: {}, size: {}). "
			    "Considering it aligned.",
			    chunkId, offset, size);
			offsetWithinBlock = 0;
		}

		uint16_t numBlocks = size / SFSBLOCKSIZE;
		uint16_t initialBlock = block;

		if (initialBlock < chunk->blocks()) {
			uint32_t bytesToBeRead = std::min<uint32_t>(
			    numBlocks * SFSBLOCKSIZE, (chunk->blocks() - initialBlock) * SFSBLOCKSIZE);
			uint32_t bytesRead = outputBuffer->copyIntoBuffer(OutputBuffer::BufferType::Block,
			                                                  chunk, bytesToBeRead, offset);

			if (bytesRead != bytesToBeRead) {
				hddAddErrorAndPreserveErrno(chunk);
				safs::log_warn("hddRead: file:{} - read error from block {} to {}",
				               chunk->fullDataFilename().c_str(), initialBlock,
				               initialBlock + numBlocks - 1);
				hddReportDamagedChunk(chunk->id(), chunk->type());
				hddChunkRelease(chunk);
				return SAUNAFS_ERROR_IO;
			}
		}

		// The data was already read in the previous step as a big block
		constexpr bool dataWasPreviouslyRead = true;

		for (uint16_t i = 0; i < numBlocks && status == SAUNAFS_STATUS_OK; i++) {
			uint16_t blockNumber = initialBlock + i;
			status = hddReadCrcAndBlock(chunk, blockNumber, outputBuffer, dataWasPreviouslyRead);

			if (status == SAUNAFS_STATUS_OK) {
				status = hddCheckCrcForFullBlock(chunk, blockNumber, outputBuffer,
				                                 startingOffsetOfBlock(i), false);
			}
		}

		// Update the size and offset for the remaining part of the read if last block is partial
		size -= numBlocks * SFSBLOCKSIZE;
		offset += numBlocks * SFSBLOCKSIZE;
		block += numBlocks;
	}

	// The remaining must be within a block
	if (offsetWithinBlock + size > SFSBLOCKSIZE) {
		safs::log_warn(
		    "hddRead: partial block read bigger than block size: offset in block: {}, size: {}",
		    offsetWithinBlock, size);
		hddChunkRelease(chunk);
		return SAUNAFS_ERROR_WRONGSIZE;
	}

	// Full blocks were read previously, the remaining case is a partial block
	if (status == SAUNAFS_STATUS_OK && size > 0) {
		// Temporary buffer to read the full block
		OutputBuffer tmp(kIgnoreHeaderSize, 1);
		status = hddReadCrcAndBlock(chunk, block, &tmp, false);

		if (status == SAUNAFS_STATUS_OK) {  // Successful read of the full block
			status = hddCheckCrcForFullBlock(chunk, block, &tmp, 0, true);

			if (status == SAUNAFS_STATUS_OK) {  // CRC is OK or check disabled
				uint8_t crcBuff[kCrcSize];
				uint8_t *crcBuffPointer = crcBuff;
				put32bit(
				    &crcBuffPointer,
				    mycrc32(0, tmp.rawData(OutputBuffer::BufferType::Block) + offsetWithinBlock,
				            size));
				outputBuffer->copyIntoBuffer(OutputBuffer::BufferType::CRC, crcBuff, kCrcSize);
				outputBuffer->copyIntoBuffer(
				    OutputBuffer::BufferType::Block,
				    tmp.rawData(OutputBuffer::BufferType::Block) + offsetWithinBlock, size);
			}
		}
	}

	if (status == SAUNAFS_STATUS_OK) {
		hddUpdateOutputBufferWithAlreadyRepliedInputBuffers(chunkId, chunkType, originalOffset,
		                                                    originalSize, outputBuffer);
	}

	PRINTTHIS(status);
	hddChunkRelease(chunk);
	return status;
}

/// A way of handling sparse files. If block is filled with zeros and crcBuffer
/// is filled with zeros as well, rewrite the crcBuffer so that it stores proper
/// CRC.
void hddRecomputeCrcIfBlockEmpty(uint8_t *block, uint8_t *crcBuffer) {
	const uint8_t* tmpPtr = crcBuffer;
	uint32_t crc;
	get32bit(&tmpPtr, crc);

	recompute_crc_if_block_empty(block, crc);
	uint8_t* tmpPtr2 = crcBuffer;
	put32bit(&tmpPtr2, crc);
}

int hddChunkWriteBlock(uint64_t chunkId, uint32_t version,
                       ChunkPartType chunkType, uint16_t blocknum,
                       uint32_t offset, uint32_t size, uint32_t crc,
                       const uint8_t *buffer) {
	auto *chunk = hddChunkFindAndLock(chunkId, chunkType);

	if (chunk == ChunkNotFound) {
		safs::log_err("hddChunkWriteBlock: ChunkNotFound; chunkId {}, version {}, type {}", chunkId, version, chunkType.toString());
		return SAUNAFS_ERROR_NOCHUNK;
	}

	auto *crcData = gOpenChunks.getResource(chunk->metaFD()).crcData();
	int status = chunk->owner()->writeChunkBlock(
	    chunk, version, blocknum, offset, size, crc, crcData, buffer);
	hddChunkRelease(chunk);

	return status;
}

int hddChunkWriteFullBlocks(uint64_t chunkId, uint32_t version, ChunkPartType chunkType,
                            uint16_t startBlock, uint16_t numBlocks, std::vector<uint32_t> &crcList,
                            std::vector<uint16_t> &blocksPerBuffer,
                            std::vector<const uint8_t *> &buffers) {
	auto *chunk = hddChunkFindAndLock(chunkId, chunkType);

	if (chunk == ChunkNotFound) {
		safs::log_err("hddChunkWriteFullBlocks: ChunkNotFound; chunkId {}, version {}, type {}",
		              chunkId, version, chunkType.toString());
		return -SAUNAFS_ERROR_NOCHUNK;
	}

	auto *crcData = gOpenChunks.getResource(chunk->metaFD()).crcData();
	int status = chunk->owner()->writeChunkBlocks(chunk, version, startBlock, numBlocks, crcList,
	                                              crcData, blocksPerBuffer, buffers);
	hddChunkRelease(chunk);

	return status;
}

/* chunk info */

int hddChunkGetNumberOfBlocks(uint64_t chunkId, ChunkPartType chunkType,
                              uint32_t version, uint16_t *blocks) {
	TRACETHIS1(chunkId);

	auto *chunk = hddChunkFindAndLock(chunkId, chunkType);
	*blocks = 0;
	if (chunk == ChunkNotFound) {
		safs::log_err("hddChunkGetNumberOfBlocks: Couldn't find chunkID {}", chunkId);
		return SAUNAFS_ERROR_NOCHUNK;
	}

	if (chunk->version() != version && version > 0) {
		hddChunkRelease(chunk);
		return SAUNAFS_ERROR_WRONGVERSION;
	}

	*blocks = chunk->blocks();
	hddChunkRelease(chunk);

	return SAUNAFS_STATUS_OK;
}

std::pair<int, IChunk *> hddInternalCreateChunk(uint64_t chunkId,
                                                uint32_t version,
                                                ChunkPartType chunkType) {
	TRACETHIS2(chunkId, version);
	IDisk *disk;
	int status;

	IChunk *chunk = ChunkNotFound;

	{
		std::scoped_lock disksLockGuard(gDisksMutex);

		disk = gDiskManager->getDiskForNewChunk(chunkType);

		if (disk == DiskNotFound) {
			return {SAUNAFS_ERROR_NOSPACE, ChunkNotFound};
		}

		chunk = hddChunkCreate(disk, chunkId, chunkType, version);
	}

	if (chunk == ChunkNotFound) {
		return {SAUNAFS_ERROR_CHUNKEXIST, ChunkNotFound};
	}

	status = hddIOBegin(chunk, 1);
	PRINTTHIS(status);

	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
		hddDeleteChunkFromRegistry(chunk);
		return {SAUNAFS_ERROR_IO, ChunkNotFound};
	}

	// Let the disk stamp any format-specific state (e.g. compression) on the
	// brand-new chunk before its header is sized and serialized.
	status = disk->applyNewChunkFormat(chunk);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
		hddIOEnd(chunk);
		disk->unlinkChunk(chunk);
		hddDeleteChunkFromRegistry(chunk);
		return {SAUNAFS_ERROR_IO, ChunkNotFound};
	}

	uint8_t *ptr = chunk->getChunkHeaderBuffer();
	memset(ptr, 0, chunk->getHeaderSize());

	{
		std::unique_ptr<ChunkSignature> signature =
		    disk->createChunkSignature(chunk);
		signature->serialize(&ptr);
	}

	{
		DiskWriteStatsUpdater updater(chunk->owner(), chunk->getHeaderSize());

		if (disk->writeChunkHeader(chunk) != SAUNAFS_STATUS_OK) {
			hddAddErrorAndPreserveErrno(chunk);
			safs_silent_errlog(LOG_WARNING,
			                   "create_newchunk: file:%s - write error",
			                   chunk->fullMetaFilename().c_str());
			hddIOEnd(chunk);
			disk->unlinkChunk(chunk);
			hddDeleteChunkFromRegistry(chunk);
			updater.markWriteAsFailed();
			return {SAUNAFS_ERROR_IO, ChunkNotFound};
		}
	}

	HddStats::overheadWrite(chunk->getHeaderSize());

	status = hddIOEnd(chunk);

	PRINTTHIS(status);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
		disk->unlinkChunk(chunk);
		hddDeleteChunkFromRegistry(chunk);
		return {status, ChunkNotFound};
	}

	return {SAUNAFS_STATUS_OK, chunk};
}

int hddInternalCreate(uint64_t chunkId, uint32_t version,
                      ChunkPartType chunkType) {
	TRACETHIS2(chunkId, version);

	HddStats::gStatsOperationsCreate++;

	auto [creationStatus, chunk] =
	    hddInternalCreateChunk(chunkId, version, chunkType);

	if (creationStatus == SAUNAFS_STATUS_OK) {
		hddChunkRelease(chunk);
	}

	return creationStatus;
}

static int hddInternalTestChunk(uint64_t chunkId, uint32_t version,
                                ChunkPartType chunkType) {
	TRACETHIS2(chunkId, version);
	uint16_t block;

	HddStats::gStatsOperationsTest++;

	auto *chunk = hddChunkFindAndLock(chunkId, chunkType);

	if (chunk == ChunkNotFound) {
		safs::log_err("hddInternalTestChunk: Couldn't find chunkID {}", chunkId);
		return SAUNAFS_ERROR_NOCHUNK;
	}

	if (chunk->version() != version && version > 0) {
		hddChunkRelease(chunk);
		return SAUNAFS_ERROR_WRONGVERSION;
	}

	int status = hddIOBegin(chunk, 0);
	PRINTTHIS(status);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
		hddChunkRelease(chunk);
		return status;
	}

	uint8_t *blockbuffer = getChunkBlockBuffer();
	// will be overwritten in the loop below if the test fails
	status = SAUNAFS_STATUS_OK;

	auto *crcData = gOpenChunks.getResource(chunk->metaFD()).crcData();
	for (block = 0; block < chunk->blocks(); ++block) {
		auto readBytes = chunk->owner()->readBlockAndCrc(
		    chunk, blockbuffer, crcData, block, "testChunk");
		uint8_t *dataInBuffer = blockbuffer + kCrcSize; // Skip crc

		if (readBytes < 0) {
			status = SAUNAFS_ERROR_IO;
			break;
		}

		HddStats::overheadRead(readBytes);

		const uint8_t* crcBuffPointer = blockbuffer;
		uint32_t crc;
		get32bit(&crcBuffPointer, crc);

		if (crc != mycrc32(0, dataInBuffer, SFSBLOCKSIZE)) {
			errno = 0; // set anything to errno
			hddAddErrorAndPreserveErrno(chunk);
			safs_pretty_syslog(LOG_WARNING,
			                   "testChunk: file:%s - crc error on block: %d",
			                   chunk->fullMetaFilename().c_str(), block);
			status = SAUNAFS_ERROR_CRC;
			break;
		}
	}
#ifdef SAUNAFS_HAVE_POSIX_FADVISE
	// Always advise the OS that tested chunks should not be cached. Don't rely
	// on hdd_delayed_ops to do it for us, because it may be disabled using a
	// config file.
	posix_fadvise(chunk->metaFD(), 0, 0, POSIX_FADV_DONTNEED);
#endif /* SAUNAFS_HAVE_POSIX_FADVISE */
	if (status != SAUNAFS_STATUS_OK) {
		// test failed -- chunk is damaged
		hddIOEnd(chunk);
		hddChunkRelease(chunk);
		return status;
	}
	status = hddIOEnd(chunk);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
		hddChunkRelease(chunk);
		return status;
	}
	hddChunkRelease(chunk);
	return SAUNAFS_STATUS_OK;
}

/// Blocks per read+write batch when copying a chunk. It matters most on a
/// zoned destination, which writes one fragment per call, so batching keeps a
/// full chunk well below the fragment limit; 256 matches the batch a compressed
/// write flushes at, and bounds the staging buffer at 16 MiB.
static constexpr uint16_t kChunkCopyBatchBlocks = 256;

/// Which side of a copy failed; only a failed read means a damaged source.
enum class ChunkCopyResult : std::uint8_t { Ok, ReadFailed, WriteFailed };

/// Copies the first \a blockCount blocks of \a sourceChunk into \a dupChunk,
/// reusing the source CRCs in \a sourceCrcData (reads decompress, so they
/// still describe the copied bytes) and updating \a dupCrcData.
///
/// Both formats copy in batches of kChunkCopyBatchBlocks: zoned destinations
/// through writeChunkBlocks(), the rest appending to the caller's data seek
/// through writeChunkData().
static ChunkCopyResult hddCopyChunkBlocks(IChunk *sourceChunk, IChunk *dupChunk,
                                          uint16_t blockCount, const uint8_t *sourceCrcData,
                                          uint8_t *dupCrcData, const char *errorMsg) {
	IDisk *sourceDisk = sourceChunk->owner();
	IDisk *dupDisk = dupChunk->owner();
	const bool isZonedDestination = dupDisk->isZonedDevice();

	// Pooled, so repeated duplications reuse the allocation; zone reads are
	// O_DIRECT straight into it, hence the buffer's IO alignment.
	auto buffer = getChunkCopyBuffersPool().get(kChunkCopyBufferHeaderSize, kChunkCopyBatchBlocks);
	auto &blockBuffer = buffer->getBlockBuffer();
	blockBuffer.resize(static_cast<size_t>(kChunkCopyBatchBlocks) * SFSBLOCKSIZE);

	// Reused across batches; only the last batch may hold fewer blocks.
	std::vector<uint32_t> crcs;
	std::vector<uint16_t> blocksPerBuffer{0};
	const std::vector<const uint8_t *> buffers{blockBuffer.data()};

	ChunkCopyResult result = ChunkCopyResult::Ok;

	for (uint16_t block = 0; block < blockCount; block += kChunkCopyBatchBlocks) {
		const uint16_t blocksInBatch =
		    std::min<uint16_t>(kChunkCopyBatchBlocks, blockCount - block);
		const int32_t batchSize = static_cast<int32_t>(blocksInBatch) * SFSBLOCKSIZE;

		{
			DiskReadStatsUpdater updater(sourceDisk, batchSize);

			if (sourceDisk->preadData(sourceChunk, blockBuffer.data(), batchSize,
			                          static_cast<uint64_t>(block) * SFSBLOCKSIZE) != batchSize) {
				hddAddErrorAndPreserveErrno(sourceChunk);
				safs::log_warn_with_error_code(errno, "{}: file:{} - data read error", errorMsg,
				                               sourceChunk->fullMetaFilename());
				updater.markReadAsFailed();
				result = ChunkCopyResult::ReadFailed;
				break;
			}
		}
		HddStats::overheadRead(batchSize);

		{
			DiskWriteStatsUpdater updater(dupDisk, batchSize);
			int written = 0;

			if (isZonedDestination) {
				crcs.resize(blocksInBatch);
				for (uint16_t i = 0; i < blocksInBatch; i++) {
					const uint8_t *crcPointer =
					    sourceCrcData + static_cast<size_t>(block + i) * kCrcSize;
					get32bit(&crcPointer, crcs[i]);
				}
				blocksPerBuffer[0] = blocksInBatch;

				written =
				    dupDisk->writeChunkBlocks(dupChunk, dupChunk->version(), block, blocksInBatch,
				                              crcs, dupCrcData, blocksPerBuffer, buffers);
			} else {
				written = dupDisk->writeChunkData(dupChunk, blockBuffer.data(), batchSize, 0);
			}

			if (written != batchSize) {
				hddAddErrorAndPreserveErrno(dupChunk);
				safs::log_warn_with_error_code(errno, "{}: file:{} - data write error", errorMsg,
				                               dupChunk->fullMetaFilename());
				updater.markWriteAsFailed();
				result = ChunkCopyResult::WriteFailed;
				break;
			}
		}
		HddStats::overheadWrite(batchSize);
	}

	getChunkCopyBuffersPool().put(std::move(buffer));
	return result;
}

/// Unwinds a half-built chunk copy on the failure paths of duplicate and
/// duptrunc.
///
/// Both operations lock a source chunk, register a copy, open both for I/O and
/// then run a dozen fallible steps, and every one of them has to undo exactly
/// the steps that already succeeded - in reverse order, without ending an I/O
/// twice and without leaving a registered chunk whose file was never written.
/// Spelled out at each site that is the same five calls repeated nine times.
/// Here every step records what it completed and the destructor undoes whatever
/// is still outstanding, so a new early return cannot forget one.
class ChunkCopyGuard {
public:
	explicit ChunkCopyGuard(IChunk *sourceChunk) : source_(sourceChunk) {}

	ChunkCopyGuard(const ChunkCopyGuard &) = delete;
	ChunkCopyGuard(ChunkCopyGuard &&) = delete;
	ChunkCopyGuard &operator=(const ChunkCopyGuard &) = delete;
	ChunkCopyGuard &operator=(ChunkCopyGuard &&) = delete;

	~ChunkCopyGuard() {
		if (committed_) {
			if (copy_ != nullptr) { hddChunkRelease(copy_); }
			hddChunkRelease(source_);
			return;
		}

		if (copyIoOpen_) { hddIOEnd(copy_); }
		if (copyHasFiles_) { copy_->owner()->unlinkChunk(copy_); }
		// Deleting the copy also unlocks it, so it never needs releasing.
		if (copy_ != nullptr) { hddDeleteChunkFromRegistry(copy_); }
		if (sourceIoOpen_) { hddIOEnd(source_); }
		if (sourceDamaged_) {
			hddReportDamagedChunk(source_->id(), source_->type());
		}
		hddChunkRelease(source_);
	}

	/// The copy is in the chunk registry, but has no files of its own yet.
	void copyRegistered(IChunk *copyChunk) { copy_ = copyChunk; }

	/// hddIOBegin() succeeded on the source chunk.
	void sourceIoBegan() { sourceIoOpen_ = true; }

	/// hddIOBegin() succeeded on the copy, creating its files.
	void copyIoBegan() {
		copyIoOpen_ = true;
		copyHasFiles_ = true;
	}

	/// Ends the source chunk's I/O and returns hddIOEnd()'s status. Whether it
	/// succeeds or not, the unwind will not end that I/O again.
	int endSourceIo() {
		sourceIoOpen_ = false;
		return hddIOEnd(source_);
	}

	/// Ends the copy's I/O and returns hddIOEnd()'s status. As above, but the
	/// files it created are still unlinked if the operation fails later.
	int endCopyIo() {
		copyIoOpen_ = false;
		return hddIOEnd(copy_);
	}

	/// Report the source chunk to the master as damaged while unwinding.
	void markSourceAsDamaged() { sourceDamaged_ = true; }

	/// The copy is complete: keep it, and only release both chunks.
	void commit() { committed_ = true; }

private:
	IChunk *source_;
	IChunk *copy_ = nullptr;
	bool sourceIoOpen_ = false;
	bool copyIoOpen_ = false;
	bool copyHasFiles_ = false;
	bool sourceDamaged_ = false;
	bool committed_ = false;
};

/// The buffers a chunk copy sets up once and then threads through every step:
/// the destination's header buffer, which accumulates the copy's CRCs until
/// they are persisted, and both chunks' in-memory CRC blocks.
///
/// The header buffer belongs to the calling thread and is sized for one chunk,
/// so it only stays valid while nothing else on this thread asks for another
/// chunk's header buffer.
struct ChunkCopyBuffers {
	uint8_t *header = nullptr;
	uint8_t *sourceCrc = nullptr;
	uint8_t *copyCrc = nullptr;
};

/// Picks a destination disk and registers the copy on it, handing the new chunk
/// to @p guard and to @p copy.
static int createChunkCopy(ChunkCopyGuard &guard, ChunkPartType chunkType, uint64_t copyChunkId,
                           uint32_t copyChunkVersion, IChunk **copy) {
	IChunk *newChunk = ChunkNotFound;

	{
		std::unique_lock disksUniqueLock(gDisksMutex);

		IDisk *copyDisk = gDiskManager->getDiskForNewChunk(chunkType);

		if (copyDisk == DiskNotFound) {
			// The guard lives in the caller, so the source chunk is released
			// well after this scope drops gDisksMutex.
			return SAUNAFS_ERROR_NOSPACE;
		}

		newChunk = hddChunkCreate(copyDisk, copyChunkId, chunkType, copyChunkVersion);
	}

	if (newChunk == ChunkNotFound) { return SAUNAFS_ERROR_CHUNKEXIST; }

	guard.copyRegistered(newChunk);
	*copy = newChunk;

	return SAUNAFS_STATUS_OK;
}

/// Opens the source chunk for reading, first moving it from @p chunkVersion to
/// @p chunkNewVersion when the two differ.
///
/// The bump is the same three steps hddInternalUpdateVersion() and
/// hddInternalTruncate() use, in the same order: rename the files to the new
/// version, open with the version the header still holds so its CRCs validate,
/// then rewrite the header. All three act on the chunk whose version changes,
/// which is the source - the copy is created at its own version and never
/// renamed.
static int openSourceForCopy(ChunkCopyGuard &guard, IChunk *sourceChunk, uint32_t chunkVersion,
                             uint32_t chunkNewVersion, const char *errorMsg) {
	if (chunkNewVersion == chunkVersion) {
		const int status = hddIOBegin(sourceChunk, 0);

		if (status != SAUNAFS_STATUS_OK) {
			hddAddErrorAndPreserveErrno(sourceChunk);
			guard.markSourceAsDamaged();
			return status;
		}

		guard.sourceIoBegan();

		return SAUNAFS_STATUS_OK;
	}

	if (sourceChunk->renameChunkFile(chunkNewVersion) < 0) {
		hddAddErrorAndPreserveErrno(sourceChunk);
		safs::log_warn_with_error_code(errno, "{}: file:{} - rename error", errorMsg,
		                               sourceChunk->fullMetaFilename());
		return SAUNAFS_ERROR_IO;
	}

	int status = hddIOBegin(sourceChunk, 0, chunkVersion);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(sourceChunk);
		return status;  //can't change file version
	}
	guard.sourceIoBegan();

	status = sourceChunk->owner()->overwriteChunkVersion(sourceChunk, chunkNewVersion);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(sourceChunk);
		safs::log_warn_with_error_code(errno, "{}: file:{} - write error", errorMsg,
		                               sourceChunk->fullMetaFilename());
		return SAUNAFS_ERROR_IO;
	}

	return SAUNAFS_STATUS_OK;
}

/// Opens the copy for writing and gives it a valid header: the destination
/// disk's format, an empty signature carrying its own id and version, and the
/// source's CRC block, which the copy then updates in place as blocks arrive.
static int prepareChunkCopyForWriting(ChunkCopyGuard &guard, IChunk *sourceChunk, IChunk *copy,
                                      const char *errorMsg, ChunkCopyBuffers *buffers) {
	int status = hddIOBegin(copy, 1);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(copy);
		return status;
	}
	guard.copyIoBegan();

	IDisk *copyDisk = copy->owner();

	// The copy takes the destination disk's configured format, not the source
	// chunk's, and must be stamped before getChunkHeaderBuffer() below sizes
	// the header buffer from it.
	status = copyDisk->applyNewChunkFormat(copy);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(copy);
		return SAUNAFS_ERROR_IO;
	}

	// hddChunkCreate() built the copy from the requested id and version, so the
	// chunk itself carries what the signature needs.
	buffers->header = copy->getChunkHeaderBuffer();
	memset(buffers->header, 0, copy->getHeaderSize());

	uint8_t *headerPtr = buffers->header;
	copyDisk->serializeEmptyChunkSignature(&headerPtr, copy->id(), copy->version(), copy->type(),
	                                       copy);

	buffers->sourceCrc = gOpenChunks.getResource(sourceChunk->metaFD()).crcData();
	buffers->copyCrc = gOpenChunks.getResource(copy->metaFD()).crcData();

	// Into the header buffer to reach the device, and into the in-memory block
	// the write paths keep up to date.
	memcpy(buffers->header + copy->getCrcOffset(), buffers->sourceCrc, copy->getCrcBlockSize());
	memcpy(buffers->copyCrc, buffers->sourceCrc, copy->getCrcBlockSize());

	// Write the header before the copy, which may add format-specific metadata
	// of its own to that same region (a compressed chunk stores its dictionary
	// there), so nothing here can overwrite it afterwards.
	copyDisk->lseekMetadata(copy, 0, SEEK_SET);

	{
		DiskWriteStatsUpdater updater(copyDisk, copy->getHeaderSize());

		if (copyDisk->writeChunkHeader(copy) != SAUNAFS_STATUS_OK) {
			hddAddErrorAndPreserveErrno(copy);
			safs::log_warn_with_error_code(errno, "{}: file:{} - hdr write error", errorMsg,
			                               copy->fullMetaFilename());
			updater.markWriteAsFailed();
			return SAUNAFS_ERROR_IO;
		}
	}

	HddStats::overheadWrite(copy->getHeaderSize());

	return SAUNAFS_STATUS_OK;
}

/// Finishes a copy whose block count differs from the source's: fills in the
/// CRCs of the blocks an expansion adds and grows the file, or copies the block
/// a misaligned shrink cuts in half, with its CRC recomputed over the bytes
/// that survive. Does nothing when the truncation lands on a block boundary.
static int finishTruncatedCopy(ChunkCopyGuard &guard, IChunk *sourceChunk, IChunk *copy,
                               uint16_t blocks, uint32_t lastBlockSize,
                               const ChunkCopyBuffers &buffers, const char *errorMsg) {
	IDisk *copyDisk = copy->owner();

	if (blocks > sourceChunk->blocks()) {
		for (uint16_t block = sourceChunk->blocks(); block < blocks; block++) {
			memcpy(buffers.header + copy->getCrcOffset() + kCrcSize * block, &gEmptyBlockCrc,
			       kCrcSize);
		}

		if (copyDisk->ftruncateData(copy, copy->getFileSizeFromBlockCount(blocks)) < 0) {
			hddAddErrorAndPreserveErrno(copy);
			safs::log_warn_with_error_code(errno, "{}: file:{} - ftruncate error", errorMsg,
			                               copy->fullMetaFilename());
			return SAUNAFS_ERROR_IO;  // write error
		}

		return SAUNAFS_STATUS_OK;
	}

	if (lastBlockSize == 0) { return SAUNAFS_STATUS_OK; }

	const uint16_t block = blocks - 1;
	uint8_t *blockBuffer = getChunkBlockBuffer() + kCrcSize;
	int32_t retSize = 0;

	{
		DiskReadStatsUpdater updater(sourceChunk->owner(), SFSBLOCKSIZE);

		retSize = sourceChunk->owner()->preadData(sourceChunk, blockBuffer, SFSBLOCKSIZE,
		                                          static_cast<uint64_t>(block) * SFSBLOCKSIZE);

		if (retSize != static_cast<int32_t>(SFSBLOCKSIZE)) {
			hddAddErrorAndPreserveErrno(sourceChunk);
			safs::log_warn_with_error_code(errno, "{}: file:{} - data read error", errorMsg,
			                               sourceChunk->fullMetaFilename());
			guard.markSourceAsDamaged();
			updater.markReadAsFailed();
			return SAUNAFS_ERROR_IO;
		}
	}
	HddStats::overheadRead(SFSBLOCKSIZE);

	uint8_t *crcPtr = buffers.header + copy->getCrcOffset() + kCrcSize * block;
	const uint32_t crc =
	    mycrc32_zeroexpanded(0, blockBuffer, lastBlockSize, SFSBLOCKSIZE - lastBlockSize);
	put32bit(&crcPtr, crc);

	// Fill with zeros the remaining part of the block
	memset(blockBuffer + lastBlockSize, 0, SFSBLOCKSIZE - lastBlockSize);

	{
		DiskWriteStatsUpdater updater(copyDisk, SFSBLOCKSIZE);

		if (copyDisk->isZonedDevice()) {
			retSize = copyDisk->writeChunkBlock(copy, copy->version(), block, 0, SFSBLOCKSIZE, crc,
			                                    buffers.copyCrc, blockBuffer) == SAUNAFS_STATUS_OK
			              ? SFSBLOCKSIZE
			              : 0;
		} else {
			retSize = copyDisk->writeChunkData(copy, blockBuffer, SFSBLOCKSIZE, 0);
		}

		if (retSize != static_cast<int32_t>(SFSBLOCKSIZE)) {
			hddAddErrorAndPreserveErrno(copy);
			safs::log_warn_with_error_code(errno, "{}: file:{} - data write error", errorMsg,
			                               copy->fullMetaFilename());
			updater.markWriteAsFailed();
			return SAUNAFS_ERROR_IO;
		}
	}
	HddStats::overheadWrite(SFSBLOCKSIZE);

	return SAUNAFS_STATUS_OK;
}

/// Persists the CRCs the copy accumulated in its header buffer.
static int persistChunkCopyCrc(IChunk *copy, const ChunkCopyBuffers &buffers,
                               const char *errorMsg) {
	IDisk *copyDisk = copy->owner();

	// The header buffer accumulated the copy's CRCs; persist just those.
	memcpy(buffers.copyCrc, buffers.header + copy->getCrcOffset(), copy->getCrcBlockSize());

	{
		DiskWriteStatsUpdater updater(copyDisk, copy->getCrcBlockSize());

		if (copyDisk->writeCrc(copy, buffers.copyCrc) !=
		    static_cast<ssize_t>(copy->getCrcBlockSize())) {
			hddAddErrorAndPreserveErrno(copy);
			safs::log_warn_with_error_code(errno, "{}: file:{} - crc write error", errorMsg,
			                               copy->fullMetaFilename());
			updater.markWriteAsFailed();
			return SAUNAFS_ERROR_IO;
		}
	}

	HddStats::overheadWrite(copy->getCrcBlockSize());

	return SAUNAFS_STATUS_OK;
}

static int hddInternalDuplicate(uint64_t chunkId, uint32_t chunkVersion,
                                uint32_t chunkNewVersion,
                                ChunkPartType chunkType, uint64_t copyChunkId,
                                uint32_t copyChunkVersion) {
	TRACETHIS();

	HddStats::gStatsOperationsDuplicate++;

	IChunk *originalChunk = hddChunkFindAndLock(chunkId, chunkType);

	if (originalChunk == ChunkNotFound) {
		safs::log_err("hddInternalDuplicate: Couldn't find original chunk, ID {}", chunkId);
		return SAUNAFS_ERROR_NOCHUNK;
	}

	// Owns the source chunk, and the copy once it is registered: every return
	// below unwinds whatever has been done so far unless commit() runs.
	ChunkCopyGuard guard(originalChunk);

	if (originalChunk->version() != chunkVersion && chunkVersion > 0) {
		return SAUNAFS_ERROR_WRONGVERSION;
	}
	if (copyChunkVersion == 0) {
		copyChunkVersion = chunkNewVersion;
	}

	IChunk *dupChunk = nullptr;
	int status = createChunkCopy(guard, chunkType, copyChunkId, copyChunkVersion, &dupChunk);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	sassert(dupChunk->chunkFormat() == originalChunk->chunkFormat());

	status = openSourceForCopy(guard, originalChunk, chunkVersion, chunkNewVersion, "duplicate");
	if (status != SAUNAFS_STATUS_OK) { return status; }

	ChunkCopyBuffers buffers;
	status = prepareChunkCopyForWriting(guard, originalChunk, dupChunk, "duplicate", &buffers);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	IDisk *dupDisk = dupChunk->owner();

	originalChunk->owner()->lseekData(originalChunk, dupChunk->getBlockOffset(0), SEEK_SET);

	// Read each original block and write it to the duplicated chunk
	const auto copyResult =
	    hddCopyChunkBlocks(originalChunk, dupChunk, originalChunk->blocks(),
	                       buffers.sourceCrc, buffers.copyCrc, "duplicate");

	if (copyResult != ChunkCopyResult::Ok) {
		if (copyResult == ChunkCopyResult::ReadFailed) {
			guard.markSourceAsDamaged();
		}
		return SAUNAFS_ERROR_IO;
	}

	status = guard.endSourceIo();
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(originalChunk);
		guard.markSourceAsDamaged();
		return status;
	}

	status = guard.endCopyIo();
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(dupChunk);
		return status;
	}

	dupChunk->setBlocks(originalChunk->blocks());
	dupDisk->setNeedRefresh(true);
	guard.commit();

	return SAUNAFS_STATUS_OK;
}

int hddInternalUpdateVersion(IChunk *chunk, uint32_t version,
                             uint32_t newversion) {
	TRACETHIS();
	int status;
	assert(chunk);
	if (chunk->version() != version && version > 0) {
		return SAUNAFS_ERROR_WRONGVERSION;
	}
	if (chunk->renameChunkFile(newversion) < 0) {
		hddAddErrorAndPreserveErrno(chunk);
		safs_silent_errlog(LOG_WARNING,
		                   "hddInternalUpdateVersion: file:%s - rename error",
		                   chunk->fullMetaFilename().c_str());
		return SAUNAFS_ERROR_IO;
	}
	status = hddIOBegin(chunk, 0, version);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
		safs_silent_errlog(LOG_WARNING,
		                   "hddInternalUpdateVersion: file:%s - open error",
		                   chunk->fullMetaFilename().c_str());
		return status;
	}
	status = chunk->owner()->overwriteChunkVersion(chunk, newversion);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
		safs_silent_errlog(LOG_WARNING,
		                   "hddInternalUpdateVersion: file:%s - write error",
		                   chunk->fullMetaFilename().c_str());
		hddIOEnd(chunk);
		return SAUNAFS_ERROR_IO;
	}
	status = hddIOEnd(chunk);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
	}
	return status;
}

int hddInternalUpdateVersion(uint64_t chunkId, uint32_t version,
                             uint32_t newversion, ChunkPartType chunkType) {
	TRACETHIS();

	HddStats::gStatsOperationsVersion++;

	auto *chunk = hddChunkFindAndLock(chunkId, chunkType);
	if (chunk == ChunkNotFound) {
		safs::log_err("hddInternalUpdateVersion: Couldn't find original chunk, ID {}", chunkId);
		return SAUNAFS_ERROR_NOCHUNK;
	}

	int status = hddInternalUpdateVersion(chunk, version, newversion);
	hddChunkRelease(chunk);

	return status;
}

static int hddInternalTruncate(uint64_t chunkId, ChunkPartType chunkType,
                               uint32_t oldVersion, uint32_t newVersion,
                               uint32_t length) {
	TRACETHIS4(chunkId, oldVersion, newVersion, length);
	int status;
	IChunk *chunk;
	uint32_t blocks;
	uint32_t crc;

	HddStats::gStatsOperationsTruncate++;

	if (length > SFSCHUNKSIZE) {
		return SAUNAFS_ERROR_WRONGSIZE;
	}

	chunk = hddChunkFindAndLock(chunkId, chunkType);

	// step 1 - change version
	if (chunk == ChunkNotFound) {
		safs::log_err("hddInternalTruncate: Couldn't find original chunk, ID {}", chunkId);
		return SAUNAFS_ERROR_NOCHUNK;
	}
	if (chunk->version() != oldVersion && oldVersion > 0) {
		hddChunkRelease(chunk);
		return SAUNAFS_ERROR_WRONGVERSION;
	}

	auto *disk = chunk->owner();
	uint8_t *blockBuffer = getChunkBlockBuffer() + kCrcSize;
	auto originalBlocks = chunk->blocks();

	if (chunk->renameChunkFile(newVersion) < 0) {
		hddAddErrorAndPreserveErrno(chunk);
		safs_silent_errlog(LOG_WARNING,
		                   "truncate: file:%s - rename error",
		                   chunk->fullMetaFilename().c_str());
		hddChunkRelease(chunk);
		return SAUNAFS_ERROR_IO;
	}

	status = hddIOBegin(chunk, 0, oldVersion);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
		hddChunkRelease(chunk);
		return status;  //can't change file version
	}

	status = disk->overwriteChunkVersion(chunk, newVersion);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
		safs_silent_errlog(LOG_WARNING,
		                   "truncate: file:%s - write error",
		                   chunk->fullMetaFilename().c_str());
		hddIOEnd(chunk);
		hddChunkRelease(chunk);
		return SAUNAFS_ERROR_IO;
	}
	chunk->setWasChanged(true);

	// step 2. truncate
	blocks = ((length + SFSBLOCKSIZE - 1) / SFSBLOCKSIZE);

	if (blocks > chunk->blocks()) {  //Expanding
		// Fill new blocks with empty CRC
		uint8_t *crcData = gOpenChunks.getResource(chunk->metaFD()).crcData();
		for (auto block = chunk->blocks(); block < blocks; block++) {
			memcpy(crcData + block * kCrcSize, &gEmptyBlockCrc, kCrcSize);
		}

		// Do the actual truncation to the aligned block size
		if (disk->ftruncateData(chunk,
		                        chunk->getFileSizeFromBlockCount(blocks)) < 0) {
			hddAddErrorAndPreserveErrno(chunk);
			safs_silent_errlog(LOG_WARNING,
			                   "truncate: file:%s - ftruncate error",
			                   chunk->fullDataFilename().c_str());
			hddIOEnd(chunk);
			hddChunkRelease(chunk);
			return SAUNAFS_ERROR_IO;
		}
	} else {  //Shrinking
		uint32_t fullBlocks = length / SFSBLOCKSIZE;
		uint32_t lastPartialBlockSize = length - fullBlocks * SFSBLOCKSIZE;

		if (lastPartialBlockSize > 0) {
			auto len = chunk->getFileSizeFromBlockCount(fullBlocks) +
			           lastPartialBlockSize;
			if (disk->ftruncateData(chunk, len) < 0) {
				hddAddErrorAndPreserveErrno(chunk);
				safs_silent_errlog(LOG_WARNING,
				    "truncate: file:%s - ftruncate error",
				    chunk->fullMetaFilename().c_str());
				hddIOEnd(chunk);
				hddChunkRelease(chunk);
				return SAUNAFS_ERROR_IO;
			}
		}

		if (disk->ftruncateData(chunk,
		                        chunk->getFileSizeFromBlockCount(blocks)) < 0) {
			hddAddErrorAndPreserveErrno(chunk);
			safs_silent_errlog(LOG_WARNING,
			                   "truncate: file:%s - ftruncate error",
			                   chunk->fullDataFilename().c_str());
			hddIOEnd(chunk);
			hddChunkRelease(chunk);
			return SAUNAFS_ERROR_IO;
		}

		// remove unneeded blocks
		if (disk->isZonedDevice()) {
			chunk->shrinkToBlocks(static_cast<uint16_t>(blocks));
		}

		if (lastPartialBlockSize > 0) {
			auto offset = chunk->getBlockOffset(fullBlocks);

			auto toBeRead =
			    disk->isZonedDevice() ? SFSBLOCKSIZE : lastPartialBlockSize;

			{
				DiskReadStatsUpdater updater(disk, toBeRead);

				// Check that we can read the truncated file
				if (disk->preadData(chunk, blockBuffer, toBeRead, offset) !=
				    static_cast<ssize_t>(toBeRead)) {
					hddAddErrorAndPreserveErrno(chunk);
					safs_silent_errlog(LOG_WARNING,
					                   "truncate: file:%s - read error",
					                   chunk->fullMetaFilename().c_str());
					hddIOEnd(chunk);
					hddChunkRelease(chunk);
					updater.markReadAsFailed();
					return SAUNAFS_ERROR_IO;
				}
			}

			HddStats::overheadRead(toBeRead);

			if (disk->isZonedDevice()) {
				memset(blockBuffer + lastPartialBlockSize, 0,
				       SFSBLOCKSIZE - lastPartialBlockSize);
			}

			crc = mycrc32_zeroexpanded(0, blockBuffer, lastPartialBlockSize,
			                           SFSBLOCKSIZE - lastPartialBlockSize);

			uint8_t crcBuff[kCrcSize];
			uint8_t* crcBuffPointer = crcBuff;
			put32bit(&crcBuffPointer, crc);

			uint8_t *crData = gOpenChunks.getResource(chunk->metaFD()).crcData();
			memcpy(crData + fullBlocks * kCrcSize, crcBuff, kCrcSize);

			uint32_t jump = disk->isZonedDevice() ? 2 : 1;

			for (auto block = fullBlocks + jump; block < originalBlocks;
			     block++) {
				memcpy(crData + block * kCrcSize, &gEmptyBlockCrc, kCrcSize);
			}

			if (disk->isZonedDevice()) {
				{
					DiskWriteStatsUpdater updater(disk, SFSBLOCKSIZE);

					int32_t retSize = SFSBLOCKSIZE;
					auto *crcData =
					    gOpenChunks.getResource(chunk->metaFD()).crcData();

					if (disk->writeChunkBlock(chunk, chunk->version(),
					                          fullBlocks, 0, SFSBLOCKSIZE, crc,
					                          crcData, blockBuffer) !=
					    SAUNAFS_STATUS_OK) {
						retSize = SAUNAFS_ERROR_IO;
					}

					if (retSize != SFSBLOCKSIZE) {
						hddAddErrorAndPreserveErrno(chunk);
						safs_silent_errlog(LOG_WARNING,
						    "truncate: file:%s - data write error",
						    chunk->fullMetaFilename().c_str());
						hddIOEnd(chunk);
						hddChunkRelease(chunk);
						updater.markWriteAsFailed();

						return SAUNAFS_ERROR_IO;
					}
				}

				HddStats::overheadWrite(SFSBLOCKSIZE);
			}
		}
	}

	if (chunk->blocks() != blocks) {
		disk->setNeedRefresh(true);
	}

	status = disk->setChunkBlocks(chunk, chunk->blocks(), blocks);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
		safs::log_warn("truncate: file:{} - set chunk blocks error (status {})",
		               chunk->fullMetaFilename(), status);
		hddIOEnd(chunk);
		hddChunkRelease(chunk);
		return status;
	}

	status = hddIOEnd(chunk);
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(chunk);
	}

	hddChunkRelease(chunk);

	return status;
}

static int hddInternalDuplicateTruncate(uint64_t chunkId, uint32_t chunkVersion,
                                        uint32_t chunkNewVersion,
                                        ChunkPartType chunkType,
                                        uint64_t copyChunkId,
                                        uint32_t copyChunkVersion,
                                        uint32_t copyChunkLength) {
	TRACETHIS();

	HddStats::gStatsOperationsDupTrunc++;

	if (copyChunkLength > SFSCHUNKSIZE) {
		return SAUNAFS_ERROR_WRONGSIZE;
	}

	IChunk *originalChunk = hddChunkFindAndLock(chunkId, chunkType);

	if (originalChunk == nullptr) {
		safs::log_err("hddInternalDuplicateTruncate: Couldn't find original chunk, ID {}", chunkId);
		return SAUNAFS_ERROR_NOCHUNK;
	}

	// Owns the source chunk, and the copy once it is registered: every return
	// below unwinds whatever has been done so far unless commit() runs.
	ChunkCopyGuard guard(originalChunk);

	if (originalChunk->version() != chunkVersion && chunkVersion > 0) {
		return SAUNAFS_ERROR_WRONGVERSION;
	}

	if (copyChunkVersion == 0) {
		copyChunkVersion = chunkNewVersion;
	}

	IChunk *dupChunk = nullptr;
	int status = createChunkCopy(guard, chunkType, copyChunkId, copyChunkVersion, &dupChunk);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = openSourceForCopy(guard, originalChunk, chunkVersion, chunkNewVersion, "duptrunc");
	if (status != SAUNAFS_STATUS_OK) { return status; }

	ChunkCopyBuffers buffers;
	status = prepareChunkCopyForWriting(guard, originalChunk, dupChunk, "duptrunc", &buffers);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	IDisk *dupDisk = dupChunk->owner();
	IDisk *origDisk = originalChunk->owner();

	const uint16_t blocks =
	    static_cast<uint16_t>((copyChunkLength + SFSBLOCKSIZE - 1) / SFSBLOCKSIZE);

	// Seek to the beginning of data block on both chunks
	if (!dupDisk->isZonedDevice()) {
		dupDisk->lseekData(dupChunk, dupChunk->getBlockOffset(0), SEEK_SET);
	}

	if (!origDisk->isZonedDevice()) {
		origDisk->lseekData(originalChunk, originalChunk->getBlockOffset(0),
		                    SEEK_SET);
	}

	// A misaligned shrink ends mid-block: that trailing block is copied on its
	// own below, with a CRC recomputed over the bytes that survive.
	const uint32_t lastBlockSize = copyChunkLength % SFSBLOCKSIZE;
	const bool isExpanding = blocks > originalChunk->blocks();
	const uint16_t blocksToCopy =
	    isExpanding ? originalChunk->blocks()
	                : static_cast<uint16_t>(lastBlockSize == 0 ? blocks : blocks - 1);

	const auto copyResult =
	    hddCopyChunkBlocks(originalChunk, dupChunk, blocksToCopy, buffers.sourceCrc,
	                       buffers.copyCrc, "duptrunc");

	if (copyResult != ChunkCopyResult::Ok) {
		if (copyResult == ChunkCopyResult::ReadFailed) {
			guard.markSourceAsDamaged();
		}
		return SAUNAFS_ERROR_IO;
	}

	status = finishTruncatedCopy(guard, originalChunk, dupChunk, blocks, lastBlockSize, buffers,
	                             "duptrunc");
	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = persistChunkCopyCrc(dupChunk, buffers, "duptrunc");
	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = guard.endSourceIo();
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(originalChunk);
		guard.markSourceAsDamaged();
		return status;
	}

	// Persist the new layout before ending dupChunk's IO, so it lands inside
	// the same fsync as the rest of the header (matches hddInternalTruncate).
	status = dupDisk->setChunkBlocks(dupChunk, originalChunk->blocks(), blocks);
	dupDisk->setNeedRefresh(true);

	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(dupChunk);
		return status;
	}

	status = guard.endCopyIo();
	if (status != SAUNAFS_STATUS_OK) {
		hddAddErrorAndPreserveErrno(dupChunk);
		return status;
	}

	guard.commit();

	return SAUNAFS_STATUS_OK;
}

int hddInternalDelete(IChunk *chunk, uint32_t version) {
	TRACETHIS();
	assert(chunk);
	if (chunk->version() != version && version > 0) {
		hddChunkRelease(chunk);
		return SAUNAFS_ERROR_WRONGVERSION;
	}
	if (chunk->owner()->unlinkChunk(chunk) != SAUNAFS_STATUS_OK) {
		uint8_t err = errno;
		hddAddErrorAndPreserveErrno(chunk);
		safs_silent_errlog(LOG_WARNING,
		                   "hddInternalDelete: file: %s - unlink error",
		                   chunk->fullMetaFilename().c_str());
		if (err == ENOENT) {
			hddDeleteChunkFromRegistry(chunk);
		} else {
			hddChunkRelease(chunk);
		}
		return SAUNAFS_ERROR_IO;
	}

	hddDeleteChunkFromRegistry(chunk);

	return SAUNAFS_STATUS_OK;
}

int hddInternalDelete(uint64_t chunkId, uint32_t version,
                      ChunkPartType chunkType) {
	TRACETHIS();

	HddStats::gStatsOperationsDelete++;

	auto *chunk = hddChunkFindAndLock(chunkId, chunkType);
	if (chunk == ChunkNotFound) {
		safs::log_err("hddInternalDelete: could not find chunkid {}", chunkId);
		return SAUNAFS_ERROR_NOCHUNK;
	}

	return hddInternalDelete(chunk, version);
}

int hddTruncate(uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
	uint32_t chunkNewVersion, uint32_t length) {
	return hddInternalTruncate(chunkId, chunkType, chunkVersion, chunkNewVersion, length);
}

int hddDuplicate(uint64_t chunkId, uint32_t chunkVersion, uint32_t chunkNewVersion,
                 ChunkPartType chunkType, uint64_t copyChunkId, uint32_t copyChunkVersion) {
	return hddInternalDuplicate(chunkId, chunkVersion, chunkNewVersion, chunkType, copyChunkId,
	                            copyChunkVersion);
}

int hddDuplicateTruncate(uint64_t chunkId, uint32_t chunkVersion, uint32_t chunkNewVersion,
                         ChunkPartType chunkType, uint64_t copyChunkId, uint32_t copyChunkVersion,
                         uint32_t length) {
	return hddInternalDuplicateTruncate(chunkId, chunkVersion, chunkNewVersion, chunkType,
	                                    copyChunkId, copyChunkVersion, length);
}

static UniqueQueue<ChunkWithVersionAndType> gTestChunkQueue;

static void hddTestChunkThread() {
	pthread_setname_np(pthread_self(), "testChunkThread");

	bool terminate = false;

	while (!terminate) {
		Timeout time(std::chrono::seconds(1));

		try {
			ChunkWithVersionAndType chunk = gTestChunkQueue.get();
			std::string name = chunk.toString();
			if (hddInternalTestChunk(chunk.id, chunk.version, chunk.type) !=
			    SAUNAFS_STATUS_OK) {
				safs_pretty_syslog(LOG_NOTICE,
				                   "Chunk %s corrupted (detected by a client)",
				                   name.c_str());
				hddReportDamagedChunk(chunk.id, chunk.type);
			} else {
				safs_pretty_syslog(LOG_NOTICE,
				                   "Chunk %s spuriously reported as corrupted",
				                   name.c_str());
			}
		} catch (UniqueQueueEmptyException &) {
			// hooray, nothing to do
		}

		// rate-limit to 1/sec
		usleep(time.remaining_us());
		terminate = gTerminate;
	};
}

void hddAddChunkToTestQueue(ChunkWithVersionAndType chunk) {
	gTestChunkQueue.put(chunk);
}

void hddTesterThread() {
	TRACETHIS();

	pthread_setname_np(pthread_self(), "testerThread");

	IChunk *chunk = ChunkNotFound;
	uint64_t chunkId = 0;
	uint32_t version = 0;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	uint32_t elapsedTimeMs = 0;
	uint64_t startMicroSecs = 0;
	uint64_t endMicroSecs = 0;
	std::string chunkName;

	while (!gTerminate) {
		startMicroSecs = getMicroSecsTime();
		chunk = ChunkNotFound;

		{
			// We could use a scoped_lock here, but helgrind complains about it
			std::lock_guard gDisksLock(gDisksMutex);
			std::lock_guard gChunksMapLock(gChunksMapMutex);
			std::lock_guard gTestsLock(gTestsMutex);

			bool testerResetExpected = true;
			if (gResetTester.compare_exchange_strong(testerResetExpected,
			                                         false)) {
				gDiskManager->resetDiskIteratorForTests();
				elapsedTimeMs = 0;
			}

			chunk = gDiskManager->getChunkToTest(elapsedTimeMs);

			if (chunk != ChunkNotFound) {
				chunkId = chunk->id();
				version = chunk->version();
				chunkType = chunk->type();
				chunkName = chunk->fullDataFilename();
			}
		}

		if (chunk != ChunkNotFound) {
			if (hddInternalTestChunk(chunkId, version, chunkType) !=
			    SAUNAFS_STATUS_OK) {
				hddReportDamagedChunk(chunkId, chunkType);
			} else {
				safs_pretty_syslog(LOG_DEBUG,
				                   "Tester: chunk: %lu, v: %u, type: %s, file: "
				                   "%s: tested (OK)",
				                   chunkId, version, chunkType.toString().c_str(),
				                   chunkName.c_str());
			}
		}

		endMicroSecs = getMicroSecsTime();

		if (endMicroSecs > startMicroSecs) {
			unsigned usToSleep =
			    1000 * std::min(gHDDTestFreq_ms.load(), kMaxTestFreqMs);
			endMicroSecs -= startMicroSecs;

			if (endMicroSecs < usToSleep) { usleep(usToSleep - endMicroSecs); }
		}
	}
}

void hddDiskRandomizeChunksForTests(IDisk *disk) {
	TRACETHIS();

	std::lock_guard testsLockGuard(gTestsMutex);
	safs_pretty_syslog(LOG_NOTICE, "Randomizing chunks for disk: %s",
	                   disk->getPaths().c_str());
	disk->chunks().shuffle();
}

/* initialization */

static inline void hddAddChunkFromDiskScan(IDisk *disk,
                                           const std::string &fullname,
                                           uint64_t chunkId, uint32_t version,
                                           ChunkPartType chunkType) {
	TRACETHIS();

	auto *chunk = hddChunkFindOrCreatePlusLock(
	    disk, chunkId, chunkType, disk::ChunkGetMode::kFindOrCreate);

	if (chunk == ChunkNotFound) {
		safs_pretty_syslog(LOG_ERR, "Can't use file %s as chunk",
		                   fullname.c_str());
		return;
	}

	bool isNewChunk = chunk->metaFilename().empty();

	if (!isNewChunk) {
		// already have this chunk
		if (version <= chunk->version()) {
			// current chunk is older
			if (!disk->isReadOnly()) {
				unlink(fullname.c_str());
			}
			hddChunkRelease(chunk);
			return;
		}

		if (!disk->isReadOnly()) {
			chunk->owner()->unlinkChunk(chunk);
		}
	}

	if (!isNewChunk) {
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		chunk = hddRecreateChunk(disk, chunk, chunkId, chunkType);
	}

	chunk->setVersion(version);
	sassert(chunk->fullMetaFilename() == fullname);

	int attrStatus = hddUpdateChunkAttributesWithRetry(disk, chunk, true);
	if (attrStatus != SAUNAFS_STATUS_OK) {
		safs::log_warn("hddAddChunkFromDiskScan: could not read attributes for {} (status {})",
		               fullname, attrStatus);
	}

	// Attributes are re-read on first access, whether or not the scan read them.
	chunk->setValidAttr(0);

	{
		std::lock_guard testsLockGuard(gTestsMutex);
		disk->chunks().insert(chunk);
	}

	if (isNewChunk) {
		hddReportNewChunkToMaster(chunk->id(), chunk->version(),
		                          chunk->owner()->isMarkedForDeletion(),
		                          chunk->type());
	}

	hddChunkRelease(chunk);
}

/// Registers the chunks of disks which synthesize their chunk list instead
/// of scanning directories (e.g. mock disks used for tests). All registry
/// mutation happens here, so such disks never touch the registry directly.
/// Returns true if the disk handled the scan itself.
static bool hddScanSyntheticChunks(IDisk *disk) {
	IDisk::SyntheticChunkSink sink;

	sink.reserve = [](uint64_t expectedChunkCount) {
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		gChunksMap.reserve(gChunksMap.size() + expectedChunkCount);
	};

	auto totalInserted = std::make_shared<uint64_t>(0);
	auto totalSkipped = std::make_shared<uint64_t>(0);

	sink.emitBulk = [disk, totalInserted,
	                 totalSkipped](std::vector<ChunkWithVersionAndType> &&bulk) {
		// Values rather than pointers: a chunk becomes reachable the moment it
		// turns Available, so once the locks are dropped a delete ordered by the
		// master, which an on-demand location query can prompt as soon as the
		// chunk is in the registry, may destroy it. Reporting from the entries
		// keeps the loop below off freed memory.
		std::vector<ChunkWithVersionAndType> insertedChunks;
		insertedChunks.reserve(bulk.size());

		{
			// Both containers are populated before anything is published, in the
			// order hddSendDataToMaster established. Nested guards rather than a
			// scoped_lock, matching the disk-test loop above: helgrind complains
			// about the latter.
			std::lock_guard chunksMapLockGuard(gChunksMapMutex);
			std::lock_guard testsLockGuard(gTestsMutex);
			for (const auto &entry : bulk) {
				const auto key = makeChunkKey(entry.id, entry.type);
				if (gChunksMap.find(key) != gChunksMap.end()) {
					// Already known, e.g. from a rescan after reload
					++(*totalSkipped);
					continue;
				}
				auto *chunk = disk->instantiateNewConcreteChunk(entry.id, entry.type);
				passert(chunk);
				gChunksMap.insert({key, std::unique_ptr<IChunk>(chunk)});
				hddNotePresentChunkType(entry.type);
				chunk->setVersion(entry.version);
				disk->updateChunkAttributes(chunk, true);
				disk->chunks().insert(chunk);
				chunk->setState(ChunkState::Available);
				insertedChunks.push_back(entry);
			}
		}

		for (const auto &entry : insertedChunks) {
			hddReportNewChunkToMaster(entry.id, entry.version, disk->isMarkedForDeletion(),
			                          entry.type);
		}

		*totalInserted += insertedChunks.size();
	};

	sink.isTerminating = [disk]() {
		std::lock_guard disksLockGuard(gDisksMutex);
		return disk->scanState() == IDisk::ScanState::kTerminate;
	};

	const bool handled = disk->scanSyntheticChunks(sink);

	if (handled) {
		safs::log_info("synthetic scan of disk {}: {} chunks registered, {} already known",
		               disk->getPaths().c_str(), *totalInserted, *totalSkipped);
	}

	return handled;
}

/// Scans the Disk for new Chunks in bulks of 1000 Chunks
void hddDiskScan(IDisk *disk, uint32_t beginTime) {
	std::unique_lock uniqueLock(gDisksMutex);
	IDisk::ScanState scanState = disk->scanState();
	uniqueLock.unlock();

	if (scanState == IDisk::ScanState::kTerminate) {
		return;
	}

	if (hddScanSyntheticChunks(disk)) {
		gHddSpaceChanged = true;  // report chunk count to master
		safs::log_info("scanning disk {}: synthetic scan complete ({}s)", disk->getPaths().c_str(),
		               static_cast<uint32_t>(time(nullptr)) - beginTime);
		hddVerifyPresentChunkTypes();
		return;
	}

	DIR *dd;
	struct dirent *dirEntry;
	uint32_t totalCheckCount = 0;
	uint8_t lastPercent = 0, currentPercent = 0;
	bool terminateScan = false;
	uint32_t lastTime = time(nullptr), currentTime;

	for (unsigned subfolderNumber = 0;
	     subfolderNumber < Subfolder::kNumberOfSubfolders && !terminateScan;
	     ++subfolderNumber) {
		std::string subfolderPath = disk->metaPath()
		    + Subfolder::getSubfolderNameGivenNumber(subfolderNumber) + "/";
		dd = opendir(subfolderPath.c_str());
		if (!dd) {
			continue;
		}

		while (!terminateScan) {
			dirEntry = readdir(dd);
			if (!dirEntry) {
				break;
			}

			const std::string filename = dirEntry->d_name;
			ChunkFilenameParser filenameParser(filename);

			if (filenameParser.parse() != ChunkFilenameParser::Status::OK) {
				if (filename != "." && filename != ".." &&
				    filename.find(CHUNK_DATA_FILE_EXTENSION) ==
				        std::string::npos) {
					safs_pretty_syslog(LOG_WARNING,
					                   "Invalid file %s placed in chunks "
					                   "directory %s; skipping it.",
					                   dirEntry->d_name, subfolderPath.c_str());
				}
				continue;
			}

			if (Subfolder::getSubfolderNumber(filenameParser.chunkId()) !=
			    subfolderNumber) {
				safs_pretty_syslog(LOG_WARNING,
				    "Chunk %s%s placed in a wrong directory; skipping it.",
				    subfolderPath.c_str(), dirEntry->d_name);
				continue;
			}

			std::string chunkName = dirEntry->d_name;

			if(chunkName.empty()) {
				continue;
			}

			hddAddChunkFromDiskScan(
			    disk, subfolderPath + chunkName, filenameParser.chunkId(),
			    filenameParser.chunkVersion(), filenameParser.chunkType());

			totalCheckCount++;

			if (totalCheckCount >= 1000) {
				uniqueLock.lock();

				if (disk->scanState() == IDisk::ScanState::kTerminate) {
					terminateScan = true;
				}

				uniqueLock.unlock();

				totalCheckCount = 0;
			}
		}

		closedir(dd);

		currentTime = time(nullptr);

		static constexpr float kMaxSubfolderFloat = 256.0f;
		currentPercent = (subfolderNumber * 100.0) / kMaxSubfolderFloat;

		if (currentPercent > lastPercent && currentTime > lastTime) {
			lastPercent = currentPercent;
			lastTime = currentTime;

			uniqueLock.lock();
			disk->setScanProgress(currentPercent);
			uniqueLock.unlock();

			gHddSpaceChanged = true;  // report chunk count to master

			safs_pretty_syslog(
			    LOG_NOTICE, "scanning disk %s: %" PRIu8 "%% (%" PRIu32 "s)",
			    disk->getPaths().c_str(), lastPercent, currentTime - beginTime);
		}
	}

	if (disk->isZonedDevice()) {
		// Check for dirty zones and update conventional zones' write head
		disk->updateAfterScan();
	}
}

void hddDiskScanThread(IDisk *disk) {
	TRACETHIS();
	uint32_t beginTime = static_cast<uint32_t>(time(nullptr));

	gScansInProgress++;

	{
		std::lock_guard disksLockGuard(gDisksMutex);
		disk->refreshDataDiskUsage();
	}

	gHddSpaceChanged = true;
	ChunkTrashManager::registerDiskPath(disk->metaPath());
	if (!disk->isZonedDevice() && disk->metaPath() != disk->dataPath()) {
		ChunkTrashManager::registerDiskPath(disk->dataPath());
	}
	hddDiskScan(disk, beginTime);
	hddDiskRandomizeChunksForTests(disk);
	gScansInProgress--;

	// Before taking gDisksMutex: this acquires gChunksMapMutex, and no other
	// path holds the disks lock while waiting for the chunk map one.
	hddVerifyPresentChunkTypes();

	std::lock_guard disksLockGuard(gDisksMutex);

	if (disk->scanState() == IDisk::ScanState::kTerminate) {
		safs_pretty_syslog(LOG_NOTICE, "scanning disk %s: interrupted",
		                   disk->getPaths().c_str());
	} else {
		safs_pretty_syslog(LOG_NOTICE,
		                   "scanning disk %s: complete (%" PRIu32 "s)",
		                   disk->getPaths().c_str(),
		                   static_cast<uint32_t>(time(nullptr)) - beginTime);
	}

	disk->setScanState(IDisk::ScanState::kThreadFinished);
	disk->setScanProgress(100);
}

void hddDisksThread() {
	TRACETHIS();

	pthread_setname_np(pthread_self(), "disksThread");

	while (!gTerminate) {
		hddCheckDisks();
		sleep(1);
	}
}

void hddFreeResourcesThread() {
	static const int kDelayedStep = 2;
	static const uint32_t kOldIoBuffersExpirationTimeMs = kDelayedStep * 1000;
	static const int kMaxFreeUnused = 1024;
	TRACETHIS();

	pthread_setname_np(pthread_self(), "freeResThread");

	while (!gTerminate) {
		Timeout timeout(std::chrono::duration_cast<std::chrono::microseconds>(
		    std::chrono::seconds(kDelayedStep)));
		gOpenChunks.freeUnused(eventloop_time(), gChunksMapMutex, kMaxFreeUnused);
		ChunkTrashManager::collectGarbage();
		hddReleaseDisksToBeDeleted();
		/// Release buffers older than kDelayedStep seconds
		releaseOldIoBuffers(kOldIoBuffersExpirationTimeMs);

		usleep(timeout.remaining_us());
	}
}

void hddTerminate(void) {
	TRACETHIS();

	// if gTerminate is true here, then it means that threads have not been
	// started, so do not join with them
	uint32_t terminate = gTerminate.exchange(true);

	// Request plugins to cleanup before removing Chunks and Disks
	pluginManager.cleanupPlugins();

	if (terminate == 0) {
		gTesterThread.join();
		gDisksThread.join();
		gDelayedThread.join();

		ChunkTrashManager::terminate();

		try {
			gChunkTesterThread.join();
		} catch (std::system_error &e) {
			safs_pretty_syslog(
			    LOG_NOTICE, "Failed to join test chunk thread: %s", e.what());
		}
	}

	{
		std::lock_guard disksLockGuard(gDisksMutex);
		terminate = 0;

		for (auto &disk : gDisks) {
			if (disk->scanState() == IDisk::ScanState::kInProgress) {
				disk->setScanState(IDisk::ScanState::kTerminate);
			}
			if (disk->scanState() == IDisk::ScanState::kTerminate
			    || disk->scanState() == IDisk::ScanState::kThreadFinished) {
				terminate++;
			}
		}
	}

	while (terminate > 0) {
		usleep(10000); // not very elegant solution.

		std::lock_guard disksLockGuard(gDisksMutex);

		for (auto &disk : gDisks) {
			if (disk->scanState() == IDisk::ScanState::kThreadFinished) {
				disk->scanThread().join();
				// any state - to prevent calling join again
				disk->setScanState(IDisk::ScanState::kWorking);
				terminate--;
			}
		}
	}

	for (auto &chunkEntry : gChunksMap) {
		IChunk *chunk = chunkEntry.second.get();

		if (chunk->state() == ChunkState::Available) {
			if (chunk->wasChanged()) {
				safs_pretty_syslog(LOG_WARNING, "hddTerminate: CRC not flushed "
				                   "- writing now");

				if (chunkWriteCrc(chunk) != SAUNAFS_STATUS_OK) {
					safs_silent_errlog(LOG_WARNING,
					                   "hddTerminate: file: %s - write error",
					                   chunk->fullMetaFilename().c_str());
				}
			}
			gOpenChunks.purge(chunk->metaFD());
		} else {
			safs::log_warn("hddTerminate: locked chunk !!! (chunkid: {:#04x}, "
			               "chunktype: {})",
			               chunk->id(),
			               chunk->type().toString());
		}
	}

	// Delete chunks even not in AVAILABLE state here, as all threads using
	// chunk objects should already be joined (by this function and other
	// cleanup functions of other chunkserver modules that are registered on
	// eventloop termination) This function should always be executed after all
	// other chunkserver modules' (that use chunk objects) cleanup functions
	// were executed.
	gChunksMap.clear();
	gPresentChunkTypes.clear();
	gOpenChunks.freeUnused(eventloop_time(), gChunksMapMutex);
	gDisks.clear();
	// Destroy the disks-to-be-deleted related structures
	gNewDisksToBeDeletedWithPendingChunks.clear();
	gDisksToBeDeletedWithPendingChunks.clear();
}

void hddReload(void) {
	TRACETHIS();

	gAdviseNoCache = cfg_getuint32("HDD_ADVISE_NO_CACHE", 0);
	gPerformFsync = cfg_getuint32("PERFORM_FSYNC", 1);

	gStatChunksAtDiskScan = cfg_getuint8("STAT_CHUNKS_AT_DISK_SCAN", 1) != 0U;
	if (!gStatChunksAtDiskScan) {
		safs::log_warn(
		    "hdd space manager: STAT_CHUNKS_AT_DISK_SCAN = {} (disabled): the "
		    "chunk data files will not be checked at scan time, make sure they "
		    "have not changed.",
		    static_cast<uint8_t>(gStatChunksAtDiskScan));
	}

	gHDDTestFreq_ms =
	    cfg_ranged_get("HDD_TEST_FREQ", 10., 0.001, 1000000.) * 1000;
	gCheckCrcWhenReading = cfg_getuint8("HDD_CHECK_CRC_WHEN_READING", 1) != 0U;
	gCheckCrcWhenWriting = cfg_getuint8("HDD_CHECK_CRC_WHEN_WRITING", 1) != 0U;
	gPunchHolesInFiles = cfg_getuint32("HDD_PUNCH_HOLES", 0);

	hddReloadChunkBulkSize(true);

	char *leaveFreeStr = cfg_getstr("HDD_LEAVE_SPACE_DEFAULT",
	                                disk::gLeaveSpaceDefaultDefaultStrValue);
	auto parsedLeaveFree = cfg_parse_size(leaveFreeStr);

	if (parsedLeaveFree < 0) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "hdd space manager: HDD_LEAVE_SPACE_DEFAULT parse "
		                   "error - left unchanged");
	} else {
		disk::gLeaveFree = parsedLeaveFree;
	}

	free(leaveFreeStr);

	if (disk::gLeaveFree < static_cast<int64_t>(SFSCHUNKSIZE)) {
		safs_pretty_syslog(LOG_NOTICE,
		    "hdd space manager: HDD_LEAVE_SPACE_DEFAULT < chunk size - leaving "
		    "so small space on hdd is not recommended");
	}

	safs_pretty_syslog(LOG_NOTICE,"reloading hdd data ...");

	try {
		gDiskManager->reloadConfiguration();
		gDiskManager->reloadDisksFromCfg();
		ChunkTrashManager::reloadConfig();
	} catch (const Exception& ex) {
		safs_pretty_syslog(LOG_ERR, "%s", ex.what());
	}

	// Request plugins to reload
	pluginManager.reloadPlugins();
}

int hddLateInit() {
	TRACETHIS();
	gTerminate = false;
	gTesterThread = std::thread(hddTesterThread);
	gDisksThread = std::thread(hddDisksThread);
	gDelayedThread = std::thread(hddFreeResourcesThread);

	try {
		gChunkTesterThread = std::thread(hddTestChunkThread);
	} catch (std::system_error &e) {
		safs_pretty_syslog(LOG_ERR, "Failed to create test chunk thread: %s",
		                   e.what());
		abort();
	}

	return 0;
}

/// Initializes the default disk manager and reads the disk manager type from
/// configuration. This function must be called before plugins initialization.
int initDiskManager() {
	// Initialize the default disk manager and set the disk manager type.
	// The default will be overwritten if the DISK_MANAGER_TYPE is set in
	// the configuration file.
	gDiskManager = std::make_unique<DefaultDiskManager>();

	std::string diskManagerType = cfg_get("DISK_MANAGER_TYPE", "default");
	std::transform(diskManagerType.begin(), diskManagerType.end(),
	               diskManagerType.begin(), ::tolower);
	gDiskManagerType = std::move(diskManagerType);

	return SAUNAFS_STATUS_OK;
}

int loadPlugins() {
	const std::array<std::string, 5> pluginPaths = {
	    cfg_getstring("PLUGINS_DIR", ""),              // Higher priority (user-defined)
	    PLUGINS_PATH "/chunkserver",                   // Build-time defined path
	    BUILD_PATH "/plugins/chunkserver",             // Build tree (for development)
	    "/usr/local/lib/saunafs/plugins/chunkserver",  // Local install
	    "/usr/lib/saunafs/plugins/chunkserver"         // Standard install
	};

	for (const auto &path : pluginPaths) {
		if (path.empty()) { continue; }

		if (!pluginManager.loadPlugins(path)) {
			safs::log_debug("PluginManager: No plugins loaded from: {}", path);
		} else {
			safs::log_info("PluginManager: Plugins loaded from: {}", path);
			// stop after the first successful load, we don't want to load from multiple dirs
			break;
		}
	}

	pluginManager.showLoadedPlugins();

	return SAUNAFS_STATUS_OK;
}

int hddInit() {
	TRACETHIS();

	initializeEmptyBlockCrcForDisks();

	gPerformFsync = cfg_getuint32("PERFORM_FSYNC", 1);

	gStatChunksAtDiskScan = cfg_getuint8("STAT_CHUNKS_AT_DISK_SCAN", 1) != 0U;
	if (!gStatChunksAtDiskScan) {
		safs::log_warn(
		    "hdd space manager: STAT_CHUNKS_AT_DISK_SCAN = {} (disabled): the "
		    "chunk data files will not be checked at scan time, make sure they "
		    "have not changed.",
		    static_cast<uint8_t>(gStatChunksAtDiskScan));
	}

	int64_t leaveSpaceDefaultDefaultValue =
	    cfg_parse_size(disk::gLeaveSpaceDefaultDefaultStrValue);
	sassert(leaveSpaceDefaultDefaultValue > 0);

	char *leaveFreeStr = cfg_getstr("HDD_LEAVE_SPACE_DEFAULT",
	                                disk::gLeaveSpaceDefaultDefaultStrValue);
	auto parsedLeaveFree = cfg_parse_size(leaveFreeStr);

	if (parsedLeaveFree < 0) {
		safs_pretty_syslog(
		    LOG_WARNING,
		    "%s: HDD_LEAVE_SPACE_DEFAULT parse error - using default (%s)",
		    cfg_filename().c_str(), disk::gLeaveSpaceDefaultDefaultStrValue);
		disk::gLeaveFree = leaveSpaceDefaultDefaultValue;
	} else {
		disk::gLeaveFree = parsedLeaveFree;
	}

	free(leaveFreeStr);

	if (disk::gLeaveFree < static_cast<int64_t>(SFSCHUNKSIZE)) {
		safs_pretty_syslog(LOG_WARNING,
		                   "%s: HDD_LEAVE_SPACE_DEFAULT < chunk size - "
		                   "leaving so small space on hdd is not recommended",
		                   cfg_filename().c_str());
	}

	try {
		gDiskManager->reloadConfiguration();
		gDiskManager->reloadDisksFromCfg();
		ChunkTrashManager::init();
	} catch (const Exception& ex) {
		safs_pretty_syslog(LOG_ERR, "%s", ex.what());
	}

	{
		std::lock_guard disksLockGuard(gDisksMutex);
		for (const auto &disk: gDisks) {
			safs_pretty_syslog(LOG_INFO, "hdd space manager: disk to scan: %s",
			                   disk->getPaths().c_str());
			if (disk->isDamaged() || disk->isMarkedForDeletion() || disk->isReadOnly()) {
				safs_pretty_syslog(LOG_WARNING,
				                   "hdd space manager: disk %s is damaged, "
				                   "marked for deletion or read-only",
				                   disk->getPaths().c_str());
				continue;
			}
		}
	}

	safs_pretty_syslog(LOG_INFO,
	                   "hdd space manager: start background hdd scanning "
	                   "(searching for available chunks)");

	gAdviseNoCache = cfg_getuint32("HDD_ADVISE_NO_CACHE", 0);
	gHDDTestFreq_ms =
	    cfg_ranged_get("HDD_TEST_FREQ", 10., 0.001, 1000000.) * 1000;
	gCheckCrcWhenReading = cfg_getuint8("HDD_CHECK_CRC_WHEN_READING", 1) != 0U;
	gCheckCrcWhenWriting = cfg_getuint8("HDD_CHECK_CRC_WHEN_WRITING", 1) != 0U;

	gPunchHolesInFiles = cfg_getuint32("HDD_PUNCH_HOLES", 0);

	hddReloadChunkBulkSize(false);

	eventloop_reloadregister(hddReload);
	eventloop_timeregister(TIMEMODE_RUN_LATE, SECONDS_IN_ONE_MINUTE, 0,
	                       hddDiskInfoRotateStats);
	eventloop_destructregister(hddTerminate);

	gTerminate = true;

	return 0;
}
