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
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "chunkserver-common/disk_interface.h"
#include "common/chunk_part_type.h"

#define CHUNK_METADATA_FILE_EXTENSION ".met"
#define CHUNK_DATA_FILE_EXTENSION     ".dat"

constexpr uint32_t kCrcSize = sizeof(uint32_t);
constexpr uint32_t kHddBlockSize = SFSBLOCKSIZE + kCrcSize;

constexpr size_t kMaxCrcBlockSize = SFSBLOCKSINCHUNK * kCrcSize;
using CrcDataContainer = std::array<uint8_t, kMaxCrcBlockSize>;

/// Possible Chunk states.
enum class ChunkState : std::uint8_t {
	Available   = 0,  ///< The was already released (not used by any thread)
	Locked      = 10, ///< The chunk was acquired in a thread
	Deleted     = 20, ///< The chunk is ready to be deleted
	ToBeDeleted = 30  ///< The chunk was locked and eventually will be deleted
};

/// Condition variable with a count of threads which are waiting on it.
struct CondVarWithWaitCount {
	std::condition_variable condVar;
	uint32_t numberOfWaitingThreads = 0;
};

/// Possible Chunk formats.
/// Additional formats may be added in the future.
/// SPLIT format is used by default.
enum class ChunkFormat {
	IMPROPER,  ///< Not valid format detected or uninitialized Chunk
	SPLIT      ///< The Chunk is split in two files (metadata and data)
};

/**
 * Chunk is a part of a file stored on the filesystem.
 *
 * Each Chunk is divided into two files in this implementation:
 * - metadata file
 * - data file
 **/
class IChunk {
public:
	/// Default constructor
	IChunk() = default;

	// No need to copy or move them so far

	IChunk(const IChunk &) = delete;
	IChunk(IChunk &&) = delete;
	IChunk &operator=(const IChunk &) = delete;
	IChunk &operator=(IChunk &&) = delete;

	/// Virtual destructor needed for correct polymorphism
	virtual ~IChunk() = default;

	/// Returns the full path to the metadata file.
	virtual std::string fullMetaFilename() const = 0;

	/// Getter for the name of the metadata file.
	virtual const std::string metaFilename() const = 0;

	/// Returns the full path to the data file.
	virtual std::string fullDataFilename() const = 0;

	/// Getter for the name of the data file.
	virtual const std::string dataFilename() const = 0;

	/// Generates the name of the metadata file for the given version.
	virtual std::string generateMetadataFilenameForVersion(
	    uint32_t _version) const = 0;

	/// Generates the name of the data file for the given version.
	virtual std::string generateDataFilenameForVersion(
	    uint32_t _version) const = 0;

	/// Generates metadata filename (if isForMetadata is true) or data filename
	/// for the given version.
	virtual std::string generateFilenameForVersion(
	    uint32_t _version, bool isForMetadata) const = 0;

	/// Renames the metadata and data filenames according to the new version.
	/// This function updates the filenames and also renames the files on the
	/// the filesystem.
	virtual int renameChunkFile(uint32_t new_version) = 0;

	/// Returns a pointer to the buffer containing the Chunk header.
	/// The returned pointer is assumed to be thread local.
	/// Check the implementation of CMRDisk as reference.
	virtual uint8_t* getChunkHeaderBuffer() const = 0;

	/// Returns the offset in the data file for the requested blockNumber. This
	/// value is usually blockNumber times SFSBLOCKSIZE.
	virtual off_t getBlockOffset(uint16_t blockNumber) const = 0;

	/// Returns the size of the data file for the given blockCount. Usually,
	/// this value is blockCount times SFSBLOCKSIZE.
	virtual off_t getFileSizeFromBlockCount(uint32_t blockCount) const = 0;

	/// Returns true if the data file size is valid for the given fileSize.
	/// A data file size is valid if it is a multiple of SFSBLOCKSIZE.
	virtual bool isDataFileSizeValid(off_t fileSize) const = 0;

	/// Returns the offset for the signature of the Chunk.
	virtual off_t getSignatureOffset() const = 0;

	/// Performs a read-ahead operation on the Chunk header to improve file
	/// reading performance.
	virtual void readaheadHeader() const = 0;

	/// Returns the Chunk header size. Usually, the header size contains the
	/// signature and the CRC blocks, but can contain other data, like the
	/// fragments in case of chunks supporting fragmentation.
	virtual size_t getHeaderSize() const = 0;

	/// Returns the offset for the CRC. It is often the size of the signature,
	/// but it may be different for some Chunk types.
	virtual off_t getCrcOffset() const = 0;

	/// Returns the size of the CRC block.
	/// Usually, sizeof(uint32_t) * maxBlocksInFile(), if mycrc32 is used.
	virtual size_t getCrcBlockSize() const = 0;

	/// Returns the maximum number of full blocks that can be stored in a
	/// Chunk part. Hints:
	/// For standard Chunks, should be SFSBLOCKSINCHUNK.
	/// For EC(m,n) should be SFSBLOCKSINCHUNK / m.
	virtual uint32_t maxBlocksInFile() const = 0;

	/// Sets the number of blocks for the given fileSize. The given fileSize
	/// must be a multiple of SFSBLOCKSIZE.
	virtual void setBlockCountFromDataFileSize(off_t fileSize) = 0;

	/// Shrink the chunk to the given number of blocks. This is used when
	/// truncating a file.
	virtual void shrinkToBlocks(uint16_t newBlocks) = 0;

	/// Releases memory the chunk caches only to speed up I/O (e.g. per-chunk
	/// compression dictionaries). Called when the chunk leaves the open-chunks
	/// cache (its descriptors are being closed), with the chunk locked, so the
	/// caches can be rebuilt lazily on the next I/O. Default: no-op.
	virtual void releaseCachedResources() noexcept {}

	/// Returns the Chunk format. Check \ref ChunkFormat for more information.
	virtual ChunkFormat chunkFormat() const = 0;

	/// Returns the Chunk type. Check \ref ChunkPartType for more information.
	virtual ChunkPartType type() const = 0;

	/// Returns the Chunk id.
	virtual uint64_t id() const = 0;
	/// Sets the Chunk id.
	virtual void setId(uint64_t newId) = 0;

	/// Returns the Chunk version.
	virtual uint32_t version() const = 0;
	/// Sets the Chunk version.
	virtual void setVersion(uint32_t _version) = 0;

	/// Returns a string representation of the Chunk members.
	virtual std::string toString() const = 0;

	/// Returns the metadata file descriptor.
	virtual int32_t metaFD() const = 0;
	/// Returns the data file descriptor.
	virtual int32_t dataFD() const = 0;

	/// Sets the metadata file descriptor.
	virtual void setMetaFD(int32_t newMetaFD) = 0;
	/// Sets the data file descriptor.
	virtual void setDataFD(int32_t newDataFD) = 0;

	/// Returns 1 if the attributes were recently updated, 0 otherwise.
	virtual uint8_t validAttr() const = 0;
	/// Sets the value of validAttr.
	virtual void setValidAttr(uint8_t newValidAttr) = 0;

	/// Returns 1 if the Chunk was changed since the last flush, 0 otherwise.
	virtual uint8_t wasChanged() const = 0;
	/// Sets the value of wasChanged.
	virtual void setWasChanged(uint8_t newWasChanged) = 0;

	/// Epoch of the last master-driven registration sweep which reported this
	/// chunk (see hddRegistrationSweep* in hddspacemgr). Guarded by
	/// gChunksMapMutex, like the registry itself.
	virtual uint32_t registrationEpoch() const = 0;
	/// Sets the registration sweep epoch.
	virtual void setRegistrationEpoch(uint32_t newEpoch) = 0;

	/// Returns the number of blocks in the Chunk.
	virtual uint16_t blocks() const = 0;
	/// Sets the number of blocks in the Chunk.
	virtual void setBlocks(uint16_t newBlocks) = 0;

	/// Returns Disk where the Chunk resides.
	virtual IDisk *owner() const = 0;
	/// Sets the owner of the Chunk. Should be enough to set the owner at
	/// instantiation time.
	virtual void setOwner(IDisk *newOwner) = 0;

	/// Returns the index of the Chunk in the Disk.
	virtual size_t indexInDisk() const = 0;
	/// Sets the index of the Chunk in the Disk. Used to select the chunks to be
	/// tested.
	virtual void setIndexInDisk(size_t newIndexInDisk) = 0;

	/// Returns Chunk state, the state is used mainly for multithreading.
	virtual ChunkState state() const = 0;
	/// Sets the state of the Chunk.
	virtual void setState(ChunkState newState) = 0;

	/// Returns a reference to the condition variable of the Chunk.
	virtual std::unique_ptr<CondVarWithWaitCount> &condVar() = 0;
	/// Sets the condition variable of the Chunk, taking ownership.
	virtual void setCondVar(
	    std::unique_ptr<CondVarWithWaitCount> &&newCondVar) = 0;

	/// Returns the number of threads referencing this Chunk.
	/// The Chunk can only be released if the refCount is 0.
	virtual uint16_t refCount() const = 0;
	/// Sets the number of references of the Chunk.
	virtual void setRefCount(uint16_t newRefCount) = 0;

	/// Returns the number of blocks expected to be read next. This is used to
	/// improve the read performance by reading ahead some blocks.
	virtual uint16_t blockExpectedToBeReadNext() const = 0;
	/// Sets the new block expected to be read.
	virtual void setBlockExpectedToBeReadNext(
	    uint16_t newBlockExpectedToBeReadNext) = 0;
};
