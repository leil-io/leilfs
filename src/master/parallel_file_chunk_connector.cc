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

	std::vector<std::pair<uint64_t, uint8_t>> chunkGoalPairs;

	chunkGoalPairs.reserve(files.size());

	auto startTime = std::chrono::steady_clock::now();
	size_t totalChunks = 0;

	std::set<uint32_t> chunkHashes;

	for (auto *file : files) {
		totalChunks += file->chunks.size();
		//connectFileToChunksBatched(file);
		for (uint64_t chunkId : file->chunks) {
			if (chunkId > 0) {
				chunkGoalPairs.emplace_back(chunkId, file->goal);
			}
			chunkHashes.insert(chunkHashPos(chunkId));
		}
	}

	// std::vector<int> results = chunk_add_files_bulk(chunkGoalPairs, chunkHashes);

	auto endTime = std::chrono::steady_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

	safs::log_info("Thread {} completed: {} files, {} chunks, {}ms ({} chunks/sec)", batchId,
	               files.size(), totalChunks, duration.count(),
	               (totalChunks * 1000) / std::max(duration.count(), 1L));
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
	//safs::log_info("{}: Starting bulk chunk addition of {}", chunkGoalPairs.size());
	std::vector<std::pair<uint64_t, uint8_t>> chunkGoalPairs;
	std::unordered_set<uint64_t> processedChunks;

	chunkGoalPairs.reserve(file->chunks.size());
	processedChunks.reserve(file->chunks.size());

	// Collect unique chunks
	for (uint64_t chunkId : file->chunks) {
		if (chunkId > 0 && processedChunks.insert(chunkId).second) {
			chunkGoalPairs.emplace_back(chunkId, file->goal);
		}
	}

	// Single bulk operation instead of multiple individual calls
	safs::log_info("{}: Starting bulk chunk addition of {}", __func__, chunkGoalPairs.size());
	//std::vector<int> results = chunk_add_files_bulk(chunkGoalPairs);

	// Report possible errors
	/*for (size_t i = 0; i < results.size(); ++i) {
		if (results[i] != SAUNAFS_STATUS_OK) {
			safs::log_warn("Failed to add file to chunk {}: error {}", chunkGoalPairs[i].first,
			               results[i]);
		}
	}*/
}
