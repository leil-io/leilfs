// A fix for https://stackoverflow.com/q/77034039/10788155
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#include <filesystem>
#include <gtest/gtest.h>
#pragma GCC diagnostic pop

#include <fstream>

#include "cmr_disk.h"  // Include your CmrDisk header file
#include "chunk_with_fd.h"  // Include your FDChunk header file

class CmrDiskTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Create a temporary directory to use for the tests
		testDir = std::filesystem::temp_directory_path() / "cmr_disk_test";
		std::filesystem::create_directories(testDir);
	}

	void TearDown() override {
		// Clean up the temporary test directory after each test
//		std::filesystem::remove_all(testDir);
	}

	static std::filesystem::path testDir;
};

std::filesystem::path CmrDiskTest::testDir;

// Test getDeletionTimeString() to ensure it produces a UTC timestamp
TEST_F(CmrDiskTest, GetDeletionTimeStringTest) {
	std::string timestamp = CmrDisk::getDeletionTimeString();  // Now accessible due to friend declaration
	EXPECT_EQ(timestamp.size(), 14);  // Check that the timestamp length is correct (YYYYMMDDHHMMSS)
}

// Test moveToTrash() with a valid file path
TEST_F(CmrDiskTest, MoveToTrashValidFile) {
	// Create a dummy file in the test directory
	std::filesystem::path filePath = testDir / "test_file.txt";
	std::ofstream(filePath) << "dummy content";  // Create the file

	// Create a CmrDisk instance using one of its constructors
	CmrDisk cmrDiskInstance(testDir.string(),
	                        testDir.string(),
	                        false, false);

	// Call the moveToTrash function
	std::string diskPath = testDir.string();
	std::string deletionTime = CmrDisk::getDeletionTimeString();
	int result = cmrDiskInstance.moveToTrash(filePath, diskPath, deletionTime);

	// Check that the file was moved successfully
	std::filesystem::path expectedTrashPath = testDir / ".trash.bin" / ("test_file.txt." + deletionTime);
	EXPECT_TRUE(std::filesystem::exists(expectedTrashPath));
	EXPECT_EQ(result, SAUNAFS_STATUS_OK);
}

// Test moveToTrash() with a non-existent file
TEST_F(CmrDiskTest, MoveToTrashNonExistentFile) {
	// Define a non-existent file path
	std::filesystem::path filePath = testDir / "non_existent_file.txt";

	// Create a CmrDisk instance using one of its constructors
	CmrDisk cmrDiskInstance(testDir.string(),
	                        testDir.string(),
	                        false, false);

	// Call the moveToTrash function
	std::string diskPath = testDir.string();
	std::string deletionTime = CmrDisk::getDeletionTimeString();
	int result = cmrDiskInstance.moveToTrash(filePath, diskPath, deletionTime);

	// Check that the function returned the correct error code for a non-existent file
	EXPECT_EQ(result, SAUNAFS_ERROR_ENOENT);
}

// Test unlinkChunk() to ensure both meta and data files are moved to the trash
TEST_F(CmrDiskTest, UnlinkChunkTest) {
	// Mock the IChunk object
	class MockChunk : public FDChunk {
	public:
		MockChunk()
		    : FDChunk(0, ChunkPartType(detail::SliceType::kECFirst), ChunkState::Available) {}  // Call base class constructor with dummy values

		std::string metaFilename() const override { return (CmrDiskTest::testDir / "chunk_file.met").string(); }
		std::string dataFilename() const override { return (CmrDiskTest::testDir / "chunk_file.dat").string(); }

		// Implement all the pure virtual methods with simple stubs
		std::string generateDataFilenameForVersion(uint32_t) const override { return ""; }
		int renameChunkFile(uint32_t) override { return 0; }
		uint8_t* getChunkHeaderBuffer() const override { return nullptr; }
		size_t getHeaderSize() const override { return 0; }
		off_t getCrcOffset() const override { return 0; }
		void shrinkToBlocks(uint16_t) override {}
		bool isDirty() override { return false; }
		std::string toString() const override { return "MockChunk"; }
	};

	MockChunk chunk;

	// Create dummy meta and data files
	std::ofstream(chunk.metaFilename()) << "meta content";
	std::ofstream(chunk.dataFilename()) << "data content";

	// Create a CmrDisk instance using one of its constructors
	CmrDisk cmrDiskInstance(CmrDiskTest::testDir.string(),
	                        CmrDiskTest::testDir.string(),
	                        false, false);

	// Call the unlinkChunk function
	int result = cmrDiskInstance.unlinkChunk(&chunk);

	// Check that both files were moved to the trash successfully
	std::string deletionTime = CmrDisk::getDeletionTimeString();
	std::filesystem::path expectedMetaTrashPath = CmrDiskTest::testDir / ".trash.bin" / ("chunk_file.met." + deletionTime);
	std::filesystem::path expectedDataTrashPath = CmrDiskTest::testDir / ".trash.bin" / ("chunk_file.dat." + deletionTime);

	EXPECT_TRUE(std::filesystem::exists(expectedMetaTrashPath));
	EXPECT_TRUE(std::filesystem::exists(expectedDataTrashPath));
	EXPECT_EQ(result, SAUNAFS_STATUS_OK);
}
