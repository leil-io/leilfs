#include "common/platform.h"

#include "master/parallel_file_chunk_connector.h"

#include <algorithm>
#include <future>
#include <unordered_set>

#include "master/chunks.h"
#include "master/filesystem_metadata.h"
#include "slogger/slogger.h"

void ParallelFileChunkConnector::connectFilesToChunks() {
	safs::log_info("Starting parallel file-to-chunk connection");

	// Collect all file nodes
	auto fileNodes = collectFileNodes();
	safs::log_info("Found {} file nodes to process", fileNodes.size());

	if (fileNodes.empty()) { return; }

	// Determine optimal thread count
	const size_t numThreads = std::min(
	    {std::thread::hardware_concurrency(), static_cast<unsigned int>(kDefaultThreadCount)});

	safs::log_info("Using {} threads for file-chunk connection", numThreads);

	// Split files into batches
	const size_t filesPerThread = (fileNodes.size() + numThreads - 1) / numThreads;

	std::vector<std::future<void>> futures;
	futures.reserve(numThreads);

	for (size_t i = 0; i < numThreads; ++i) {
		size_t startIndex = i * filesPerThread;
		size_t endIndex = std::min(startIndex + filesPerThread, fileNodes.size());

		if (startIndex >= fileNodes.size()) { break; }

		std::vector<FSNodeFile *> batch(fileNodes.begin() + startIndex, fileNodes.begin() + endIndex);

		futures.emplace_back(std::async(std::launch::async, processFileBatch, std::move(batch), i));
	}

	// Wait for all threads to complete
	for (auto &future : futures) { future.wait(); }

	safs::log_info("Parallel file-to-chunk connection completed");
}

void ParallelFileChunkConnector::processFileBatch(const std::vector<FSNodeFile *> &files,
                                                  size_t batchId) {
	safs::log_info("Thread {} processing {} files", batchId, files.size());

	for (auto *file : files) { connectFileToChunksBatched(file); }

	safs::log_info("Thread {} completed processing {} files", batchId, files.size());
}

std::vector<FSNodeFile *> ParallelFileChunkConnector::collectFileNodes() {
	std::vector<FSNodeFile *> fileNodes;
	fileNodes.reserve(gMetadata->fileNodes);  // Pre-allocate based on known count

	for (uint32_t i = 0; i < NODEHASHSIZE; ++i) {
		for (const auto &node : gMetadata->nodeHash[i]) {
			if (node->type == FSNodeType::kFile || node->type == FSNodeType::kTrash ||
			    node->type == FSNodeType::kReserved) {
				fileNodes.push_back(static_cast<FSNodeFile *>(node));
			}
		}
	}

	return fileNodes;
}

void ParallelFileChunkConnector::connectFileToChunksBatched(FSNodeFile *file) {
	// Group chunks by hash bucket for better cache locality
	std::unordered_map<uint32_t, std::vector<uint64_t>> chunksByHashBucket;
	std::unordered_set<uint64_t> processedChunks;

	// Reserve space to avoid reallocations
	processedChunks.reserve(file->chunks.size());

	// Group chunks by hash bucket
	for (uint64_t chunkId : file->chunks) {
		if (chunkId > 0 && processedChunks.insert(chunkId).second) {
			uint32_t hashPos = chunkHashPos(chunkId);
			chunksByHashBucket[hashPos].push_back(chunkId);
		}
	}

	// Process chunks bucket by bucket for better cache locality
	for (const auto &[hashPos, chunkIds] : chunksByHashBucket) {
		// All chunks in this bucket will access the same hash table entry
		for (uint64_t chunkId : chunkIds) {
			chunk_add_file(chunkId, file->goal);
		}
	}
}
