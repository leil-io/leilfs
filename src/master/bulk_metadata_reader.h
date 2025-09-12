#pragma once

#include "common/platform.h"

#include <cstring>
#include <memory>
#include <vector>

#include "common/memory_mapped_file.h"

/// Structure representing chunk data during bulk loading
struct ChunkLoadData {
	uint64_t chunkId;
	uint32_t version;
	uint32_t lockedTo;
	uint32_t lockId;

	ChunkLoadData() = default;
	ChunkLoadData(uint64_t id, uint32_t ver, uint32_t locked, uint32_t lock)
	    : chunkId(id), version(ver), lockedTo(locked), lockId(lock) {}
};

/// High-performance bulk reader for metadata chunks
class BulkMetadataReader {
public:
	explicit BulkMetadataReader(const std::shared_ptr<MemoryMappedFile> &file, size_t offset,
	                            size_t length, bool loadLockIds);

	/// Read chunks in bulk without per-chunk bounds checking
	std::vector<ChunkLoadData> readChunksBulk(size_t maxChunks);

	/// Check if more chunks are available
	bool hasMoreData() const;

	/// Get current position for progress reporting
	size_t getCurrentPosition() const { return currentOffset_; }
	size_t getTotalSize() const { return maxOffset_; }

private:
	const uint8_t *basePtr_;
	size_t currentOffset_;
	size_t maxOffset_;
	bool loadLockIds_;
	size_t chunkEntrySize_;

	/// Direct memory access for performance
	inline uint64_t readUint64Direct(const uint8_t *ptr) const;
	inline uint32_t readUint32Direct(const uint8_t *ptr) const;
};
