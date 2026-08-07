/*
   Copyright 2023 Leil Storage

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

#include "chunkserver-common/chunk_signature.h"
#include "chunkserver-common/disk_chunks.h"
#include "chunkserver-common/disk_utils.h"
#include "common/chunk_part_type.h"
#include "common/disk_info.h"

constexpr IDisk* DiskNotFound = nullptr;

class IChunk;

/// Shared with dlopen'ed Disk plugins, so changing it breaks every plugin
/// built against an older revision of the same release.
///
/// Represents a data disk in the Chunkserver context.
///
/// Each Disk maps to a single line in the hdd.cfg file.
/// In production, the recommended configuration is one Disk per physical drive.
/// For tests, it is fine to have different "Disks" pointing to the same device.
/// Entries starting with '*' are considered marked for removal, and the system
/// will copy their Chunks to other Chunkservers or Disks.
/// E.g.:
/// /mnt/hdd_02
/// /mnt/ssd_04
/// */mnt/ssd_08
///
/// The metadata and data folders can be in different directories, or even
/// devices. Allowing for example, to have the Chunk data parts (big) in
/// mechanical drives, while the metadata parts (small) are stored in NVMe.
/// E.g.:
/// /mnt/nvme01 | /mnt/hdd01
/// The previous line says that the metadata parts will be stored in /mnt/nvme01
/// while the data parts will be in /mnt/hdd01.
class IDisk {
public:
	enum class ScanState {
		kNeeded = 0U,  ///< Scanning is scheduled (thread is not running yet).
		kInProgress = 1U,      ///< Scan in progress (thread is running).
		kTerminate = 2U,       ///< Scanning thread should stop scanning ASAP.
		kThreadFinished = 3U,  ///< The scanning thread has finished its work
		                       ///< and can be joined.
		kSendNeeded = 4U,      ///< Resend the content of this Disk to Master
		kWorking = 5U          ///< Scan is complete and the Disk can be used.
	};

	/// Default constructor
	IDisk() = default;

	// No need to copy or move them so far

	IDisk(const IDisk &) = delete;
	IDisk(IDisk &&) = delete;
	IDisk &operator=(const IDisk &) = delete;
	IDisk &operator=(IDisk &&) = delete;

	/// Virtual destructor needed for correct polymorphism
	virtual ~IDisk() = default;

	/// Tells if this Disk is 'logically' marked for deletion.
	///
	/// Disks are considered marked for deletion, from the master's
	/// perspective, if they are explicitly marked for removal in the hdd.cfg
	/// file (* at the beginning) or if it is on a read-only file system.
	virtual bool isMarkedForDeletion() const = 0;

	/// Tells if this Disk is a Zoned device. A Zoned device is the one that
	/// have their address space divided into zones, for instance SMR drives.
	virtual bool isZonedDevice() const = 0;

	/// Tells if block N of a Chunk on this Disk lives at N * SFSBLOCKSIZE in
	/// the data file.
	///
	/// When true, the caller may compute data-file offsets itself, truncate the
	/// data file to a logical length and append raw bytes with writeChunkData().
	/// When false the Disk owns its own data layout, so every access must go
	/// through the block-level API (writeChunkBlock/writeChunkBlocks/preadData).
	virtual bool hasImplicitBlockOffsets() const = 0;

	/// Tells if this Disk is suitable for storing new chunks, according to its
	/// general state (available space, not readonly, etc.).
	virtual bool isSelectableForNewChunk() const = 0;

	/// Returns a DiskInfo object from the information inside this Disk.
	/// This information is usually sent to master.
	virtual DiskInfo toDiskInfo() const = 0;

	/// Retrieves the paths in the format [*]metaPath[ | dataPath].
	/// [] means optional, depending on current configuration.
	virtual std::string getPaths() const = 0;

	/// Tries to create the paths and subfolders for metaPath and dataPath
	///
	/// The calls to mkdir will fail if the paths already exists or if the
	/// effective user does not have permission to create the directories.
	virtual void createPathsAndSubfolders() = 0;

	/// Creates the lock files for metadata and data directories
	virtual void createLockFiles(
	    bool isLockNeeded, std::vector<std::unique_ptr<IDisk>> &allDisks) = 0;

	/// Updates the disk usage information preserving the reserved space.
	/// No locks inside, should be locked by caller.
	virtual void refreshDataDiskUsage() = 0;

	// Chunk operations

	/// Updates the Chunk attributes according to this kind of Disk
	virtual int updateChunkAttributes(IChunk *chunk, bool isFromScan) = 0;

	/// Writes the crcData in the correct offset of the Chunks' file descriptor
	virtual ssize_t writeCrc(IChunk *chunk, uint8_t *crcData) = 0;

	/// Creates a new ChunkSignature for the given Chunk.
	/// Used mostly to write the metadata file of an existing in-memory Chunk.
	virtual std::unique_ptr<ChunkSignature> createChunkSignature(
	    IChunk *chunk) = 0;

	/// Creates a new empty ChunkSignature that later will be filled with the
	/// information of the Chunk using readFromDescriptor.
	virtual std::unique_ptr<ChunkSignature> createChunkSignature() = 0;

	/// Initializes an empty metadata signature in the provided buffer.
	/// Useful in the duplicate and duplicate-truncate operations.
	/// @param formatSource Chunk whose on-disk format the signature must match;
	///        null selects the legacy default.
	virtual void serializeEmptyChunkSignature(uint8_t **destination,
	                                          uint64_t chunkId,
	                                          uint32_t chunkVersion,
	                                          ChunkPartType chunkType,
	                                          const IChunk *formatSource = nullptr) = 0;

	/// Instantiates a new Chunk for this type of Disk.
	/// The ChunkState is CH_LOCKED by default.
	virtual IChunk *instantiateNewConcreteChunk(uint64_t chunkId,
	                                            ChunkPartType type) = 0;

	/// Applies any disk-specific on-disk format to a brand-new chunk before its
	/// header is sized and serialized. Called when creating a chunk and when
	/// duplicating one: a duplicate adopts this disk's configured format
	/// instead of the source chunk's. Default: no-op.
	virtual int applyNewChunkFormat(IChunk * /*chunk*/) = 0;

	/// Sets the number of blocks for \a chunk from \a originalBlocks to \a
	/// newBlocks.
	/// @return SAUNAFS_STATUS_OK on success, or a SAUNAFS_ERROR_* code if the
	///         new layout could not be persisted.
	virtual int setChunkBlocks(IChunk *chunk, uint16_t originalBlocks,
	                           uint16_t newBlocks) = 0;

	/// Updates this disk attributes after a scan.
	/// Useful for SMR drives, for instance, to update the zones state after
	/// knowing all the Chunks.
	virtual void updateAfterScan() = 0;

	// IO

	/// Creates physically the file for this Chunk, opens it and updates the fd
	virtual void creat(IChunk *chunk) = 0;

	/// Opens the file for this Chunk and updates the fd
	virtual void open(IChunk *chunk) = 0;

	/// Removes the Chunk filename from the filesystem
	virtual int unlinkChunk(IChunk *chunk) = 0;

	/// Synchronize the Chunk state with the storage device
	virtual int fsyncChunk(IChunk *chunk) = 0;

	/// Truncates the data file to size
	virtual int ftruncateData(IChunk *chunk, uint64_t size) = 0;

	/// Reads \a size bytes starting at \a offset into \a blockBuffer
	virtual ssize_t preadData(IChunk *chunk, uint8_t *blockBuffer,
	                          uint64_t size, uint64_t offset) = 0;

	/// lseeks the metadata file descriptor
	///
	/// Should be possible for all Disk types if the metadata is stored in CMR
	/// drives.
	virtual off64_t lseekMetadata(IChunk *chunk, off64_t offset,
	                              int whence) = 0;

	/// lseeks the data file descriptor if possible
	virtual off64_t lseekData(IChunk *chunk, off64_t offset, int whence) = 0;

	/// Reads the complete CRC into the provided buffer.
	/// This buffer usually matches with the gOpenChunks entry for this Chunk.
	virtual int readChunkCrc(IChunk *chunk, uint32_t chunkVersion,
	                         uint8_t *buffer) = 0;

	/// Reads ahead blockCount blocks from firstBlock in an attempt to
	/// improve the performance of next reads.
	virtual void prefetchChunkBlocks(IChunk &chunk, uint16_t firstBlock,
	                                 uint32_t blockCount) = 0;

	/// Reads the CRC and the data for exactly one block
	///
	/// Assumes blockBuffer can fit both, data and CRC.
	/// Returns the number of read bytes on success, -1 on failure.
	virtual int readBlockAndCrc(IChunk *chunk, uint8_t *blockBuffer,
	                            uint8_t *crcData, uint16_t blocknum,
	                            const char *errorMsg) = 0;

	/// Overwrites the Chunk version in the metadata file and in memory.
	/// Also regenerates the filenames according to the new version.
	virtual int overwriteChunkVersion(IChunk *chunk, uint32_t newVersion) = 0;

	/// Writes the data and the CRC for exactly one block
	/// \returns number of written bytes on success or the negative of the error code on failure.
	virtual int writePartialBlockAndCrc(IChunk *chunk, const uint8_t *buffer,
	                                    uint32_t offsetInBlock, uint32_t size,
	                                    const uint8_t *crcBuff, uint8_t *crcData, uint16_t blockNum,
	                                    bool isNewBlock, const char *errorMsg) = 0;
	/// Writes the data and the CRC for a number of full blocks
	/// \returns number of written bytes on success or the negative of the error code on failure.
	/// If the written bytes are less than expected, it is IO error.
	virtual int writeFullBlocksAndCrcs(IChunk *chunk, const std::vector<uint16_t> &blocksPerBuffer,
	                                   const std::vector<const uint8_t *> &buffers,
	                                   uint16_t startBlock, const uint8_t *crcBuff,
	                                   uint8_t *crcData, bool areNewBlocks,
	                                   const char *errorMsg) = 0;

	/// Writes a Chunk block
	/// \return SAUNAFS_STATUS_OK on success or specific SAUNAFS_ error code
	virtual int writeChunkBlock(IChunk *chunk, uint32_t version,
	                            uint16_t blocknum, uint32_t offsetInBlock,
	                            uint32_t size, uint32_t crc, uint8_t *crcData,
	                            const uint8_t *buffer, bool isFromReplication = false) = 0;
	/// Writes `numBlocks` full Chunk blocks
	/// \returns number of written bytes on success or the negative of the error code on failure.
	/// If the written bytes are less than expected, it is IO error.
	virtual int writeChunkBlocks(IChunk *chunk, uint32_t version, uint16_t startBlock,
	                             uint16_t numBlocks, std::vector<uint32_t> &crc, uint8_t *crcData,
	                             const std::vector<uint16_t> &blocksPerBuffer,
	                             const std::vector<const uint8_t *> &buffers,
	                             bool isFromReplication = false) = 0;

	/// Writes the Chunk header into the device
	///
	/// Assumes that the thread local header buffer was filled with correct
	/// in memory information (header + CRC).
	/// \return SAUNAFS_STATUS_OK on success or SAUNAFS_ERROR_IO
	virtual int writeChunkHeader(IChunk *chunk) = 0;

	/// Writes to device custom blockSize from blockBuffer.
	/// \returns the written bytes
	virtual int writeChunkData(IChunk *chunk, uint8_t *blockBuffer,
	                           int32_t blockSize, off64_t offset) = 0;

	/// Getter for currentStats
	virtual HddAtomicStatistics &getCurrentStats() = 0;
	/// Returns the statistics for the last 24 hours
	virtual std::array<HddStatistics, disk::kStatsHistoryIn24Hours>
	    &stats() = 0;
	/// Current stats position in the history
	virtual uint32_t statsPos() const = 0;
	/// Helper to rotate the current statistics in the history
	virtual void setStatsPos(uint32_t newStatsPos) = 0;

	/// Getter for chunks in this Disk.
	/// Utility to facilitate selections of chunks to be tested.
	virtual DiskChunks &chunks() = 0;

	/// Returns true if a read-only file system is detected in the Disk
	virtual bool isReadOnly() const = 0;
	/// Setter for isReadOnly
	virtual void setIsReadOnly(bool newIsReadOnly) = 0;

	/// Returns true if the Disk usage needs to be updated
	virtual bool needRefresh() const = 0;
	/// Setter for needRefresh
	virtual void setNeedRefresh(bool newNeedRefresh) = 0;

	/// Returns timestamp in seconds of the last time the Disk was refreshed
	virtual uint32_t lastRefresh() const = 0;
	/// Setter for lastRefresh
	virtual void setLastRefresh(uint32_t newLastRefresh) = 0;

	/// Returns the path of the metadata directory
	virtual const std::string &metaPath() const = 0;
	/// Returns the path of the data directory
	virtual const std::string &dataPath() const = 0;

	/// Returns the reserved space in bytes defined by configuration
	virtual uint64_t leaveFreeSpace() const = 0;
	/// Setter for leaveFreeSpace
	virtual void setLeaveFreeSpace(uint64_t newLeaveFreeSpace) = 0;

	/// Returns the disk probability to be selected for copying new chunks
	virtual double carry() const = 0;
	/// Setter for carry
	virtual void setCarry(double newCarry) = 0;

	/// Returns the available space of the disk in bytes
	virtual uint64_t availableSpace() const = 0;
	/// Setter for availableSpace
	virtual void setAvailableSpace(uint64_t newAvailableSpace) = 0;

	// Return the total usable space of the disk in bytes
	virtual uint64_t totalSpace() const = 0;
	/// Setter for totalSpace
	virtual void setTotalSpace(uint64_t newTotalSpace) = 0;

	/// Returns true if the Disk was removed from the configuration after
	/// reloading
	virtual bool wasRemovedFromConfig() const = 0;
	/// Setter for wasRemovedFromConfig
	virtual void setWasRemovedFromConfig(bool newWasRemovedFromConfig) = 0;

	/// Returns true if the Disk is marked for removal from config file
	virtual bool isMarkedForRemoval() const = 0;
	/// Setter for isMarkedForRemoval
	virtual void setIsMarkedForRemoval(bool newIsMarkedForRemoval) = 0;

	/// Returns the scanning state of the Disk
	virtual ScanState scanState() const = 0;
	/// Setter for scanState
	virtual void setScanState(ScanState newScanState) = 0;

	/// Returns the scanning progress percent of the Disk
	virtual uint8_t scanProgress() const = 0;
	/// Setter for scanProgress
	virtual void setScanProgress(uint8_t newScanProgress) = 0;

	/// Returns true if the Disk is reported as damaged
	virtual bool isDamaged() const = 0;
	/// Setter for isDamaged
	virtual void setIsDamaged(bool newIsDamaged) = 0;

	/// Returns the scanning thread of the Disk
	virtual std::thread &scanThread() = 0;
	/// Setter for scanThread
	virtual void setScanThread(std::thread &&newScanThread) = 0;

	/// Returns the history of the last errors
	virtual std::array<disk::IoError, disk::kLastErrorSize> &lastErrorTab() = 0;

	/// Returns the index of the last error
	virtual uint32_t lastErrorIndex() const = 0;
	/// Setter for lastErrorIndex
	virtual void setLastErrorIndex(uint32_t newLastErrorIndex) = 0;
};
