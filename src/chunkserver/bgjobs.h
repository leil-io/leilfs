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

#include "chunkserver-common/chunk_map.h"
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

/// @brief Mode for consumer threads to pick IO jobs from the JobPool.
enum class IOPriorityMode : uint8_t {
	Fifo,   ///< Process IO jobs in the order they were added, regardless of type.
	Switch  ///< Switch between read and write jobs to prevent starvation of either type.
};

inline IOPriorityMode gIOPriorityMode;

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
	enum ChunkOperation : uint8_t {
		Exit,     ///< Special operation to signal worker threads to exit. Master and client.
		Invalid,  ///< Invalid operation, used for testing and error handling. Master and client.
		ChangeVersion,      ///< Change the version of a chunk. Master only.
		Duplicate,          ///< Duplicate a chunk. Master only.
		Truncate,           ///< Truncate a chunk. Master only.
		DuplicateTruncate,  ///< Duplicate and truncate a chunk. Master only.
		Delete,             ///< Delete a chunk. Master only.
		Create,             ///< Create a chunk. Master only.
		Replicate,          ///< Replicate a chunk. Master only.
		VerifyPart,         ///< Answer whether a part is still held here. Master only.
		Open,               ///< Open a chunk for reading or writing. Client only.
		Close,              ///< Close a chunk. Client only.
		GetBlocks,          ///< Get the blocks of a chunk. Client (actually other CS) only.
		Read,               ///< Read data from a chunk. Client only.
		Prefetch,           ///< Prefetch data from a chunk. Client only.
		Write               ///< Write data to a chunk. Client only.
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

	/// @brief Add job functions type. It is a lambda that contains the expected parameters for add
	/// job call.
	/// @return The ID of the added job.
	using AddJobFunc = std::function<uint32_t()>;

	/// @brief Constructor for JobPool.
	///
	/// @param name Human readable name for this pool, useful for debugging.
	/// @param workers The number of worker threads in the pool.
	/// @param maxJobs The maximum number of jobs that can be queued.
	/// @param nrListeners The number of listeners that will use this JobPool.
	/// @param wakeupFDs A vector of file descriptors for wakeup notifications.
	/// @param numPriorities The number of priority levels for the job queue.
	/// @throws std::runtime_error If the pipe creation fails.
	/// @note After construction, call start() to spawn worker threads. This two-phase
	///       init avoids a vtable-pointer race when constructing derived classes.
	JobPool(const std::string &name, uint8_t workers, uint32_t maxJobs, uint32_t nrListeners,
	        std::vector<int> &wakeupFDs, uint8_t numPriorities = 1);

	/// @brief Spawns the worker threads.
	///
	/// Must be called once, after the fully-derived object has been constructed, so
	/// that virtual dispatch in the worker threads resolves correctly. Calling it
	/// more than once or before the object is fully constructed is undefined behaviour.
	void startWorkers();

	/// @brief Destructor for JobPool.
	/// @note stop() must have been called before destruction (e.g. from a derived class destructor)
	/// to ensure worker threads are shut down correctly. ~JobPool() only releases resources.
	virtual ~JobPool();

	/// @brief Shuts down all worker threads and drains pending status.
	///
	/// Enqueues one Exit job per worker (via the virtual putExitJobToQueue() so
	/// derived classes use the correct priority), joins all threads, and drains
	/// any remaining status queues. Safe to call more than once.
	void stop();

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

	/// @brief Returns whether all jobs in the JobPool have been processed by the worker threads.
	/// This is a very accurate way to check if there are pending jobs in the JobPool, as it counts
	/// the number of jobs that have been added but not yet passed by processCompletedJobs. Must
	/// not be used for the masterConn's jobPool due to the special behavior of the lock jobs.
	bool allJobsProcessed() const;

	/// @brief Checks if the JobPool has no jobs and no status to be sent.
	/// This function is a lighter version of allJobsProcessed that can be used for the masterConn's
	/// jobPool to check if it is idle.
	bool isEmpty();

	/// @brief Gets the number of jobs in the JobPool.
	uint32_t getJobCount() const;

	/// @brief Checks if the JobPool is full.
	bool isFull() const;

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
	/// @param listenerId The ID of the listener associated with the jobs.
	void changeCallback(std::list<uint32_t> &jobIds, const JobCallback &callback,
	                    uint32_t listenerId = 0);

protected:
	/// True on the pool that serves client reads and writes. The H6 hold applies only there:
	/// the master connection pool carries the lease and command traffic that the drain
	/// question is asked about, not the work the question is about.
	bool clientFacing_ = false;

	/// @brief Represents a job in the JobPool.
	struct Job {
		uint32_t jobId;                 // The ID of the job.
		JobCallback callback;           // The callback function to be called upon job completion.
		ProcessJobCallback processJob;  // The callback function to process the job.
		void *extra;                    // Additional data for the callback.
		JobPool::State state;           // The state of the job.
		uint32_t listenerId;            // The ID of the listener associated with the job.
	};

	/// @brief Structure to hold information about a listener.
	struct ListenerInfo {
		int notifierFD;            /// File descriptor for notifications.
		std::mutex notifierMutex;  /// Mutex for event notifications.
		std::mutex jobsMutex;      /// Mutex for job operations.
		std::queue<std::pair<uint32_t, uint8_t>> statusQueue;        /// Queue for job statuses.
		std::unordered_map<uint32_t, std::unique_ptr<Job>> jobHash;  /// Hash map of job.
		uint32_t nextJobId;                                          /// Next job ID to be assigned.
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

	/// @brief Puts an exit job into the job queue.
	virtual void putExitJobToQueue();

	/// @brief Puts a job into the job queue.
	/// @param jobId The ID of the job to be added.
	/// @param operation The type of operation to be performed on the chunk.
	/// @param jobPtrArg A pointer to the data of the job to be added.
	virtual void putToJobQueue(uint32_t jobId, uint32_t operation, uint8_t *jobPtrArg);

	/// @brief Gets a job from the job queue.
	/// @param jobId The ID of the job to be retrieved.
	/// @param operation The type of operation to be performed on the chunk.
	/// @param jobPtrArg A pointer to the data of the job to be retrieved.
	virtual void getFromJobQueue(uint32_t *jobId, uint32_t *operation, uint8_t **jobPtrArg);

	std::vector<ListenerInfo> listenerInfos_;  /// Vector of listener information.
	std::string name_;                         /// Human readable id of the JobPool.
	uint8_t workers;                           /// Number of worker threads in the pool.
	std::vector<std::thread> workerThreads;    /// Vector of worker threads.
	std::unique_ptr<ProducerConsumerQueueWithPriority> jobsQueue;  /// Queue for jobs.
	/// Counter for unprocessed jobs, i.e jobs that have been added to the JobPool but have not yet
	/// been passed by processCompletedJobs and had their callbacks called. This is used to make
	/// sure the JobPool is truly empty when stopping the chunkserver.
	std::atomic<uint32_t> unprocessedJobs_{0};
	/// Guards against double-shutdown (stop() called more than once or from destructor).
	std::atomic<bool> stopped_{false};
};

/**
 * @class MasterJobPool
 * @brief Specialized JobPool for managing master server related jobs with chunk lock handling.
 *
 * The MasterJobPool class extends the JobPool class to provide additional functionality for
 * managing jobs related to master server operations, including handling chunk locks.
 */
class MasterJobPool : public JobPool {
public:
	/// @brief Function type for creating lock job callbacks based on chunk information and listener
	/// ID.
	using LockJobCallbackMaker =
	    std::function<JobCallback(ChunkWithType chunkWithType, uint32_t listenerId)>;

	/// @brief Constructor for MasterJobPool.
	/// @param name Human readable name for this pool, useful for debugging.
	/// @param workers The number of worker threads in the pool.
	/// @param maxJobs The maximum number of jobs that can be queued.
	/// @param nrListeners The number of listeners that will use this JobPool.
	/// @param wakeupFDs A vector of file descriptors for wakeup notifications.
	MasterJobPool(const std::string &name, uint8_t workers, uint32_t maxJobs, uint32_t nrListeners,
	              std::vector<int> &wakeupFDs)
	    : JobPool(name, workers, maxJobs, nrListeners, wakeupFDs) {
		startWorkers();
	}

	/// @brief Adds a job to the JobPool if the chunk is not locked.
	/// If the chunk is locked, the job will be stored and added once the lock is released.
	/// @param chunkWithType The chunk and its type associated with the job.
	/// @param operation The type of operation to be performed on the chunk.
	/// @param callback The callback function to be called upon job completion.
	/// @param extra Additional data to be passed to the callback.
	/// @param processJob The callback function to process the job.
	/// @param listenerId The ID of the listener associated with the job.
	/// @return The ID of the added job, or the lock job ID if the chunk is locked.
	uint32_t addJobIfNotLocked(ChunkWithType chunkWithType, ChunkOperation operation,
	                           JobCallback callback, void *extra, ProcessJobCallback processJob,
	                           uint32_t listenerId = 0);

	/// @brief Changes the callback function for all lock jobs associated with a specific listener.
	/// This function is used to update the callback for all lock jobs when the master server
	/// disconnects to ensure that no chunks with broken data remain registered.
	/// @param lockJobCallbackMaker The function to create new callback functions for all lock jobs.
	/// @param listenerId The ID of the listener associated with the lock jobs.
	void changeLockJobsCallback(const LockJobCallbackMaker &lockJobCallbackMaker,
	                            uint32_t listenerId = 0);

	/// @brief Starts a chunk lock job for a specific chunk and type.
	/// This function is triggered when the master server sends a chunk lock request for a chunk
	/// that is not currently locked. It adds a lock job to the JobPool and associates it with the
	/// locked chunk. If the chunk is already locked, it returns false and does not add a new job,
	/// as the existing lock job will be responsible for handling the lock.
	/// @param callback The callback function to be called when the lock is released.
	/// @param packet The packet to be sent to the master server with the write end status.
	/// @param chunkId The ID of the chunk to be locked.
	/// @param chunkType The type of the chunk to be locked.
	/// @param listenerId The ID of the listener associated with the job.
	/// @return true if the lock job was successfully added, false if the chunk is already locked.
	bool startChunkLock(const JobPool::JobCallback &callback, void *packet, uint64_t chunkId,
	                    ChunkPartType chunkType, uint32_t listenerId = 0);

	/// @brief Enforces a chunk lock for a specific chunk and type.
	/// This function is called when the client sends a write initialization for a chunk that is
	/// already locked, to ensure that the lock is properly enforced. From now on, the master
	/// requests on the locked chunk will wait for the lock to be released before being processed.
	/// @param chunkId The ID of the chunk to enforce the lock on.
	/// @param chunkType The type of the chunk to enforce the lock on.
	/// @return true if the lock was successfully enforced, false if no lock job was found for the
	/// chunk.
	bool enforceChunkLock(uint64_t chunkId, ChunkPartType chunkType);

	/// @brief Ends a chunk lock for a specific chunk and type.
	/// This function is called when cleaning up the write operation on the locked chunk, to end the
	/// lock and allow any pending jobs to be added and processed. It sends the write end status to
	/// the master server and removes the lock job from the JobPool.
	/// @param chunkId The ID of the chunk to end the lock on.
	/// @param chunkType The type of the chunk to end the lock on.
	/// @param status The status to be sent to the master server for the write end.
	void endChunkLock(uint64_t chunkId, ChunkPartType chunkType, uint8_t status);

	/// @brief Erases a chunk lock for a specific chunk and type.
	/// This function is called when the master server sends a chunk unlock request for a chunk that
	/// is currently locked. It removes the lock job from the JobPool and allows any pending jobs
	/// that were waiting for the lock to be released to be added and processed.
	/// @param chunkId The ID of the chunk to erase the lock on.
	/// @param chunkType The type of the chunk to erase the lock on.
	void eraseChunkLock(uint64_t chunkId, ChunkPartType chunkType);

private:
	/// @brief Structure to hold information about a locked chunk.
	struct LockedChunkData {
		uint32_t lockJobId;   /// The ID of the lock job associated with the locked chunk.
		uint32_t listenerId;  /// The ID of the listener associated with the locked chunk.
		/// A vector of functions to add pending jobs to be executed once the lock is released.
		std::vector<AddJobFunc> pendingAddJobs;
		/// Flag to indicate if the write initialization has been received for the locked chunk.
		bool writeInitReceived = false;

		LockedChunkData(uint32_t lockJobId, uint32_t listenerId)
		    : lockJobId(lockJobId), listenerId(listenerId) {}

		LockedChunkData() = default;
	};

	/// @brief Adds a lock job to the JobPool for a specific chunk and type.
	/// This function is used to create a lock job that will be associated with a locked chunk.
	/// @param callback The callback function to be called when the lock is released.
	/// @param extra Additional data to be passed to the callback.
	/// @param listenerId The ID of the listener associated with the job.
	/// @return The ID of the added lock job.
	uint32_t addLockJob(JobCallback callback, void *extra, uint32_t listenerId = 0);

	/// @brief Releases a chunk lock entry for a specific chunk and type.
	/// This is a helper function that returns the lock job ID, listener ID, and pending jobs
	/// associated with a locked chunk if found, and removes the lock entry from the internal map.
	/// @param chunkId The ID of the chunk to release the lock on.
	/// @param chunkType The type of the chunk to release the lock on.
	/// @param callerName The name of the function calling this helper, used for logging.
	/// @param lockJobId The ID of the lock job associated with the chunk.
	/// @param listenerId The ID of the listener associated with the lock job.
	/// @param pendingAddJobs The list of pending jobs associated with the locked chunk.
	/// @return true if the lock entry was found and released, false otherwise.
	bool releaseChunkLockEntry(uint64_t chunkId, ChunkPartType chunkType, const char *callerName,
	                           uint32_t &lockJobId, uint32_t &listenerId,
	                           std::vector<AddJobFunc> &pendingAddJobs);

	/// Mutex to protect access to the chunkToJobReplyMap_.
	std::mutex chunkToJobReplyMapMutex_;
	/// Map to associate locked chunks with their corresponding lock job and pending jobs.
	std::unordered_map<ChunkWithType, LockedChunkData, KeyOperations, KeyOperations>
	    chunkToJobReplyMap_;
};

/**
 * @class ClientJobPool
 * @brief Specialized JobPool for managing client server related jobs.
 *
 * The ClientJobPool class extends the JobPool class to provide additional functionality for
 * managing jobs related to client server operations. It includes a mechanism to prioritize read and
 * write jobs based on the IOPriorityMode.
 */
class ClientJobPool : public JobPool {
public:
	/// @brief Constructor for ClientJobPool.
	/// @param name Human readable name for this pool, useful for debugging.
	/// @param workers The number of worker threads in the pool.
	/// @param maxJobs The maximum number of jobs that can be queued.
	/// @param nrListeners The number of listeners that will use this JobPool.
	/// @param wakeupFDs A vector of file descriptors for wakeup notifications.
	ClientJobPool(const std::string &name, uint8_t workers, uint32_t maxJobs, uint32_t nrListeners,
	              std::vector<int> &wakeupFDs, IOPriorityMode ioPriorityMode)
	    : JobPool(name, workers, maxJobs, nrListeners, wakeupFDs,
	              ioPriorityMode == IOPriorityMode::Fifo ? 2 : 3),
	      ioPriorityMode_(ioPriorityMode) {
		clientFacing_ = true;
		if (ioPriorityMode_ == IOPriorityMode::Switch) {
			preferredIOType_.store(kPreferRead);
		} else {
			preferredIOType_.store(kPreferAny);
		}

		startWorkers();
	}

	/// @brief Destructor for ClientJobPool.
	/// Calls stop() while the derived object is still alive so that
	/// putExitJobToQueue() virtual dispatch resolves to the correct override,
	/// enqueuing Exit at the right (lowest) priority.
	~ClientJobPool() override;

private:
	constexpr static uint8_t kPreferAny = 0;
	constexpr static uint8_t kReadLevel = 1;
	constexpr static uint8_t kWriteLevelFifoMode = kReadLevel;
	constexpr static uint8_t kWriteLevelSwitchMode = 2;

	constexpr static uint8_t kPreferRead = kReadLevel;
	constexpr static uint8_t kPreferWrite = kWriteLevelSwitchMode;
	constexpr static uint8_t kSwitchValue = kPreferRead ^ kPreferWrite;

	/// @brief Gets the job priority based on the operation type.
	/// @param operation The type of operation to be performed on the chunk.
	/// @return The priority of the job, where lower values indicate higher priority.
	uint8_t getJobPriority(ChunkOperation operation);

	/// @brief Puts an exit job into the client job queue.
	void putExitJobToQueue() override;

	/// @brief Puts a job into the client job queue.
	/// @note The ClientJobPool uses the priority parameter of the put function to give higher
	/// priority to Open, Close and GetBlocks operations.
	/// @param jobId The ID of the job to be added.
	/// @param operation The type of operation to be performed on the chunk.
	/// @param jobPtrArg A pointer to the data of the job to be added.
	void putToJobQueue(uint32_t jobId, uint32_t operation, uint8_t *jobPtrArg) override;

	/// @brief Gets a job from the client job queue.
	/// @note The ClientJobPool uses the preferredIOType_ member to switch between preferring read
	/// and write jobs when the IOPriorityMode is set to Switch. The preferredIOType_ is updated
	/// every time a job is retrieved from the queue if in switch mode, to give more balanced access
	/// to read and write operations.
	/// @param jobId The ID of the job to be retrieved.
	/// @param operation The type of operation to be performed on the chunk.
	/// @param jobPtrArg A pointer to the data of the job to be retrieved.
	void getFromJobQueue(uint32_t *jobId, uint32_t *operation, uint8_t **jobPtrArg) override;

	/// The preferred IO type for the Switch mode, used to switch between read and write jobs.
	std::atomic<uint8_t> preferredIOType_;
	IOPriorityMode ioPriorityMode_;
};

/// @brief Adds an open job to the ClientJobPool.
///
/// @param jobPool The ClientJobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param chunkId The ID of the chunk.
/// @param chunkType The type of the chunk.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_open(ClientJobPool &jobPool, JobPool::JobCallback callback, uint64_t chunkId,
                  ChunkPartType chunkType, uint32_t listenerId = 0);

/// @brief Adds a close job to the JobPool.
///
/// @param jobPool The JobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param chunkId The ID of the chunk.
/// @param chunkType The type of the chunk.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_close(ClientJobPool &jobPool, JobPool::JobCallback callback, uint64_t chunkId,
                   ChunkPartType chunkType, uint32_t listenerId = 0);

/// @brief Adds a read job to the ClientJobPool.
///
/// @param jobPool The ClientJobPool instance.
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
uint32_t job_read(ClientJobPool &jobPool, JobPool::JobCallback callback, uint64_t chunkId,
                  uint32_t version, ChunkPartType chunkType, uint32_t offset, uint32_t size,
                  uint32_t maxBlocksToBeReadBehind, uint32_t blocksToBeReadAhead,
                  OutputBuffer *outputBuffer, bool performHddOpen, uint32_t listenerId = 0);

/// @brief Adds a prefetch job to the ClientJobPool.
///
/// @param jobPool The ClientJobPool instance.
/// @param chunkId The ID of the chunk.
/// @param chunkType The type of the chunk.
/// @param firstBlockToBePrefetched The first block to be prefetched.
/// @param blocksToBePrefetched The number of blocks to be prefetched.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_prefetch(ClientJobPool &jobPool, uint64_t chunkId, ChunkPartType chunkType,
                      uint32_t firstBlockToBePrefetched, uint32_t blocksToBePrefetched,
                      uint32_t listenerId = 0);

/// @brief Adds a write job to the ClientJobPool.
///
/// @param jobPool The ClientJobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param chunkId The ID of the chunk.
/// @param chunkVersion The version of the chunk.
/// @param chunkType The type of the chunk.
/// @param inputBuffers The input buffers containing the data, offsets, block indexes and CRCs to be
/// written.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_write(ClientJobPool &jobPool, JobPool::JobCallback callback, uint64_t chunkId,
                   uint32_t chunkVersion, ChunkPartType chunkType,
                   std::vector<InputBuffer *> inputBuffers, uint32_t listenerId = 0);

/// @brief Adds a get blocks job to the ClientJobPool.
///
/// @param jobPool The ClientJobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param chunkId The ID of the chunk.
/// @param version The version of the chunk.
/// @param chunkType The type of the chunk.
/// @param blocks The blocks to get.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_get_blocks(ClientJobPool &jobPool, JobPool::JobCallback callback, uint64_t chunkId,
                        uint32_t version, ChunkPartType chunkType, uint16_t *blocks,
                        uint32_t listenerId = 0);

/// @brief Adds a replicate job to the JobPool.
///
/// @param jobPool The MasterJobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param chunkId The ID of the chunk.
/// @param chunkVersion The version of the chunk.
/// @param chunkType The type of the chunk.
/// @param sourcesBufferSize The size of the sources buffer.
/// @param sourcesBuffer The sources buffer.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_replicate(MasterJobPool &jobPool, JobPool::JobCallback callback, void *extra,
                       uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                       uint32_t sourcesBufferSize, const uint8_t *sourcesBuffer,
                       uint32_t listenerId = 0);

/// @brief Adds an invalid job to the JobPool.
///
/// @param jobPool The MasterJobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_invalid(MasterJobPool &jobPool, JobPool::JobCallback callback, void *extra,
                     uint32_t listenerId = 0);

/// @brief Adds a delete job to the JobPool.
///
/// @param jobPool The MasterJobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param chunkId The ID of the chunk.
/// @param chunkVersion The version of the chunk.
/// @param chunkType The type of the chunk.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_delete(MasterJobPool &jobPool, JobPool::JobCallback callback, void *extra,
                    uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                    uint32_t listenerId = 0);

/// @brief Adds a part verification job to the JobPool.
///
/// It answers whether this chunkserver still has a record of the part, so it runs on the pool
/// rather than the network thread: a registry lookup can wait behind IO already in flight on that
/// chunk, and a question about bookkeeping may not hold up the connection that asked it.
///
/// @param jobPool The MasterJobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param chunkId The ID of the chunk.
/// @param chunkType The type of the chunk.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_verify_part(MasterJobPool &jobPool, JobPool::JobCallback callback, void *extra,
                         uint64_t chunkId, ChunkPartType chunkType, uint32_t listenerId = 0);

/// @brief Adds a create job to the JobPool.
///
/// @param jobPool The MasterJobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param chunkId The ID of the chunk.
/// @param chunkVersion The version of the chunk.
/// @param chunkType The type of the chunk.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_create(MasterJobPool &jobPool, JobPool::JobCallback callback, void *extra,
                    uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                    uint32_t listenerId = 0);

/// @brief Adds a change version job to the JobPool.
///
/// @param jobPool The MasterJobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param chunkId The ID of the chunk.
/// @param chunkVersion The version of the chunk.
/// @param chunkType The type of the chunk.
/// @param newChunkVersion The new version of the chunk.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_version(MasterJobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                     uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                     uint32_t newChunkVersion, uint32_t listenerId = 0);

/// @brief Adds a truncate job to the JobPool.
///
/// @param jobPool The MasterJobPool instance.
/// @param callback The callback function to be called upon job completion.
/// @param extra Additional data to be passed to the callback.
/// @param chunkId The ID of the chunk.
/// @param chunkType The type of the chunk.
/// @param chunkVersion The version of the chunk.
/// @param newChunkVersion The new version of the chunk.
/// @param length The length to truncate to.
/// @param listenerId The ID of the listener associated with the job.
/// @return The ID of the added job.
uint32_t job_truncate(MasterJobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                      uint64_t chunkId, ChunkPartType chunkType, uint32_t chunkVersion,
                      uint32_t newChunkVersion, uint32_t length, uint32_t listenerId = 0);

/// @brief Adds a duplicate job to the JobPool.
///
/// @param jobPool The MasterJobPool instance.
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
uint32_t job_duplicate(MasterJobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                       uint64_t chunkId, uint32_t chunkVersion, uint32_t newChunkVersion,
                       ChunkPartType chunkType, uint64_t chunkIdCopy, uint32_t chunkVersionCopy,
                       uint32_t listenerId = 0);

/// @brief Adds a duplicate and truncate job to the JobPool.
///
/// @param jobPool The MasterJobPool instance.
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
uint32_t job_duptrunc(MasterJobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                      uint64_t chunkId, uint32_t chunkVersion, uint32_t newChunkVersion,
                      ChunkPartType chunkType, uint64_t chunkIdCopy, uint32_t chunkVersionCopy,
                      uint32_t length, uint32_t listenerId = 0);
