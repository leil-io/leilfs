/*
   Copyright 2023 Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <utility>

#include "chunkserver-common/chunk_map.h"
#include "chunkserver-common/disk_interface.h"
#include "chunkserver-common/disk_manager_interface.h"
#include "chunkserver-common/hdd_utils.h"
#include "chunkserver-common/indexed_resource_pool.h"
#include "chunkserver-common/iostat.h"
#include "chunkserver-common/open_chunk.h"
#include "chunkserver-common/plugin_manager.h"
#include "slogger/slogger.h"

inline IndexedResourcePool<OpenChunk> gOpenChunks;

/// Protects access to the list of chunks of every Disk. This list contains the
/// chunks to be tested.
inline std::mutex gTestsMutex;

/// Maximum frequency for chunk testing in milliseconds.
constexpr uint32_t kMaxTestFreqMs = 1000U;

/// Frequency for chunk testing in milliseconds.
/// Can be changed via the configuration file (HDD_TEST_FREQ).
inline std::atomic<unsigned> gHDDTestFreq_ms(10 * 1000);

/// Global unordered_map of all chunks stored in this chunkserver.
inline ChunkMap gChunksMap;

/// Only guards access to gChunksMap.
/// Chunk objects stored in the registry have their own separate locks.
inline std::mutex gChunksMapMutex;

/// Pull-sweep cursor and outstanding keys. Guard every access with gChunksMapMutex.
struct RegistrationSweepState {
	std::size_t bucket{0};
	std::size_t bucketCount{0};
	bool cursorDone{false};
	/// Next entry in this bucket; insertion, removal, and rehash hooks keep it valid.
	std::optional<ChunkMap::local_iterator> bucketPosition;
	std::deque<ChunkWithType> pending;
	std::unordered_set<ChunkWithType, KeyOperations, KeyOperations> pendingKeys;

	void enqueue(ChunkWithType key) {
		if (pendingKeys.insert(key).second) { pending.push_back(key); }
	}

	void restartAfterRehash(std::size_t newBucketCount) {
		if (!cursorDone && bucketCount != newBucketCount) {
			bucketCount = newBucketCount;
			bucket = 0;
			bucketPosition.reset();
		}
	}
};

inline RegistrationSweepState gRegistrationSweepState;

/// Call after inserting or invalidating a registry entry, under gChunksMapMutex.
inline void hddNoteChunkForRegistration(ChunkWithType key) {
	auto &sweep = gRegistrationSweepState;
	if (!gRegistrationSweepActive.load(std::memory_order_relaxed)) { return; }
	// Notice rehashes immediately, even if another resize precedes the next credit.
	sweep.restartAfterRehash(gChunksMap.bucket_count());
	// Insertion can precede a saved position even in the current bucket.
	if (sweep.cursorDone || gChunksMap.bucket(key) <= sweep.bucket) { sweep.enqueue(key); }
}

/// Call before erasing a registry entry, under gChunksMapMutex.
inline void hddNoteChunkRemovalForRegistration(ChunkWithType key) {
	auto &sweep = gRegistrationSweepState;
	if (!gRegistrationSweepActive.load(std::memory_order_relaxed)) { return; }
	sweep.restartAfterRehash(gChunksMap.bucket_count());
	if (sweep.bucketPosition && *sweep.bucketPosition != gChunksMap.end(sweep.bucket) &&
	    KeyOperations{}((*sweep.bucketPosition)->first, key)) {
		++*sweep.bucketPosition;
	}
}

/// Number of chunks per ChunkPartType currently present in gChunksMap.
/// Maintained at every gChunksMap insert/erase, under gChunksMapMutex.
/// Lets id-only lookups (e.g. the master's on-demand chunk-location query)
/// probe one (id, type) key per hosted type instead of scanning the whole
/// type space. In practice it holds a handful of entries.
inline std::map<ChunkPartType, uint64_t> gPresentChunkTypes;

/// Registers one more chunk of \p type. Call under gChunksMapMutex, next to
/// the gChunksMap insert.
inline void hddNotePresentChunkType(ChunkPartType type) { ++gPresentChunkTypes[type]; }

/// Unregisters one chunk of \p type. Call under gChunksMapMutex, next to the
/// gChunksMap erase.
inline void hddForgetPresentChunkType(ChunkPartType type) {
	auto typeIterator = gPresentChunkTypes.find(type);
	if (typeIterator != gPresentChunkTypes.end() && --(typeIterator->second) == 0) {
		gPresentChunkTypes.erase(typeIterator);
	}
}

/// Registry mutation and its indexes form one operation under gChunksMapMutex.
inline auto hddRegistryInsert(ChunkWithType key, std::unique_ptr<IChunk> chunk) {
	auto result = gChunksMap.emplace(key, std::move(chunk));
	if (result.second) {
		hddNotePresentChunkType(key.type);
		hddNoteChunkForRegistration(key);
	}
	return result;
}

inline auto hddRegistryErase(ChunkMap::iterator entry) {
	hddNoteChunkRemovalForRegistration(entry->first);
	hddForgetPresentChunkType(entry->first.type);
	return gChunksMap.erase(entry);
}

/// Logs and asserts if gPresentChunkTypes totals differ from gChunksMap.size().
/// Detects missing type-count updates that can hide chunks from id-only queries.
/// Acquires gChunksMapMutex; callers must not already hold it.
inline void hddVerifyPresentChunkTypes() {
	uint64_t countedChunks = 0;
	uint64_t registrySize = 0;
	// Captured in the same locked snapshot as the totals: reading it afterwards
	// would race with the scan and worker threads mutating the index.
	std::size_t typeCount = 0;
	{
		const std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		for (const auto &[type, count] : gPresentChunkTypes) { countedChunks += count; }
		registrySize = gChunksMap.size();
		typeCount = gPresentChunkTypes.size();
	}

	if (countedChunks != registrySize) {
		safs::log_err(
		    "gPresentChunkTypes out of sync with the chunk registry: {} chunks counted across {} "
		    "types, {} chunks in the registry -- an insert or erase path is missing its "
		    "hddNotePresentChunkType/hddForgetPresentChunkType call",
		    countedChunks, typeCount, registrySize);
		assert(false && "gPresentChunkTypes out of sync with gChunksMap");
	}
}

inline const int kOpenRetryCount = 4;
inline const int kOpenRetry_ms = 5;

inline bool gPunchHolesInFiles;

/// The collection of data Disks (directories where chunks are stored).
/// Protected by gDisksMutex.
inline std::vector<std::unique_ptr<IDisk>> gDisks;

/// Protects gDisks + all data in structures (except Disk::cstat)
inline std::mutex gDisksMutex;

/// Configuration variable to define the type of DiskManager to be used.
/// The default is an instance of DefaultDiskManager, but can be be changed from
/// plugins offering custom implementations of IDiskManager.
inline std::string gDiskManagerType = "default";

/// The DiskManager instance to be used by the Chunkserver.
inline std::unique_ptr<IDiskManager> gDiskManager;

/// Container to reuse free condition variables (guarded by `gChunksMapMutex`)
inline std::vector<std::unique_ptr<CondVarWithWaitCount>> gFreeCondVars;

/// Disk scans that have been scheduled and have not completed yet.
///
/// Count a scan before its worker starts so an empty registry cannot be
/// mistaken for a completed startup scan.
inline std::atomic_int gScansInProgress(0);

/// Scheduled scans, including workers not started yet. Guard with gDisksMutex.
/// Tracking the disk makes scheduling and settlement idempotent across reloads.
inline std::unordered_set<const IDisk *> gScheduledDiskScans;

/// Call with gDisksMutex held, after updating the disk's damage state.
inline void hddScanScheduled(const IDisk *disk) {
	if (!disk->isDamaged() && gScheduledDiskScans.insert(disk).second) {
		gScansInProgress.fetch_add(1, std::memory_order_relaxed);
	}
}

/// Call with gDisksMutex held when a worker finishes or a scheduled scan is cancelled.
inline void hddScanSettled(const IDisk *disk) {
	if (gScheduledDiskScans.erase(disk) != 0) {
		gScansInProgress.fetch_sub(1, std::memory_order_release);
	}
}

inline std::atomic_bool gStatChunksAtDiskScan{true};

inline std::atomic_bool gPerformFsync(true);

inline std::atomic_bool gCheckCrcWhenWriting{true};

/// Value of HDD_ADVISE_NO_CACHE from config
inline std::atomic_bool gAdviseNoCache = false;

inline IoStat gIoStat;

inline PluginManager pluginManager;
