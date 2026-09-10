/*

   Copyright 2017 Skytechnology sp. z o.o.
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
#include "mount/direntry_cache.h"

#include <gtest/gtest.h>
#include <iostream>

class DirEntryCacheIntrospect : public DirEntryCache {
public:
	DirEntryCacheIntrospect(uint64_t timeout)
		: DirEntryCache(timeout) {
	}

	LookupSet::const_iterator lookup_begin() const {
		return lookup_set_.begin();
	}

	IndexSet::const_iterator index_begin() const {
		return index_set_.begin();
	}

	InodeMultiset::const_iterator inode_begin() const {
		return inode_multiset_.begin();
	}

	LookupSet::const_iterator lookup_end() const {
		return lookup_set_.end();
	}

	InodeMultiset::const_iterator inode_end() const {
		return inode_multiset_.end();
	}

	size_t index_size() const {
		return index_set_.size();
	}

	size_t lookup_size() const {
		return lookup_set_.size();
	}
};

void check_expected_content(
    DirEntryCacheIntrospect &cache,
    std::vector<std::tuple<int, int, int, std::string>> index_output,
    std::vector<std::tuple<int, int, int, std::string>> lookup_output) {
	auto index_it = cache.index_begin();
	auto index_output_it = index_output.begin();
	while (index_it != cache.index_end()) {
		ASSERT_EQ(*index_output_it, std::make_tuple(index_it->inode, index_it->parent_inode, index_it->index, index_it->name));
		index_it++;
		index_output_it++;
	}
	ASSERT_TRUE(index_output_it == index_output.end());

	auto lookup_it = cache.lookup_begin();
	auto lookup_output_it = lookup_output.begin();
	while (lookup_it != cache.lookup_end()) {
		ASSERT_EQ(*lookup_output_it, std::make_tuple(lookup_it->inode, lookup_it->parent_inode, lookup_it->index, lookup_it->name));
		lookup_it++;
		lookup_output_it++;
	}
	ASSERT_TRUE(lookup_output_it == lookup_output.end());
}

TEST(DirEntryCache, Basic) {
	DirEntryCacheIntrospect cache(5000000);

	Attributes dummy_attributes;
	dummy_attributes.fill(0);
	Attributes attributes_with_6 = dummy_attributes;
	Attributes attributes_with_9 = dummy_attributes;
	attributes_with_6[0] = 6;
	attributes_with_9[0] = 9;
	auto current_time = cache.updateTime();
	cache.insertSequence(
		SaunaClient::Context(0, 0, 0, 0), 9,
		std::vector<DirectoryEntry>{
			{0, 1, 7, "a1", dummy_attributes},
			{1, 2, 8, "a2", dummy_attributes},
			{2, 3, 9, "a3", dummy_attributes}
		}, current_time
	);
	cache.insertSequence(
		SaunaClient::Context(1, 2, 0, 0), 11,
		std::vector<DirectoryEntry>{
			{7, 8, 5, "a1", dummy_attributes},
			{8, 9, 4, "a2", dummy_attributes},
			{9, 10, 3, "a3", dummy_attributes}
		}, current_time
	);
	cache.insertSequence(
		SaunaClient::Context(0, 0, 0, 0), 9,
		std::vector<DirectoryEntry>{
			{1, 2, 11, "a4", dummy_attributes},
			{2, 3, 13, "a3", attributes_with_9},
			{3, 4, 12, "a2", attributes_with_6}
		}, current_time
	);
	size_t initial_size = cache.size();

	// Overwrite old lookup entry, but do not insert in the index set
	cache.insert(SaunaClient::Context(1, 2, 0, 0), 11, 15, "a1",
	             dummy_attributes, current_time);

	ASSERT_EQ(cache.size(), initial_size);
	ASSERT_EQ(cache.size(), cache.lookup_size());
	ASSERT_EQ(cache.size(), cache.index_size() + 1);

	// Insert to the lookup set (different ctx) and do not insert in the index
	// set
	cache.insert(SaunaClient::Context(0, 0, 0, 0), 11, 15, "a1",
	             dummy_attributes, current_time);

	ASSERT_EQ(cache.size(), initial_size + 1);
	ASSERT_EQ(cache.size(), cache.lookup_size());
	ASSERT_EQ(cache.size(), cache.index_size() + 2);

	// Add plain inode entry
	cache.insert(SaunaClient::Context(0, 0, 0, 0), 6, dummy_attributes,
	             current_time);

	ASSERT_EQ(cache.size(), initial_size + 2);
	ASSERT_EQ(cache.size(), cache.lookup_size() + 1);
	ASSERT_EQ(cache.size(), cache.index_size() + 3);

	// Overwrite plain inode entry, should remove old entries in index and
	// lookup sets
	cache.insert(SaunaClient::Context(0, 0, 0, 0), 12, attributes_with_6,
	             current_time);

	ASSERT_EQ(cache.size(), initial_size + 2);
	ASSERT_EQ(cache.size(), cache.lookup_size() + 2);
	ASSERT_EQ(cache.size(), cache.index_size() + 4);

	std::vector<std::tuple<int, int, int, std::string>> index_output {
		std::make_tuple(7, 9, 0, "a1"),
		std::make_tuple(11, 9, 1, "a4"),
		std::make_tuple(13, 9, 2, "a3"),
		std::make_tuple(4, 11, 8, "a2"),
		std::make_tuple(3, 11, 9, "a3")
	};

	std::vector<std::tuple<int, int, int, std::string>> lookup_output {
		std::make_tuple(7, 9, 0, "a1"),
		std::make_tuple(13, 9, 2, "a3"),
		std::make_tuple(11, 9, 1, "a4"),
		std::make_tuple(15, 11, kInvalidParent, "a1"),
		std::make_tuple(15, 11, kInvalidParent, "a1"),
		std::make_tuple(4, 11, 8, "a2"),
		std::make_tuple(3, 11, 9, "a3")
	};

	check_expected_content(cache, index_output, lookup_output);

	auto by_inode_it = cache.find(SaunaClient::Context(0, 0, 0, 0), 12);
	ASSERT_NE(by_inode_it, cache.inode_end());
	ASSERT_EQ(by_inode_it->attr[0], 6);
	by_inode_it++;
	ASSERT_NE(by_inode_it, cache.inode_end());
	ASSERT_EQ(by_inode_it->attr[0], 9);
	by_inode_it++;
	ASSERT_NE(by_inode_it, cache.inode_end());
	ASSERT_EQ(by_inode_it->inode, 15U);
	by_inode_it++;
	ASSERT_NE(by_inode_it, cache.inode_end());
	ASSERT_EQ(by_inode_it->inode, 15U);
	by_inode_it++;
	ASSERT_EQ(by_inode_it, cache.inode_end());
}

TEST(DirEntryCache, Repetitions) {
	DirEntryCacheIntrospect cache(5000000);

	Attributes dummy_attributes;
	dummy_attributes.fill(0);
	auto current_time = cache.updateTime();

	cache.insertSequence(
	    SaunaClient::Context(0, 0, 0, 0), 9,
	    std::vector<DirectoryEntry>{{0, 1, 7, "a1", dummy_attributes},
	                                {1, 2, 8, "a2", dummy_attributes},
	                                {2, 3, 9, "a3", dummy_attributes},
	                                {3, 4, 10, "a4", dummy_attributes},
	                                {4, 5, 11, "a5", dummy_attributes},
	                                {5, 6, 12, "a6", dummy_attributes}},
	    current_time);
	// Lookup set finds the entries and should overwrite them
	cache.insertSequence(
	    SaunaClient::Context(0, 0, 0, 0), 9,
	    std::vector<DirectoryEntry>{{1, 2, 8, "a1", dummy_attributes},
	                                {2, 3, 9, "a2", dummy_attributes},
	                                {3, 4, 10, "a3", dummy_attributes},
	                                {4, 5, 11, "a4", dummy_attributes},
	                                {5, 6, 12, "a5", dummy_attributes},
	                                {6, 7, 13, "a6", dummy_attributes}},
	    current_time);

	ASSERT_EQ(cache.size(), 6U);
	cache.removeOldest(5);
	ASSERT_EQ(cache.size(), 1U);
}

TEST(DirEntryCache, RandomOrder) {
	DirEntryCacheIntrospect cache(5000000);

	Attributes dummy_attributes;
	dummy_attributes.fill(0);
	auto current_time = cache.updateTime();
	cache.insertSequence(
		SaunaClient::Context(0, 0, 0, 0), 9,
		std::vector<DirectoryEntry>{
			{0, 1, 7, "a1", dummy_attributes},
			{1, 2, 8, "a2", dummy_attributes},
			{2, 3, 9, "a3", dummy_attributes}
		}, current_time
	);
	cache.insertSequence(
		SaunaClient::Context(0, 0, 0, 0), 9,
		std::vector<DirectoryEntry>{
			{7, 8, 5, "a4", dummy_attributes},
			{8, 9, 4, "a5", dummy_attributes},
			{9, 10, 3, "a6", dummy_attributes}
		}, current_time
	);
	cache.insertSequence(
		SaunaClient::Context(0, 0, 0, 0), 9,
		std::vector<DirectoryEntry>{
			{7, 0, 5, "a4", dummy_attributes},
			{0, 2, 7, "a2", dummy_attributes},
			{2, 3, 8, "a1", dummy_attributes}
		}, current_time
	);
	std::vector<std::tuple<int, int, int, std::string>> index_output {
		std::make_tuple(7, 9, 0, "a2"),
		std::make_tuple(8, 9, 2, "a1"),
		std::make_tuple(5, 9, 7, "a4"),
		std::make_tuple(4, 9, 8, "a5"),
		std::make_tuple(3, 9, 9, "a6")
	};

	std::vector<std::tuple<int, int, int, std::string>> lookup_output {
		std::make_tuple(8, 9, 2, "a1"),
		std::make_tuple(7, 9, 0, "a2"),
		std::make_tuple(5, 9, 7, "a4"),
		std::make_tuple(4, 9, 8, "a5"),
		std::make_tuple(3, 9, 9, "a6")
	};

	check_expected_content(cache, index_output, lookup_output);

	cache.insert(SaunaClient::Context(0, 0, 0, 0), 9, 10, 3, 100, "a7",
	             dummy_attributes, current_time);
	// Check last entry was actually inserted
	auto lookup_it = cache.lookup_end();
	lookup_it--;
	ASSERT_EQ(lookup_it->inode, 10U);
	// Should remove the sequence of entries starting from index 0, including the last one inserted
	cache.invalidate(SaunaClient::Context(0, 0, 0, 0), 9, 0);

	index_output = {
		std::make_tuple(5, 9, 7, "a4"),
		std::make_tuple(4, 9, 8, "a5"),
		std::make_tuple(3, 9, 9, "a6")
	};

	lookup_output = {
		std::make_tuple(5, 9, 7, "a4"),
		std::make_tuple(4, 9, 8, "a5"),
		std::make_tuple(3, 9, 9, "a6")
	};
	check_expected_content(cache, index_output, lookup_output);
}

TEST(DirEntryCache, LatestLookup) {
	DirEntryCacheIntrospect cache(5000000);

	Attributes dummy_attributes;
	dummy_attributes.fill(0);
	Attributes attributes_with_6 = dummy_attributes;
	Attributes attributes_with_9 = dummy_attributes;
	attributes_with_6[0] = 6;
	attributes_with_9[0] = 9;
	auto current_time = cache.updateTime();

	cache.insert(SaunaClient::Context(0, 0, 0, 0), 11, attributes_with_9,
	             current_time + 500);
	cache.insert(SaunaClient::Context(0, 0, 0, 0), 12, attributes_with_6,
	             current_time + 500);
	cache.insertSequence(
		SaunaClient::Context(0, 0, 0, 0), 9,
		std::vector<DirectoryEntry>{
			{7, 8, 10, "a1", dummy_attributes},
			{8, 9, 11, "a2", dummy_attributes},
			{9, 10, 12, "a3", dummy_attributes}
		}, current_time + 1
	);

	Attributes attr;
	dummy_attributes.fill(0);
	ASSERT_FALSE(cache.lookup(SaunaClient::Context(0, 0, 0, 0), 9, attr));

	ASSERT_TRUE(cache.lookup(SaunaClient::Context(0, 0, 0, 0), 10, attr));
	ASSERT_EQ(attr[0], 0);

	ASSERT_TRUE(cache.lookup(SaunaClient::Context(0, 0, 0, 0), 11, attr));
	ASSERT_EQ(attr[0], 9);

	ASSERT_TRUE(cache.lookup(SaunaClient::Context(0, 0, 0, 0), 12, attr));
	ASSERT_EQ(attr[0], 6);
}

TEST(DirEntryCache, InvalidateParentItself) {
	DirEntryCacheIntrospect cache(5000000);

	Attributes dummy_attributes;
	dummy_attributes.fill(0);
	Attributes attributes_with_6 = dummy_attributes;
	Attributes attributes_with_9 = dummy_attributes;
	attributes_with_6[0] = 6;
	attributes_with_9[0] = 9;
	auto current_time = cache.updateTime();

	cache.insert(SaunaClient::Context(0, 0, 0, 0), 8, attributes_with_6,
	             current_time + 500);
	cache.insert(SaunaClient::Context(1, 1, 0, 0), 8, attributes_with_9,
	             current_time + 500);
	cache.insert(SaunaClient::Context(0, 0, 0, 0), 9, attributes_with_6,
	             current_time + 500);
	cache.insert(SaunaClient::Context(1, 0, 0, 0), 11, attributes_with_9,
	             current_time + 500);
	cache.insert(SaunaClient::Context(0, 0, 0, 0), 12, attributes_with_6,
	             current_time + 500);
	cache.insert(SaunaClient::Context(0, 0, 0, 0), 13, attributes_with_9,
	             current_time + 500);
	cache.insert(SaunaClient::Context(0, 0, 0, 0), 15, attributes_with_6,
	             current_time + 500);
	cache.insertSequence(
		SaunaClient::Context(0, 0, 0, 0), 8,
		std::vector<DirectoryEntry>{
			{7, 8, 10, "a1", dummy_attributes},
			{8, 9, 11, "a2", dummy_attributes},
			{9, 10, 12, "a3", dummy_attributes}
		}, current_time + 1
	);
	cache.insertSequence(
		SaunaClient::Context(1, 0, 0, 0), 8,
		std::vector<DirectoryEntry>{
			{7, 8, 10, "a10", dummy_attributes},
			{8, 9, 11, "a11", dummy_attributes},
			{9, 10, 22, "a22", dummy_attributes}
		}, current_time + 1
	);
	cache.insertSequence(
		SaunaClient::Context(1, 1, 0, 0), 12,
		std::vector<DirectoryEntry>{
			{7, 8, 14, "a4", dummy_attributes},
			{8, 9, 15, "a5", dummy_attributes},
			{9, 10, 16, "a6", dummy_attributes}
		}, current_time + 1
	);

	Attributes attr;
	dummy_attributes.fill(0);
	cache.lockAndInvalidateParent(12);

	ASSERT_FALSE(cache.lookup(SaunaClient::Context(0, 0, 0, 0), 12, attr));
	ASSERT_FALSE(cache.lookup(SaunaClient::Context(1, 1, 0, 0), 12, attr));

	ASSERT_TRUE(cache.lookup(SaunaClient::Context(0, 0, 0, 0), 13, attr));
	ASSERT_EQ(attr[0], 9);

	ASSERT_FALSE(cache.lookup(SaunaClient::Context(1, 1, 0, 0), 14, attr));
	ASSERT_FALSE(cache.lookup(SaunaClient::Context(1, 1, 0, 0), 16, attr));

	ASSERT_FALSE(cache.lookup(SaunaClient::Context(1, 1, 0, 0), 15, attr));
	ASSERT_TRUE(cache.lookup(SaunaClient::Context(0, 0, 0, 0), 15, attr));
	ASSERT_EQ(attr[0], 6);

	cache.lockAndInvalidateParent(SaunaClient::Context(0, 0, 0, 0), 8);

	ASSERT_FALSE(cache.lookup(SaunaClient::Context(0, 0, 0, 0), 8, attr));
	ASSERT_FALSE(cache.lookup(SaunaClient::Context(0, 0, 0, 0), 10, attr));
	ASSERT_FALSE(cache.lookup(SaunaClient::Context(0, 0, 0, 0), 11, attr));
	ASSERT_FALSE(cache.lookup(SaunaClient::Context(0, 0, 0, 0), 12, attr));

	ASSERT_TRUE(cache.lookup(SaunaClient::Context(1, 1, 0, 0), 8, attr));
	ASSERT_EQ(attr[0], 9);

	ASSERT_TRUE(cache.lookup(SaunaClient::Context(0, 0, 0, 0), 9, attr));
	ASSERT_EQ(attr[0], 6);

	ASSERT_TRUE(cache.lookup(SaunaClient::Context(1, 0, 0, 0), 10, attr));
	ASSERT_EQ(attr[0], 0);

	ASSERT_TRUE(cache.lookup(SaunaClient::Context(1, 0, 0, 0), 10, attr));
	ASSERT_EQ(attr[0], 0);

	ASSERT_TRUE(cache.lookup(SaunaClient::Context(1, 0, 0, 0), 11, attr));
	ASSERT_EQ(attr[0], 9);

	ASSERT_TRUE(cache.lookup(SaunaClient::Context(1, 0, 0, 0), 22, attr));
	ASSERT_EQ(attr[0], 0);
}
