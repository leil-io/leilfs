#pragma once

#include "common/platform.h"

#include <vector>

#include "master/filesystem_node_types.h"

/// Parallel file-to-chunk connection optimization
class ParallelFileChunkConnector {
public:
	static constexpr size_t kDefaultThreadCount = 8;

	/// Connect all files to chunks in parallel
	static void connectFilesToChunks();

private:
	/// Process a batch of files in a worker thread
	static void processFileBatch(const std::vector<FSNodeFile *> &files, size_t batchId);

	/// Collect all file nodes from hash table
	static std::vector<FSNodeFile *> collectFileNodes();

	/// Batch chunks by hash position for better cache locality
	static void connectFileToChunksBatched(FSNodeFile *file);
};
