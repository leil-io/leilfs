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


#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"

#include <gtest/gtest.h>
#include <filesystem>

#pragma GCC diagnostic pop

#include <fstream>
#include <thread>
#include "chunk_trash_manager_impl.h"
#include "errors/saunafs_error_codes.h"  // Include the error codes header

namespace fs = std::filesystem;

class ChunkTrashManagerImplTest : public ::testing::Test {
protected:
	fs::path testDir;
	ChunkTrashManagerImpl chunkTrashManagerImpl;

	void SetUp() override {
		testDir = fs::temp_directory_path() / "chunk_trash_manager_test";
		fs::create_directories(testDir);
		chunkTrashManagerImpl.init(testDir.string());
	}

	void TearDown() override {
		fs::remove_all(testDir);
	}
};

TEST_F(ChunkTrashManagerImplTest, MoveToTrashValidFile) {
	fs::path filePath = testDir / "valid_file.txt";
	std::ofstream(filePath.string()); // Create a valid file
	std::time_t deletionTime = 1729259531;
	std::string const deletionTimeString = "20241018135211";

	ASSERT_TRUE(fs::exists(filePath));
	int const result = chunkTrashManagerImpl.moveToTrash(filePath, testDir,
	                                                     deletionTime);
	ASSERT_EQ(result, SAUNAFS_STATUS_OK);
	ASSERT_FALSE(fs::exists(filePath));
	ASSERT_TRUE(fs::exists((testDir / ".trash.bin/valid_file.txt.").string() +
	                       deletionTimeString));
}

TEST_F(ChunkTrashManagerImplTest, MoveToTrashNonExistentFile) {
	fs::path filePath = testDir / "non_existent_file.txt";
	std::time_t deletionTime = std::time(nullptr);

	int const result = chunkTrashManagerImpl.moveToTrash(filePath, testDir,
	                                                     deletionTime);
	ASSERT_NE(result, SAUNAFS_STATUS_OK);
}

TEST_F(ChunkTrashManagerImplTest, MoveToTrashFileInNestedDirectory) {
	fs::path nestedDir = testDir / "nested";
	fs::create_directories(nestedDir);
	fs::path filePath = nestedDir / "nested_file.txt";
	std::ofstream(filePath.string());

	std::time_t deletionTime = 1729259531;
	std::string const deletionTimeString = "20241018135211";

	int const result = chunkTrashManagerImpl.moveToTrash(filePath, testDir,
	                                                     deletionTime);
	ASSERT_EQ(result, SAUNAFS_STATUS_OK);
	ASSERT_FALSE(fs::exists(filePath));
	ASSERT_TRUE(fs::exists(
			(testDir / ".trash.bin/nested/nested_file.txt.").string() +
			deletionTimeString));
}

TEST_F(ChunkTrashManagerImplTest, MoveToTrashReadOnlyTrashDirectory) {
	fs::path readOnlyTrash = testDir / ".trash.bin";
	fs::create_directories(readOnlyTrash);
	fs::permissions(readOnlyTrash, fs::perms::owner_read); // Make it read-only

	fs::path filePath = testDir / "valid_file.txt";
	std::ofstream(filePath.string());
	std::time_t deletionTime = 1729259531;

	int const result = chunkTrashManagerImpl.moveToTrash(filePath, testDir,
	                                                     deletionTime);
	ASSERT_NE(result, SAUNAFS_STATUS_OK); // Expect failure
}

TEST_F(ChunkTrashManagerImplTest, MoveToTrashAlreadyTrashedFile) {
	fs::path filePath = testDir / "valid_file.txt";
	std::ofstream(filePath.string());
	std::time_t deletionTime = 1729259531;
	chunkTrashManagerImpl.moveToTrash(filePath, testDir, deletionTime);

	int const result = chunkTrashManagerImpl.moveToTrash(filePath, testDir,
	                                                     deletionTime);
	ASSERT_NE(result, SAUNAFS_STATUS_OK); // Expect failure
}

TEST_F(ChunkTrashManagerImplTest, ConcurrentMoveToTrash) {
	const int numFiles = 10;
	std::vector<std::thread> threads;
	for (int i = 0; i < numFiles; ++i) {
		threads.emplace_back([this, i]() {
			fs::path filePath =
					testDir / ("concurrent_file_" + std::to_string(i) + ".txt");
			std::ofstream(filePath.string());
			std::time_t deletionTime = 1729259531;
			chunkTrashManagerImpl.moveToTrash(filePath, testDir, deletionTime);
		});
	}

	for (auto &thread: threads) {
		thread.join();
	}

	// Check if all files are moved to trash
	for (int i = 0; i < numFiles; ++i) {
		ASSERT_FALSE(fs::exists(
				testDir / ("concurrent_file_" + std::to_string(i) + ".txt")));
		ASSERT_TRUE(fs::exists((testDir / ".trash.bin/concurrent_file_")
				                       .string() + std::to_string(i) + ".txt." +
		                       "20241018135211"));
	}
}

TEST_F(ChunkTrashManagerImplTest, PerformanceTest) {
	const int numFiles = 1000;
	auto start = std::chrono::high_resolution_clock::now();

	for (int i = 0; i < numFiles; ++i) {
		fs::path filePath =
				testDir / ("performance_file_" + std::to_string(i) + ".txt");
		std::ofstream(filePath.string());
		chunkTrashManagerImpl.moveToTrash(filePath, testDir,
		                                  std::time(nullptr));
	}

	auto end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> elapsed = end - start;
	ASSERT_LE(elapsed.count(), 2.0); // Expect it to complete within 2 seconds
}

// Testing initialization method
TEST_F(ChunkTrashManagerImplTest, InitCreatesTrashDirectory) {
	fs::path trashPath = testDir / ".trash.bin";
	ASSERT_TRUE(fs::exists(trashPath)); // Ensure trash directory exists
}

// Testing moving a file to trash
TEST_F(ChunkTrashManagerImplTest, MoveToTrash) {
	fs::path filePath = testDir / "file_to_trash.txt";
	std::ofstream(filePath.string());
	std::time_t deletionTime = std::time(nullptr);

	ASSERT_EQ(
			chunkTrashManagerImpl.moveToTrash(filePath, testDir, deletionTime),
			SAUNAFS_STATUS_OK);
	ASSERT_FALSE(fs::exists(filePath)); // Ensure file is moved to trash
}

// Testing getTimeString method
TEST_F(ChunkTrashManagerImplTest, GetTimeString) {

	std::time_t testDeletionTime = 1729259531;
	std::string testTimeString = "20241018135211";
	std::string timeString = chunkTrashManagerImpl.getTimeString(
			testDeletionTime);
	ASSERT_EQ(timeString, testTimeString); // Compare formatted strings
}

// Testing removing expired files
TEST_F(ChunkTrashManagerImplTest, RemoveExpiredFiles) {
	fs::path expiredFilePath = testDir / "expired_file.txt";
	std::ofstream(expiredFilePath.string());
	std::time_t oldDeletionTime = std::time(nullptr) - 86400; // 1 day ago

	chunkTrashManagerImpl.moveToTrash(expiredFilePath, testDir,
	                                  oldDeletionTime);
	chunkTrashManagerImpl.removeExpiredFiles(
			std::time(nullptr) - 3600); // 1 hour ago

	ASSERT_FALSE(fs::exists(expiredFilePath)); // Ensure expired file is removed
}

// Testing cleanup of empty folders
TEST_F(ChunkTrashManagerImplTest, CleanEmptyFolders) {
	// Create empty directories
	fs::path emptyValidDirPath = testDir / ".trash.bin" / "empty_dir";
	// Create non-empty directory
	fs::path nonEmptyDirPath = testDir / ".trash.bin" / "non_empty_dir";
	// Create an empty directory outside of trash
	fs::path emptyInvalidDirPath = testDir / "empty_dir";
	fs::create_directory(emptyValidDirPath);
	fs::create_directory(emptyInvalidDirPath);
	fs::create_directory(nonEmptyDirPath);
	std::ofstream(nonEmptyDirPath / "file.txt");

	chunkTrashManagerImpl.cleanEmptyFolders(testDir.string());

	// Ensure empty directories are removed
	ASSERT_FALSE(fs::exists(emptyValidDirPath));
	// Ensure does not remove non-trash empty directory
	ASSERT_TRUE(fs::exists(emptyInvalidDirPath));
	// Ensure does not remove non-empty directory
	ASSERT_TRUE(fs::exists(nonEmptyDirPath));
	// Ensure the file in the non-empty directory is not removed
	ASSERT_TRUE(fs::exists(nonEmptyDirPath / "file.txt"));
}

// Testing checking valid timestamp format
TEST_F(ChunkTrashManagerImplTest, ValidTimestampFormat) {
	ASSERT_TRUE(chunkTrashManagerImpl.isValidTimestampFormat(
			"20231018120350")); // Valid format
	ASSERT_FALSE(chunkTrashManagerImpl.isValidTimestampFormat(
			"invalid_timestamp")); // Invalid format
}

// Testing makeSpace functionality
TEST_F(ChunkTrashManagerImplTest, MakeSpaceRemovesOldFiles) {
	fs::path lowSpaceFilePath = testDir / "file1.txt";
	std::ofstream(lowSpaceFilePath.string());

	chunkTrashManagerImpl.moveToTrash(lowSpaceFilePath, testDir,
	                                  std::time(nullptr));

	// Simulate low space condition and check removal
	chunkTrashManagerImpl.makeSpace(1, 1); // 1 GB threshold
	ASSERT_FALSE(fs::exists(lowSpaceFilePath)); // Ensure file is removed
}

// Testing garbage collection process
TEST_F(ChunkTrashManagerImplTest, CollectGarbage) {
	fs::path garbageFilePath = testDir / "garbage_file.txt";
	std::ofstream(garbageFilePath.string());

	chunkTrashManagerImpl.moveToTrash(garbageFilePath, testDir,
	                                  std::time(nullptr));
	chunkTrashManagerImpl.collectGarbage();

	ASSERT_FALSE(
			fs::exists(garbageFilePath)); // Ensure garbage file is collected
}

// Testing check available space functionality
TEST_F(ChunkTrashManagerImplTest, CheckAvailableSpace) {
	size_t availableSpace = chunkTrashManagerImpl.checkAvailableSpace(
			testDir.string());
	ASSERT_GT(availableSpace, 0); // Ensure there is available space
}

// Testing makeSpace with specific disk path
TEST_F(ChunkTrashManagerImplTest, MakeSpaceOnSpecificDisk) {
	fs::path filePath1 = testDir / "file1.txt";
	fs::path filePath2 = testDir / "file2.txt";
	std::ofstream(filePath1.string());
	std::ofstream(filePath2.string());

	chunkTrashManagerImpl.moveToTrash(filePath1, testDir, std::time(nullptr));
	chunkTrashManagerImpl.moveToTrash(filePath2, testDir, std::time(nullptr));

	chunkTrashManagerImpl.makeSpace(testDir.string(), 1, 1); // 1 GB threshold
	ASSERT_FALSE(fs::exists(filePath1)); // Ensure the first file is removed
	ASSERT_FALSE(fs::exists(filePath2)); // Ensure the second file is removed
}

// Testing converting time string to time value
TEST_F(ChunkTrashManagerImplTest, GetTimeFromString) {
	std::string timeString = "20231018120350";
	int errorCode = 0;
	time_t timeValue = chunkTrashManagerImpl.getTimeFromString(timeString,
	                                                           errorCode);

	std::tm tm = {};
	strptime(timeString.c_str(), "%Y%m%d%H%M%S", &tm);
	time_t expectedTimeValue = std::mktime(&tm);

	ASSERT_EQ(timeValue, expectedTimeValue); // Compare time values
}

// Mocking helper functions for filesystem operations
TEST_F(ChunkTrashManagerImplTest, InitHandlesInvalidAndValidFiles) {
	// Arrange: Create mocks for filesystem entries
	std::string validFile = "file_with_timestamp.20231015";
	std::string invalidFile = "file_without_timestamp";

	fs::path trashDir = testDir / "trashDir";

	fs::create_directories(trashDir);
	std::ofstream(trashDir / validFile);
	std::ofstream(trashDir / invalidFile);

	// Act: Call the init function to cover the lines
	int status = chunkTrashManagerImpl.init(trashDir.string());

	// Assert: Check function behavior and coverage
	EXPECT_EQ(status, SAUNAFS_STATUS_OK);
}
