/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include "chunkserver/io_buffers.h"
#include "common/pcqueue.h"

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

constexpr auto kEmptyCallback = nullptr;
constexpr auto kEmptyExtra = nullptr;

/**
 * @class JobPool
 * @brief Manages and processes background jobs in a thread pool.
 *
 * The JobPool class is responsible for managing and processing background jobs
 * using a pool of worker threads. It provides functions to manage jobs and
 * callbacks to process them. The class uses a producer-consumer pattern to handle
 * job requests and status updates.
 */
class JobPool {
public:
	/// @enum State
	/// @brief Represents the state of a job.
	enum class State : uint8_t {
		Disabled,   /// Job is disabled and will not be processed.
		Enabled,    /// Job is enabled and ready to be processed.
		InProgress  /// Job is currently being processed.
	};

	/// @enum ChunkOperation
	/// @brief Represents the type of operation to be performed on a chunk.
	enum ChunkOperation {
		Exit,
		Invalid,
		ChangeVersion,
		Duplicate,
		Truncate,
		DuplicateTruncate,
		Delete,
		Create,
		Open,
		Close,
		Read,
		Prefetch,
		Write,
		Replicate,
		GetBlocks
	};

	/// @brief Callback function type for job completion.
	///
	/// @param status The status of the job.
	/// @param extra Additional data passed to the callback, like ChunkserverEntry entries.
	using JobCallback = std::function<void(uint8_t status, void *extra)>;

	/// @brief Callback function type for processing a job.
	///
	/// @return The status of the job processing.
	using ProcessJobCallback = std::function<uint8_t()>;

	/// @brief Constructor for JobPool.
	///
	/// @param name Human readable name for this pool, useful for debugging.
	/// @param workers The number of worker threads in the pool.
	/// @param maxJobs The maximum number of jobs that can be queued.
	/// @param nrListeners The number of listeners that will use this JobPool.
	/// @param wakeupFDs A vector of file descriptors for wakeup notifications.
	/// @throws std::runtime_error If the pipe creation fails.
	JobPool(const std::string &name, uint8_t workers, uint32_t maxJobs, uint32_t nrListeners,
	        std::vector<int> &wakeupFDs);

	/// @brief Destructor for JobPool.
	~JobPool();

	/// @brief Adds a job to the JobPool.
	///
	/// @param operation The type of operation to be performed on the chunk.
	/// @param callback The callback function to be called upon job completion.
	/// @param extra Additional data to be passed to the callback.
	/// @param processJob The callback function to process the job.
	/// @param listenerId The ID of the listener associated with the job.
	/// @return The ID of the added job.
	uint32_t addJob(ChunkOperation operation, JobCallback callback, void *extra,
	                ProcessJobCallback processJob, uint32_t listenerId = 0);

	/// @brief Gets the number of jobs in the JobPool.
	uint32_t getJobCount() const;

	/// @brief Disables all jobs and changes their callback function.
	///
	/// @param callback The new callback function to be set for all jobs.
	/// @param listenerId The ID of the listener associated with the jobs.
	void disableAndChangeCallbackAll(const JobCallback &callback, uint32_t listenerId = 0);

	/// @brief Disables a specific job.
	///
	/// @param jobId The ID of the job to be disabled.
	/// @param listenerId The ID of the listener associated with the job.
	void disableJob(uint32_t jobId, uint32_t listenerId = 0);

	/// @brief Disables a list of jobs.
	///
	/// @param jobIds The list of jobs by IDs to be disabled.
	/// @param listenerId The ID of the listener associated with the jobs.
	/// @return A list of job IDs that were successfully disabled.
	std::list<uint32_t> disableJobs(const std::list<uint32_t> &jobIds, uint32_t listenerId = 0);

	/// @brief Checks the status of jobs in the JobPool and calls their callbacks.
	///
	/// @param listenerId The ID of the listener associated with the jobs.
	void processCompletedJobs(uint32_t listenerId = 0);

	/// @brief Changes the callback function for a specific job.
	///
	/// @param jobId The ID of the job.
	/// @param callback The new callback function.
	/// @param extra Additional data to be passed to the new callback.
	/// @param listenerId The ID of the listener associated with the jobs.
	void changeCallback(uint32_t jobId, JobCallback callback, void *extra, uint32_t listenerId = 0);

	/// @brief Changes the callback function for a list of jobs.
	///
	/// @param jobIds The list of jobs by IDs.
	/// @param callback The new callback function.
	/// @param extra Additional data to be passed to the new callback.
	/// @param listenerId The ID of the listener associated with the jobs.
	void changeCallback(std::list<uint32_t> &jobIds, const JobCallback &callback, void *extra,
	                    uint32_t listenerId = 0);

private:
	/// @brief Represents a job in the JobPool.
	struct Job {
		uint32_t jobId;                 // The ID of the job.
		JobCallback callback;           // The callback function to be called upon job completion.
		ProcessJobCallback processJob;  // The callback function to process the job.
		void *extra;                    // Additional data for the callback.
		JobPool::State state;           // The state of the job.
		uint32_t listenerId;            // The ID of the listener associated with the job.
	};

	/// @brief Worker thread function.
	/// @param poolName Parent pool name, used to name the specific thread.
	/// @param workerId Worker index in this pool, used to name the thread.
	void workerThread(const std::string &poolName, uint8_t workerId);

	/// @brief Sends the status of a job.
	///
	/// @param jobId The ID of the job.
	/// @param status The status of the job.
	/// @param listenerId The ID of the listener associated with the job.
	void sendStatus(uint32_t jobId, uint8_t status, uint32_t listenerId = 0);

	/// @brief Receives the status of a job.
	///
	/// @param jobId The ID of the job.
	/// @param status The status of the job.
	/// @param listenerId The ID of the listener associated with the job.
	/// @return 1 if a status is not the last one, 0 if it is the last status.
	bool receiveStatus(uint32_t &jobId, uint8_t &status, uint32_t listenerId = 0);

	/// @brief Structure to hold information about a listener.
	struct ListenerInfo {
		int notifierFD;            /// File descriptor for notifications.
		std::mutex notifierMutex;  /// Mutex for event notifications.
		std::mutex jobsMutex;      /// Mutex for job operations.
		std::queue<std::pair<uint32_t, uint8_t>> statusQueue;        /// Queue for job statuses.
		std::unordered_map<uint32_t, std::unique_ptr<Job>> jobHash;  /// Hash map of job.
		uint32_t nextJobId;                                          /// Next job ID to be assigned.
	};

	std::vector<ListenerInfo> listenerInfos_;          /// Vector of listener information.
	std::string name_;                                 /// Human readable id of the JobPool.
	uint8_t workers;                                   /// Number of worker threads in the pool.
	std::vector<std::thread> workerThreads;            /// Vector of worker threads.
	std::unique_ptr<ProducerConsumerQueue> jobsQueue;  /// Queue for jobs.
};

/// @brief Adds an open job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param chunkId The ID of the chunk.
/// @param chunkType The type of the chunk.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_open(JobPool &jobPool, JobPool::JobCallback callback, uint64_t chunkId,
                  ChunkPartType chunkType, uint32_t listenerId = 0);

/// @brief Adds a close job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param chunkId The ID of the chunk.
/// @param chunkType The type of the chunk.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_close(JobPool &jobPool, JobPool::JobCallback callback, uint64_t chunkId,
                   ChunkPartType chunkType, uint32_t listenerId = 0);

/// @brief Adds a read job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param chunkId The ID of the chunk.
/// @param version The version of the chunk.
/// @param chunkType The type of the chunk.
/// @param offset The offset to read from.
/// @param size The size to read.
/// @param maxBlocksToBeReadBehind The maximum blocks to be read behind.
/// @param blocksToBeReadAhead The blocks to be read ahead.
/// @param outputBuffer The output buffer for the read data.
/// @param performHddOpen Whether to perform HDD open.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_read(JobPool &jobPool, JobPool::JobCallback callback, uint64_t chunkId,
                  uint32_t version, ChunkPartType chunkType, uint32_t offset, uint32_t size,
                  uint32_t maxBlocksToBeReadBehind, uint32_t blocksToBeReadAhead,
                  OutputBuffer *outputBuffer, bool performHddOpen, uint32_t listenerId = 0);

/// @brief Adds a prefetch job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param chunkId The ID of the chunk.
/// @param chunkType The type of the chunk.
/// @param firstBlockToBePrefetched The first block to be prefetched.
/// @param blocksToBePrefetched The number of blocks to be prefetched.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_prefetch(JobPool &jobPool, uint64_t chunkId, ChunkPartType chunkType,
                      uint32_t firstBlockToBePrefetched, uint32_t blocksToBePrefetched,
                      uint32_t listenerId = 0);

/// @brief Adds a write job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param chunkId The ID of the chunk.
/// @param chunkVersion The version of the chunk.
/// @param chunkType The type of the chunk.
/// @param blockNum The block number to write.
/// @param offset The offset to write to.
/// @param size The size to write.
/// @param crc The CRC of the data.
/// @param buffer The data buffer to write.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_write(JobPool &jobPool, JobPool::JobCallback callback, uint64_t chunkId,
                   uint32_t chunkVersion, ChunkPartType chunkType, InputBuffer *inputBuffer,
                   uint32_t listenerId = 0);

/// @brief Adds a get blocks job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param chunkId The ID of the chunk.
/// @param version The version of the chunk.
/// @param chunkType The type of the chunk.
/// @param blocks The blocks to get.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_get_blocks(JobPool &jobPool, JobPool::JobCallback callback, uint64_t chunkId,
                        uint32_t version, ChunkPartType chunkType, uint16_t *blocks,
                        uint32_t listenerId = 0);

/// @brief Adds a replicate job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param chunkId The ID of the chunk.
/// @param chunkVersion The version of the chunk.
/// @param chunkType The type of the chunk.
/// @param sourcesBufferSize The size of the sources buffer.
/// @param sourcesBuffer The sources buffer.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_replicate(JobPool &jobPool, JobPool::JobCallback callback, void *extra,
                       uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                       uint32_t sourcesBufferSize, const uint8_t *sourcesBuffer,
                       uint32_t listenerId = 0);

/// @brief Adds an invalid job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_invalid(JobPool &jobPool, JobPool::JobCallback callback, void *extra,
                     uint32_t listenerId = 0);

/// @brief Adds a delete job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param chunkId The ID of the chunk.
/// @param chunkVersion The version of the chunk.
/// @param chunkType The type of the chunk.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_delete(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                    uint32_t chunkVersion, ChunkPartType chunkType, uint32_t listenerId = 0);

/// @brief Adds a create job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param chunkId The ID of the chunk.
/// @param chunkVersion The version of the chunk.
/// @param chunkType The type of the chunk.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_create(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                    uint32_t chunkVersion, ChunkPartType chunkType, uint32_t listenerId = 0);

/// @brief Adds a change version job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param chunkId The ID of the chunk.
/// @param chunkVersion The version of the chunk.
/// @param chunkType The type of the chunk.
/// @param newChunkVersion The new version of the chunk.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_version(JobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                     uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                     uint32_t newChunkVersion, uint32_t listenerId = 0);

/// @brief Adds a truncate job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param chunkId The ID of the chunk.
/// @param chunkType The type of the chunk.
/// @param chunkVersion The version of the chunk.
/// @param newChunkVersion The new version of the chunk.
/// @param length The length to truncate to.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_truncate(JobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                      uint64_t chunkId, ChunkPartType chunkType, uint32_t chunkVersion,
                      uint32_t newChunkVersion, uint32_t length, uint32_t listenerId = 0);

/// @brief Adds a duplicate job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param chunkId The ID of the chunk.
/// @param chunkVersion The version of the chunk.
/// @param newChunkVersion The new version of the chunk.
/// @param chunkType The type of the chunk.
/// @param chunkIdCopy The ID of the chunk to copy.
/// @param chunkVersionCopy The version of the chunk to copy.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_duplicate(JobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                       uint64_t chunkId, uint32_t chunkVersion, uint32_t newChunkVersion,
                       ChunkPartType chunkType, uint64_t chunkIdCopy, uint32_t chunkVersionCopy,
                       uint32_t listenerId = 0);

/// @brief Adds a duplicate and truncate job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param chunkId The ID of the chunk.
/// @param chunkVersion The version of the chunk.
/// @param newChunkVersion The new version of the chunk.
/// @param chunkType The type of the chunk.
/// @param chunkIdCopy The ID of the chunk to copy.
/// @param chunkVersionCopy The version of the chunk to copy.
/// @param length The length to truncate to.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_duptrunc(JobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                      uint64_t chunkId, uint32_t chunkVersion, uint32_t newChunkVersion,
                      ChunkPartType chunkType, uint64_t chunkIdCopy, uint32_t chunkVersionCopy,
                      uint32_t length, uint32_t listenerId = 0);
