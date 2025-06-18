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

#include "chunkserver-common/chunk_with_fd.h"

#include <fcntl.h>
#include <iomanip>

#include "chunkserver-common/subfolder.h"
#include "common/slice_traits.h"

FDChunk::FDChunk(uint64_t chunkId, ChunkPartType type, ChunkState state)
    : id_(chunkId), type_(type), state_(state) {}

std::string FDChunk::fullMetaFilename() const {
	return owner_->metaPath() + Subfolder::getSubfolderNameGivenChunkId(id_) +
	       "/" + generateFilenameForVersion(version_, true);
}

const std::string FDChunk::metaFilename() const {
	if (version_ == 0) {
		return "";
	}
	return generateFilenameForVersion(version_, true);
}

std::string FDChunk::fullDataFilename() const {
	return owner_->dataPath() + Subfolder::getSubfolderNameGivenChunkId(id_) +
	       "/" + generateFilenameForVersion(version_, false);
}

const std::string FDChunk::dataFilename() const {
	if (version_ == 0) {
		return "";
	}
	return generateFilenameForVersion(version_, false);
}

std::string FDChunk::generateMetadataFilenameForVersion(
    uint32_t _version) const {
	return generateFilenameForVersion(_version, true);
}

std::string FDChunk::generateFilenameForVersion(uint32_t _version,
                                                bool isForMetadata) const {
	std::stringstream result;
	result << "chunk_";

	if (slice_traits::isXor(type_)) {
		if (slice_traits::xors::isXorParity(type_)) {
			result << "xor_parity_of_";
		} else {
			result << "xor_"
			       << static_cast<unsigned>(
			              slice_traits::xors::getXorPart(type_))
			       << "_of_";
		}
		result << static_cast<unsigned>(slice_traits::xors::getXorLevel(type_))
		       << "_";
	}
	if (slice_traits::isEC(type_)) {
		result << "ec2_" << (type_.getSlicePart() + 1) << "_of_"
		       << slice_traits::ec::getNumberOfDataParts(type_) << "_"
		       << slice_traits::ec::getNumberOfParityParts(type_) << "_";
	}

	result << std::setfill('0') << std::hex << std::uppercase;
	result << std::setw(16) << id_ << "_";
	result << std::setw(8) << _version;

	result << (isForMetadata ? CHUNK_METADATA_FILE_EXTENSION
	                         : CHUNK_DATA_FILE_EXTENSION);

	return result.str();
}

off_t FDChunk::getBlockOffset(uint16_t blockNumber) const {
	return blockNumber * SFSBLOCKSIZE;
}

off_t FDChunk::getFileSizeFromBlockCount(uint32_t blockCount) const {
	return blockCount * SFSBLOCKSIZE;
}

bool FDChunk::isDataFileSizeValid(off_t fileSize) const {
	if (fileSize % SFSBLOCKSIZE != 0) {
		return false;
	}

	if (fileSize / SFSBLOCKSIZE > maxBlocksInFile()) {
		return false;
	}

	return true;
}

off_t FDChunk::getSignatureOffset() const { return 0; }

void FDChunk::readaheadHeader() const {
#ifdef SAUNAFS_HAVE_POSIX_FADVISE
	posix_fadvise(metaFD_, 0, getHeaderSize(), POSIX_FADV_WILLNEED);
#elif defined(__APPLE__)
	struct radvisory ra;
	ra.ra_offset = 0;
	ra.ra_count = getHeaderSize();
	fcntl(fd, F_RDADVISE, &ra);
#endif
}

size_t FDChunk::getCrcBlockSize() const { return kCrcSize * maxBlocksInFile(); }

uint32_t FDChunk::maxBlocksInFile() const {
	const int data_part_count = slice_traits::getNumberOfDataParts(type_);
	return (SFSBLOCKSINCHUNK + data_part_count - 1) / data_part_count;
}

void FDChunk::setBlockCountFromDataFileSize(off_t fileSize) {
	sassert(isDataFileSizeValid(fileSize));
	blocks_ = fileSize / SFSBLOCKSIZE;
}

void FDChunk::setVersion(uint32_t _version) { version_ = _version; }

int32_t FDChunk::metaFD() const { return metaFD_; }

int32_t FDChunk::dataFD() const { return dataFD_; }

void FDChunk::setMetaFD(int32_t newMetaFD) { metaFD_ = newMetaFD; }

void FDChunk::setDataFD(int32_t newDataFD) { dataFD_ = newDataFD; }

void FDChunk::setValidAttr(uint8_t newValidAttr) { validAttr_ = newValidAttr; }

IDisk *FDChunk::owner() const { return owner_; }

void FDChunk::setOwner(IDisk *newOwner) { owner_ = newOwner; }

size_t FDChunk::indexInDisk() const { return indexInDisk_; }

void FDChunk::setIndexInDisk(size_t newIndexInDisk) {
	indexInDisk_ = newIndexInDisk;
}

ChunkState FDChunk::state() const { return state_; }

void FDChunk::setState(ChunkState newState) { state_ = newState; }

void FDChunk::setCondVar(std::unique_ptr<CondVarWithWaitCount> &&newCondVar) {
	condVar_ = std::move(newCondVar);
}

uint16_t FDChunk::refCount() const { return refCount_; }

void FDChunk::setRefCount(uint16_t newRefCount) { refCount_ = newRefCount; }

uint16_t FDChunk::blockExpectedToBeReadNext() const {
	return blockExpectedToBeReadNext_;
}

void FDChunk::setBlockExpectedToBeReadNext(
    uint16_t newBlockExpectedToBeReadNext) {
	blockExpectedToBeReadNext_ = newBlockExpectedToBeReadNext;
}

uint8_t FDChunk::wasChanged() const { return wasChanged_; }

uint8_t FDChunk::validAttr() const { return validAttr_; }

std::unique_ptr<CondVarWithWaitCount> &FDChunk::condVar() { return condVar_; }

void FDChunk::setId(uint64_t newId) { id_ = newId; }

void FDChunk::setWasChanged(uint8_t newWasChanged) {
	wasChanged_ = newWasChanged;
}

ChunkFormat FDChunk::chunkFormat() const { return ChunkFormat::SPLIT; }

ChunkPartType FDChunk::type() const { return type_; }

uint16_t FDChunk::blocks() const { return blocks_; }

void FDChunk::setBlocks(uint16_t newBlocks) { blocks_ = newBlocks; }

uint64_t FDChunk::id() const { return id_; }

uint32_t FDChunk::version() const { return version_; }
