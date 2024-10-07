#pragma once

// A fix for https://stackoverflow.com/q/77034039/10788155
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#include <filesystem>
#pragma GCC diagnostic pop

#include "common/platform.h"

#include "chunkserver-common/disk_with_fd.h"

class CmrDisk : public FDDisk {
public:
	/// Constructs a Disk with previous information from hdd.cfg and assigns
	/// default values.
	explicit CmrDisk(const std::string &_metaPath, const std::string &_dataPath,
	                 bool _isMarkedForRemoval, bool _isZonedDevice);

	/// Constructs a Disk from a Configuration object read from hdd.cfg and
	/// assigns default values.
	explicit CmrDisk(const disk::Configuration &configuration);

	// No need to copy or move them so far

	CmrDisk(const CmrDisk &) = delete;
	CmrDisk(CmrDisk &&) = delete;
	CmrDisk &operator=(const CmrDisk &) = delete;
	CmrDisk &operator=(CmrDisk &&) = delete;

	/// Virtual destructor needed for correct polymorphism
	~CmrDisk() = default;

	/// Tries to create the paths and subfolders for metaPath and dataPath
	///
	/// The calls to mkdir will fail if the paths already exists or if the
	/// effective user does not have permission to create the directories.
	void createPathsAndSubfolders() override;

	/// Creates the lock files for metadata and data directories
	void createLockFiles(
	    bool isLockNeeded,
	    std::vector<std::unique_ptr<IDisk>> &allDisks) override;

	/// Updates the disk usage information preserving the reserved space
	///
	/// No locks inside, should be locked by caller.
	void refreshDataDiskUsage() override;

	// Chunk operations

	/// Updates the Chunk attributes according to this kind of Disk
	int updateChunkAttributes(IChunk *chunk, bool isFromScan) override;

	/// Creates a new ChunkSignature for the given Chunk.
	/// Used mostly to write the metadata file of an existing in-memory Chunk.
	std::unique_ptr<ChunkSignature> createChunkSignature(
	    IChunk *chunk) override;

	/// Creates a new empty ChunkSignature that later will be filled with the
	/// information of the Chunk using readFromDescriptor.
	std::unique_ptr<ChunkSignature> createChunkSignature() override;

	void serializeEmptyChunkSignature(uint8_t **destination, uint64_t chunkId,
	                                  uint32_t chunkVersion,
	                                  ChunkPartType chunkType) override;

	/// Instantiates a new Chunk for this type of Disk.
	/// The ChunkState is CH_LOCKED by default.
	IChunk *instantiateNewConcreteChunk(uint64_t chunkId,
	                                    ChunkPartType type) override;

	void setChunkBlocks(IChunk *chunk, uint16_t originalBlocks,
	                    uint16_t newBlocks) override;

	int defragmentOrMoveChunk(IChunk *chunk, uint8_t *crcData) override;

	void updateAfterScan() override;

	// IO

	/// Creates physically the file for this Chunk, opens it and updates the fd
	void creat(IChunk *chunk) override;

	/// Opens the file for this Chunk and updates the fd
	void open(IChunk *chunk) override;

	/// Removes the Chunk filename from the filesystem
	int unlinkChunk(IChunk *chunk) override;

	/// Truncates the data file to size
	int ftruncateData(IChunk *chunk, uint64_t size) override;

	/// pread wrapper to allow custom implementations later
	ssize_t preadData(IChunk *chunk, uint8_t *blockBuffer, uint64_t size,
	                  uint64_t offset) override;

	/// Reads ahead blockCount blocks from firstBlock in an attempt to
	/// improve the performance of next reads.
	void prefetchChunkBlocks(IChunk &chunk, uint16_t firstBlock,
	                         uint32_t blockCount) override;

	/// Reads the CRC and the data for exactly one block
	///
	/// Assumes blockBuffer can fit both, data and CRC.
	/// Returns the number of read bytes on success or negative number if fails.
	int readBlockAndCrc(IChunk *chunk, uint8_t *blockBuffer, uint8_t *crcData,
	                    uint16_t blocknum, const char *errorMsg) override;

	/// Overwrites the Chunk version in the metadata file and in memory
	///
	/// Also regenerates the filenames according to the new version.
	int overwriteChunkVersion(IChunk *chunk, uint32_t newVersion) override;

	/// Writes the data and the CRC for exactly one block
	int writePartialBlockAndCrc(IChunk *chunk, const uint8_t *buffer,
	                            uint32_t offsetInBlock, uint32_t size,
	                            const uint8_t *crcBuff, uint8_t *crcData,
	                            uint16_t blockNum, bool isNewBlock,
	                            const char *errorMsg) override;

	/// If supported, deallocates space (creates a hole) in the byte range
	/// starting at offset and continuing for size bytes.
	///
	/// After a successful call, subsequent reads from this range will return
	/// zeros. The function is used to optimize space in sparse chunks.
	void punchHoles(IChunk *chunk, const uint8_t *buffer, uint32_t offset,
	                uint32_t size);

	/// Writes a Chunk block
	int writeChunkBlock(IChunk *chunk, uint32_t version, uint16_t blocknum,
	                    uint32_t offsetInBlock, uint32_t size, uint32_t crc,
	                    uint8_t *crcData, const uint8_t *buffer) override;

	/// Writes to device custom blockSize from blockBuffer
	int writeChunkData(IChunk *chunk, uint8_t *blockBuffer, int32_t blockSize,
	                   off64_t offset) override;

private:
	/// Helper function to get the current timestamp as a UTC string
	static std::string getDeletionTimeString();

	/// Move a file to the trash directory
	int moveToTrash(const std::filesystem::path& filePath, const std::filesystem::path& diskPath, const std::string& deletionTime) ;

	friend class CmrDiskTest_GetDeletionTimeStringTest_Test;
	friend class CmrDiskTest_MoveToTrashValidFile_Test;
	friend class CmrDiskTest_MoveToTrashNonExistentFile_Test;
	friend class CmrDiskTest_UnlinkChunkTest_Test;
};
