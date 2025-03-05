#include "chunk_with_fd.h"
#include <fcntl.h>

#include <iomanip>

#include "chunkserver-common/subfolder.h"
#include "common/slice_traits.h"

FDChunk::FDChunk(uint64_t chunkId, ChunkPartType type, ChunkState state)
    : id_(chunkId), type_(type), state_(state) {}

std::string FDChunk::fullMetaFilename() const {
	return owner_->metaPath() + Subfolder::getSubfolderNameGivenChunkId(id_) +
	       "/" + metaFilename_;
}

std::string FDChunk::fullLegacyFilename() const {
	return owner_->metaPath() + Subfolder::getSubfolderNameGivenChunkId(id_) +
	       "/" + legacyFilename_;
}

const std::string &FDChunk::metaFilename() const { return metaFilename_; }

void FDChunk::setMetaFilename(const std::string &_metaFilename) {
	metaFilename_ = _metaFilename;
}

std::string FDChunk::fullDataFilename() const {
	return owner_->dataPath() + Subfolder::getSubfolderNameGivenChunkId(id_) +
	       "/" + dataFilename_;
}

const std::string &FDChunk::dataFilename() const { return dataFilename_; }

void FDChunk::setDataFilename(const std::string &_dataFilename) {
	dataFilename_ = _dataFilename;
}

std::string FDChunk::legacyFilename() const { return legacyFilename_; }

void FDChunk::setLegacyFilename(const std::string &_legacyFilename) {
	legacyFilename_ = _legacyFilename;
}

void FDChunk::updateFilenamesFromVersion(uint32_t _version) {
	metaFilename_ = generateMetadataFilenameForVersion(_version);
	dataFilename_ = generateDataFilenameForVersion(_version);
	legacyFilename_ = !this->needsMigration() ? "" :
	                  this->metaFilename_.substr(0,
	                                             this->metaFilename_.size() - 4) +
	                                              CHUNK_LEGACY_FILE_EXTENSION;
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

int32_t FDChunk::legacyFD() const { return legacyFD_; }

void FDChunk::setMetaFD(int32_t newMetaFD) { metaFD_ = newMetaFD; }

void FDChunk::setDataFD(int32_t newDataFD) { dataFD_ = newDataFD; }

void FDChunk::setLegacyFD(int32_t newLegacyFD) { legacyFD_ = newLegacyFD; }

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

void FDChunk::requiereMigration(bool migrate) {
	needsMigration_ = migrate;
}

bool FDChunk::needsMigration() const { return needsMigration_; }

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
