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

#include "common/platform.h"
#include "common/linear_assignment_cache.h"

#include <gtest/gtest.h>
#include <vector>

#include "common/random.h"
#include "common/slice_traits.h"

class LinearAssignmentCacheIntrospect : public LinearAssignmentCache {
public:
	EntriesContainer &data() { return cachedData_; }

	static HashType basePart() { return kBasePart_; }

	static HashType modPart() { return kModPart_; }

	static HashType baseSlice() { return kBaseSlice_; }

	static HashType modSlice() { return kModSlice_; }

	static HashType initialStartingHash() { return kInitialStartingHash_; }
};

using LACI = LinearAssignmentCacheIntrospect;
using SimpleSlice = std::vector<std::vector<std::pair<int, int>>>;

int64_t expectedHashSlice(int64_t initialHash, SimpleSlice &slice) {
	int64_t hash = initialHash;
	for (auto part : slice) {
		int64_t hashPart = 1;
		for (auto [labelId, copies] : part) {
			hashPart = (hashPart * LACI::basePart() +
			            (labelId * Goal::kMaxExpectedCopies + copies) %
			                LACI::basePart()) %
			           LACI::modPart();
		}
		hash = (hash * LACI::baseSlice() + hashPart) % LACI::modSlice();
	}
	return hash;
}

int64_t expectedHash(SimpleSlice &targetSlice, SimpleSlice &sourceSlice) {
	int64_t hash = LACI::initialStartingHash();
	hash = expectedHashSlice(hash, targetSlice);
	hash = expectedHashSlice(hash, sourceSlice);
	return hash;
}

void buildSliceFromSimpleSlice(SimpleSlice &simpleSlice, Goal::Slice &slice) {
	for (size_t i = 0; i < simpleSlice.size(); i++) {
		for (auto [labelId, copies] : simpleSlice[i]) {
			slice[i][MediaLabel(labelId)] = copies;
		}
	}
}

TEST(LinearAssignmentCacheTests, TestBasic) {
	LACI laci;

	Goal::Slice targetSlice(
	    Goal::Slice::Type{slice_traits::ec::getSliceType(5, 2)});
	Goal::Slice sourceSlice(
	    Goal::Slice::Type{slice_traits::ec::getSliceType(5, 2)});
	Goal::Slice sourceSliceSwapped(
	    Goal::Slice::Type{slice_traits::ec::getSliceType(5, 2)});
	SimpleSlice simpleTargetSlice = {{{1, 1}, {65535, 2}},
	                                 {{2, 1}},
	                                 {{3, 1}},
	                                 {{1, 3}, {3, 1}, {65535, 1}},
	                                 {{65535, 1}},
	                                 {{1, 1}},
	                                 {{1, 100}, {3, 2}}};
	SimpleSlice simpleSourceSlice = {
	    {{1, 1}},     {{3, 1}, {65535, 1}}, {{2, 1}}, {{1, 1}, {65535, 1}},
	    {{65535, 1}}, {{1, 10}, {3, 1}},    {{3, 1}},
	};
	std::array<int, 7> assignment = {5, 3, 1, 0, 4, 6, 2};

	SimpleSlice simpleSourceSliceSwapped = simpleSourceSlice;
	swap(simpleSourceSliceSwapped[1], simpleSourceSliceSwapped[4]);
	// for some reason it is not possible to call swap on arrays, so manual
	// swapping goes between elements at indexes 1 and 4
	std::array<int, 7> assignmentSwapped = {5, 4, 1, 0, 3, 6, 2};

	buildSliceFromSimpleSlice(simpleTargetSlice, targetSlice);
	buildSliceFromSimpleSlice(simpleSourceSlice, sourceSlice);
	buildSliceFromSimpleSlice(simpleSourceSliceSwapped, sourceSliceSwapped);
	std::array<int, 7> replyAssignment;
	replyAssignment.fill(0);

	int64_t hash = expectedHash(simpleTargetSlice, simpleSourceSlice);
	int64_t innerHash =
	    LinearAssignmentCache::hashSlicePair(targetSlice, sourceSlice);
	ASSERT_EQ(hash, innerHash);

	// the cache is empty
	ASSERT_FALSE(
	    laci.checkMatchAndGetResult(targetSlice, sourceSlice, replyAssignment));
	for (auto value : replyAssignment) { ASSERT_EQ(value, 0); }

	laci.store(targetSlice, sourceSlice, assignment);

	// the assignment can be found querying the cache
	ASSERT_TRUE(
	    laci.checkMatchAndGetResult(targetSlice, sourceSlice, replyAssignment));
	for (size_t i = 0; i < replyAssignment.size(); i++) {
		ASSERT_EQ(replyAssignment[i], assignment[i]);
	}
	replyAssignment.fill(0);

	// and directly to the expected entry
	ASSERT_TRUE(laci.data()[hash].checkMatchAndGetResult(
	    targetSlice, sourceSlice, replyAssignment));
	for (size_t i = 0; i < replyAssignment.size(); i++) {
		ASSERT_EQ(replyAssignment[i], assignment[i]);
	}
	replyAssignment.fill(0);

	// the swapped input cannot be flund until now
	ASSERT_FALSE(laci.checkMatchAndGetResult(targetSlice, sourceSliceSwapped,
	                                         replyAssignment));
	for (auto value : replyAssignment) { ASSERT_EQ(value, 0); }

	laci.store(targetSlice, sourceSliceSwapped, assignmentSwapped);

	// now both can be found
	ASSERT_TRUE(
	    laci.checkMatchAndGetResult(targetSlice, sourceSlice, replyAssignment));
	for (size_t i = 0; i < replyAssignment.size(); i++) {
		ASSERT_EQ(replyAssignment[i], assignment[i]);
	}
	replyAssignment.fill(0);
	ASSERT_TRUE(laci.checkMatchAndGetResult(targetSlice, sourceSliceSwapped,
	                                        replyAssignment));
	for (size_t i = 0; i < replyAssignment.size(); i++) {
		ASSERT_EQ(replyAssignment[i], assignmentSwapped[i]);
	}
	replyAssignment.fill(0);

	laci.data()[hash] = LinearAssignmentCache::OptimizerInputOutput();

	// the original entry has already been deleted, so cache should not find it
	ASSERT_FALSE(
	    laci.checkMatchAndGetResult(targetSlice, sourceSlice, replyAssignment));
	for (auto value : replyAssignment) { ASSERT_EQ(value, 0); }
}

TEST(LinearAssignmentCacheTests, TestOverwriteCollide) {
	LACI laci;

	Goal::Slice targetSliceEC(
	    Goal::Slice::Type{slice_traits::ec::getSliceType(2, 2)});
	Goal::Slice sourceSliceEC(
	    Goal::Slice::Type{slice_traits::ec::getSliceType(2, 2)});
	SimpleSlice simpleTargetSliceEC = {{{1, 1}}, {{1, 1}}, {{2, 1}}, {{2, 1}}};
	SimpleSlice simpleSourceSliceEC = {{{1, 1}}, {{2, 1}}, {}, {}};
	std::array<int, 7> assignmentEC = {0, 2, 1, 3};

	// note the number of part copies is not realistic, but will serve for the
	// test purposes
	Goal::Slice targetSliceXor(Goal::Slice::Type{Goal::Slice::Type::kXor3});
	Goal::Slice sourceSliceXor(Goal::Slice::Type{Goal::Slice::Type::kXor3});
	SimpleSlice simpleTargetSliceXor = {
	    {{1, 1}}, {{1, 1}}, {{2, 1}}, {{1, Goal::kMaxExpectedCopies + 1}}};
	SimpleSlice simpleSourceSliceXor = {
	    {{1, 1}}, {{1, Goal::kMaxExpectedCopies + 1}}, {}, {}};
	std::array<int, 7> assignmentXor = {0, 3, 1, 2};

	buildSliceFromSimpleSlice(simpleTargetSliceEC, targetSliceEC);
	buildSliceFromSimpleSlice(simpleSourceSliceEC, sourceSliceEC);
	buildSliceFromSimpleSlice(simpleTargetSliceXor, targetSliceXor);
	buildSliceFromSimpleSlice(simpleSourceSliceXor, sourceSliceXor);
	std::array<int, 7> replyAssignment;
	replyAssignment.fill(0);

	int64_t hashEC = expectedHash(simpleTargetSliceEC, simpleSourceSliceEC);
	int64_t hashXor = expectedHash(simpleTargetSliceXor, simpleSourceSliceXor);
	int64_t innerHashEC =
	    LinearAssignmentCache::hashSlicePair(targetSliceEC, sourceSliceEC);
	int64_t innerHashXor =
	    LinearAssignmentCache::hashSlicePair(targetSliceXor, sourceSliceXor);
	ASSERT_EQ(hashEC, innerHashEC);
	ASSERT_EQ(hashXor, innerHashXor);
	ASSERT_EQ(hashXor, hashEC);  // the pair of slices will collide

	// cache is empty, neither should be found
	ASSERT_FALSE(laci.checkMatchAndGetResult(targetSliceEC, sourceSliceEC,
	                                         replyAssignment));
	for (auto value : replyAssignment) { ASSERT_EQ(value, 0); }
	ASSERT_FALSE(laci.checkMatchAndGetResult(targetSliceXor, sourceSliceXor,
	                                         replyAssignment));
	for (auto value : replyAssignment) { ASSERT_EQ(value, 0); }

	laci.store(targetSliceEC, sourceSliceEC, assignmentEC);

	// cache has EC input, Xor is not found
	ASSERT_TRUE(laci.checkMatchAndGetResult(targetSliceEC, sourceSliceEC,
	                                        replyAssignment));
	for (size_t i = 0; i < replyAssignment.size(); i++) {
		ASSERT_EQ(replyAssignment[i], assignmentEC[i]);
	}
	replyAssignment.fill(0);
	ASSERT_FALSE(laci.checkMatchAndGetResult(targetSliceXor, sourceSliceXor,
	                                         replyAssignment));
	for (auto value : replyAssignment) { ASSERT_EQ(value, 0); }

	// Xor input overwrites previous EC input
	laci.store(targetSliceXor, sourceSliceXor, assignmentXor);

	// cache has only Xor input, EC is not found
	ASSERT_FALSE(laci.checkMatchAndGetResult(targetSliceEC, sourceSliceEC,
	                                         replyAssignment));
	for (auto value : replyAssignment) { ASSERT_EQ(value, 0); }
	ASSERT_TRUE(laci.checkMatchAndGetResult(targetSliceXor, sourceSliceXor,
	                                        replyAssignment));
	for (size_t i = 0; i < replyAssignment.size(); i++) {
		ASSERT_EQ(replyAssignment[i], assignmentXor[i]);
	}
}
