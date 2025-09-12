#pragma once

#include "common/platform.h"

#include <memory>
#include <vector>

#include "master/metadata_loader.h"

// Forward declarations
class Chunk;
struct ChunkLoadData;

using ChunkCreatorFunc = std::function<Chunk*(uint64_t, uint32_t)>;
using ChunkSetterFunc = std::function<void(Chunk*, uint32_t, uint32_t)>;

/// Parallel chunk loading with thread pool
class ParallelChunkLoader {
public:
	static constexpr size_t kDefaultBatchSize = 50000;  // Chunks per batch
	static constexpr size_t kDefaultThreadCount = 8;    // Max threads to use

	/// Load chunks from metadata file in parallel
	static bool loadChunksParallel(MetadataLoader::Options options,
	                               const ChunkCreatorFunc &chunkCreator,
	                               const ChunkSetterFunc &chunkSetter);

private:
	/// Load a batch of chunks in a worker thread
	static std::vector<ChunkLoadData> loadChunkBatch(const std::shared_ptr<MemoryMappedFile> &file,
	                                                 size_t offset, size_t length, bool loadLockIds,
	                                                 size_t batchId);

	static bool createChunksFromData(const std::vector<ChunkLoadData> &chunkData,
	                                 const ChunkCreatorFunc &chunkCreator,
	                                 const ChunkSetterFunc &chunkSetter);

	static uint32_t totalChunksLoaded;
};
