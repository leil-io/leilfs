/*

   Copyright 2026 Leil Storage OÜ

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

#include "mount/negative_cache.h"

#include <ranges>

#include <gtest/gtest.h>

class NegativeCacheTest : public testing::Test {
protected:
    void SetUp() override {
        NegativeCache::setGlobalMaxSize(kMaxSize);
        negativeCache.setMaxSize(NegativeCache::getGlobalMaxSize());
        NegativeCache::setGlobalTimeoutMs(k1000ms);
        negativeCache.setTimeoutMs(NegativeCache::getGlobalTimeoutMs());
    }

    static constexpr uint32_t kMaxSize = 10;
    static constexpr uint32_t k1000ms = 1000;
    static constexpr inode_t kParent = 0;
    static constexpr std::string_view kName = "some_file";

    NegativeCache negativeCache;
};

TEST_F(NegativeCacheTest, InitiallyEmpty) {
    EXPECT_EQ(negativeCache.size(), 0U);
}

TEST_F(NegativeCacheTest, SingleInsertion) {
    negativeCache.add(kParent, kName);
    EXPECT_EQ(negativeCache.size(), 1U);
}

TEST_F(NegativeCacheTest, RemoveOldestWithLRUUpdate) {
    NegativeCache::setGlobalMaxSize(2);
    negativeCache.setMaxSize(NegativeCache::getGlobalMaxSize());
    EXPECT_EQ(negativeCache.size(), 0U);
    negativeCache.add(1, "A");
    negativeCache.add(1, "B");
    EXPECT_EQ(negativeCache.size(), 2U);
    EXPECT_TRUE(negativeCache.lookup(1, "A"));
    EXPECT_TRUE(negativeCache.lookup(1, "B"));

    negativeCache.add(1, "A");
    EXPECT_EQ(negativeCache.size(), 2U);
    EXPECT_TRUE(negativeCache.lookup(1, "A"));
    EXPECT_TRUE(negativeCache.lookup(1, "B"));

    negativeCache.add(1, "C");
    EXPECT_EQ(negativeCache.size(), 2U);
    EXPECT_TRUE(negativeCache.lookup(1, "A"));
    EXPECT_FALSE(negativeCache.lookup(1, "B"));
    EXPECT_TRUE(negativeCache.lookup(1, "C"));
}

TEST_F(NegativeCacheTest, LookupSuccess) {
    negativeCache.add(kParent, kName);
    EXPECT_TRUE(negativeCache.lookup(kParent, kName));
}

TEST_F(NegativeCacheTest, LookupTimeoutOrNotFound) {
    NegativeCache::setGlobalTimeoutMs(0);
    negativeCache.setTimeoutMs(NegativeCache::getGlobalTimeoutMs());
    negativeCache.add(kParent, kName);
    EXPECT_FALSE(negativeCache.lookup(kParent, kName));
    EXPECT_FALSE(negativeCache.lookup(1, "not-added-to-ncache"));
}

TEST_F(NegativeCacheTest, TimeoutOrMaxSizeChangeClearAll) {
    for (auto parent : std::views::iota(1U, kMaxSize)) {
        negativeCache.add(parent, kName);
    }
    EXPECT_EQ(negativeCache.size(), 9U);
    NegativeCache::setGlobalTimeoutMs(1001);
    negativeCache.add(kParent, kName);
    EXPECT_EQ(negativeCache.size(), 1U);
    for (auto parent : std::views::iota(1U, kMaxSize)) {
        negativeCache.add(parent, kName);
    }
    EXPECT_EQ(negativeCache.size(), 10U);
    NegativeCache::setGlobalMaxSize(11);
    negativeCache.add(kParent, kName);
    EXPECT_EQ(negativeCache.size(), 1U);
}
