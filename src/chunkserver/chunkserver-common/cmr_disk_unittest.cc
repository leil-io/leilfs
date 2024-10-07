#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#include <gtest/gtest.h>
#pragma GCC diagnostic pop
#include <fstream>
#include "chunk_trash_manager.h"
#include "cmr_disk.h"
#include "errors/saunafs_error_codes.h"
#include "chunk_with_fd.h"

class MockChunk : public FDChunk {
public:
	MockChunk()
	    : FDChunk(0, ChunkPartType(detail::SliceType::kECFirst), ChunkState::Available) {}

	MockChunk(const std::filesystem::path& metaFile, const std::filesystem::path& dataFile)
	    : FDChunk(0, ChunkPartType(detail::SliceType::kECFirst), ChunkState::Available),
	      metaFile_(metaFile.string()), dataFile_(dataFile.string()) {}

	std::string metaFilename() const override { return metaFile_; }
	std::string dataFilename() const override { return dataFile_; }

	// Implement all the pure virtual methods with simple stubs
	std::string generateDataFilenameForVersion(uint32_t) const override { return ""; }
	int renameChunkFile(uint32_t) override { return 0; }
	uint8_t* getChunkHeaderBuffer() const override { return nullptr; }
	size_t getHeaderSize() const override { return 0; }
	off_t getCrcOffset() const override { return 0; }
	void shrinkToBlocks(uint16_t) override {}
	bool isDirty() override { return false; }
	std::string toString() const override { return "MockChunk"; }

private:
	std::string metaFile_;
	std::string dataFile_;
};

class CmrDiskTest : public ::testing::Test {
protected:
	CmrDiskTest()
	    : cmrDiskInstance("", "",false, false) {}
	static std::filesystem::path testDir;
	CmrDisk cmrDiskInstance;

	void SetUp() override {
		testDir = std::filesystem::temp_directory_path() / "cmr_disk_test";
		std::filesystem::create_directories(testDir);

		// Create dummy disk paths
		cmrDiskInstance.setMetaPath(testDir / "meta");
		cmrDiskInstance.setDataPath(testDir / "data");
		std::filesystem::create_directories(cmrDiskInstance.metaPath());
		std::filesystem::create_directories(cmrDiskInstance.dataPath());
	}

	void TearDown() override {
		std::filesystem::remove_all(testDir);
	}
};

std::filesystem::path CmrDiskTest::testDir;

TEST_F(CmrDiskTest, UnlinkChunkSuccessful) {
	// Create a dummy meta and data file in the test directory
	std::filesystem::path metaFile = std::filesystem::path(cmrDiskInstance.metaPath()) / "chunk_meta_file.txt";
	std::filesystem::path dataFile = std::filesystem::path(cmrDiskInstance.dataPath()) / "chunk_data_file.txt";
	std::ofstream(metaFile) << "meta content";
	std::ofstream(dataFile) << "data content";

	MockChunk chunk(metaFile, dataFile);

	// Call the unlinkChunk method
	int result = cmrDiskInstance.unlinkChunk(&chunk);

	// Capture the deletion time immediately after unlinking to ensure consistency
	std::string deletionTime = ChunkTrashManager::getDeletionTimeString();
	std::filesystem::path expectedMetaTrashPath = std::filesystem::path(cmrDiskInstance.metaPath()) / ChunkTrashManager::kTrashDirname / (metaFile.filename().string() + "." + deletionTime);
	std::filesystem::path expectedDataTrashPath = std::filesystem::path(cmrDiskInstance.dataPath()) / ChunkTrashManager::kTrashDirname / (dataFile.filename().string() + "." + deletionTime);

	// Verify that the meta and data files were moved to the trash
	EXPECT_TRUE(std::filesystem::exists(expectedMetaTrashPath));
	EXPECT_TRUE(std::filesystem::exists(expectedDataTrashPath));
	EXPECT_EQ(result, SAUNAFS_STATUS_OK);
}

TEST_F(CmrDiskTest, UnlinkChunkMetaFileMissing) {
	// Create only the data file
	std::filesystem::path dataFile = std::filesystem::path(cmrDiskInstance.dataPath()) / "chunk_data_file.txt";
	std::ofstream(dataFile) << "data content";

	MockChunk chunk("non_existent_meta_file.txt", dataFile);

	// Call the unlinkChunk method
	int result = cmrDiskInstance.unlinkChunk(&chunk);

	// Check that the correct error code is returned
	EXPECT_EQ(result, SAUNAFS_ERROR_ENOENT);
}

TEST_F(CmrDiskTest, UnlinkChunkDataFileMissing) {
	// Create only the meta file
	std::filesystem::path metaFile = std::filesystem::path (cmrDiskInstance.metaPath()) / "chunk_meta_file.txt";
	std::ofstream(metaFile) << "meta content";

	MockChunk chunk(metaFile, "non_existent_data_file.txt");

	// Call the unlinkChunk method
	int result = cmrDiskInstance.unlinkChunk(&chunk);

	// Check that the correct error code is returned
	EXPECT_EQ(result, SAUNAFS_ERROR_ENOENT);
}

TEST_F(CmrDiskTest, UnlinkChunkDiskPathError) {
	// Set disk paths to empty strings to simulate a disk path error
	cmrDiskInstance.setMetaPath("");
	cmrDiskInstance.setDataPath("");

	MockChunk chunk("chunk_meta_file.txt", "chunk_data_file.txt");

	// Call the unlinkChunk method
	int result = cmrDiskInstance.unlinkChunk(&chunk);

	// Check that the correct error code is returned
	EXPECT_EQ(result, SAUNAFS_ERROR_ENOENT);
}
