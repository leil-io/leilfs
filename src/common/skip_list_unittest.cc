/*
   Copyright 2025 Leil Storage OÜ

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

#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <ext/pb_ds/assoc_container.hpp>
#include <ext/pb_ds/tree_policy.hpp>
#include <random>
#include <string>
#include <vector>

#include "common/skip_list.h"

static constexpr uint32_t kConsistencyCheckOpsPeriod = 500;
static constexpr int32_t kMaxKeyToGenerate = 10000000;
static constexpr int32_t kMaxStringKeySize = 200;
static constexpr int32_t kMinStringKeySize = 4;

/// Configuration struct for SkipListTypedTest
template <typename Key, typename Compare, uint32_t ProbNum, uint32_t ProbDen>
struct SkipListConfig {
	using key_type = Key;
	using compare_type = Compare;
	static constexpr uint32_t prob_num = ProbNum;
	static constexpr uint32_t prob_den = ProbDen;
};

/// Red-black tree based reference implementation using PBDS
template <typename Key, typename Compare>
using OrderedSet = __gnu_pbds::tree<Key, __gnu_pbds::null_type, Compare, __gnu_pbds::rb_tree_tag,
                                    __gnu_pbds::tree_order_statistics_node_update>;

/// Test fixture for SkipList with configurable parameters
template <typename Config>
class SkipListTypedTest : public ::testing::Test {
protected:
	using Key = typename Config::key_type;
	using Compare = typename Config::compare_type;
	static constexpr uint32_t prob_num = Config::prob_num;
	static constexpr uint32_t prob_den = Config::prob_den;

	void SetUp() override {}

	void TearDown() override {
		skiplist.clear();
		referenceSet.clear();
	}

	/// Generate random key based on type
	Key generateKey() {
		if constexpr (std::is_same_v<Key, uint32_t>) {
			std::uniform_int_distribution<uint32_t> dist(0, kMaxKeyToGenerate);
			return dist(gen);
		} else if constexpr (std::is_same_v<Key, std::string>) {
			std::uniform_int_distribution<uint32_t> lenDist(kMinStringKeySize, kMaxStringKeySize);
			std::uniform_int_distribution<char> charDist('a', 'z');
			int len = lenDist(gen);
			std::string str;
			for (int i = 0; i < len; ++i) { str += charDist(gen); }
			return str;
		} else if constexpr (std::is_same_v<Key, uint64_t>) {
			std::uniform_int_distribution<uint32_t> dist(0, kMaxKeyToGenerate);
			return dist(gen);
		} else if constexpr (std::is_same_v<Key, std::pair<uint64_t, uint32_t>>) {
			std::uniform_int_distribution<uint32_t> dist(0, kMaxKeyToGenerate);
			return {dist(gen), dist(gen)};
		} else if constexpr (std::is_same_v<Key, std::pair<uint32_t, uint64_t>>) {
			std::uniform_int_distribution<uint32_t> dist(0, kMaxKeyToGenerate);
			return {dist(gen), dist(gen)};
		} else if constexpr (std::is_same_v<Key, std::pair<uint64_t, uint64_t>>) {
			std::uniform_int_distribution<uint32_t> dist(0, kMaxKeyToGenerate);
			return {dist(gen), dist(gen)};
		}
	}

	/// Verify consistency between the skiplist and referenceSet members
	void verifyConsistency() { verifyConsistency(this->skiplist, this->referenceSet); }

	/// Verify consistency between a skiplist and a reference set
	void verifyConsistency(const SkipList<Key, Compare, prob_num, prob_den> &sList,
	                       const OrderedSet<Key, Compare> &refSet) {
		// Verify size matches
		EXPECT_EQ(sList.size(), refSet.size());

		if (sList.empty()) {
			EXPECT_TRUE(refSet.empty());
			return;
		}

		// Verify all elements are equal and in the same order
		auto skiplist_it = sList.begin();
		auto reference_it = refSet.begin();

		size_t count = 0;
		while (skiplist_it != sList.end() && reference_it != refSet.end()) {
			bool equals =
			    !Compare()(*skiplist_it, *reference_it) && !Compare()(*reference_it, *skiplist_it);
			EXPECT_TRUE(equals) << "Mismatch at position " << count;
			++skiplist_it;
			++reference_it;
			++count;
		}

		EXPECT_TRUE(skiplist_it == sList.end() && reference_it == refSet.end());
	}

	/// Test insert and find operations
	void testBasicOperations(size_t numOperations) {
		for (size_t i = 0; i < numOperations; ++i) {
			Key key = generateKey();

			// Test insert
			auto skiplistResult = skiplist.insert(key);
			auto referenceResult = referenceSet.insert(key);
			EXPECT_EQ(skiplistResult.second, referenceResult.second);

			if (i % kConsistencyCheckOpsPeriod == 0) { verifyConsistency(); }

			// Test find
			auto skiplistFind = skiplist.find(key);
			bool foundInSkiplist = (skiplistFind != skiplist.end());
			EXPECT_TRUE(foundInSkiplist);

			if (foundInSkiplist) { EXPECT_EQ(*skiplistFind, key); }
		}
	}

	/// Test copy constructor, assignment, and move operations
	void testCopyMoveOperations(size_t numOperations) {
		// Test copy constructor
		auto skipCopy1 = skiplist;
		auto refCopy1 = referenceSet;
		verifyConsistency(skipCopy1, refCopy1);

		// Insert elements into original containers
		for (size_t i = 0; i < numOperations / 2; ++i) {
			Key key = generateKey();
			skiplist.insert(key);
			referenceSet.insert(key);
		}

		// Verify copies remain unchanged
		verifyConsistency(skipCopy1, refCopy1);

		// Test copy assignment
		auto skipCopy2 = skiplist;
		auto refCopy2 = referenceSet;

		// Insert into all containers
		for (size_t i = 0; i < numOperations / 2; ++i) {
			Key key = generateKey();
			skiplist.insert(key);
			referenceSet.insert(key);
			skipCopy2.insert(key);
			refCopy2.insert(key);
			skipCopy1.insert(key);
			refCopy1.insert(key);
		}

		verifyConsistency(skipCopy1, refCopy1);
		verifyConsistency(skipCopy2, refCopy2);
		verifyConsistency(skiplist, referenceSet);

		// Verify skipCopy2 equals original
		verifyConsistency(skipCopy2, referenceSet);

		// Test move operations
		auto skipCopy3 = std::move(skipCopy2);
		auto refCopy3 = std::move(refCopy2);
		verifyConsistency(skipCopy3, referenceSet);
		verifyConsistency(skipCopy3, refCopy3);
	}

	/// Test order statistics (order_of_key, find_by_order)
	void testOrderOperations(size_t numOperations) {
		for (size_t i = 0; i < numOperations; ++i) {
			Key key = generateKey();

			// Insert in both containers
			skiplist.insert(key);
			referenceSet.insert(key);

			// Test order_of_key
			if (!referenceSet.empty()) {
				size_t idx = std::uniform_int_distribution<size_t>(0, referenceSet.size() - 1)(gen);
				auto testKey = *referenceSet.find_by_order(idx);
				size_t skiplist_order = skiplist.order_of_key(testKey);
				EXPECT_EQ(skiplist_order, idx) << "order_of_key mismatch for key number: " << idx;
			}

			// Test find_by_order
			if (!referenceSet.empty()) {
				size_t order =
				    std::uniform_int_distribution<size_t>(0, referenceSet.size() - 1)(gen);

				auto skiplist_it = skiplist.find_by_order(order);
				auto reference_it = referenceSet.find_by_order(order);

				if (order < referenceSet.size()) {
					EXPECT_NE(skiplist_it, skiplist.end());
					EXPECT_EQ(*skiplist_it, *reference_it)
					    << "find_by_order mismatch at order: " << order;
				}
			}

			if (i % kConsistencyCheckOpsPeriod == 0) { verifyConsistency(); }
		}
	}

	/// Test erase operations by key and iterator
	void testEraseOperations(size_t numOperations) {
		std::vector<Key> keys_to_insert;

		// Insert initial keys
		for (size_t i = 0; i < numOperations / 2; ++i) {
			Key key = generateKey();
			keys_to_insert.push_back(key);
			skiplist.insert(key);
			referenceSet.insert(key);
		}

		verifyConsistency();

		// Erase by key
		for (const Key &key : keys_to_insert) {
			size_t skiplistEraseCount = skiplist.erase(key);
			size_t referenceEraseCount = referenceSet.erase(key);

			EXPECT_EQ(skiplistEraseCount, referenceEraseCount);

			verifyConsistency();
		}

		// Insert more keys
		for (size_t i = 0; i < numOperations / 2; ++i) {
			Key key = generateKey();
			skiplist.insert(key);
			referenceSet.insert(key);
		}

		verifyConsistency();

		// Erase by iterator
		while (!skiplist.empty() && !referenceSet.empty()) {
			size_t order = std::uniform_int_distribution<size_t>(0, referenceSet.size() - 1)(gen);

			auto skiplist_it = skiplist.find_by_order(order);
			auto reference_it = referenceSet.find_by_order(order);

			EXPECT_EQ(*skiplist_it, *reference_it);

			skiplist.erase(skiplist_it);
			referenceSet.erase(reference_it);

			verifyConsistency();
		}
	}

	/// Test lower_bound operations
	void testLowerBoundOperations(size_t numOperations) {
		std::vector<Key> testKeys;

		for (size_t i = 0; i < numOperations; ++i) {
			Key key = generateKey();
			testKeys.push_back(key);

			// Randomly insert about half the keys
			if (std::uniform_real_distribution<double>(0, 1)(gen) < 0.5) {
				skiplist.insert(key);
				referenceSet.insert(key);
			}
		}

		// Test lower_bound for all test keys
		for (const Key &key : testKeys) {
			auto skiplistLB = skiplist.lower_bound(key);
			auto referenceLB = referenceSet.lower_bound(key);

			if (referenceLB == referenceSet.end()) {
				EXPECT_EQ(skiplistLB, skiplist.end());
			} else {
				EXPECT_NE(skiplistLB, skiplist.end());
				EXPECT_EQ(*skiplistLB, *referenceLB);
			}
		}
	}

	/// Test mixed operations to simulate real usage
	void testMixedOperations(size_t numOperations) {
		std::vector<Key> currentKeys;

		for (size_t i = 0; i < numOperations; ++i) {
			double operationType = std::uniform_real_distribution<double>(0, 1)(gen);
			Key key = generateKey();

			if (operationType < 0.4) {
				// Insert
				auto skiplistResult = skiplist.insert(key);
				auto referenceResult = referenceSet.insert(key);
				EXPECT_EQ(skiplistResult.second, referenceResult.second);

				if (skiplistResult.second) { currentKeys.push_back(key); }

			} else if (operationType < 0.7) {
				// Find
				bool skiplistFound = (skiplist.find(key) != skiplist.end());
				bool referenceFound = (referenceSet.find(key) != referenceSet.end());
				EXPECT_EQ(skiplistFound, referenceFound);

			} else if (operationType < 0.85 && !currentKeys.empty()) {
				// Erase by key
				size_t idx = std::uniform_int_distribution<size_t>(0, currentKeys.size() - 1)(gen);
				Key keyToErase = currentKeys[idx];

				size_t skiplistEraseCount = skiplist.erase(keyToErase);
				size_t referenceEraseCount = referenceSet.erase(keyToErase);
				EXPECT_EQ(skiplistEraseCount, referenceEraseCount);

				if (skiplistEraseCount > 0) { currentKeys.erase(currentKeys.begin() + idx); }

			} else if (!currentKeys.empty()) {
				// Order operations
				size_t order =
				    std::uniform_int_distribution<size_t>(0, currentKeys.size() - 1)(gen);

				auto skiplist_it = skiplist.find_by_order(order);
				auto reference_it = referenceSet.find_by_order(order);
				EXPECT_EQ(*skiplist_it, *reference_it);
			}

			// Periodic consistency check
			if (i % kConsistencyCheckOpsPeriod == 0) {
				verifyConsistency();

				// Verify order_of_key for random existing key
				if (!currentKeys.empty()) {
					size_t idx =
					    std::uniform_int_distribution<size_t>(0, currentKeys.size() - 1)(gen);
					Key testKey = currentKeys[idx];

					size_t skiplistOrder = skiplist.order_of_key(testKey);
					size_t referenceOrder = referenceSet.order_of_key(testKey);
					EXPECT_EQ(skiplistOrder, referenceOrder);
				}
			}
		}

		verifyConsistency();
	}

	/// Test complex scenarios with multiple skiplists
	void testMultipleListsComplex(size_t numOperations, size_t numLists = 4) {
		std::vector<SkipList<Key, Compare, prob_num, prob_den>> skiplists(numLists);
		std::vector<OrderedSet<Key, Compare>> referenceSets(numLists);

		std::uniform_int_distribution<size_t> listDist(0, numLists - 1);
		std::uniform_int_distribution<size_t> opDist(0, 6);

		for (size_t i = 0; i < numOperations; ++i) {
			int operation = opDist(gen);
			size_t list1 = listDist(gen);
			size_t list2 = listDist(gen);

			switch (operation) {
			case 0: {  // Insert unique key in random list.
				Key key = generateKey();
				skiplists[list1].insert(key);
				referenceSets[list1].insert(key);
				break;
			}

			case 1: {  // Insert same key in two lists
				Key key = generateKey();
				auto result1 = skiplists[list1].insert(key);
				auto result2 = skiplists[list2].insert(key);
				auto ref_result1 = referenceSets[list1].insert(key);
				auto ref_result2 = referenceSets[list2].insert(key);

				EXPECT_EQ(result1.second, ref_result1.second);
				EXPECT_EQ(result2.second, ref_result2.second);
				break;
			}

			case 2: {  // Move key from one list to another
				if (!referenceSets[list1].empty()) {
					size_t key_idx = std::uniform_int_distribution<size_t>(
					    0, referenceSets[list1].size() - 1)(gen);
					Key key = *referenceSets[list1].find_by_order(key_idx);

					// Erase from list1
					size_t erase1 = skiplists[list1].erase(key);
					size_t erase_ref1 = referenceSets[list1].erase(key);
					EXPECT_EQ(erase1, erase_ref1);

					// Insert to list2
					skiplists[list2].insert(key);
					referenceSets[list2].insert(key);
				}
				break;
			}

			case 3: {  // Find same key in multiple lists
				if (!referenceSets[list1].empty()) {
					size_t key_idx = std::uniform_int_distribution<size_t>(
					    0, referenceSets[list1].size() - 1)(gen);
					Key key = *referenceSets[list1].find_by_order(key_idx);

					bool found1 = (skiplists[list1].find(key) != skiplists[list1].end());
					bool found2 = (skiplists[list2].find(key) != skiplists[list2].end());
					bool ref_found1 =
					    (referenceSets[list1].find(key) != referenceSets[list1].end());
					bool ref_found2 =
					    (referenceSets[list2].find(key) != referenceSets[list2].end());

					EXPECT_EQ(found1, ref_found1);
					EXPECT_EQ(found2, ref_found2);
				}
				break;
			}

			case 4: {  // Verify weak consistency (size, first element and last element)
				for (size_t j = 0; j < numLists; ++j) {
					EXPECT_EQ(skiplists[j].size(), referenceSets[j].size());

					if (!referenceSets[j].empty()) {
						auto skiplistFirst = *skiplists[j].begin();
						auto referenceFirst = *referenceSets[j].begin();
						EXPECT_EQ(skiplistFirst, referenceFirst);

						auto skiplistLast = *skiplists[j].find_by_order(skiplists[j].size() - 1);
						auto referenceLast =
						    *referenceSets[j].find_by_order(referenceSets[j].size() - 1);
						EXPECT_EQ(skiplistLast, referenceLast);
					}
				}
				break;
			}

			case 5: {  // Order operations for specific list.
				if (!referenceSets[list1].empty()) {
					size_t order = std::uniform_int_distribution<size_t>(
					    0, referenceSets[list1].size() - 1)(gen);

					auto skiplist_it = skiplists[list1].find_by_order(order);
					auto reference_it = referenceSets[list1].find_by_order(order);

					EXPECT_EQ(*skiplist_it, *reference_it);
				}
				break;
			}

			case 6: {  // Clean random list occasionally
				if (std::uniform_real_distribution<double>(0, 1)(gen) < 0.1) {
					skiplists[list1].clear();
					referenceSets[list1].clear();
				}
				break;
			}
			}

			// Periodic consistency check
			if (i % kConsistencyCheckOpsPeriod == 0) {
				for (size_t j = 0; j < numLists; ++j) {
					verifyConsistency(skiplists[j], referenceSets[j]);
				}
			}
		}

		// Final verification
		for (size_t j = 0; j < numLists; ++j) { verifyConsistency(skiplists[j], referenceSets[j]); }
	}

private:
	SkipList<Key, Compare, prob_num, prob_den> skiplist;
	OrderedSet<Key, Compare> referenceSet;
	std::mt19937 gen;
};

// Test configurations for various key types and probability parameters
using TestConfigs = ::testing::Types<
    SkipListConfig<uint32_t, std::less<>, 1, 2>, SkipListConfig<uint32_t, std::less<>, 3, 4>,
    SkipListConfig<uint64_t, std::greater<>, 1, 2>, SkipListConfig<uint64_t, std::less<>, 3, 4>,
    SkipListConfig<std::pair<uint64_t, uint32_t>, std::greater<>, 1, 2>,
    SkipListConfig<std::pair<uint32_t, uint64_t>, std::less<>, 3, 4>,
    SkipListConfig<std::pair<uint64_t, uint64_t>, std::greater<>, 1, 2>,
    SkipListConfig<std::pair<uint64_t, uint64_t>, std::less<>, 4, 5>,
    SkipListConfig<std::pair<uint64_t, uint64_t>, std::less<>, 3, 4>,
    SkipListConfig<std::string, std::less<>, 1, 2>, SkipListConfig<std::string, std::less<>, 3, 4>>;

TYPED_TEST_SUITE(SkipListTypedTest, TestConfigs);

/// Individual test cases
TYPED_TEST(SkipListTypedTest, BasicOperations) { this->testBasicOperations(10000); }

TYPED_TEST(SkipListTypedTest, CopyMoveOperations) { this->testCopyMoveOperations(3000); }

TYPED_TEST(SkipListTypedTest, OrderOperations) { this->testOrderOperations(20000); }

TYPED_TEST(SkipListTypedTest, EraseOperations) { this->testEraseOperations(6000); }

TYPED_TEST(SkipListTypedTest, LowerBoundOperations) { this->testLowerBoundOperations(4000); }

TYPED_TEST(SkipListTypedTest, MixedOperations) { this->testMixedOperations(30000); }

TYPED_TEST(SkipListTypedTest, MultipleListsComplex) { this->testMultipleListsComplex(100000, 5); }
