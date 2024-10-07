// chunk_trash_manager_unittest.cc
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
		std::filesystem::remove_all(testDir);
	}
};

std::filesystem::path ChunkTrashManagerTest::testDir;

// Test getDeletionTimeString() to ensure it produces a UTC timestamp
TEST_F(ChunkTrashManagerTest, GetDeletionTimeStringTest) {
	ChunkTrashManager manager;
	std::string timestamp = manager.getDeletionTimeString();
	EXPECT_EQ(timestamp.size(), 14);  // Check that the timestamp length is correct (YYYYMMDDHHMMSS)
	EXPECT_TRUE(std::all_of(timestamp.begin(), timestamp.end(), ::isdigit));  // Check that all characters are digits
}

TEST_F(ChunkTrashManagerTest, MoveToTrashValidFile) {
	// Test the moveToTrash() method directly using ChunkTrashManager
	ChunkTrashManager manager;
	std::filesystem::path filePath = testDir / "test_file.txt";
	std::ofstream(filePath) << "dummy content";

	std::string deletionTime = manager.getDeletionTimeString();
	int result = manager.moveToTrash(filePath, testDir, deletionTime);

	std::filesystem::path expectedTrashPath = testDir / ChunkTrashManager::kTrashDirname / ("test_file.txt." + deletionTime);
	EXPECT_TRUE(std::filesystem::exists(expectedTrashPath));
	EXPECT_EQ(result, SAUNAFS_STATUS_OK);
}

TEST_F(ChunkTrashManagerTest, MoveToTrashNonExistentFile) {
	// Test the moveToTrash() method directly using ChunkTrashManager
	ChunkTrashManager manager;
	// Define a non-existent file path
	std::filesystem::path filePath = testDir / "non_existent_file.txt";

	std::string deletionTime = manager.getDeletionTimeString();
	int result = manager.moveToTrash(filePath, testDir, deletionTime);

	// Check that the function returned the correct error code for a non-existent file
	EXPECT_EQ(result, SAUNAFS_ERROR_ENOENT);
}

TEST_F(ChunkTrashManagerTest, MoveToTrashFileInNestedDirectory) {
	ChunkTrashManager manager;
	std::filesystem::path nestedDir = testDir / "nested/dir/structure";
	std::filesystem::create_directories(nestedDir);
	std::filesystem::path filePath = nestedDir / "test_file_nested.txt";
	std::ofstream(filePath) << "nested content";

	std::string deletionTime = manager.getDeletionTimeString();
	int result = manager.moveToTrash(filePath, testDir, deletionTime);

	std::filesystem::path expectedTrashPath = testDir / ChunkTrashManager::kTrashDirname / ("test_file_nested.txt." + deletionTime);
	EXPECT_TRUE(std::filesystem::exists(expectedTrashPath));
	EXPECT_EQ(result, SAUNAFS_STATUS_OK);
}
