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

#include "chunk_trash_index.h"

class ChunkTrashIndexTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Resetting state before each test to ensure no interference
		ChunkTrashIndex::instance().reset("/testDisk");
	}

	void TearDown() override {
		// Resetting state after each test to clean up
		ChunkTrashIndex::instance().reset("/testDisk");
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
	ASSERT_EQ(expiredFiles.size(), 1);
	EXPECT_EQ(expiredFiles["/testDisk"].begin()->second, "/.trash.bin/path/to/file");
}

TEST_F(ChunkTrashIndexTest, RemoveFileEntry) {
	auto &index = ChunkTrashIndex::instance();
	index.add(1234567890, "/path/to/file", "/testDisk");
	index.remove(1234567890, "/path/to/file", "/testDisk");

	auto expiredFiles = index.getExpiredFiles(1234567891);
	EXPECT_EQ(expiredFiles["/testDisk"].size(), 0);
}

TEST_F(ChunkTrashIndexTest, ThreadSafety) {
	auto &index = ChunkTrashIndex::instance();
	std::thread t1([&index]() {
		index.add(1234567890, "/path/to/file1", "/testDisk");
	});
	std::thread t2([&index]() {
		index.add(1234567891, "/path/to/file2", "/testDisk");
	});

	t1.join();
	t2.join();

	auto expiredFiles = index.getExpiredFiles(1234567892);
	ASSERT_EQ(expiredFiles.size(), 1);
	EXPECT_EQ(expiredFiles["/testDisk"].size(), 2);
}

TEST_F(ChunkTrashIndexTest, RemoveFileEntry2) {
	auto &index = ChunkTrashIndex::instance();

	// Arrange: Add a file to the trash index
	std::string diskPath = "/testDisk";
	std::string filePath1 = diskPath + "/.trash.bin/path/to/file1";
	time_t deletionTime = 1234567890;
	index.add(deletionTime, filePath1, diskPath);

	// Act: Remove the file entries
	index.remove(deletionTime, filePath1);

	// Assert: Verify the file entry is removed
	auto expiredFiles = index.getExpiredFiles(deletionTime + 1);
	EXPECT_EQ(expiredFiles.size(), 1);
	EXPECT_EQ(expiredFiles[diskPath].size(), 0);
}
