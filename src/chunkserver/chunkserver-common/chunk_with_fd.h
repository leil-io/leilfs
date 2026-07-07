/*
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

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

#pragma once

#include "common/platform.h"

#include <malloc.h>
#include <sys/types.h>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "chunkserver-common/chunk_interface.h"
#include "chunkserver-common/disk_interface.h"
#include "common/chunk_part_type.h"

/**
 * FDChunk is a specialization of IChunk for Chunks based on file descriptors.
 *
 * FDChunk contains common attributes and functions needed by Chunks based on
 * file descriptors.
 **/
class FDChunk : public IChunk {
public:
	static const size_t kMaxSignatureBlockSize = 1024;
	static const size_t kMaxCrcBlockSize = SFSBLOCKSINCHUNK * kCrcSize;
	static const size_t kMaxPaddingBlockSize = 4096;
	static const size_t kMaxHeaderSize =
	    kMaxSignatureBlockSize + kMaxCrcBlockSize + kMaxPaddingBlockSize;
	static const size_t kDiskBlockSize = 4096;

	/// Constructs a Chunk with the given ID, type and state.
	explicit FDChunk(uint64_t chunkId, ChunkPartType type, ChunkState state);

	// No need to copy or move them so far

	FDChunk(const FDChunk &) = delete;
	FDChunk(FDChunk &&) = delete;
	FDChunk &operator=(const FDChunk &) = delete;
	FDChunk &operator=(FDChunk &&) = delete;

	/// Virtual destructor needed for correct polymorphism
	virtual ~FDChunk() = default;

	/// Returns the full path to the metadata file.
	std::string fullMetaFilename() const override;

	/// Getter for the name of the metadata filename.
	const std::string metaFilename() const override;

	/// Returns the full path to the data file.
	std::string fullDataFilename() const override;

	/// Getter for the name of the data filename.
	const std::string dataFilename() const override;

	/// Generates the metadata filename for the given version.
	std::string generateMetadataFilenameForVersion(
	    uint32_t _version) const override;

	/// Generates metadata filename (if isForMetadata is true) or data filename
	/// for the given version.
	std::string generateFilenameForVersion(uint32_t _version,
	                                       bool isForMetadata) const override;

	/// Returns the offset in the data file for the requested blockNumber.
	off_t getBlockOffset(uint16_t blockNumber) const override;
	/// Returns the size of the data file for the given blockCount.
	off_t getFileSizeFromBlockCount(uint32_t blockCount) const override;
	/// Returns true if the data file size is valid for the given fileSize.
	bool isDataFileSizeValid(off_t fileSize) const override;

	/// Returns the offset for the signature.
	off_t getSignatureOffset() const override;
	/// Reads the Chunk header.
	void readaheadHeader() const override;

	/// Returns the size of the CRC block.
	size_t getCrcBlockSize() const override;
	/// Returns the maximum number of blocks in the data file.
	uint32_t maxBlocksInFile() const override;
	/// Sets the number of blocks for the given fileSize.
	void setBlockCountFromDataFileSize(off_t fileSize) override;

	/// Returns the Chunk format.
	ChunkFormat chunkFormat() const override;
	/// Returns the Chunk type.
	ChunkPartType type() const override;

	/// Returns the number of blocks in the Chunk.
	uint16_t blocks() const override;
	/// Sets the number of blocks in the Chunk.
	void setBlocks(uint16_t newBlocks) override;

	/// Returns the Chunk id.
	uint64_t id() const override;
	/// Sets the Chunk id.
	void setId(uint64_t newId) override;

	/// Returns the Chunk version.
	uint32_t version() const override;
	/// Sets the Chunk version.
	void setVersion(uint32_t _version) override;

	/// Returns the metadata file descriptor.
	int32_t metaFD() const override;
	/// Returns the data file descriptor.
	int32_t dataFD() const override;

	/// Sets the metadata file descriptor.
	void setMetaFD(int32_t newMetaFD) override;
	/// Sets the data file descriptor.
	void setDataFD(int32_t newDataFD) override;

	/// Returns the valid attribute.
	uint8_t validAttr() const override;
	/// Sets the valid attribute.
	void setValidAttr(uint8_t newValidAttr) override;

	/// Returns the wasChanged attribute.
	uint8_t wasChanged() const override;
	/// Sets the wasChanged attribute.
	void setWasChanged(uint8_t newWasChanged) override;

	/// Returns the registration sweep epoch.
	uint8_t registrationEpoch() const override { return registrationEpoch_; }
	/// Sets the registration sweep epoch.
	void setRegistrationEpoch(uint8_t newEpoch) override { registrationEpoch_ = newEpoch; }

	/// Returns the owner of the Chunk.
	IDisk *owner() const override;
	/// Sets the owner of the Chunk.
	void setOwner(IDisk *newOwner) override;

	/// Returns the index of the Chunk in the Disk.
	size_t indexInDisk() const override;
	/// Sets the index of the Chunk in the Disk.
	void setIndexInDisk(size_t newIndexInDisk) override;

	/// Returns the state of the Chunk.
	ChunkState state() const override;
	/// Sets the state of the Chunk.
	void setState(ChunkState newState) override;

	/// Returns a pointer to the condition variable of the Chunk.
	std::unique_ptr<CondVarWithWaitCount> &condVar() override;
	/// Sets the condition variable of the Chunk.
	void setCondVar(
	    std::unique_ptr<CondVarWithWaitCount> &&newCondVar) override;

	/// Returns the number of references to the Chunk.
	uint16_t refCount() const override;
	/// Sets the number of references to the Chunk.
	void setRefCount(uint16_t newRefCount) override;

	/// Returns the number of blocks expected to be read next.
	uint16_t blockExpectedToBeReadNext() const override;
	/// Sets the number of blocks expected to be read next.
	void setBlockExpectedToBeReadNext(
	    uint16_t newBlockExpectedToBeReadNext) override;

private:
	/// The pointer to the condition variable on which threads wait until this
	/// chunk is unlocked.
	///
	/// We only assign a condition variable to a chunk which is locked
	/// and another thread attempts to lock it.
	/// Guarded by `gChunksMapMutex`.
	std::unique_ptr<CondVarWithWaitCount> condVar_ = nullptr;

	IDisk *owner_ = nullptr;     ///< The Disk which owns this Chunk.
	uint64_t id_;                ///< The ID of the chunk.
	uint32_t version_ = 0;       ///< The version of the chunk.
	bool isVersionSet_ = false;  ///< Tells if the version was set at least once
	ChunkPartType type_;         ///< The type of the chunk (ec:5, xor:2, etc.).
	int32_t metaFD_ = -1;        ///< Metadata file descriptor
	int32_t dataFD_ = -1;        ///< Data file descriptor
	uint16_t blocks_ = 0;        ///< Number of blocks in the chunk
	uint16_t refCount_ = 0;      ///< Used to properly release the chunk
	uint16_t blockExpectedToBeReadNext_ = 0;  ///< Read ahead helper
	uint8_t validAttr_ = 0;   ///< Tells if the attributes were recently updated
	uint8_t wasChanged_ = 0;  ///< Tells if it was changed from last flush
	/// Epoch of the last pull-registration sweep reporting this chunk
	uint8_t registrationEpoch_ = 0;
	ChunkState state_;        ///< The state of the chunk

	/// The index of the chunk within the Disk.
	///
	/// This value may change during the lifetime of the chunk, and corresponds
	/// to the order in which the chunk will be subject to checksum testing.
	///
	/// This is `kInvalidIndex` if the value
	/// is not valid, i.e., when the chunk is not assigned into a Disk yet.
	///
	/// This value is governed by the owning Disk's `chunks` collection.
	size_t indexInDisk_ = DiskChunks::kInvalidIndex;
};

// Get thread specific buffer
#ifdef SAUNAFS_HAVE_THREAD_LOCAL

inline uint8_t *getChunkBlockBuffer() {
	// Align to IO block size to be able to use O_DIRECT
	alignas(disk::kIoBlockSize) static thread_local std::array<
	    uint8_t, disk::kIoBlockSize + SFSBLOCKSIZE>
	    blockbuffer;
	return blockbuffer.data() + (disk::kIoBlockSize - kCrcSize);
}

inline uint8_t *getAlignedBlockBuffer() {
	// Align to IO block size to be able to use O_DIRECT
	alignas(disk::kIoBlockSize) static thread_local std::array<
	    uint8_t, disk::kIoBlockSize + SFSBLOCKSIZE>
	    extraBlockbuffer;
	return extraBlockbuffer.data();
}

#else  // SAUNAFS_HAVE_THREAD_LOCAL

inline static pthread_key_t hdrbufferkey;
inline static pthread_key_t blockbufferkey;

inline void initializeThreadLocal() {
	zassert(pthread_key_create(&hdrbufferkey, free));
	zassert(pthread_key_create(&blockbufferkey, free));
}

inline uint8_t *getChunkBlockBuffer() {
	// Pad in order to make block data aligned in cache (helps CRC)
	static constexpr int kMaxCacheLine = 64;
	static constexpr int kPadding = kMaxCacheLine - kCrcSize;
	uint8_t *blockbuffer =
	    static_cast<uint8_t *>(pthread_getspecific(blockbufferkey));
	if (blockbuffer == NULL) {
		blockbuffer = static_cast<uint8_t *>(malloc(kHddBlockSize + kPadding));
		passert(blockbuffer);
		zassert(pthread_setspecific(blockbufferkey, blockbuffer));
	}
	return blockbuffer + kPadding;
}

#endif  // SAUNAFS_HAVE_THREAD_LOCAL
