/*
   Copyright 2026 Leil Storage OÜ

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

#include <cstddef>
#include <deque>
#include <optional>
#include <unordered_set>

#include "chunkserver-common/chunk_map.h"

/// Bound auxiliary registration storage independently of the inventory size.
/// Overflow is recovered by another bounded registry walk after scans finish.
inline constexpr std::size_t kRegistrationSweepPendingLimit = 65'536;

/// Pull-sweep cursor and outstanding keys. Guard shared instances with gChunksMapMutex.
struct RegistrationSweepState {
	std::size_t bucket{0};
	std::size_t bucketCount{0};
	bool cursorDone{false};
	bool rescanNeeded{false};
	/// True during overflow recovery; the initial walk requests recovery promptly.
	bool recoveryWalk{false};
	/// Whether any part has been emitted since the cursor was last restarted.
	/// Includes parts emitted while retrying pending keys.
	bool madeProgress{false};
	/// Next entry in this bucket; insertion, removal, and rehash hooks keep it valid.
	std::optional<ChunkMap::local_iterator> bucketPosition;
	std::deque<ChunkWithType> pending;
	std::unordered_set<ChunkWithType, KeyOperations, KeyOperations> pendingKeys;

	void enqueue(ChunkWithType key) {
		if (pendingKeys.size() >= kRegistrationSweepPendingLimit) {
			// Repeated notifications for a queued key do not require another walk.
			if (!pendingKeys.contains(key)) { rescanNeeded = true; }
			return;
		}
		if (pendingKeys.insert(key).second) { pending.push_back(key); }
	}

	/// Restart the cursor while retaining pending retries and per-chunk epoch marks.
	void restartWalk(std::size_t newBucketCount) {
		bucketCount = newBucketCount;
		bucket = 0;
		cursorDone = false;
		bucketPosition.reset();
		rescanNeeded = false;
		madeProgress = false;
	}

	void restartAfterRehash(std::size_t newBucketCount) {
		if (!cursorDone && bucketCount != newBucketCount) { restartWalk(newBucketCount); }
	}
};
