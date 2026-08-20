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

#include "chunkserver-common/chunk_map.h"
#include "chunkserver-common/disk_interface.h"
#include "chunkserver-common/disk_manager_interface.h"
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

/// Checks that the per-type counts still add up to the number of chunks in the
/// registry, and complains if they do not.
///
/// The index is only correct as long as every gChunksMap insert and erase is
/// paired with hddNotePresentChunkType / hddForgetPresentChunkType. A path that
/// forgets the pairing does not fail loudly: it makes id-only lookups skip a
/// type, so the master's on-demand chunk-location query silently reports chunks
/// as absent that this chunkserver actually holds. This turns that into
/// something visible.
///
/// The invariant holds at every point where gChunksMapMutex is free, so this
/// can be called from anywhere; disk-scan completion is a natural moment, being
/// when the registry has just changed in bulk. Cost is one pass over the
/// distinct hosted types, of which there are a handful.
///
/// Takes gChunksMapMutex, so do not call with it already held.
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

/// Active Disks scans in progress.
/// Note: theoretically it would return a false positive if scans haven't
/// started yet, but it's a _very_ unlikely situation.
inline std::atomic_int gScansInProgress(0);

inline std::atomic_bool gStatChunksAtDiskScan{true};

inline std::atomic_bool gPerformFsync(true);

inline std::atomic_bool gCheckCrcWhenWriting{true};

/// Value of HDD_ADVISE_NO_CACHE from config
inline std::atomic_bool gAdviseNoCache = false;

inline IoStat gIoStat;

inline PluginManager pluginManager;
