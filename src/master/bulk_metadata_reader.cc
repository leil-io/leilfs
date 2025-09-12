#include "common/platform.h"

#include "master/bulk_metadata_reader.h"

#include "slogger/slogger.h"

BulkMetadataReader::BulkMetadataReader(const std::shared_ptr<MemoryMappedFile> &file, size_t offset,
                                       size_t length, bool loadLockIds)
    : basePtr_(file->seek(offset)),
      currentOffset_(0),
      maxOffset_(length),
      loadLockIds_(loadLockIds),
      chunkEntrySize_(loadLockIds ? 20 : 16) {
	safs::log_debug("BulkMetadataReader initialized: offset={}, length={}, lockIds={}", offset,
	                length, loadLockIds);
}

inline uint64_t BulkMetadataReader::readUint64Direct(const uint8_t *ptr) const {
	uint64_t value;
	std::memcpy(&value, ptr, sizeof(uint64_t));
	return be64toh(value);  // Convert from big endian
}

inline uint32_t BulkMetadataReader::readUint32Direct(const uint8_t *ptr) const {
	uint32_t value;
	std::memcpy(&value, ptr, sizeof(uint32_t));
	return be32toh(value);  // Convert from big endian
}

std::vector<ChunkLoadData> BulkMetadataReader::readChunksBulk(size_t maxChunks) {
	std::vector<ChunkLoadData> chunks;
	chunks.reserve(maxChunks);

	while (chunks.size() < maxChunks && currentOffset_ + chunkEntrySize_ <= maxOffset_) {
		const uint8_t *chunkPtr = basePtr_ + currentOffset_;

		// Direct memory access - much faster than get64bit()/get32bit()
		uint64_t chunkId = readUint64Direct(chunkPtr);
		uint32_t version = readUint32Direct(chunkPtr + 8);
		uint32_t lockedTo = readUint32Direct(chunkPtr + 12);
		uint32_t lockId = 0;

		if (loadLockIds_) { lockId = readUint32Direct(chunkPtr + 16); }

		// Check for end marker
		if (chunkId == 0) {
			currentOffset_ += chunkEntrySize_;
			break;
		}

		chunks.emplace_back(chunkId, version, lockedTo, lockId);
		currentOffset_ += chunkEntrySize_;
	}

	return chunks;
}

bool BulkMetadataReader::hasMoreData() const {
	return currentOffset_ + chunkEntrySize_ <= maxOffset_;
}
