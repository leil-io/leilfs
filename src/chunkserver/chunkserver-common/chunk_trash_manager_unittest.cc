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
// A fix for https://stackoverflow.com/q/77034039/10788155
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#include <filesystem>
#include <gtest/gtest.h>
#pragma GCC diagnostic pop

#include <fstream>

#include "chunk_trash_manager.h"
#include "errors/saunafs_error_codes.h"

class ChunkTrashManagerTest : public ::testing::Test {
protected:
	static std::filesystem::path testDir;

	void SetUp() override {
		testDir = std::filesystem::temp_directory_path() / "chunk_trash_manager_test";
		std::filesystem::create_directories(testDir);
	}

	void TearDown() override {
		if (std::filesystem::remove_all(testDir) == 0) {
			std::cerr << "Failed to remove test directory: " << testDir << '\n';
		}
	}
};

std::filesystem::path ChunkTrashManagerTest::testDir;

// Test getDeletionTimeString() to ensure it produces a UTC timestamp
TEST_F(ChunkTrashManagerTest, GetDeletionTimeStringTest) {
	std::string timestamp = ChunkTrashManager::getDeletionTimeString();
	EXPECT_EQ(timestamp.size(), strlen("YYYYMMDDHHMMSS"));  // Check that the timestamp has the correct length
	EXPECT_TRUE(std::all_of(timestamp.begin(), timestamp.end(), ::isdigit));  // Check that all characters are digits
}

TEST_F(ChunkTrashManagerTest, MoveToTrashValidFile) {
	std::filesystem::path const filePath = testDir / "test_file.txt";
	std::ofstream(filePath) << "dummy content";

	// Test the moveToTrash() method directly using ChunkTrashManager
	std::string const deletionTime = ChunkTrashManager::getDeletionTimeString();
	int const result = ChunkTrashManager::moveToTrash(filePath, testDir, deletionTime);

	std::filesystem::path const expectedTrashPath = testDir / ChunkTrashManager::kTrashDirname /
	                                          ("test_file.txt." + deletionTime);
	EXPECT_TRUE(std::filesystem::exists(expectedTrashPath));
	EXPECT_EQ(result, SAUNAFS_STATUS_OK);
}

TEST_F(ChunkTrashManagerTest, MoveToTrashNonExistentFile) {
	// Define a non-existent file path
	std::filesystem::path const filePath = testDir / "non_existent_file.txt";

	// Test the moveToTrash() method directly using ChunkTrashManager
	std::string const deletionTime = ChunkTrashManager::getDeletionTimeString();
	int const result = ChunkTrashManager::moveToTrash(filePath, testDir, deletionTime);

	// Check that the function returned the correct error code for a non-existent file
	EXPECT_EQ(result, SAUNAFS_ERROR_ENOENT);
}

TEST_F(ChunkTrashManagerTest, MoveToTrashFileInNestedDirectory) {
	std::filesystem::path const nestedDir = testDir / "nested/dir/structure";
	std::filesystem::create_directories(nestedDir);
	std::filesystem::path const filePath = nestedDir / "test_file_nested.txt";
	std::ofstream(filePath) << "nested content";

	std::string const deletionTime = ChunkTrashManager::getDeletionTimeString();
	int const result = ChunkTrashManager::moveToTrash(filePath, testDir, deletionTime);

	std::filesystem::path const expectedTrashPath = testDir /
	                                          ChunkTrashManager::kTrashDirname /
	                                          ("test_file_nested.txt." + deletionTime);
	EXPECT_TRUE(std::filesystem::exists(expectedTrashPath));
	EXPECT_EQ(result, SAUNAFS_STATUS_OK);
}
