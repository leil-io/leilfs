/*

   Copyright 2015 Skytechnology sp. z o.o.
   Copyright 2023 Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include "common/generic_lru_cache.h"
#include "common/goal.h"
#include "master/chunk_goal_counters.h"

struct CountersHasher {
	size_t operator()(const ChunkGoalCounters &counters) const {
		std::hash<uint8_t> hasher;
		size_t seed = 0x17;
		for (const auto &i : counters) {
			seed ^= hasher(i.goal) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		}
		return seed;
	}
};

struct CountersComparator {
	using GoalCounter = ChunkGoalCounters::GoalCounter;
	bool operator()(const ChunkGoalCounters &c1, const ChunkGoalCounters &c2) const {
		return c1.size() == c2.size() && std::equal(c1.begin(), c1.end(), c2.begin(),
				[](const GoalCounter &g1, const GoalCounter &g2){
			return g1.goal == g2.goal;
		});
	}
};

using GoalCache = GenericLruCache<ChunkGoalCounters, Goal, 0x10000, CountersHasher, CountersComparator>;
