#include "common/platform.h"

#include "master/chunks_parallel_loader.h"

#include <algorithm>
#include <future>
#include <thread>

#include "master/bulk_metadata_reader.h"
#include "slogger/slogger.h"

uint32_t ParallelChunkLoader::totalChunksLoaded = 0;

bool ParallelChunkLoader::loadChunksParallel(MetadataLoader::Options options,
											 const ChunkCreatorFunc& chunkCreator,
											 const ChunkSetterFunc& chunkSetter) {
	// TODO(Crash): Separate the size of both constants with the real values
	constexpr uint8_t kSerializedChunkSizeWithLockId = 20;
	constexpr uint8_t kSerializedChunkSizeWithoutLockId = 16;

	// Calculate total chunks to estimate batch sizes: sectionLength - sizeof(nextchunkid)
	size_t remainingBytes = options.sectionLength - sizeof(uint64_t);
	size_t chunkEntrySize =
	    options.loadLockIds ? kSerializedChunkSizeWithLockId : kSerializedChunkSizeWithoutLockId;
	size_t estimatedChunks = remainingBytes / chunkEntrySize;

	safs::log_info("Loading {} chunks in parallel (estimated)", estimatedChunks);

	// Determine optimal thread count
	const size_t numThreads = std::min(
	    {std::thread::hardware_concurrency(), static_cast<unsigned int>(kDefaultThreadCount),
	     static_cast<unsigned int>((estimatedChunks + kDefaultBatchSize - 1) / kDefaultBatchSize)});

	safs::log_info("Using {} threads for parallel chunk loading", numThreads);

	// Calculate batch sizes for each thread
	const size_t chunksPerThread = (estimatedChunks + numThreads - 1) / numThreads;
	const size_t bytesPerThread = chunksPerThread * chunkEntrySize;

	std::vector<std::future<std::vector<ChunkLoadData>>> futures;
	futures.reserve(numThreads);

	// Launch worker threads
	for (size_t i = 0; i < numThreads; ++i) {
		size_t startOffset = options.offset + (i * bytesPerThread);
		size_t batchLength =
		    (i == numThreads - 1) ? (remainingBytes - (i * bytesPerThread)) : bytesPerThread;

		if (startOffset >= options.offset + remainingBytes) {
			break;  // No more data for this thread
		}

		futures.emplace_back(std::async(std::launch::async, loadChunkBatch, options.metadataFile,
		                                startOffset, batchLength, options.loadLockIds, i));
	}

	// Collect results and insert into hash table
	bool status = false;
	for (auto &future : futures) {
		try {
			auto chunkData = future.get();
			// TODO: Validate like the original function that the last chunk batch contains chunkId = 0 and lockedTo = 0
			status |= createChunksFromData(chunkData, chunkCreator, chunkSetter);
			ParallelChunkLoader::totalChunksLoaded += chunkData.size();
		} catch (const std::exception &ex) {
			safs::log_err("Chunk loading thread failed: {}", ex.what());
			return false;
		}
	}

	safs::log_info("Parallel chunk loading completed: {} chunks loaded",
	               ParallelChunkLoader::totalChunksLoaded);

	return status;
}

std::vector<ChunkLoadData> ParallelChunkLoader::loadChunkBatch(
    const std::shared_ptr<MemoryMappedFile> &file, size_t offset, size_t length, bool loadLockIds,
    size_t batchId) {
	safs::log_debug("Thread {} loading batch: offset={}, length={}", batchId, offset, length);

	std::vector<ChunkLoadData> chunksData;
	chunksData.reserve(kDefaultBatchSize);

	try {
		BulkMetadataReader reader(file, offset, length, loadLockIds);

		while (reader.hasMoreData()) {
			auto data = reader.readChunksBulk(kDefaultBatchSize);
			if (data.empty()) { break; }

			// Just collect the data, don't create chunks yet
			chunksData.insert(chunksData.end(), data.begin(), data.end());
		}

	} catch (const std::exception &ex) {
		safs::log_err("Bulk reader failed in thread {}: {}", batchId, ex.what());
		throw;
	}

	safs::log_debug("Thread {} completed: {} chunk data entries loaded", batchId, chunksData.size());
	return chunksData;
}

bool ParallelChunkLoader::createChunksFromData(const std::vector<ChunkLoadData> &chunkData,
                                               const ChunkCreatorFunc &chunkCreator,
                                               const ChunkSetterFunc &chunkSetter) {
	try {
		for (const auto &data : chunkData) {
			Chunk *chunk = chunkCreator(data.chunkId, data.version);
			chunkSetter(chunk, data.lockedTo, data.lockId);
		}
	} catch (const std::exception &ex) {
		safs::log_err("{} failed creating chunk from data {}: {}", __func__, ex.what());
		return false;
	}

	return true;
}