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

#include "common/platform.h"

#include "chunkserver-common/cmr_disk.h"

#include <fcntl.h>
#include <linux/falloc.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/uio.h>
#include <filesystem>

#include "chunkserver-common/chunk_interface.h"
#include "chunkserver-common/chunk_trash_manager.h"
#include "chunkserver-common/cmr_chunk.h"
#include "chunkserver-common/global_shared_resources.h"
#include "chunkserver-common/hdd_stats.h"
#include "chunkserver-common/subfolder.h"
#include "common/crc.h"
#include "devtools/TracePrinter.h"
#include "devtools/request_log.h"
#include "errors/saunafs_error_codes.h"

CmrDisk::CmrDisk(const std::string &_metaPath, const std::string &_dataPath,
                 bool _isMarkedForRemoval, bool _isZonedDevice)
    : FDDisk(_metaPath, _dataPath, _isMarkedForRemoval, _isZonedDevice) {}

CmrDisk::CmrDisk(const disk::Configuration &configuration)
    : FDDisk(configuration) {}

void CmrDisk::createPathsAndSubfolders() {
	bool ret = true;

	constexpr int mode = 0755;

	if (!isMarkedForDeletion()) {
		ret &= (::mkdir(metaPath().c_str(), mode) == 0);
		ret &= (::mkdir((std::filesystem::path(metaPath()) /
		                 ChunkTrashManager::kTrashDirname).c_str(), mode) == 0);

		if (dataPath() != metaPath()) {
			ret &= (::mkdir(dataPath().c_str(), mode) == 0);
			ret &= (::mkdir((std::filesystem::path(dataPath()) /
			                 ChunkTrashManager::kTrashDirname).c_str(), mode)
			        == 0);
		}

		for (uint32_t i = 0; i < Subfolder::kNumberOfSubfolders; ++i) {
			const auto subfolderName =
			    Subfolder::getSubfolderNameGivenNumber(i);
			ret &= (::mkdir((metaPath() + subfolderName).c_str(), mode) == 0);

			if (dataPath() != metaPath()) {
				ret &=
				    (::mkdir((dataPath() + subfolderName).c_str(), mode) == 0);
			}
		}
	}

	if (ret) {
		safs_pretty_syslog(LOG_INFO,
		                   "Folders structures for disk %s "
		                   "auto-generated successfully",
		                   getPaths().c_str());
	}
}

void CmrDisk::createLockFiles(bool isLockNeeded,
                              std::vector<std::unique_ptr<IDisk>> &allDisks) {
	createLockFile(isLockNeeded, metaPath() + ".lock", true, allDisks);

	if (metaPath() != dataPath()) {
		createLockFile(isLockNeeded, dataPath() + ".lock", false, allDisks);
	}
}

void CmrDisk::refreshDataDiskUsage() {
	TRACETHIS();
	struct statvfs fsinfo {};

	if (statvfs(dataPath().c_str(), &fsinfo) < 0) {
		setAvailableSpace(0ULL);
		setTotalSpace(0ULL);
		return;
	}

	setAvailableSpace(static_cast<uint64_t>(fsinfo.f_frsize) *
	                  static_cast<uint64_t>(fsinfo.f_bavail));
	setTotalSpace(static_cast<uint64_t>(fsinfo.f_frsize) *
	              static_cast<uint64_t>(fsinfo.f_blocks -
	                                    (fsinfo.f_bfree - fsinfo.f_bavail)));

	if (availableSpace() < leaveFreeSpace()) {
		setAvailableSpace(0ULL);
	} else {
		setAvailableSpace(availableSpace() - leaveFreeSpace());
	}
}

int CmrDisk::updateChunkAttributes(IChunk *chunk, bool isFromScan) {
	assert(chunk);
	TRACETHIS1(chunk->id());

	if (isFromScan && !gStatChunksAtDiskScan) {
		return SAUNAFS_STATUS_OK;
	}

	struct stat metaStat {};
	if (::stat(chunk->fullMetaFilename().c_str(), &metaStat) < 0) {
		safs::log_err("CmrDisk::updateChunkAttributes: could not access chunk metadata: chunk->metaFilename ({}), strerror ({})",
				chunk->fullMetaFilename(), strerror(errno));
		// Only a confirmed-gone file is NOCHUNK; other errno values
		// (EMFILE, EINTR, EIO...) must not delete a healthy chunk.
		return (errno == ENOENT || errno == ENOTDIR) ? SAUNAFS_ERROR_NOCHUNK
		                                             : SAUNAFS_ERROR_IO;
	}
	if (!S_ISREG(metaStat.st_mode)) {
		safs::log_critical("CmrDisk::updateChunkAttributes: chunk metadata file not a regular file: metaFilename ({})",
					 chunk->fullMetaFilename());
		return SAUNAFS_ERROR_NOCHUNK;
	}

	struct stat dataStat {};
	if (::stat(chunk->fullDataFilename().c_str(), &dataStat) < 0) {
		safs::log_err("CmrDisk::updateChunkAttributes: could not access chunk data {}: {}",
				chunk->fullDataFilename(), strerror(errno));
		return (errno == ENOENT || errno == ENOTDIR) ? SAUNAFS_ERROR_NOCHUNK
		                                             : SAUNAFS_ERROR_IO;
	}
	if ((dataStat.st_mode & S_IFMT) != S_IFREG) {
		safs::log_critical("CmrDisk::updateChunkAttributes: chunk data file not a regular file: dataFilename ({})",
					 chunk->fullDataFilename());
		return SAUNAFS_ERROR_NOCHUNK;
	}
	if (!chunk->isDataFileSizeValid(dataStat.st_size)) {
		safs::log_critical("CmrDisk::updateChunkAttributes: chunk data file size is invalid: data file ({}), file size ({}), block size ({}), max blocks in chunk ({})",
					 chunk->fullDataFilename(), dataStat.st_size, SFSBLOCKSIZE, chunk->maxBlocksInFile());
		return SAUNAFS_ERROR_NOCHUNK;
	}

	chunk->setBlockCountFromDataFileSize(dataStat.st_size);
	chunk->setValidAttr(1);

	return SAUNAFS_STATUS_OK;
}

std::unique_ptr<ChunkSignature> CmrDisk::createChunkSignature(IChunk *chunk) {
	return std::make_unique<ChunkSignature>(
	    ChunkSignature(chunk->id(), chunk->version(), chunk->type()));
}

std::unique_ptr<ChunkSignature> CmrDisk::createChunkSignature() {
	return std::make_unique<ChunkSignature>(ChunkSignature());
}

void CmrDisk::serializeEmptyChunkSignature(uint8_t **destination,
                                           uint64_t chunkId,
                                           uint32_t chunkVersion,
                                           ChunkPartType chunkType) {
	serialize(destination, ChunkSignature(chunkId, chunkVersion, chunkType));
}

IChunk *CmrDisk::instantiateNewConcreteChunk(uint64_t chunkId,
                                             ChunkPartType type) {
	auto *chunk = new CmrChunk(chunkId, type, ChunkState::Locked);
	chunk->setOwner(this);

	return chunk;
}

int CmrDisk::setChunkBlocks(IChunk *chunk, uint16_t originalBlocks,
                            uint16_t newBlocks) {
	(void)originalBlocks;
	chunk->setBlocks(newBlocks);
	return SAUNAFS_STATUS_OK;
}

void CmrDisk::updateAfterScan() {
	// Nothing to do, but we need the function in the interface
}

void CmrDisk::creat(IChunk *chunk) {
	chunk->setMetaFD(::open(chunk->fullMetaFilename().c_str(),
	                        O_RDWR | O_TRUNC | O_CREAT,
	                        disk::kDefaultOpenMode));

	chunk->setDataFD(::open(chunk->fullDataFilename().c_str(),
	                        O_RDWR | O_TRUNC | O_CREAT,
	                        disk::kDefaultOpenMode));
}

void CmrDisk::open(IChunk *chunk) {
	chunk->setMetaFD(::open(chunk->fullMetaFilename().c_str(),
	                        isReadOnly() ? O_RDONLY : O_RDWR));

	chunk->setDataFD(::open(chunk->fullDataFilename().c_str(),
	                        isReadOnly() ? O_RDONLY : O_RDWR));
}

int CmrDisk::unlinkChunk(IChunk *chunk) {
	// Get absolute paths for meta and data files
	const std::filesystem::path metaFile = chunk->fullMetaFilename();
	const std::filesystem::path dataFile = chunk->fullDataFilename();

	// Use the metaPath() and dataPath() to get the disk paths
	const std::string metaDiskPath = metaPath();
	const std::string dataDiskPath = dataPath();

	// Ensure we found a valid disk path
	if (metaDiskPath.empty() || dataDiskPath.empty()) {
		safs_pretty_errlog(LOG_ERR, "Error finding disk path for chunk: %s",
		                   chunk->metaFilename().c_str());
		return SAUNAFS_ERROR_ENOENT;
	}

	if (ChunkTrashManager::isEnabled) {
		// Create a deletion timestamp
		const std::time_t deletionTime = std::time(nullptr);

		// Move meta file to trash
		saunafs_error_code result = static_cast<saunafs_error_code>(
		    ChunkTrashManager::moveToTrash(metaFile, metaDiskPath, deletionTime));
		if (result != SAUNAFS_STATUS_OK) {
			safs::log_error_code(result, "Error moving meta file to trash: {}", metaFile.c_str());
			return result;
		}

		// Move data file to trash
		result = static_cast<saunafs_error_code>(
		    ChunkTrashManager::moveToTrash(dataFile, dataDiskPath, deletionTime));
		if (result != SAUNAFS_STATUS_OK) {
			safs::log_error_code(result, "Error moving data file to trash: {}", dataFile.c_str());
			return result;
		}
	} else {
		// Unlink the meta file
		if (::unlink(metaFile.c_str()) != 0) {
			safs::log_error_code(errno, "Error unlinking meta file: {}", metaFile.c_str());
			return SAUNAFS_ERROR_NOTDONE;
		}

		// Unlink the data file
		if (::unlink(dataFile.c_str()) != 0) {
			safs::log_error_code(errno, "Error unlinking data file: {}", dataFile.c_str());
			return SAUNAFS_ERROR_NOTDONE;
		}
	}

	return SAUNAFS_STATUS_OK;
}

int CmrDisk::ftruncateData(IChunk *chunk, uint64_t size) {
	return ::ftruncate(chunk->dataFD(), size);
}

ssize_t CmrDisk::preadData(IChunk *chunk, uint8_t *blockBuffer, uint64_t size,
                           uint64_t offset) {
	return ::pread(chunk->dataFD(), blockBuffer, size, offset);
}

void CmrDisk::prefetchChunkBlocks(IChunk &chunk, uint16_t firstBlock,
                                  uint32_t blockCount) {
	if (blockCount > 0) {
		auto blockSize = SFSBLOCKSIZE;
#ifdef SAUNAFS_HAVE_POSIX_FADVISE
		posix_fadvise(chunk.dataFD(), chunk.getBlockOffset(firstBlock),
		              blockCount * blockSize, POSIX_FADV_WILLNEED);
#elif defined(__APPLE__)
		struct radvisory ra;
		ra.ra_offset = chunk.getBlockOffset(firstBlock);
		ra.ra_count = uint32_t(blockCount) * blockSize;
		fcntl(chunk.dataFD, F_RDADVISE, &ra);
#endif
	}
}

int CmrDisk::readBlockAndCrc(IChunk *chunk, uint8_t *blockBuffer,
                             uint8_t *crcData, uint16_t blocknum,
                             const char *errorMsg) {
	assert(chunk);

	memcpy(blockBuffer, crcData + blocknum * kCrcSize, kCrcSize);

	{
		DiskReadStatsUpdater updater(chunk->owner(), SFSBLOCKSIZE);
		const ssize_t bytesRead =
		    ::pread(chunk->dataFD(), blockBuffer + kCrcSize, SFSBLOCKSIZE,
		            chunk->getBlockOffset(blocknum));
		if (bytesRead != SFSBLOCKSIZE) {
			hddAddErrorAndPreserveErrno(chunk);
			safs_silent_errlog(LOG_WARNING, "%s: file:%s - read error",
			                   errorMsg, chunk->fullMetaFilename().c_str());
			hddReportDamagedChunk(chunk->id(), chunk->type());
			updater.markReadAsFailed();

			return -SAUNAFS_ERROR_IO;
		}
	}

	return SFSBLOCKSIZE;
}

int CmrDisk::overwriteChunkVersion(IChunk *chunk, uint32_t newVersion) {
	assert(chunk);

	std::vector<uint8_t> buffer;
	serialize(buffer, newVersion);
	const ssize_t size = buffer.size();

	{
		DiskWriteStatsUpdater updater(chunk->owner(), size);

		if (pwrite(chunk->metaFD(), buffer.data(), size,
		           ChunkSignature::kVersionOffset) != size) {
			updater.markWriteAsFailed();
			return SAUNAFS_ERROR_IO;
		}
	}

	HddStats::overheadWrite(size);

	chunk->setVersion(newVersion);

	return SAUNAFS_STATUS_OK;
}

int CmrDisk::writePartialBlockAndCrc(IChunk *chunk, const uint8_t *buffer,
                                     uint32_t offsetInBlock, uint32_t size,
                                     const uint8_t *crcBuff, uint8_t *crcData,
                                     uint16_t blockNum, bool isNewBlock,
                                     const char *errorMsg) {
	(void)isNewBlock;

	{
		DiskWriteStatsUpdater updater(chunk->owner(), size);

		auto ret = pwrite(chunk->dataFD(), buffer, size,
		                  chunk->getBlockOffset(blockNum) + offsetInBlock);

		if (ret != size) {
			hddAddErrorAndPreserveErrno(chunk);
			safs_silent_errlog(LOG_WARNING, "%s: file:%s - write error",
			                   errorMsg, chunk->fullMetaFilename().c_str());
			hddReportDamagedChunk(chunk->id(), chunk->type());
			updater.markWriteAsFailed();
			return -1;
		}
	}

	punchHoles(chunk, buffer, chunk->getBlockOffset(blockNum) + offsetInBlock,
	           size);

	memcpy(crcData + blockNum * kCrcSize, crcBuff, kCrcSize);

	return size;
}

int CmrDisk::writeFullBlocksAndCrcs(IChunk *chunk, const std::vector<uint16_t> &blocksPerBuffer,
                                    const std::vector<const uint8_t *> &buffers,
                                    uint16_t startBlock, const uint8_t *crcBuff, uint8_t *crcData,
                                    bool areNewBlocks, const char *errorMsg) {
	(void)areNewBlocks;
	assert(blocksPerBuffer.size() == buffers.size());

	constexpr size_t kMaxIovecsPerCall = 16;
	std::array<struct iovec, kMaxIovecsPerCall> ioVectors{};

	size_t buffersWritten = 0;
	uint16_t blockCursor = startBlock;
	uint32_t committedBytes = 0;
	while (buffersWritten < buffers.size()) {
		const size_t batchEnd = std::min(buffersWritten + kMaxIovecsPerCall, buffers.size());
		size_t size = 0;
		for (size_t j = buffersWritten; j < batchEnd; ++j) {
			// const_cast is needed because POSIX struct iovec is not const-correct for write-style
			// syscalls.
			ioVectors[j - buffersWritten].iov_base = const_cast<uint8_t *>(buffers[j]);
			ioVectors[j - buffersWritten].iov_len =
			    static_cast<size_t>(blocksPerBuffer[j]) * SFSBLOCKSIZE;
			size += ioVectors[j - buffersWritten].iov_len;
		}

		const off_t offset = chunk->getBlockOffset(blockCursor);
		{
			DiskWriteStatsUpdater updater(chunk->owner(), size);

			ssize_t ret = 0;
			do {
				ret = pwritev(chunk->dataFD(), ioVectors.data(), batchEnd - buffersWritten, offset);
			} while (ret < 0 && errno == EINTR);

			if (ret != static_cast<ssize_t>(size)) {
				hddAddErrorAndPreserveErrno(chunk);
				safs::log_warn("{}: file:{} - write error", errorMsg, chunk->fullDataFilename());
				hddReportDamagedChunk(chunk->id(), chunk->type());
				updater.markWriteAsFailed();
				return -1;  // damaged chunk, caller should handle it
			}
		}
		// Successfully written the batch, now punch holes in the blocks that are fully zero and
		// update CRCs
		uint16_t blocksWritten = size / SFSBLOCKSIZE;

		uint16_t blockCursorPunchHoles = blockCursor;
		for (size_t i = 0; i < batchEnd - buffersWritten; ++i) {
			punchHoles(chunk, static_cast<const uint8_t *>(ioVectors[i].iov_base),
			           chunk->getBlockOffset(blockCursorPunchHoles), ioVectors[i].iov_len);
			blockCursorPunchHoles += blocksPerBuffer[buffersWritten + i];
		}

		memcpy(crcData + blockCursor * kCrcSize, crcBuff + (blockCursor - startBlock) * kCrcSize,
		       kCrcSize * blocksWritten);

		blockCursor += blocksWritten;
		buffersWritten = batchEnd;
		committedBytes += size;
	}

	return committedBytes;
}

void CmrDisk::punchHoles(IChunk *chunk, const uint8_t *buffer, uint32_t offset,
                         uint32_t size) {
#if defined(SAUNAFS_HAVE_FALLOCATE) && \
    defined(SAUNAFS_HAVE_FALLOC_FL_PUNCH_HOLE)

	if (!gPunchHolesInFiles) {
		return;
	}

	assert(chunk);

	constexpr uint32_t blockSize = 4096;
	uint32_t step =
	    (offset % blockSize) == 0 ? 0 : blockSize - (offset % blockSize);
	uint32_t holeStart = 0;
	uint32_t holeSize = 0;

	for (; (step + blockSize) <= size; step += blockSize) {
		const auto *zero_test =
		    reinterpret_cast<const std::size_t *>(buffer + step);
		bool is_zero = true;
		for (unsigned i = 0; i < blockSize / sizeof(std::size_t); ++i) {
			if (zero_test[i] != 0) {
				is_zero = false;
				break;
			}
		}

		if (is_zero) {
			if (holeSize == 0) {
				holeStart = offset + step;
			}
			holeSize += blockSize;
		} else {
			if (holeSize > 0) {
				fallocate(chunk->dataFD(),
				          FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, holeStart,
				          holeSize);
			}
			holeSize = 0;
		}
	}
	if (holeSize > 0) {
		fallocate(chunk->dataFD(), FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
		          holeStart, holeSize);
	}
#else
	(void)chunk;
	(void)buffer;
	(void)offset;
	(void)size;
#endif
}

int CmrDisk::writeChunkBlock(IChunk *chunk, uint32_t version, uint16_t blocknum,
                             uint32_t offsetInBlock, uint32_t size,
                             uint32_t crc, uint8_t *crcData,
                             const uint8_t *buffer, bool isFromReplication) {
	assert(chunk);
	LOG_AVG_TILL_END_OF_SCOPE0("writeChunkBlock");
	TRACETHIS3(chunk->id(), offsetInBlock, size);
	uint32_t preCrc, postCrc, combinedCrc, chcrc;

	if (chunk->version() != version && version > 0) {
		return SAUNAFS_ERROR_WRONGVERSION;
	}
	if (blocknum >= chunk->maxBlocksInFile()) {
		return SAUNAFS_ERROR_BNUMTOOBIG;
	}
	if (size > SFSBLOCKSIZE) {
		return SAUNAFS_ERROR_WRONGSIZE;
	}
	if ((offsetInBlock >= SFSBLOCKSIZE) ||
	    (offsetInBlock + size > SFSBLOCKSIZE)) {
		return SAUNAFS_ERROR_WRONGOFFSET;
	}

	if (gCheckCrcWhenWriting && !isFromReplication) {
		if (crc != mycrc32(0, buffer, size)) { return SAUNAFS_ERROR_CRC; }
	}

	chunk->setWasChanged(1U);
	bool isNewBlock = false;

	if (offsetInBlock == 0 && size == SFSBLOCKSIZE) {  // A complete block
		std::array<uint8_t, kCrcSize> crcBuff{};

		if (blocknum >= chunk->blocks()) {
			const uint16_t prevBlocks = chunk->blocks();
			chunk->setBlocks(blocknum + 1);
			isNewBlock = true;

			// Fill new blocks' CRCs with empty data
			for (uint16_t i = prevBlocks; i < blocknum; i++) {
				memcpy(crcData + i * kCrcSize, &gEmptyBlockCrc, kCrcSize);
			}
		}

		uint8_t *crcBuffPointer = crcBuff.data();
		put32bit(&crcBuffPointer, crc);

		int written = writePartialBlockAndCrc(chunk, buffer, 0, SFSBLOCKSIZE,
		                                      crcBuff.data(), crcData, blocknum,
		                                      isNewBlock, "writeChunkBlock");
		if (written < 0) {
			return SAUNAFS_ERROR_IO;
		}
	} else {  // It is not a complete block request
		uint8_t *crcAndBlockbuffer = getChunkBlockBuffer();

		if (blocknum < chunk->blocks()) {  // It is an existing block
			auto readBytes = chunk->owner()->readBlockAndCrc(
			    chunk, crcAndBlockbuffer, crcData, blocknum, "writeChunkBlock");
			uint8_t *dataInBuffer = crcAndBlockbuffer + kCrcSize;  // Skip crc
			if (readBytes < 0) {
				return SAUNAFS_ERROR_IO;
			}

			preCrc = mycrc32(0, dataInBuffer, offsetInBlock);
			chcrc = mycrc32(0, dataInBuffer + offsetInBlock, size);
			postCrc = mycrc32(0, dataInBuffer + offsetInBlock + size,
			                  SFSBLOCKSIZE - (offsetInBlock + size));

			if (offsetInBlock == 0) {
				combinedCrc = mycrc32_combine(
				    chcrc, postCrc, SFSBLOCKSIZE - (offsetInBlock + size));
			} else {
				combinedCrc = mycrc32_combine(preCrc, chcrc, size);
				if ((offsetInBlock + size) < SFSBLOCKSIZE) {
					combinedCrc =
					    mycrc32_combine(combinedCrc, postCrc,
					                    SFSBLOCKSIZE - (offsetInBlock + size));
				}
			}

			const uint8_t *crcBuffPointer = crcAndBlockbuffer;
			const uint8_t **tmpPtr = &crcBuffPointer;
			uint32_t tmpCrc;
			get32bit(tmpPtr, tmpCrc);

			if (tmpCrc != combinedCrc) {
				errno = 0;
				hddAddErrorAndPreserveErrno(chunk);
				safs_pretty_syslog(LOG_WARNING,
				                   "writeChunkBlock: file:%s - crc error",
				                   chunk->fullMetaFilename().c_str());
				hddReportDamagedChunk(chunk->id(), chunk->type());
				return SAUNAFS_ERROR_CRC;
			}
		} else {  // It is a new block at the end
			if (::ftruncate(chunk->dataFD(), chunk->getFileSizeFromBlockCount(
			                                     blocknum + 1)) < 0) {
				hddAddErrorAndPreserveErrno(chunk);
				safs_silent_errlog(LOG_WARNING,
				                   "writeChunkBlock: file:%s - ftruncate error",
				                   chunk->fullMetaFilename().c_str());
				hddReportDamagedChunk(chunk->id(), chunk->type());
				return SAUNAFS_ERROR_IO;
			}

			const uint16_t prevBlocks = chunk->blocks();
			chunk->setBlocks(blocknum + 1);
			isNewBlock = true;

			// Fill new blocks' CRCs with empty data
			for (uint16_t i = prevBlocks; i < blocknum; i++) {
				memcpy(crcData + i * kCrcSize, &gEmptyBlockCrc, kCrcSize);
			}

			preCrc = mycrc32_zeroblock(0, offsetInBlock);
			postCrc =
			    mycrc32_zeroblock(0, SFSBLOCKSIZE - (offsetInBlock + size));
		}

		if (offsetInBlock == 0) {
			combinedCrc = mycrc32_combine(
			    crc, postCrc, SFSBLOCKSIZE - (offsetInBlock + size));
		} else {
			combinedCrc = mycrc32_combine(preCrc, crc, size);
			if ((offsetInBlock + size) < SFSBLOCKSIZE) {
				combinedCrc =
				    mycrc32_combine(combinedCrc, postCrc,
				                    SFSBLOCKSIZE - (offsetInBlock + size));
			}
		}

		uint8_t *crcBuffPointer = crcAndBlockbuffer;
		put32bit(&crcBuffPointer, combinedCrc);

		int written = writePartialBlockAndCrc(
		    chunk, buffer, offsetInBlock, size, crcAndBlockbuffer, crcData,
		    blocknum, isNewBlock, "writeChunkBlock");

		if (written < 0) {
			return SAUNAFS_ERROR_IO;
		}
	}

	return SAUNAFS_STATUS_OK;
}

int CmrDisk::writeChunkBlocks(IChunk *chunk, uint32_t version, uint16_t startBlock,
                              uint16_t numBlocks, std::vector<uint32_t> &crc, uint8_t *crcData,
                              const std::vector<uint16_t> &blocksPerBuffer,
                              const std::vector<const uint8_t *> &buffers, bool isFromReplication) {
	assert(chunk);
	assert(blocksPerBuffer.size() == buffers.size());
	LOG_AVG_TILL_END_OF_SCOPE0("writeChunkBlocks");
	TRACETHIS3(chunk->id(), startBlock, numBlocks);

	if (chunk->version() != version && version > 0) {
		return -SAUNAFS_ERROR_WRONGVERSION;
	}
	if (startBlock + numBlocks > chunk->maxBlocksInFile()) {
		return -SAUNAFS_ERROR_BNUMTOOBIG;
	}
	if (numBlocks > SFSBLOCKSINCHUNK || crc.size() != static_cast<size_t>(numBlocks)) {
		return -SAUNAFS_ERROR_WRONGSIZE;
	}

	if (gCheckCrcWhenWriting && !isFromReplication) {
		uint16_t crcIndex = 0;
		for (size_t bufferIndex = 0; bufferIndex < buffers.size(); ++bufferIndex) {
			uint16_t blocksInBuffer = blocksPerBuffer[bufferIndex];
			const uint8_t *buffer = buffers[bufferIndex];
			for (uint16_t i = 0; i < blocksInBuffer; ++i) {
				if (crc[crcIndex] != mycrc32(0, buffer + i * SFSBLOCKSIZE, SFSBLOCKSIZE)) {
					return -SAUNAFS_ERROR_CRC;
				}
				crcIndex++;
			}
		}
	}

	chunk->setWasChanged(1U);
	bool areNewBlocks = false;

	std::vector<uint8_t> crcBuff(kCrcSize * numBlocks);

	uint8_t *crcBuffPointer = crcBuff.data();
	for (auto &crcValue : crc) {
		put32bit(&crcBuffPointer, crcValue);
	}

	const uint16_t prevBlocks = chunk->blocks();
	if (startBlock >= chunk->blocks()) {
		// Fill new blocks' CRCs with empty data
		for (uint16_t i = prevBlocks; i < startBlock; i++) {
			memcpy(crcData + i * kCrcSize, &gEmptyBlockCrc, kCrcSize);
		}
	}
	if (startBlock + numBlocks > chunk->blocks()) {
		// New blocks are added
		chunk->setBlocks(startBlock + numBlocks);
		areNewBlocks = true;
	}

	int written = writeFullBlocksAndCrcs(chunk, blocksPerBuffer, buffers, startBlock,
	                                     crcBuff.data(), crcData, areNewBlocks, "writeChunkBlocks");
	if (written < 0) { return -SAUNAFS_ERROR_IO; }

	return written;
}

int CmrDisk::writeChunkData(IChunk *chunk, uint8_t *blockBuffer,
                            int32_t blockSize, off64_t offset) {
	(void)offset;  // Not needed for conventional disks

	return ::write(chunk->dataFD(), blockBuffer, blockSize);
}
