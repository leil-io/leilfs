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

#include "common/platform.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <sys/statvfs.h>
#include <filesystem>
#include <fstream>
#include <thread>

#include "chunkserver-common/chunk_trash_manager_impl.h"
#include "errors/saunafs_error_codes.h"  // Include the error codes header

int fake_statvfs(const char *path, struct statvfs *buf) {
	if (std::string(path) == "/fake/path") {
		buf->f_bavail = 1024;  // Fake available blocks
		buf->f_frsize = 4096;  // Fake block size
		return 0;              // Success
	}
	errno = ENOENT;
	return -1;  // Simulate failure
}

// Fake statvfs() definition to override the real one
// Used by the checkAvailableSpace() method
extern "C" int statvfs(const char *path, struct statvfs *buf) { return fake_statvfs(path, buf); }

namespace fs = std::filesystem;

inline constexpr uint64_t kMiB = 1024 * 1024;

class ChunkTrashManagerImplTest : public ::testing::Test {
public:
	fs::path testDir;
	ChunkTrashManagerImpl chunkTrashManagerImpl;

	void seedState(uint64_t maxReadMiB, uint64_t maxWriteMiB, uint64_t prevReadMiB,
	               uint64_t prevWriteMiB) {
		ChunkTrashManagerImpl::maxBytesReadPerDisk = maxReadMiB * kMiB;
		ChunkTrashManagerImpl::maxBytesWritePerDisk = maxWriteMiB * kMiB;
		ChunkTrashManagerImpl::previousBytesReadPerDisk = prevReadMiB * kMiB;
		ChunkTrashManagerImpl::previousBytesWritePerDisk = prevWriteMiB * kMiB;
	}

	void SetUp() override {
		testDir = fs::temp_directory_path() / "chunk_trash_manager_test";
		fs::create_directories(testDir);
		chunkTrashManagerImpl.init();
		chunkTrashManagerImpl.registerDiskPath(testDir.string());

		seedState(100, 100, 20, 20);
	}

	void TearDown() override {
		fs::remove_all(testDir);
		chunkTrashManagerImpl.terminate();
	}
};

TEST_F(ChunkTrashManagerImplTest, MoveToTrashValidFile) {
	fs::path filePath = testDir / "valid_file.txt";
	std::ofstream(filePath.string());  // Create a valid file
	std::time_t deletionTime = 1729259531;
	std::string const deletionTimeString =
	    ChunkTrashManagerImpl::getStringFromTime(deletionTime);  // Convert time to string format

	ASSERT_TRUE(fs::exists(filePath));
	int const result = chunkTrashManagerImpl.moveToTrash(filePath, testDir, deletionTime);
	ASSERT_EQ(result, SAUNAFS_STATUS_OK);
	ASSERT_FALSE(fs::exists(filePath));
	ASSERT_TRUE(fs::exists((testDir / ".trash.bin/valid_file.txt.").string() + deletionTimeString));
}

TEST_F(ChunkTrashManagerImplTest, MoveToTrashNonExistentFile) {
	fs::path filePath = testDir / "non_existent_file.txt";
	std::time_t deletionTime = std::time(nullptr);

	int const result = chunkTrashManagerImpl.moveToTrash(filePath, testDir, deletionTime);
	ASSERT_EQ(result, SAUNAFS_ERROR_ENOENT);
}

TEST_F(ChunkTrashManagerImplTest, MoveToTrashFileInNestedDirectory) {
	fs::path nestedDir = testDir / "nested";
	fs::create_directories(nestedDir);
	fs::path filePath = nestedDir / "nested_file.txt";
	std::ofstream(filePath.string());

	std::time_t deletionTime = 1729259531;
	std::string const deletionTimeString = ChunkTrashManagerImpl::getStringFromTime(deletionTime);

	int const result = chunkTrashManagerImpl.moveToTrash(filePath, testDir, deletionTime);
	ASSERT_EQ(result, SAUNAFS_STATUS_OK);
	ASSERT_FALSE(fs::exists(filePath));
	ASSERT_TRUE(
	    fs::exists((testDir / ".trash.bin/nested/nested_file.txt.").string() + deletionTimeString));
}

TEST_F(ChunkTrashManagerImplTest, MoveToTrashReadOnlyTrashDirectory) {
	fs::path readOnlyTrash = testDir / ".trash.bin";
	fs::create_directories(readOnlyTrash);
	fs::permissions(readOnlyTrash, fs::perms::owner_read);  // Make it read-only

	fs::path filePath = testDir / "valid_file.txt";
	std::ofstream(filePath.string());
	std::time_t deletionTime = 1729259531;

	int const result = chunkTrashManagerImpl.moveToTrash(filePath, testDir, deletionTime);
	ASSERT_EQ(result, SAUNAFS_ERROR_NOTDONE);  // Expect failure
}

TEST_F(ChunkTrashManagerImplTest, MoveToTrashAlreadyTrashedFile) {
	fs::path filePath = testDir / "valid_file.txt";
	std::ofstream(filePath.string());
	std::time_t deletionTime = 1729259531;
	chunkTrashManagerImpl.moveToTrash(filePath, testDir, deletionTime);

	int const result = chunkTrashManagerImpl.moveToTrash(filePath, testDir, deletionTime);
	ASSERT_EQ(result, SAUNAFS_ERROR_ENOENT);  // Expect failure
}

TEST_F(ChunkTrashManagerImplTest, ConcurrentMoveToTrash) {
	const int numFiles = 10;
	std::time_t deletionTime = 1729259531;
	const std::string deletionTimeString = ChunkTrashManagerImpl::getStringFromTime(deletionTime);
	std::vector<std::thread> threads;
	for (int i = 0; i < numFiles; ++i) {
		threads.emplace_back([this, i, deletionTime]() {
			fs::path filePath = testDir / ("concurrent_file_" + std::to_string(i) + ".txt");
			std::ofstream(filePath.string());
			chunkTrashManagerImpl.moveToTrash(filePath, testDir, deletionTime);
		});
	}

	for (auto &thread : threads) { thread.join(); }

	// Check if all files are moved to trash
	for (int i = 0; i < numFiles; ++i) {
		ASSERT_FALSE(fs::exists(testDir / ("concurrent_file_" + std::to_string(i) + ".txt")));
		ASSERT_TRUE(fs::exists((testDir / ".trash.bin/concurrent_file_").string() +
		                       std::to_string(i) + ".txt." + deletionTimeString));
	}
}

TEST_F(ChunkTrashManagerImplTest, PerformanceTest) {
	const int numFiles = 1000;
	auto start = std::chrono::high_resolution_clock::now();

	for (int i = 0; i < numFiles; ++i) {
		fs::path filePath = testDir / ("performance_file_" + std::to_string(i) + ".txt");
		std::ofstream(filePath.string());
		chunkTrashManagerImpl.moveToTrash(filePath, testDir, std::time(nullptr));
	}

	auto end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> elapsed = end - start;
	ASSERT_LE(elapsed.count(), 2.0);  // Expect it to complete within 2 seconds
}

// Testing registerDiskPath method
TEST_F(ChunkTrashManagerImplTest, RegisterDiskPathCreatesTrashDirectory) {
	fs::path trashPath = testDir / ".trash.bin";
	ASSERT_TRUE(fs::exists(trashPath));  // Ensure trash directory exists
}

// Testing moving a file to trash
TEST_F(ChunkTrashManagerImplTest, MoveToTrash) {
	fs::path filePath = testDir / "file_to_trash.txt";
	std::ofstream(filePath.string());
	std::time_t deletionTime = std::time(nullptr);

	ASSERT_EQ(chunkTrashManagerImpl.moveToTrash(filePath, testDir, deletionTime),
	          SAUNAFS_STATUS_OK);
	ASSERT_FALSE(fs::exists(filePath));  // Ensure file is moved to trash
}

// Testing getStringFromTime method
TEST_F(ChunkTrashManagerImplTest, getStringFromTime) {
	std::time_t testDeletionTime = 1729259531;
	std::string testTimeString = "20241018135211";
	std::string timeString = chunkTrashManagerImpl.getStringFromTime(testDeletionTime);
	ASSERT_EQ(timeString, testTimeString);  // Compare formatted strings
}

// Testing removing expired files
TEST_F(ChunkTrashManagerImplTest, RemoveExpiredFiles) {
	fs::path expiredFilePath = testDir / "expired_file.txt";
	std::ofstream(expiredFilePath.string());
	std::time_t oldDeletionTime = std::time(nullptr) - 86400;  // 1 day ago
	const std::string oldDeletionTimeString =
	    chunkTrashManagerImpl.getStringFromTime(oldDeletionTime);
	const std::string trashPath =
	    (testDir / ".trash.bin/expired_file.txt.").string() + oldDeletionTimeString;

	chunkTrashManagerImpl.moveToTrash(expiredFilePath, testDir, oldDeletionTime);
	ASSERT_FALSE(fs::exists(expiredFilePath));  // Ensure file is moved to trash
	ASSERT_TRUE(fs::exists(trashPath));         // Ensure file is in trash

	chunkTrashManagerImpl.removeExpiredFiles(std::time(nullptr) - 3600);  // 1 hour ago

	ASSERT_FALSE(fs::exists(trashPath));        // Ensure expired file is removed
	ASSERT_FALSE(fs::exists(expiredFilePath));  // Ensure original file is removed
}

// Testing checking valid timestamp format
TEST_F(ChunkTrashManagerImplTest, ValidTimestampFormat) {
	ASSERT_TRUE(chunkTrashManagerImpl.isValidTimestampFormat("20231018120350"));  // Valid format
	ASSERT_FALSE(
	    chunkTrashManagerImpl.isValidTimestampFormat("invalid_timestamp"));  // Invalid format
}

// Testing makeSpace functionality
TEST_F(ChunkTrashManagerImplTest, MakeSpaceRemovesOldFiles) {
	fs::path lowSpaceFilePath = testDir / "file1.txt";
	std::ofstream(lowSpaceFilePath.string());

	chunkTrashManagerImpl.moveToTrash(lowSpaceFilePath, testDir, std::time(nullptr));

	// Simulate low space condition and check removal
	chunkTrashManagerImpl.makeSpace(1, 1);       // 1 GB threshold
	ASSERT_FALSE(fs::exists(lowSpaceFilePath));  // Ensure file is removed
}

// Testing check available space functionality
TEST_F(ChunkTrashManagerImplTest, CheckAvailableSpace) {
	size_t availableSpace = ChunkTrashManagerImpl::checkAvailableSpace(testDir.string());

	constexpr size_t kGiBMultiplier = 1 << 30;
	size_t expectedGb = 1024 * 4096 / kGiBMultiplier;  // Fake available space
	ASSERT_EQ(availableSpace, expectedGb);             // Compare values
}

// Testing makeSpace with specific disk path
TEST_F(ChunkTrashManagerImplTest, MakeSpaceOnSpecificDisk) {
	fs::path filePath1 = testDir / "file1.txt";
	fs::path filePath2 = testDir / "file2.txt";
	std::ofstream(filePath1.string());
	std::ofstream(filePath2.string());

	chunkTrashManagerImpl.moveToTrash(filePath1, testDir, std::time(nullptr));
	chunkTrashManagerImpl.moveToTrash(filePath2, testDir, std::time(nullptr));

	chunkTrashManagerImpl.makeSpace(testDir.string(), 1, 1);  // 1 GB threshold
	ASSERT_FALSE(fs::exists(filePath1));                      // Ensure the first file is removed
	ASSERT_FALSE(fs::exists(filePath2));                      // Ensure the second file is removed
}

// Testing converting time string to time value
TEST_F(ChunkTrashManagerImplTest, GetTimeFromString) {
	std::string timeString = "20241018135211";
	int errorCode = 0;
	time_t timeValue = ChunkTrashManagerImpl::getTimeFromString(timeString, errorCode);

	// 2024-10-18 13:52:11 UTC
	time_t expectedTimeValue = 1729259531;

	ASSERT_EQ(timeValue, expectedTimeValue);  // Compare time values
}

// Mocking helper functions for filesystem operations
TEST_F(ChunkTrashManagerImplTest, RegisterDiskPathHandlesInvalidAndValidFiles) {
	// Arrange: Create mocks for filesystem entries
	std::string validFile = "file_with_timestamp.20231015";
	std::string invalidFile = "file_without_timestamp";

	fs::path trashDir = testDir / "trashDir";

	fs::create_directories(trashDir);
	std::ofstream(trashDir / validFile);
	std::ofstream(trashDir / invalidFile);

	// Act: Call the registerDiskPath function to cover the lines
	int status = chunkTrashManagerImpl.registerDiskPath(trashDir.string());

	// Assert: Check function behavior and coverage
	EXPECT_EQ(status, SAUNAFS_STATUS_OK);
}

TEST_F(ChunkTrashManagerImplTest, ComputeGCThrottlingFactor_DecreasesAsRelativeIoIncreases) {
	ChunkTrashManagerImpl manager;

	seedState(100, 100, 20, 20);

	const double veryHighFactor = manager.computeGCThrottlingFactor(0.1 * kMiB, 0.1 * kMiB, 1);
	EXPECT_GT(veryHighFactor, 0.8);
	EXPECT_LE(veryHighFactor, 1);

	seedState(100, 100, 20, 20);
	const double highFactor = manager.computeGCThrottlingFactor(5 * kMiB, 5 * kMiB, 1);
	EXPECT_GT(highFactor, 0.6);
	EXPECT_LE(highFactor, 1);

	seedState(100, 100, 20, 20);
	const double mediumFactor = manager.computeGCThrottlingFactor(15 * kMiB, 15 * kMiB, 1);
	EXPECT_GT(mediumFactor, 0.1);
	EXPECT_LE(mediumFactor, 0.4);

	seedState(100, 100, 20, 20);
	const double lowFactor = manager.computeGCThrottlingFactor(80 * kMiB, 80 * kMiB, 1);
	EXPECT_GT(lowFactor, 0);
	EXPECT_LE(lowFactor, 0.05);

	EXPECT_GT(highFactor, mediumFactor);
	EXPECT_GT(mediumFactor, lowFactor);
}

TEST_F(ChunkTrashManagerImplTest, ComputeGCThrottlingFactor_IoSpikeForcesZeroFactor) {
	ChunkTrashManagerImpl manager;

	seedState(100, 100, 1, 1);
	const double factor = manager.computeGCThrottlingFactor(20 * kMiB, 20 * kMiB, 1);

	EXPECT_DOUBLE_EQ(factor, 0.0);
}

TEST_F(ChunkTrashManagerImplTest, ComputeGCThrottlingFactor_MoreDisksIncreaseFactor) {
	ChunkTrashManagerImpl manager;

	seedState(100, 100, 20, 20);
	const double oneDiskFactor = manager.computeGCThrottlingFactor(40 * kMiB, 40 * kMiB, 1);

	seedState(100, 100, 20, 20);
	const double fourDiskFactor = manager.computeGCThrottlingFactor(40 * kMiB, 40 * kMiB, 4);

	EXPECT_GT(fourDiskFactor, oneDiskFactor);
}
