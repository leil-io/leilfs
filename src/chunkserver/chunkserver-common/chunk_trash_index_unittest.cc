/*
   Copyright 2023-2024  Leil Storage OÜ

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
#include <thread>

#include "chunk_trash_index.h"

class ChunkTrashIndexTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Resetting state before each test to ensure no interference
		ChunkTrashIndex::instance().clear();
	}

	void TearDown() override {
		// Resetting state after each test to clean up
		ChunkTrashIndex::instance().clear();
	}
};

TEST_F(ChunkTrashIndexTest, SingletonInstance) {
	// Ensure that multiple calls to instance() return the same object
	ChunkTrashIndex &index1 = ChunkTrashIndex::instance();
	ChunkTrashIndex &index2 = ChunkTrashIndex::instance();
	EXPECT_EQ(&index1, &index2);
}

TEST_F(ChunkTrashIndexTest, AddFileEntry) {
	auto &index = ChunkTrashIndex::instance();
	index.add(1234567890, "/.trash.bin/path/to/file", "/testDisk");

	auto expiredFiles = index.getExpiredFiles(1234567891);
	ASSERT_EQ(expiredFiles.size(), static_cast<size_t>(1));
	EXPECT_EQ(expiredFiles["/testDisk"].begin()->second, "/.trash.bin/path/to/file");
}

TEST_F(ChunkTrashIndexTest, RemoveFileEntry) {
	auto &index = ChunkTrashIndex::instance();
	index.add(1234567890, "/path/to/file", "/testDisk");
	index.remove(1234567890, "/path/to/file", "/testDisk");

	auto expiredFiles = index.getExpiredFiles(1234567891);
	EXPECT_EQ(expiredFiles["/testDisk"].size(), static_cast<size_t>(0));
}

TEST_F(ChunkTrashIndexTest, RemoveFileEntryWithoutDiskPath) {
	auto &index = ChunkTrashIndex::instance();
	index.add(1234567890, "/path/to/file", "/testDisk");
	index.remove(1234567890, "/path/to/file");

	auto expiredFiles = index.getExpiredFiles(1234567891);
	EXPECT_EQ(expiredFiles["/testDisk"].size(), static_cast<size_t>(0));
}

TEST_F(ChunkTrashIndexTest, RemoveFileEntryFromLastDisk) {
	auto &index = ChunkTrashIndex::instance();
	index.add(1234567890, "/path/to/file", "/testDisk");
	index.add(123456, "/path/to/file", "/testDisk2");

	// Expect removing from /testDisk
	index.remove(1234567890, "/path/to/file");

	auto expiredFiles = index.getExpiredFiles(1234567891);
	// Expect /testDisk should be empty
	EXPECT_EQ(expiredFiles["/testDisk"].size(), static_cast<size_t>(0));
	// Expect /testDisk2 has one file
	EXPECT_EQ(expiredFiles["/testDisk2"].size(), static_cast<size_t>(1));
}

TEST_F(ChunkTrashIndexTest, ThreadSafety) {
	auto &index = ChunkTrashIndex::instance();
	std::thread t1([&index]() { index.add(1234567890, "/path/to/file1", "/testDisk"); });
	std::thread t2([&index]() { index.add(1234567891, "/path/to/file2", "/testDisk"); });

	t1.join();
	t2.join();

	auto expiredFiles = index.getExpiredFiles(1234567892);
	ASSERT_EQ(expiredFiles.size(), static_cast<size_t>(1));
	EXPECT_EQ(expiredFiles["/testDisk"].size(), static_cast<size_t>(2));
}
