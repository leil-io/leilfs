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

#include "chunkserver/chunk_generation_fence.h"

#include <mutex>
#include <unordered_map>

#include "common/test_event_stream.h"

namespace chunk_generation_fence {

namespace {

/// Entries accrue only for chunks touched by round-issued commands, and only until restart.
/// A development ceiling; measuring the real working set is part of the measurement campaign.
constexpr size_t kDefaultCapacity = 262144;

std::mutex gMutex;
std::unordered_map<uint64_t, uint64_t> gHighWater;
size_t gCapacity = kDefaultCapacity;

}  // namespace

bool admit(uint64_t chunkId, uint64_t generation, const char *stage) {
	// No round issued this command, so there is no order to enforce and nothing to record.
	if (generation == 0) { return true; }

	std::lock_guard<std::mutex> guard(gMutex);
	auto it = gHighWater.find(chunkId);
	if (it == gHighWater.end()) {
		if (gHighWater.size() >= gCapacity) {
			if (test_event_stream::enabled()) {
				test_event_stream::emit(
				    "fence_table_full",
				    {{"chunk", chunkId}, {"generation", generation}, {"stage", stage}});
			}
			return false;
		}
		gHighWater.emplace(chunkId, generation);
		return true;
	}
	if (generation < it->second) {
		if (test_event_stream::enabled()) {
			test_event_stream::emit("fenced_generation_refused", {{"chunk", chunkId},
			                                                      {"generation", generation},
			                                                      {"high_water", it->second},
			                                                      {"stage", stage}});
		}
		return false;
	}
	it->second = generation;
	return true;
}

void resetForTest() {
	std::lock_guard<std::mutex> guard(gMutex);
	gHighWater.clear();
	gCapacity = kDefaultCapacity;
}

void setCapacityForTest(size_t capacity) {
	std::lock_guard<std::mutex> guard(gMutex);
	gCapacity = capacity;
}

}  // namespace chunk_generation_fence
