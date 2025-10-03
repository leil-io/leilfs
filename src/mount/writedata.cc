/*
 Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
 Copyright 2013-2017 Skytechnology sp. z o.o.
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

#include "common/platform.h"
#include "mount/writedata.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <vector>

#include "common/chunk_connector.h"

#include "common/crc.h"
#include "common/datapack.h"
#include "common/exceptions.h"
#include "common/goal.h"
#include "common/massert.h"
#include "common/message_receive_buffer.h"
#include "errors/sfserr.h"
#include "common/multi_buffer_writer.h"
#include "common/pcqueue.h"
#include "common/slice_traits.h"
#include "slogger/slogger.h"
#include "common/sockets.h"
#include "common/time_utils.h"
#include "devtools/request_log.h"
#include "mount/chunk_writer.h"
#include "mount/global_chunkserver_stats.h"
#include "mount/mastercomm.h"
#include "mount/mount_info.h"
#include "mount/readdata.h"
#include "mount/stats.h"
#include "mount/tweaks.h"
#include "mount/notification_area_logging.h"
#include "mount/write_cache_block.h"
#include "protocol/cltocs.h"
#include "protocol/SFSCommunication.h"

#define IDLE_CONNECTION_TIMEOUT 6
#define NO_INODEDATA nullptr
#define NO_CHUNKDATA nullptr

enum class WriteStats : uint8_t {
	CACHED_BYTES,            ///< Total number of bytes written to cache
	FREE_CACHE_AVG_kb,       ///< Average number of free cache KB in last few seconds
	TOTAL_FLUSH_TIME_ms,     ///< Total time spent waiting for flush to complete
	ACTIVE_WORKERS_TIME_ms,  ///< Sum of time write workers were actively writing
	STATNODES
};

// Around 2s of history with usual granularity (kTicksPerSecond = 10)
constexpr uint32_t kFCBHistorySize = 20;
static std::list<uint64_t> gFCBHistory;
std::atomic<uint64_t> gCachedBytes = 0;
static std::vector<uint64_t *> statsPtr(static_cast<uint8_t>(WriteStats::STATNODES));

static void writeStatsInit() {
	statsnode *s = stats_get_subnode(nullptr, "write_details", 0);
	auto init_stat = [&](WriteStats stat, const char *name) {
		statsPtr[static_cast<uint8_t>(stat)] = stats_get_counterptr(stats_get_subnode(s, name, 0));
	};

	init_stat(WriteStats::CACHED_BYTES, "cached_bytes");
	init_stat(WriteStats::FREE_CACHE_AVG_kb, "free_cache_avg_kb");
	init_stat(WriteStats::TOTAL_FLUSH_TIME_ms, "total_flush_time_ms");
	init_stat(WriteStats::ACTIVE_WORKERS_TIME_ms, "active_workers_time_ms");
}

static void statsAdd(WriteStats type, int64_t value = 1) {
	if (value < 0) {
		::stats_dec(static_cast<uint8_t>(type), statsPtr, static_cast<uint64_t>(-value));
		return;
	}
	::stats_inc(static_cast<uint8_t>(type), statsPtr, static_cast<uint64_t>(value));
}

static void updateStats(uint32_t freeCacheBlocks) {
	// Update FCB average
	gFCBHistory.push_back(freeCacheBlocks);

	if (gFCBHistory.size() > kFCBHistorySize) {
		gFCBHistory.pop_front();
	}

	int64_t prevAvg = *statsPtr[static_cast<uint8_t>(WriteStats::FREE_CACHE_AVG_kb)];
	int64_t newAvg = 0;
	for (const auto &v : gFCBHistory) {
		newAvg += v;
	}
	newAvg *= (SFSBLOCKSIZE / 1024);  // in KB
	newAvg /= gFCBHistory.size();

	if (prevAvg != newAvg) {
		statsAdd(WriteStats::FREE_CACHE_AVG_kb, newAvg - prevAvg);
	}

	// Update cached bytes
	statsAdd(WriteStats::CACHED_BYTES, gCachedBytes.exchange(0));
}

static bool gUseInodeBasedWriteAlgorithm = false;

namespace InodeBasedWriteAlgorithm {

struct inodedata {
	inode_t inode;
	uint64_t maxfleng;
	int status;
	uint16_t flushwaiting;
	uint16_t writewaiting;
	uint16_t lcnt;
	uint32_t trycnt;
	bool inqueue;  // true it this inode is waiting in one of the queues or is being processed
	uint32_t minimumBlocksToWrite;
	std::list<WriteCacheBlock> dataChain;
	int alterations_in_chain;  // number of adherent blocks with different chunk ids in chain
	std::condition_variable flushcond;  // wait for !inqueue (flush)
	std::condition_variable writecond;  // wait for flushwaiting==0 (write)
	std::unique_ptr<WriteChunkLocator> locator;
	int newDataInChainPipe[2];
	bool workerWaitingForData;
	Timer lastWriteToDataChain;
	Timer lastWriteToChunkservers;

	inodedata(inode_t inode)
	    : inode(inode),
	      maxfleng(0),
	      status(SAUNAFS_STATUS_OK),
	      flushwaiting(0),
	      writewaiting(0),
	      lcnt(0),
	      trycnt(0),
	      inqueue(false),
	      minimumBlocksToWrite(1),
	      alterations_in_chain(),
	      workerWaitingForData(false) {
#ifdef _WIN32
		// We don't use inodeData->waitingworker and inodeData->pipe on Cygwin because
		// Cygwin's implementation of mixed socket & pipe polling is very inefficient.
		// On mingw platform pipes are unavailable.
		newDataInChainPipe[0] = newDataInChainPipe[1] = -1;
#else
		if (pipe(newDataInChainPipe) < 0) {
			safs::log_warn("creating pipe error: {}", strerr(errno));
			newDataInChainPipe[0] = -1;
		}
#endif
	}

	~inodedata() {
		if (isDataChainPipeValid()) {
			close(newDataInChainPipe[0]);
			close(newDataInChainPipe[1]);
		}
	}

	/* glock: LOCKED */
	void wakeUpWorkerIfNecessary() {
		/*
		 * Write worker always looks for the first block in chain and we modify or add always the
		 * last block in chain so it is necessary to wake up write worker only if the first block
		 * is the last one, ie. dataChain.size() == 1.
		 */
		if (workerWaitingForData && dataChain.size() == 1 && isDataChainPipeValid()) {
			if (write(newDataInChainPipe[1], " ", 1) != 1) {
				safs::log_err("write pipe error: {}", strerr(errno));
			}
			workerWaitingForData = false;
		}
	}

	/* glock: UNUSED */
	bool isDataChainPipeValid() const { return newDataInChainPipe[0] >= 0; }

	/*! Check if inode requires flushing all its data chain to chunkservers.
	 *
	 * Returns true if anyone requested to flush the data by calling write_data_flush
	 * or write_data_flush_inode or the data in data chain is too old to keep it longer in
	 * our buffers. If this function returns false, we write only full stripes from data
	 * chain to chunkservers.
	 * glock: LOCKED
	 */
	bool requiresFlushing() const {
		return (flushwaiting > 0 ||
		        lastWriteToDataChain.elapsed_ms() >= kMaximumTimeInDataChainSinceLastWrite_ms ||
		        lastWriteToChunkservers.elapsed_ms() >= kMaximumTimeInDataChainSinceLastFlush_ms);
	}

	void pushToChain(WriteCacheBlock &&block) {
		dataChain.push_back(std::move(block));
		if (dataChain.size() > 1 &&
		    dataChain.back().chunkIndex != std::next(dataChain.rbegin())->chunkIndex) {
			alterations_in_chain++;
		}
	}

	void popFromChain() {
		assert(dataChain.size() > 0);
		if (dataChain.size() > 1 &&
		    dataChain.front().chunkIndex != std::next(dataChain.begin())->chunkIndex) {
			alterations_in_chain--;
		}
		dataChain.pop_front();
	}

	void registerAlterationsInChain(int delta) { alterations_in_chain += delta; }

	bool hasMultipleChunkIdsInChain() const { return alterations_in_chain > 0; }

	bool wasLastBlockRecentlyWrittenAndTaken() const {
		return dataChain.empty() && lastWriteToDataChain.elapsed_ms() < kVeryRecentWrite_ms;
	}

private:
	/*! Limit for \p lastWriteToChunkservers after which we force a flush.
	 *
	 * Maximum time for data to be kept in data chain waiting for collecting a full stripe.
	 */
	static const uint32_t kMaximumTimeInDataChainSinceLastFlush_ms = 15000;

	/*! Limit for \p lastWriteToDataChain after which we force a flush.
	 *
	 * Maximum time for data to be kept in data chain waiting for collecting a full stripe
	 * if no new data is written into the data chain
	 */
	static const uint32_t kMaximumTimeInDataChainSinceLastWrite_ms = 5000;

	/*! Limit for considering the last write to data chain very recent.
	 *
	 * This is used to avoid writing a partial stripe to chunkservers when last block was full and
	 * next one has not even started.
	 */
	static const uint32_t kVeryRecentWrite_ms = 5;
};

struct DelayedQueueEntry {
	inodedata *inodeData;
	int32_t ticksLeft;
	static constexpr int kTicksPerSecond = 10;

	DelayedQueueEntry(inodedata *inodeData, int32_t ticksLeft)
	    : inodeData(inodeData), ticksLeft(ticksLeft) {}
};

static bool gIsInitialized = false;

static std::atomic<uint32_t> maxretries;
static std::atomic<uint32_t> gWriteWaveTimeout;
static std::mutex gMutex;
typedef std::unique_lock<std::mutex> Glock;

static std::condition_variable fcbcond;
static uint32_t fcbwaiting = 0;
static int64_t freecacheblocks;

// <inode, respective inodedata> and sorted by inode
using InodeDataMap = std::map<inode_t, inodedata *>;
static InodeDataMap inodedataMap;

static uint32_t gWriteWindowSize;
static uint32_t gChunkserverTimeout_ms;

// percentage of the free cache (1% - 100%) which can be used by one inode
static uint32_t gCachePerInodePercentage;

static pthread_t delayed_queue_worker_th;
static std::vector<pthread_t> write_worker_th;

static std::unique_ptr<ProducerConsumerQueue> jobsQueue;
static std::list<DelayedQueueEntry> delayedQueue;

static ConnectionPool gChunkserverConnectionPool;
static ChunkConnectorUsingPool gChunkConnector(gChunkserverConnectionPool);

void write_cb_release_blocks(uint32_t count, Glock &) {
	freecacheblocks += count;
	if (fcbwaiting > 0 && freecacheblocks > 0) { fcbcond.notify_all(); }
}

void write_cb_acquire_blocks(uint32_t count, Glock &) { freecacheblocks -= count; }

void write_cb_wait_for_block(inodedata *id, Glock &glock) {
	LOG_AVG_TILL_END_OF_SCOPE0("write_cb_wait_for_block");
	fcbwaiting++;
	uint64_t dataChainSize = id->dataChain.size();
	while (freecacheblocks <= 0
	       // dataChainSize / (dataChainSize + freecacheblocks) > gCachePerInodePercentage / 100
	       // really means "0 > 0"
	       || dataChainSize * 100 > (dataChainSize + freecacheblocks) * gCachePerInodePercentage) {
		fcbcond.wait(glock);
	}
	fcbwaiting--;
}

/* inode */

inodedata *write_find_inodedata(inode_t inode, Glock &) {
	if (inodedataMap.contains(inode)) { return inodedataMap[inode]; }
	return NULL;
}

inodedata *write_get_inodedata(inode_t inode, Glock &) {
	if (inodedataMap.contains(inode)) { return inodedataMap[inode]; }
	auto id = new inodedata(inode);
	inodedataMap[inode] = id;
	return id;
}

void write_free_inodedata(inodedata *fid, Glock &) {
	inode_t inode = fid->inode;
	if (!inodedataMap.contains(inode)) { return; }

	auto id = inodedataMap[inode];
	sassert(id == fid);
	delete id;
	inodedataMap.erase(inode);
}

/* delayed queue */

static void delayed_queue_put(inodedata *id, uint32_t seconds, Glock &) {
	delayedQueue.push_back(DelayedQueueEntry(id, seconds * DelayedQueueEntry::kTicksPerSecond));
}

static bool delayed_queue_remove(inodedata *id, Glock &) {
	for (auto it = delayedQueue.begin(); it != delayedQueue.end(); ++it) {
		if (it->inodeData == id) {
			delayedQueue.erase(it);
			return true;
		}
	}
	return false;
}

void *delayed_queue_worker(void *) {
	pthread_setname_np(pthread_self(), "delQueueWriter");

	for (;;) {
		Timeout timeout(std::chrono::microseconds(1000000 / DelayedQueueEntry::kTicksPerSecond));
		Glock lock(gMutex);
		auto it = delayedQueue.begin();
		while (it != delayedQueue.end()) {
			if (it->inodeData == NULL) { return NULL; }
			if (--it->ticksLeft <= 0) {
				jobsQueue->put(0, 0, reinterpret_cast<uint8_t *>(it->inodeData), 0);
				it = delayedQueue.erase(it);
			} else {
				++it;
			}
		}

		// Update the write stats
		updateStats(freecacheblocks);

		lock.unlock();
		usleep(timeout.remaining_us());
	}
	return NULL;
}

/* queues */

void write_delayed_enqueue(inodedata *id, uint32_t seconds, Glock &lock) {
	if (seconds > 0) {
		delayed_queue_put(id, seconds, lock);
	} else {
		jobsQueue->put(0, 0, reinterpret_cast<uint8_t *>(id), 0);
	}
}

void write_enqueue(inodedata *id, Glock &) {
	jobsQueue->put(0, 0, reinterpret_cast<uint8_t *>(id), 0);
}

void write_job_delayed_end(inodedata *id, int status, int seconds, Glock &lock) {
	LOG_AVG_TILL_END_OF_SCOPE0("write_job_delayed_end");
	LOG_AVG_TILL_END_OF_SCOPE1("write_job_delayed_end#sec", seconds);
	if (id->locator != nullptr && id->locator->shouldReset()) {
		id->locator.reset();
	}
	if (status != SAUNAFS_STATUS_OK) {
		safs::log_warn("error writing file number {}: {}", id->inode, saunafs_error_string(status));
		id->status = status;
	}
	status = id->status;
	if (id->requiresFlushing() > 0) {
		// Don't sleep if we have to write all the data immediately
		seconds = 0;
	}
	if (!id->dataChain.empty() && status == SAUNAFS_STATUS_OK) {  // still have some work to do
		id->trycnt = 0;                                           // on good write reset try counter
		write_delayed_enqueue(id, seconds, lock);
	} else {  // no more work or error occurred
		// if this is an error then release all data blocks
		write_cb_release_blocks(id->dataChain.size(), lock);
		id->dataChain.clear();
		id->inqueue = false;
		// We don't reset maxfleng (id->maxfleng = 0;) for a while longer, to
		// have its value ready to some quick cache responses in lookup and
		// getattr syscalls
		if (id->flushwaiting > 0) { id->flushcond.notify_all(); }
	}
}

void write_job_end(inodedata *id, int status, Glock &lock) {
	write_job_delayed_end(id, status, 0, lock);
}

class InodeChunkWriter {
public:
	InodeChunkWriter() : inodeData_(nullptr), chunkIndex_(0) {}
	void processJob(inodedata *data);

private:
	void processDataChain(ChunkWriter &writer);
	void returnJournalToDataChain(std::list<WriteCacheBlock> &&journal, Glock &);
	bool haveAnyBlockInCurrentChunk(Glock &);
	bool haveBlockWorthWriting(uint32_t unfinishedOperationCount, Glock &);
	inodedata *inodeData_;
	uint32_t chunkIndex_;
	Timer wholeOperationTimer;

	// Maximum time of writing one chunk
	static const uint32_t kMaximumTime = 30;
	static const uint32_t kMaximumTimeWhenJobsWaiting = 10;
	// For the last 'kTimeToFinishOperations' seconds of maximumTime we won't start new operations
	static const uint32_t kTimeToFinishOperations = 5;
	// Minimum tries counter value before showing write error message on
	// notifications area
	static const uint32_t kMinTryCounterToShowWriteErrorMessage = 4;
};

void InodeChunkWriter::processJob(inodedata *inodeData) {
	LOG_AVG_TILL_END_OF_SCOPE0("InodeChunkWriter::processJob");
	inodeData_ = inodeData;

	// First, choose index of some chunk to write
	Glock lock(gMutex);
	int status = inodeData_->status;
	bool haveDataToWrite;
	if (inodeData_->locator) {
		// There is a chunk lock left by a previous unfinished job -- let's finish it!
		chunkIndex_ = inodeData_->locator->chunkIndex();
		haveDataToWrite = haveAnyBlockInCurrentChunk(lock);
	} else if (!inodeData_->dataChain.empty()) {
		// There is no unfinished job, but there is some data to write -- let's start a new job
		chunkIndex_ = inodeData_->dataChain.front().chunkIndex;
		haveDataToWrite = true;
	} else {
		// No data, no unfinished jobs -- something wrong!
		// This should never happen, so the status doesn't really matter
		safs::log_warn("got inode with no data to write!!!");
		haveDataToWrite = false;
		status = SAUNAFS_ERROR_EINVAL;
	}
	if (status != SAUNAFS_STATUS_OK) {
		write_job_end(inodeData_, status, lock);
		return;
	}
	lock.unlock();

	/*  Process the job */
	ChunkWriter writer(globalChunkserverStats, gChunkConnector, inodeData_->newDataInChainPipe[0]);
	wholeOperationTimer.reset();
	std::unique_ptr<WriteChunkLocator> locator = std::move(inodeData_->locator);
	if (!locator) { locator.reset(new WriteChunkLocator()); }

	try {
		try {
			locator->locateAndLockChunk(inodeData_->inode, chunkIndex_);
			Glock lock(gMutex);
			inodeData_->maxfleng = std::max(inodeData_->maxfleng, locator->fileLength());
			lock.unlock();

			// Optimization -- talk with chunkservers only if we have to write any data.
			// Don't do this if we just have to release some previously unlocked lock.
			if (haveDataToWrite) {
				writer.init(locator.get(), gChunkserverTimeout_ms);
				processDataChain(writer);
				writer.finish(kTimeToFinishOperations * 1000);

				lock.lock();
				returnJournalToDataChain(writer.releaseJournal(), lock);
				lock.unlock();
			}
			locator->unlockChunk();
			read_inode_reconnect_and_clear_cache(inodeData_->inode, chunkIndex_);

			lock.lock();
			inodeData_->minimumBlocksToWrite = writer.getMinimumBlockCountWorthWriting();
			bool canWait = !inodeData_->requiresFlushing();
			if (!haveAnyBlockInCurrentChunk(lock)) {
				// There is no need to wait if we have just finished writing some chunk.
				// Let's immediately start writing the next chunk (if there is any).
				canWait = false;
			}
			if (inodeData_->hasMultipleChunkIdsInChain()) {
				// Don't wait if there is more than one chunk in the data chain -- the first chunk
				// has to be flushed, because no more data will be added to it
				canWait = false;
			}

			if (!locator->shouldReset()) {
				// In the case of a truncate operation that is not finished yet, we don't want to
				// reset the locator, because it will be used later to lock the chunk again
				inodeData_->locator = std::move(locator);
			}

			write_job_delayed_end(inodeData_, SAUNAFS_STATUS_OK, (canWait ? 1 : 0), lock);
		} catch (Exception &e) {
			std::string errorString = e.what();
			addPathByInodeBasedNotificationMessage("Write error: " + std::string(e.what()),
			                                       inodeData_->inode);
			Glock lock(gMutex);
			if (e.status() != SAUNAFS_ERROR_LOCKED) {
				inodeData_->trycnt++;
				errorString += " (try counter: " + std::to_string(inodeData->trycnt) + ")";
			} else if (inodeData_->trycnt == 0) {
				// Set to nonzero to inform writers, that this task needs to wait a bit
				// Don't increase -- SAUNAFS_ERROR_LOCKED means that chunk is locked by a different
				// client and we have to wait until it is unlocked
				inodeData_->trycnt = 1;
			}
			// Keep the lock
			inodeData_->locator = std::move(locator);
			// Move data left in the journal into front of the write cache
			returnJournalToDataChain(writer.releaseJournal(), lock);
			lock.unlock();

			safs::log_warn("write file error, inode: {}, index: {} - {}", inodeData_->inode,
			               chunkIndex_, errorString.c_str());
			if (inodeData_->trycnt >= maxretries) {
				// Convert error to an unrecoverable error
				throw UnrecoverableWriteException(e.message(), e.status());
			} else {
				// This may be recoverable or unrecoverable error
				throw;
			}
		}
	} catch (UnrecoverableWriteException &e) {
		addPathByInodeBasedNotificationMessage("Write error: " + std::string(e.what()),
		                                       inodeData_->inode);
		Glock lock(gMutex);
		if (e.status() == SAUNAFS_ERROR_ENOENT) {
			write_job_end(inodeData_, SAUNAFS_ERROR_EBADF, lock);
		} else if (e.status() == SAUNAFS_ERROR_QUOTA) {
			write_job_end(inodeData_, SAUNAFS_ERROR_QUOTA, lock);
		} else if (e.status() == SAUNAFS_ERROR_NOSPACE ||
		           e.status() == SAUNAFS_ERROR_NOCHUNKSERVERS) {
			write_job_end(inodeData_, SAUNAFS_ERROR_NOSPACE, lock);
		} else {
			write_job_end(inodeData_, SAUNAFS_ERROR_IO, lock);
		}
	} catch (Exception &e) {
		if (inodeData_->trycnt > kMinTryCounterToShowWriteErrorMessage) {
			addPathByInodeBasedNotificationMessage("Write error: " + std::string(e.what()),
			                                       inodeData_->inode);
		}
		Glock lock(gMutex);
		int waitTime = 1;
		if (inodeData_->trycnt > 10) { waitTime = std::min<int>(10, inodeData_->trycnt - 9); }
		write_delayed_enqueue(inodeData_, waitTime, lock);
	}
}

void InodeChunkWriter::processDataChain(ChunkWriter &writer) {
	LOG_AVG_TILL_END_OF_SCOPE0("InodeChunkWriter::processDataChain");
	uint32_t maximumTime = kMaximumTime;
	bool otherJobsAreWaiting = false;
	while (true) {
		bool newOtherJobsAreWaiting = !jobsQueue->isEmpty();
		if (!otherJobsAreWaiting && newOtherJobsAreWaiting) {
			// Some new jobs have just arrived in the queue -- we should finish faster.
			maximumTime = kMaximumTimeWhenJobsWaiting;
			// But we need at least 5 seconds to finish the operations that are in progress
			uint32_t elapsedSeconds = wholeOperationTimer.elapsed_s();
			if (elapsedSeconds + kTimeToFinishOperations >= maximumTime) {
				maximumTime = elapsedSeconds + kTimeToFinishOperations;
			}
		}
		otherJobsAreWaiting = newOtherJobsAreWaiting;

		// If we have sent the previous message and have some time left, we can take
		// another block from current chunk to process it simultaneously. We won't take anything
		// new if we've already sent 'gWriteWindowSize' blocks and didn't receive status from
		// the chunkserver.
		bool can_expect_next_block = true;
		if (wholeOperationTimer.elapsed_s() + kTimeToFinishOperations < maximumTime &&
		    writer.acceptsNewOperations()) {
			Glock lock(gMutex);
			// While there is any block worth sending, we add new write operation
			while (haveBlockWorthWriting(writer.getUnfinishedOperationsCount(), lock)) {
				// Remove block from cache and pass it to the writer
				writer.addOperation(std::move(inodeData_->dataChain.front()));
				inodeData_->popFromChain();
				write_cb_release_blocks(1, lock);
			}
			if (inodeData_->requiresFlushing() && !haveAnyBlockInCurrentChunk(lock)) {
				// No more data and some flushing is needed or required, so flush everything
				writer.startFlushMode();
			}
			if (writer.getUnfinishedOperationsCount() < gWriteWindowSize) {
				inodeData_->workerWaitingForData = true;
			}
			can_expect_next_block = haveAnyBlockInCurrentChunk(lock) ||
			                        inodeData_->wasLastBlockRecentlyWrittenAndTaken();
		} else if (writer.acceptsNewOperations()) {
			// We are running out of time...
			Glock lock(gMutex);
			if (!inodeData_->requiresFlushing()) {
				// Nobody is waiting for the data to be flushed and the data in write chain
				// isn't too old. Let's postpone any operations
				// that didn't start yet and finish them in the next time slice for this chunk
				writer.dropNewOperations();
			} else {
				// Somebody if waiting for a flush, so we have to finish writing everything.
				writer.startFlushMode();
			}
			can_expect_next_block = haveAnyBlockInCurrentChunk(lock) ||
			                        inodeData_->wasLastBlockRecentlyWrittenAndTaken();
		}

		Glock lock(gMutex);
		writer.setChunkSizeInBlocks(
		    std::min(inodeData_->maxfleng - chunkIndex_ * SFSCHUNKSIZE, (uint64_t)SFSCHUNKSIZE));
		lock.unlock();
		if (writer.startNewOperations(can_expect_next_block) > 0) {
			lock.lock();
			inodeData_->lastWriteToChunkservers.reset();
			lock.unlock();
		}
		if (writer.getPendingOperationsCount() == 0) {
			return;
		} else if (wholeOperationTimer.elapsed_s() >= maximumTime) {
			throw RecoverableWriteException(
			    "Timeout after " + std::to_string(wholeOperationTimer.elapsed_ms()) + " ms",
			    SAUNAFS_ERROR_TIMEOUT);
		}

		writer.processOperations(gWriteWaveTimeout);
	}
}

void InodeChunkWriter::returnJournalToDataChain(std::list<WriteCacheBlock> &&journal, Glock &lock) {
	if (!journal.empty()) {
		write_cb_acquire_blocks(journal.size(), lock);
		uint64_t prev_id = journal.front().chunkIndex;
		int alterations = (!inodeData_->dataChain.empty() &&
		                   journal.back().chunkIndex != inodeData_->dataChain.front().chunkIndex)
		                      ? 1
		                      : 0;
		for (auto it = std::next(journal.begin()); it != journal.end(); ++it) {
			if (it->chunkIndex != prev_id) {
				alterations++;
				prev_id = it->chunkIndex;
			}
		}
		inodeData_->dataChain.splice(inodeData_->dataChain.begin(), std::move(journal));
		inodeData_->registerAlterationsInChain(alterations);
	}
}

/*
 * Check if there is any data in the same chunk waiting to be written.
 */
bool InodeChunkWriter::haveAnyBlockInCurrentChunk(Glock &) {
	if (inodeData_->dataChain.empty()) {
		return false;
	} else {
		return inodeData_->dataChain.front().chunkIndex == chunkIndex_;
	}
}

/*
 * Check if there is any data worth sending to the chunkserver.
 * We will avoid sending blocks of size different than SFSBLOCKSIZE.
 * These can be taken only if we are close to run out of tasks to do.
 * glock: LOCKED
 */
bool InodeChunkWriter::haveBlockWorthWriting(uint32_t unfinishedOperationCount, Glock &lock) {
	if (!haveAnyBlockInCurrentChunk(lock)) { return false; }
	const auto &block = inodeData_->dataChain.front();
	if (block.type != WriteCacheBlock::kWritableBlock) {
		// Always write data, that was previously written
		return true;
	} else if (unfinishedOperationCount >= gWriteWindowSize) {
		// Don't start new operations if there is already a lot of pending writes
		return false;
	} else {
		// Always start full blocks; start partial blocks only if we have to flush the data
		// or the block won't be expanded (only the last one can be) to a full block
		return (block.size() == SFSBLOCKSIZE || inodeData_->requiresFlushing() ||
		        inodeData_->dataChain.size() > 1);
	}
}

/* main working thread | glock:UNLOCKED */
void *write_worker(void *) {
	InodeChunkWriter inodeDataWriter;

	static std::atomic_uint16_t writeWorkersCounter(0);
	std::string threadName = "writeWorker " + std::to_string(writeWorkersCounter++);
	pthread_setname_np(pthread_self(), threadName.c_str());

	for (;;) {
		// get next job
		uint32_t z1, z2, z3;
		uint8_t *data;
		{
			LOG_AVG_TILL_END_OF_SCOPE0("write_worker#idle");
			jobsQueue->get(&z1, &z2, &data, &z3);
		}
		if (data == NULL) { return NULL; }

		// process the job
		LOG_AVG_TILL_END_OF_SCOPE0("write_worker#working");
		Timer writeTimer;
		inodeDataWriter.processJob((inodedata *)data);
		statsAdd(WriteStats::ACTIVE_WORKERS_TIME_ms, writeTimer.elapsed_ms());
	}
	return NULL;
}

/* API | glock: INITIALIZED,UNLOCKED */
void write_data_init(uint32_t cachesize, uint32_t retries, uint32_t workers,
                     uint32_t writewindowsize, uint32_t chunkserverTimeout_ms,
                     uint32_t cachePerInodePercentage, uint32_t waveTimeout) {
	uint64_t cachebytecount = uint64_t(cachesize) * 1024 * 1024;
	uint64_t cacheblockcount = (cachebytecount / SFSBLOCKSIZE);
	pthread_attr_t thattr;

	gChunkConnector.setSourceIp(fs_getsrcip());
	gWriteWindowSize = writewindowsize;
	gChunkserverTimeout_ms = chunkserverTimeout_ms;
	maxretries = retries;
	gWriteWaveTimeout = waveTimeout;
	if (cacheblockcount < 10) { cacheblockcount = 10; }

	freecacheblocks = cacheblockcount;
	gCachePerInodePercentage = cachePerInodePercentage;

	writeStatsInit();

	jobsQueue = std::make_unique<ProducerConsumerQueue>(0, deleterByType<inodedata>);

	pthread_attr_init(&thattr);
	pthread_attr_setstacksize(&thattr, 0x100000);
	pthread_create(&delayed_queue_worker_th, &thattr, delayed_queue_worker, NULL);
	write_worker_th.resize(workers);
	for (auto &th : write_worker_th) { pthread_create(&th, &thattr, write_worker, NULL); }
	pthread_attr_destroy(&thattr);

	std::lock_guard lock(gMountInfoMtx);
	gTweaks.registerVariable("WriteMaxRetries", maxretries, "sfsioretries (write)");
	gTweaks.registerVariable("WriteWaveTimeout", gWriteWaveTimeout, "sfschunkserverwavewriteto");

	gIsInitialized = true;
}

void write_data_term(void) {
	uint32_t i;

	{
		Glock lock(gMutex);
		delayed_queue_put(nullptr, 0, lock);
	}
	for (i = 0; i < write_worker_th.size(); i++) { jobsQueue->put(0, 0, NULL, 0); }
	for (i = 0; i < write_worker_th.size(); i++) { pthread_join(write_worker_th[i], NULL); }
	pthread_join(delayed_queue_worker_th, NULL);
	jobsQueue.reset();
	for (const auto &[_, id] : inodedataMap) { delete id; }
	inodedataMap.clear();

	gIsInitialized = false;
}

/* glock: UNLOCKED */
int write_block(inodedata *id, uint32_t chindx, uint16_t pos, uint32_t from, uint32_t to,
                const uint8_t *data) {
	Glock lock(gMutex);
	id->lastWriteToDataChain.reset();

	// Try to expand the last block
	if (!id->dataChain.empty()) {
		auto &lastBlock = id->dataChain.back();
		if (lastBlock.chunkIndex == chindx && lastBlock.blockIndex == pos &&
		    lastBlock.type == WriteCacheBlock::kWritableBlock && lastBlock.expand(from, to, data)) {
			id->wakeUpWorkerIfNecessary();
			return 0;
		}
	}

	// Didn't manage to expand an existing block, so allocate a new one
	write_cb_wait_for_block(id, lock);
	write_cb_acquire_blocks(1, lock);
	id->pushToChain(WriteCacheBlock(chindx, pos, WriteCacheBlock::kWritableBlock));
	sassert(id->dataChain.back().expand(from, to, data));
	if (id->inqueue) {
		// Consider some speedup if there are no errors and:
		// - there is a lot of blocks in the write chain
		// - there are at least two chunks in the write chain
		if (id->trycnt == 0 &&
		    (id->dataChain.size() > id->minimumBlocksToWrite ||
		     id->dataChain.front().chunkIndex != id->dataChain.back().chunkIndex)) {
			if (delayed_queue_remove(id, lock)) { write_enqueue(id, lock); }
		}
		id->wakeUpWorkerIfNecessary();
	} else {
		id->inqueue = true;
		write_enqueue(id, lock);
	}
	return 0;
}

/* glock: UNLOCKED */
int write_blocks(inodedata *id, uint64_t offset, uint32_t size, const uint8_t *data) {
	LOG_AVG_TILL_END_OF_SCOPE0("write_blocks");
	uint32_t chindx = offset >> SFSCHUNKBITS;
	uint16_t pos = (offset & SFSCHUNKMASK) >> SFSBLOCKBITS;
	uint32_t from = offset & SFSBLOCKMASK;
	while (size > 0) {
		if (size > SFSBLOCKSIZE - from) {
			if (write_block(id, chindx, pos, from, SFSBLOCKSIZE, data) < 0) {
				return SAUNAFS_ERROR_IO;
			}

			gCachedBytes += (SFSBLOCKSIZE - from);
			size -= (SFSBLOCKSIZE - from);
			data += (SFSBLOCKSIZE - from);
			from = 0;
			pos++;
			if (pos == SFSBLOCKSINCHUNK) {
				pos = 0;
				chindx++;
			}
		} else {
			if (write_block(id, chindx, pos, from, from + size, data) < 0) {
				return SAUNAFS_ERROR_IO;
			}

			gCachedBytes += size;
			size = 0;
		}
	}
	return 0;
}

int write_data(void *vid, uint64_t offset, uint32_t size, const uint8_t *data, size_t currentSize) {
	LOG_AVG_TILL_END_OF_SCOPE0("write_data");
	int status;
	inodedata *id = (inodedata *)vid;
	if (id == NULL) { return SAUNAFS_ERROR_IO; }

	Glock lock(gMutex);
	status = id->status;
	id->maxfleng = std::max(id->maxfleng, currentSize);
	if (status == SAUNAFS_STATUS_OK) {
		if (offset + size > id->maxfleng) {  // move fleng
			id->maxfleng = offset + size;
		}
		id->writewaiting++;
		while (id->flushwaiting > 0) { id->writecond.wait(lock); }
		id->writewaiting--;
	}
	lock.unlock();

	if (status != SAUNAFS_STATUS_OK) { return status; }

	return write_blocks(id, offset, size, data);
}

static void write_data_flushwaiting_increase(inodedata *id, Glock &) { id->flushwaiting++; }

static void write_data_flushwaiting_decrease(inodedata *id, Glock &) {
	id->flushwaiting--;
	if (id->flushwaiting == 0 && id->writewaiting > 0) { id->writecond.notify_all(); }
}

static void write_data_lcnt_increase(inodedata *id, Glock &) { id->lcnt++; }

static void write_data_lcnt_decrease_check_deleted(inodedata *id, Glock &lock, bool &isDeleted) {
	// As long as it is not freed, then we don't consider the inodedata deleted
	isDeleted = false;
	id->lcnt--;
	if (id->lcnt == 0 && !id->inqueue && id->flushwaiting == 0 && id->writewaiting == 0) {
		write_free_inodedata(id, lock);
		isDeleted = true;
	}
}

inline void write_data_lcnt_decrease(inodedata *id, Glock &lock) {
	bool dummy_isDeleted;
	write_data_lcnt_decrease_check_deleted(id, lock, dummy_isDeleted);
	(void)dummy_isDeleted;
}

void *write_data_new(inode_t inode) {
	inodedata *id;
	Glock lock(gMutex);
	id = write_get_inodedata(inode, lock);
	if (id == NULL) { return NULL; }
	write_data_lcnt_increase(id, lock);
	return id;
}

static int write_data_flush(void *vid, Glock &lock) {
	Timer flushTimer;
	inodedata *id = (inodedata *)vid;
	if (id == NULL) { return SAUNAFS_ERROR_IO; }

	write_data_flushwaiting_increase(id, lock);
	// If there are no errors (trycnt==0) and inode is waiting in the delayed queue, speed it up
	if (id->trycnt == 0 && delayed_queue_remove(id, lock)) { write_enqueue(id, lock); }
	// Wait for the data to be flushed
	while (id->inqueue) { id->flushcond.wait(lock); }
	write_data_flushwaiting_decrease(id, lock);
	statsAdd(WriteStats::TOTAL_FLUSH_TIME_ms, flushTimer.elapsed_ms());
	return id->status;
}

int write_data_flush(void *vid) {
	Glock lock(gMutex);
	return write_data_flush(vid, lock);
}

uint64_t write_data_getmaxfleng(inode_t inode) {
	uint64_t maxfleng;
	inodedata *id;
	Glock lock(gMutex);
	id = write_find_inodedata(inode, lock);
	if (id) {
		maxfleng = id->maxfleng;
	} else {
		maxfleng = 0;
	}
	return maxfleng;
}

int write_data_flush_inode(inode_t inode) {
	Glock lock(gMutex);
	inodedata *id = write_find_inodedata(inode, lock);
	if (id == NULL) { return 0; }
	return write_data_flush(id, lock);
}

int write_data_truncate(inode_t inode, bool opened, uint32_t uid, uint32_t gid, uint64_t length,
                        Attributes &attr) {
	Glock lock(gMutex);

	// 1. Flush writes but don't finish it completely - it'll be done at the end of truncate
	inodedata *id = write_get_inodedata(inode, lock);
	if (id == NULL) { return SAUNAFS_ERROR_IO; }
	write_data_lcnt_increase(id, lock);
	write_data_flushwaiting_increase(id, lock);  // this will block any writing to this inode

	int err = write_data_flush(id, lock);
	if (err != 0) {
		write_data_flushwaiting_decrease(id, lock);
		write_data_lcnt_decrease(id, lock);
		return err;
	}

	// 2. Send the request to master
	uint8_t status;
	bool writeNeeded;
	uint64_t oldLength;
	uint32_t lockId;
	lock.unlock();
	int retrySleepTime_us = 200000;
	uint32_t retries = 0;
	do {
		status = fs_truncate(inode, opened, uid, gid, length, writeNeeded, attr, oldLength, lockId);
		if (status != SAUNAFS_STATUS_OK) {
			safs::log_info("truncate file {} to length {}: {} (try {}/{})", inode, length,
			               saunafs_error_string(status), int(retries + 1), int(maxretries));
		}
		if (retries >= maxretries) { break; }
		if (status == SAUNAFS_ERROR_LOCKED) {
			sleep(1);
		} else if (status == SAUNAFS_ERROR_CHUNKLOST || status == SAUNAFS_ERROR_NOTDONE) {
			usleep(retrySleepTime_us);
			retrySleepTime_us = std::min(2 * retrySleepTime_us, 60 * 1000000);
			++retries;
		}
	} while (status == SAUNAFS_ERROR_LOCKED || status == SAUNAFS_ERROR_CHUNKLOST ||
	         status == SAUNAFS_ERROR_NOTDONE);
	lock.lock();
	if (status != 0 || !writeNeeded) {
		// Something failed or we have nothing to do more (master server managed to do the truncate)
		bool isDeleted = false;
		write_data_flushwaiting_decrease(id, lock);
		write_data_lcnt_decrease_check_deleted(id, lock, isDeleted);
		if (status == SAUNAFS_STATUS_OK) {
			if (!isDeleted) { id->maxfleng = length; }
			return 0;
		} else {
			return status;
		}
	}

	// We have to write zeros in suitable region to update xor/ec parity parts.
	// Let's calculate size of the region to be zeroed
	uint64_t endOffset = std::min({
	    oldLength,  // no further than to the end of the file
	    length + slice_traits::ec::kMaxDataCount * SFSBLOCKSIZE,  // no more than the maximal stripe
	    (length + SFSCHUNKSIZE - 1) / SFSCHUNKSIZE * SFSCHUNKSIZE  // no beyond the end of chunk
	});

	if (endOffset > length) {
		// Make maxfleng big enough for the upcoming writes
		id->maxfleng = endOffset;

		// Something has to be written, so pass our lock to writing threads
		sassert(id->dataChain.empty());
		id->locator.reset(
		    new TruncateWriteChunkLocator(inode, length / SFSCHUNKSIZE, lockId, endOffset));

		// And now pass block of zeros to writing threads
		std::vector<uint8_t> zeros(endOffset - length, 0);
		lock.unlock();
		err = write_blocks(id, length, zeros.size(), zeros.data());
		lock.lock();
		if (err != 0) {
			write_data_flushwaiting_decrease(id, lock);
			write_data_lcnt_decrease(id, lock);
			return err;
		}

		// Wait for writing threads to finish
		err = write_data_flush(id, lock);
		id->locator.reset();
		if (err != 0) {
			// unlock the chunk here?
			write_data_flushwaiting_decrease(id, lock);
			write_data_lcnt_decrease(id, lock);
			return err;
		}
	}

	id->maxfleng = length;
	// Now we can tell the master server to finish the truncate operation and
	// then unblock the inode
	lock.unlock();
	status = fs_truncateend(inode, uid, gid, length, lockId, attr);
	write_data_flushwaiting_decrease(id, lock);
	write_data_lcnt_decrease(id, lock);

	if (status != SAUNAFS_STATUS_OK) {
		safs::log_warn("truncateend on file {} to length {}: {}", inode, length,
		               saunafs_error_string(status));
		return status;
	}
	return 0;
}

int write_data_end(void *vid) {
	Glock lock(gMutex);
	inodedata *id = (inodedata *)vid;
	if (id == NULL) { return SAUNAFS_ERROR_IO; }
	int status = write_data_flush(id, lock);
	write_data_lcnt_decrease(id, lock);
	return status;
}

}  // namespace InodeBasedWriteAlgorithm

namespace ChunkBasedWriteAlgorithm {

struct ChunkData;

using Lock = std::unique_lock<std::mutex>;
using ChunkDataPtr = std::unique_ptr<ChunkData>;

struct inodedata {
	inode_t inode;
	uint64_t maxfleng = 0;           // inodeLock
	int status = SAUNAFS_STATUS_OK;  // inodeLock
	uint16_t flushwaiting = 0;       // inodeLock
	uint16_t writewaiting = 0;       // inodeLock
	std::atomic<uint16_t> lcnt = 0;
	std::condition_variable flushcond;  // wait for !inqueue (flush): using globalLock
	std::condition_variable writecond;  // wait for flushwaiting==0 (write): using inodeLock
	std::list<ChunkDataPtr> chunkDataList;
	std::atomic_bool emptyChunkDataList = true;
	std::atomic<uint64_t> totalCachedBlocks = 0;
	std::mutex mutex;
	Timer lastWriteToDataChain;     // inodeLock
	Timer lastWriteToChunkservers;  // inodeLock

	inodedata(inode_t inode) : inode(inode) {}

	/*! Check if inode requires flushing all its data chain to chunkservers.
	 *
	 * Returns true if anyone requested to flush the data by calling write_data_flush
	 * or write_data_flush_inode or the data in data chain is too old to keep it longer in
	 * our buffers. If this function returns false, we write only full stripes from data
	 * chain to chunkservers.
	 * inodeLock: LOCKED
	 */
	bool requiresFlushing() {
		return (flushwaiting > 0 ||
		        lastWriteToDataChain.elapsed_ms() >= kMaximumTimeInDataChainSinceLastWrite_ms ||
		        lastWriteToChunkservers.elapsed_ms() >= kMaximumTimeInDataChainSinceLastFlush_ms);
	}

	bool wasLastBlockRecentlyWritten() const {
		return lastWriteToDataChain.elapsed_ms() < kVeryRecentWrite_ms;
	}

private:
	/*! Limit for \p lastWriteToChunkservers after which we force a flush.
	 *
	 * Maximum time for data to be kept in data chain waiting for collecting a full stripe.
	 */
	static const uint32_t kMaximumTimeInDataChainSinceLastFlush_ms = 15000;

	/*! Limit for \p lastWriteToDataChain after which we force a flush.
	 *
	 * Maximum time for data to be kept in data chain waiting for collecting a full stripe
	 * if no new data is written into the data chain
	 */
	static const uint32_t kMaximumTimeInDataChainSinceLastWrite_ms = 5000;

	/*! Limit for considering the last write to data chain very recent.
	 *
	 * This is used to avoid writing a partial stripe to chunkservers when last block was full and
	 * next one has not even started.
	 */
	static const uint32_t kVeryRecentWrite_ms = 5;
};

using InodeDataPtr = std::unique_ptr<inodedata>;

struct ChunkData {
	uint32_t chunkIndex;
	uint16_t tryCounter = 0;
	int newDataInChainPipe[2];
	uint32_t minimumBlocksToWrite = 1;
	std::list<WriteCacheBlock> dataChain;
	bool inQueue =
	    false;  // true if this chunk is waiting in one of the queues or is being processed
	bool workerWaitingForData = false;
	std::unique_ptr<WriteChunkLocator> locator;
	std::atomic<int> writesCount = 0;

	ChunkData(uint32_t chunkIndex, inodedata *parent) : chunkIndex(chunkIndex), parent_(parent) {
#ifdef _WIN32
		// We don't use inodeData->waitingworker and inodeData->pipe on Cygwin because
		// Cygwin's implementation of mixed socket & pipe polling is very inefficient.
		// On mingw platform pipes are unavailable.
		newDataInChainPipe[0] = newDataInChainPipe[1] = -1;
#else
		if (pipe(newDataInChainPipe) < 0) {
			safs::log_warn("creating pipe error: {}", strerr(errno));
			newDataInChainPipe[0] = -1;
		}
#endif
	}

	~ChunkData() {
		if (isDataChainPipeValid()) {
			close(newDataInChainPipe[0]);
			close(newDataInChainPipe[1]);
		}
	}

	/* inodeLock: UNUSED */
	bool isDataChainPipeValid() const { return newDataInChainPipe[0] >= 0; }

	/* inodeLock: LOCKED */
	void wakeUpWorkerIfNecessary() {
		/*
		 * Write worker always looks for the first block in chain and we modify or add always the
		 * last block in chain so it is necessary to wake up write worker only if the first block
		 * is the last one, ie. dataChain.size() == 1.
		 */
		if (workerWaitingForData && dataChain.size() == 1 && isDataChainPipeValid()) {
			if (write(newDataInChainPipe[1], " ", 1) != 1) {
				safs::log_err("write pipe error: {}", strerr(errno));
			}
			workerWaitingForData = false;
		}
	}

	/* inodeLock: LOCKED */
	void pushToChain(WriteCacheBlock &&block) {
		dataChain.emplace_back(std::move(block));
		parent_->totalCachedBlocks++;
	}

	/* inodeLock: LOCKED */
	void popFromChain() {
		assert(dataChain.size() > 0);
		dataChain.pop_front();
		parent_->totalCachedBlocks--;
	}

	/* inodeLock: LOCKED */
	void updateParentStatus(int8_t status) { parent_->status = status; }

	/* inodeLock: LOCKED */
	int8_t getParentStatus() { return parent_->status; }

	/* inodeLock: LOCKED */
	bool requiresFlushing() const { return parent_->requiresFlushing(); }

	/* inodeLock: LOCKED */
	void clear() {
		parent_->totalCachedBlocks -= dataChain.size();
		dataChain.clear();
	}

	/* inodeLock: UNUSED */
	inline inodedata *getParent() const { return parent_; }

private:
	inodedata *parent_;  // contains inode and other useful data
};

struct DelayedQueueEntry {
	ChunkData *chunkData;
	int32_t ticksLeft;
	static constexpr int kTicksPerSecond = 10;

	DelayedQueueEntry(ChunkData *chunkData, int32_t ticksLeft)
	    : chunkData(chunkData), ticksLeft(ticksLeft) {}
};

static bool gIsInitialized = false;

static std::atomic<uint32_t> maxretries;
static std::atomic<uint32_t> gWriteWaveTimeout;
static std::mutex gMutex;

static std::mutex fcbcondMutex;
static std::condition_variable fcbcond;
static std::atomic<uint32_t> fcbwaiting = 0;
static std::atomic<int64_t> freecacheblocks;

// <inode, respective inodedata> and sorted by inode
using InodeDataMap = std::map<inode_t, inodedata *>;
static InodeDataMap inodedataMap;

// <inode, <lockid, endoffset>> and sorted by inode
using TruncateLocatorsDataMap = std::map<inode_t, std::pair<uint64_t, uint64_t>>;
static std::mutex truncateLocatorsDataMutex;
static TruncateLocatorsDataMap truncateLocatorsData;
static uint32_t gWriteWindowSize;
static uint32_t gChunkserverTimeout_ms;

// percentage of the free cache (1% - 100%) which can be used by one inode
static uint32_t gCachePerInodePercentage;

static pthread_t delayed_queue_worker_th;
static std::vector<pthread_t> write_worker_th;

static std::unique_ptr<ProducerConsumerQueue> jobsQueue;
static std::list<DelayedQueueEntry> delayedQueue;

static ConnectionPool gChunkserverConnectionPool;
static ChunkConnectorUsingPool gChunkConnector(gChunkserverConnectionPool);

/* globalLock: UNLOCKED*/
void write_cb_release_blocks(uint32_t count) {
	freecacheblocks += count;
	if (fcbwaiting > 0 && freecacheblocks > 0) {
		Lock fcbcondLock(fcbcondMutex);
		if (count == 1) {
			fcbcond.notify_one();
		} else {
			fcbcond.notify_all();
		}
	}
}

/* globalLock: UNLOCKED*/
void write_cb_acquire_blocks(uint32_t count) { freecacheblocks -= count; }

/* globalLock: UNLOCKED*/
void write_cb_wait_for_block(inodedata *id) {
	LOG_AVG_TILL_END_OF_SCOPE0("write_cb_wait_for_block");
	fcbwaiting++;
	uint64_t totalCachedBlocks = id->totalCachedBlocks;
	Lock fcbcondLock(fcbcondMutex);
	while (freecacheblocks <= 0
	       // totalCachedBlocks / (totalCachedBlocks + freecacheblocks) >
	       // gCachePerInodePercentage / 100 really means "0 > 0"
	       || totalCachedBlocks * 100 >
	              (totalCachedBlocks + freecacheblocks) * gCachePerInodePercentage) {
		fcbcond.wait(fcbcondLock);
	}
	fcbwaiting--;
}

/* inode */

inodedata *write_find_inodedata(inode_t inode, Lock &) {
	if (inodedataMap.contains(inode)) { return inodedataMap[inode]; }
	return NO_INODEDATA;
}

inodedata *write_get_inodedata(inode_t inode, Lock &) {
	if (inodedataMap.contains(inode)) { return inodedataMap[inode]; }
	auto id = new inodedata(inode);
	inodedataMap[inode] = id;
	return id;
}

void write_free_inodedata(inodedata *fid, Lock &) {
	inode_t inode = fid->inode;
	if (!inodedataMap.contains(inode)) { return; }

	auto id = inodedataMap[inode];
	sassert(id == fid);
	delete id;
	inodedataMap.erase(inode);
}

/* chunk */

/* inodeLock: LOCKED*/
ChunkData *write_get_chunkdata(uint32_t chunkIndex, inodedata *id) {
	auto chunkDataListIt =
	    std::find_if(id->chunkDataList.begin(), id->chunkDataList.end(),
	                 [&](ChunkDataPtr &chunkData) { return chunkData->chunkIndex == chunkIndex; });
	if (chunkDataListIt != id->chunkDataList.end()) { return chunkDataListIt->get(); }
	id->chunkDataList.emplace_front(new ChunkData(chunkIndex, id));
	auto &chunkData = id->chunkDataList.front();
	id->emptyChunkDataList = false;

	Lock truncateLocatorsDataLock(truncateLocatorsDataMutex);
	auto it = truncateLocatorsData.find(id->inode);
	if (it != truncateLocatorsData.end()) {
		auto [lockid, endoffset] = it->second;
		chunkData->locator =
		    std::make_unique<TruncateWriteChunkLocator>(id->inode, chunkIndex, lockid, endoffset);
	}
	truncateLocatorsDataLock.unlock();
	return chunkData.get();
}

/* inodeLock: LOCKED*/
void write_free_chunkdata(ChunkData *fChunkData, Lock &) {
	inodedata *id = fChunkData->getParent();
	sassert(!id->chunkDataList.empty());

	auto chunkDataListIt =
	    std::find_if(id->chunkDataList.begin(), id->chunkDataList.end(),
	                 [&](ChunkDataPtr &chunkData) { return chunkData.get() == fChunkData; });
	if (chunkDataListIt != id->chunkDataList.end()) {
		id->chunkDataList.erase(chunkDataListIt);
		id->emptyChunkDataList = id->chunkDataList.empty();
	}
}

/* delayed queue */

/* globalLock: LOCKED*/
static void delayed_queue_put(ChunkData *chunkData, uint32_t seconds, Lock &) {
	delayedQueue.emplace_back(
	    DelayedQueueEntry(chunkData, seconds * DelayedQueueEntry::kTicksPerSecond));
}

/* globalLock: LOCKED*/
static bool delayed_queue_remove(ChunkData *chunkData, Lock &) {
	for (auto it = delayedQueue.begin(); it != delayedQueue.end(); ++it) {
		if (it->chunkData == chunkData) {
			delayedQueue.erase(it);
			return true;
		}
	}
	return false;
}

/* globalLock: LOCKED*/
static std::vector<ChunkData *> delayed_queue_remove(inodedata *id, Lock &) {
	std::vector<ChunkData *> removedChunkData;

	auto it = std::remove_if(delayedQueue.begin(), delayedQueue.end(),
	                         [&](const DelayedQueueEntry &entry) {
		                         if (entry.chunkData->getParent() == id) {
			                         removedChunkData.push_back(entry.chunkData);
			                         return true;
		                         }
		                         return false;
	                         });

	delayedQueue.erase(it, delayedQueue.end());
	return removedChunkData;
}

void *delayed_queue_worker(void *) {
	pthread_setname_np(pthread_self(), "delQueueWriter");

	for (;;) {
		Timeout timeout(std::chrono::microseconds(1000000 / DelayedQueueEntry::kTicksPerSecond));
		Lock globalLock(gMutex);
		auto it = delayedQueue.begin();
		while (it != delayedQueue.end()) {
			if (it->chunkData == NO_CHUNKDATA) { return NULL; }
			if (--it->ticksLeft <= 0) {
				auto *data = reinterpret_cast<uint8_t *>(it->chunkData);
				jobsQueue->put(0, 0, data, 0);
				it = delayedQueue.erase(it);
			} else {
				++it;
			}
		}
		globalLock.unlock();

		// Update the write stats
		updateStats(freecacheblocks);

		usleep(timeout.remaining_us());
	}
	return NULL;
}

/* queues */

/* globalLock: LOCKED*/
void write_delayed_enqueue(ChunkData *chunkData, uint32_t seconds, Lock &globalLock) {
	if (seconds > 0) {
		delayed_queue_put(chunkData, seconds, globalLock);
	} else {
		jobsQueue->put(0, 0, reinterpret_cast<uint8_t *>(chunkData), 0);
	}
}

void write_enqueue(ChunkData *chunkData, Lock &) {
	jobsQueue->put(0, 0, reinterpret_cast<uint8_t *>(chunkData), 0);
}

/* globalLock: LOCKED*/
void write_enqueue(std::vector<ChunkData *> &chunks, Lock &) {
	for (auto chunk : chunks) { jobsQueue->put(0, 0, reinterpret_cast<uint8_t *>(chunk), 0); }
}

/* globalLock: LOCKED*/
void write_job_delayed_end(ChunkData *chunkData, int status, int seconds, Lock &globalLock) {
	LOG_AVG_TILL_END_OF_SCOPE0("write_job_delayed_end");
	LOG_AVG_TILL_END_OF_SCOPE1("write_job_delayed_end#sec", seconds);
	inodedata *parent = chunkData->getParent();
	globalLock.unlock();

	Lock inodeLock(parent->mutex);
	if (chunkData->locator != nullptr && chunkData->locator->shouldReset()) {
		chunkData->locator.reset();
	}

	if (status != SAUNAFS_STATUS_OK) {
		safs::log_warn("error writing file number {}: {}", parent->inode,
		               saunafs_error_string(status));
		chunkData->updateParentStatus(status);
	}
	status = chunkData->getParentStatus();
	if (chunkData->requiresFlushing() > 0) {
		// Don't sleep if we have to write all the data immediately
		seconds = 0;
	}
	if (chunkData->writesCount > 0 && status == SAUNAFS_STATUS_OK) {
		inodeLock.unlock();

		globalLock.lock();
		write_enqueue(chunkData, globalLock);
	} else if (!chunkData->dataChain.empty() &&
	           status == SAUNAFS_STATUS_OK) {  // still have some work to do
		chunkData->tryCounter = 0;             // on good write reset try counter
		inodeLock.unlock();

		globalLock.lock();
		write_delayed_enqueue(chunkData, seconds, globalLock);
	} else {  // no more work or error occurred
		bool somebodyIsWriting = chunkData->writesCount > 0;
		// if this is an error then release all data blocks
		write_cb_release_blocks(chunkData->dataChain.size());

		chunkData->clear();
		if (!somebodyIsWriting) {
			// We don't reset maxfleng (id->maxfleng = 0;) for a while longer, to
			// have its value ready to some quick cache responses in lookup and
			// getattr syscalls
			write_free_chunkdata(chunkData, inodeLock);
		}
		inodeLock.unlock();

		globalLock.lock();
		if (parent->emptyChunkDataList || status != SAUNAFS_STATUS_OK) {
			parent->flushcond.notify_all();
		} else {// parent->emptyChunkDataList = false and status == SAUNAFS_STATUS_OK
			if (somebodyIsWriting) {
				write_enqueue(chunkData, globalLock);
			}
		}
	}
}

/* globalLock: LOCKED*/
void write_job_end(ChunkData *chunkData, int status, Lock &globalLock) {
	write_job_delayed_end(chunkData, status, 0, globalLock);
}

class ChunkJobWriter {
public:
	void processJob(ChunkData *chunkData);

private:
	void processDataChain(ChunkWriter &writer);

	/*
	 * Returns pending operations to datachain.
	 * inodeLock: LOCKED
	 */
	void returnJournalToDataChain(std::list<WriteCacheBlock> &&journal, Lock &);

	/*
	 * Check if there is any data in the same chunk waiting to be written.
	 * inodeLock: LOCKED
	 */
	bool haveAnyBlockInCurrentChunk(Lock &);

	/*
	 * Check if there is any data worth sending to the chunkserver.
	 * We will avoid sending blocks of size different than SFSBLOCKSIZE.
	 * These can be taken only if we are close to run out of tasks to do.
	 * inodeLock: LOCKED
	 */
	bool haveBlockWorthWriting(uint32_t unfinishedOperationCount, Lock &);
	ChunkData *chunkData_ = NO_CHUNKDATA;
	uint32_t chunkIndex_ = 0;
	Timer wholeOperationTimer;

	// Maximum time of writing one chunk
	static const uint32_t kMaximumTime = 30;
	static const uint32_t kMaximumTimeWhenJobsWaiting = 10;
	// For the last 'kTimeToFinishOperations' seconds of maximumTime we won't start new operations
	static const uint32_t kTimeToFinishOperations = 5;
	// Minimum tries counter value before showing write error message on
	// notifications area
	static const uint32_t kMinTryCounterToShowWriteErrorMessage = 4;
};

void ChunkJobWriter::processJob(ChunkData *chunkData) {
	LOG_AVG_TILL_END_OF_SCOPE0("ChunkJobWriter::processJob");
	chunkData_ = chunkData;
	chunkIndex_ = chunkData_->chunkIndex;
	inodedata *parent = chunkData_->getParent();

	/*  Process the job */
	ChunkWriter writer(globalChunkserverStats, gChunkConnector, chunkData_->newDataInChainPipe[0]);
	wholeOperationTimer.reset();
	std::unique_ptr<WriteChunkLocator> locator = std::move(chunkData_->locator);
	if (!locator) { locator.reset(new WriteChunkLocator()); }

	try {
		try {
			locator->locateAndLockChunk(parent->inode, chunkIndex_);
			Lock inodeLock(parent->mutex);
			parent->maxfleng = std::max(parent->maxfleng, locator->fileLength());

			// Optimization -- talk with chunkservers only if we have to write any data.
			// Don't do this if we just have to release some previously unlocked lock.
			if (haveAnyBlockInCurrentChunk(inodeLock)) {
				inodeLock.unlock();
				writer.init(locator.get(), gChunkserverTimeout_ms);
				processDataChain(writer);
				writer.finish(kTimeToFinishOperations * 1000);

				inodeLock.lock();
				returnJournalToDataChain(writer.releaseJournal(), inodeLock);
			}
			inodeLock.unlock();

			locator->unlockChunk();
			read_inode_reconnect_and_clear_cache(parent->inode, chunkIndex_);

			inodeLock.lock();
			chunkData_->minimumBlocksToWrite = writer.getMinimumBlockCountWorthWriting();
			bool canWait = !chunkData_->requiresFlushing();
			if (!haveAnyBlockInCurrentChunk(inodeLock)) {
				// There is no need to wait if we have just finished writing some chunk.
				// Let's immediately start writing the next chunk (if there is any).
				canWait = false;
			}

			if (!locator->shouldReset()) {
				// In the case of a truncate operation that is not finished yet, we don't want to
				// reset the locator, because it will be used later to lock the chunk again
				chunkData_->locator = std::move(locator);
			}

			inodeLock.unlock();

			Lock globalLock(gMutex);
			write_job_delayed_end(chunkData_, SAUNAFS_STATUS_OK, (canWait ? 1 : 0), globalLock);
		} catch (Exception &e) {
			std::string errorString = e.what();
			addPathByInodeBasedNotificationMessage("Write error: " + std::string(e.what()),
			                                       parent->inode);
			Lock inodeLock(parent->mutex);
			if (e.status() != SAUNAFS_ERROR_LOCKED) {
				chunkData_->tryCounter++;
				errorString += " (try counter: " + std::to_string(chunkData->tryCounter) + ")";
			} else if (chunkData_->tryCounter == 0) {
				// Set to nonzero to inform writers, that this task needs to wait a bit
				// Don't increase -- SAUNAFS_ERROR_LOCKED means that chunk is locked by a different
				// client and we have to wait until it is unlocked
				chunkData_->tryCounter = 1;
			}
			// Keep the lock
			chunkData_->locator = std::move(locator);
			// Move data left in the journal into front of the write cache
			returnJournalToDataChain(writer.releaseJournal(), inodeLock);
			inodeLock.unlock();

			safs::log_warn("write file error, inode: {}, index: {} - {}", parent->inode,
			               chunkIndex_, errorString.c_str());
			if (chunkData_->tryCounter >= maxretries) {
				// Convert error to an unrecoverable error
				throw UnrecoverableWriteException(e.message(), e.status());
			} else {
				// This may be recoverable or unrecoverable error
				throw;
			}
		}
	} catch (UnrecoverableWriteException &e) {
		addPathByInodeBasedNotificationMessage("Write error: " + std::string(e.what()),
		                                       parent->inode);
		Lock globalLock(gMutex);
		if (e.status() == SAUNAFS_ERROR_ENOENT) {
			write_job_end(chunkData_, SAUNAFS_ERROR_EBADF, globalLock);
		} else if (e.status() == SAUNAFS_ERROR_QUOTA) {
			write_job_end(chunkData_, SAUNAFS_ERROR_QUOTA, globalLock);
		} else if (e.status() == SAUNAFS_ERROR_NOSPACE ||
		           e.status() == SAUNAFS_ERROR_NOCHUNKSERVERS) {
			write_job_end(chunkData_, SAUNAFS_ERROR_NOSPACE, globalLock);
		} else {
			write_job_end(chunkData_, SAUNAFS_ERROR_IO, globalLock);
		}
	} catch (Exception &e) {
		if (chunkData_->tryCounter > kMinTryCounterToShowWriteErrorMessage) {
			addPathByInodeBasedNotificationMessage("Write error: " + std::string(e.what()),
			                                       parent->inode);
		}
		Lock globalLock(gMutex);
		int waitTime = 1;
		if (chunkData_->tryCounter > 10) {
			waitTime = std::min<int>(10, chunkData_->tryCounter - 9);
		}
		write_delayed_enqueue(chunkData_, waitTime, globalLock);
	}
}

void ChunkJobWriter::processDataChain(ChunkWriter &writer) {
	LOG_AVG_TILL_END_OF_SCOPE0("ChunkJobWriter::processDataChain");
	uint32_t maximumTime = kMaximumTime;
	bool otherJobsAreWaiting = false;
	inodedata *parent = chunkData_->getParent();
	while (true) {
		bool newOtherJobsAreWaiting = !jobsQueue->isEmpty();
		if (!otherJobsAreWaiting && newOtherJobsAreWaiting) {
			// Some new jobs have just arrived in the queue -- we should finish faster.
			maximumTime = kMaximumTimeWhenJobsWaiting;
			// But we need at least 5 seconds to finish the operations that are in progress
			uint32_t elapsedSeconds = wholeOperationTimer.elapsed_s();
			if (elapsedSeconds + kTimeToFinishOperations >= maximumTime) {
				maximumTime = elapsedSeconds + kTimeToFinishOperations;
			}
		}
		otherJobsAreWaiting = newOtherJobsAreWaiting;

		// If we have sent the previous message and have some time left, we can take
		// another block from current chunk to process it simultaneously. We won't take anything
		// new if we've already sent 'gWriteWindowSize' blocks and didn't receive status from
		// the chunkserver.
		bool can_expect_next_block = true;
		if (wholeOperationTimer.elapsed_s() + kTimeToFinishOperations < maximumTime &&
		    writer.acceptsNewOperations()) {
			Lock inodeLock(parent->mutex);
			// While there is any block worth sending, we add new write operation
			while (haveBlockWorthWriting(writer.getUnfinishedOperationsCount(), inodeLock)) {
				// Remove block from cache and pass it to the writer
				writer.addOperation(std::move(chunkData_->dataChain.front()));
				chunkData_->popFromChain();
				inodeLock.unlock();

				write_cb_release_blocks(1);

				inodeLock.lock();
			}
			if (chunkData_->requiresFlushing() && !haveAnyBlockInCurrentChunk(inodeLock)) {
				// No more data and some flushing is needed or required, so flush everything
				writer.startFlushMode();
			}
			if (writer.getUnfinishedOperationsCount() < gWriteWindowSize) {
				chunkData_->workerWaitingForData = true;
			}
			can_expect_next_block =
			    haveAnyBlockInCurrentChunk(inodeLock) ||
			    (parent->wasLastBlockRecentlyWritten() && chunkData_->dataChain.empty());
		} else if (writer.acceptsNewOperations()) {
			// We are running out of time...
			Lock inodeLock(parent->mutex);
			if (!chunkData_->requiresFlushing()) {
				// Nobody is waiting for the data to be flushed and the data in write chain
				// isn't too old. Let's postpone any operations
				// that didn't start yet and finish them in the next time slice for this chunk
				writer.dropNewOperations();
			} else {
				// Somebody if waiting for a flush, so we have to finish writing everything.
				writer.startFlushMode();
			}
			can_expect_next_block =
			    haveAnyBlockInCurrentChunk(inodeLock) ||
			    (parent->wasLastBlockRecentlyWritten() && chunkData_->dataChain.empty());
		}

		Lock inodeLock(parent->mutex);
		writer.setChunkSizeInBlocks(
		    std::min(parent->maxfleng - chunkIndex_ * SFSCHUNKSIZE, (uint64_t)SFSCHUNKSIZE));
		inodeLock.unlock();
		if (writer.startNewOperations(can_expect_next_block) > 0) {
			inodeLock.lock();
			parent->lastWriteToChunkservers.reset();
			inodeLock.unlock();
		}
		if (writer.getPendingOperationsCount() == 0) {
			return;
		} else if (wholeOperationTimer.elapsed_s() >= maximumTime) {
			throw RecoverableWriteException(
			    "Timeout after " + std::to_string(wholeOperationTimer.elapsed_ms()) + " ms",
			    SAUNAFS_ERROR_TIMEOUT);
		}

		writer.processOperations(gWriteWaveTimeout);
	}
}

void ChunkJobWriter::returnJournalToDataChain(std::list<WriteCacheBlock> &&journal,
                                              Lock &inodeLock) {
	if (!journal.empty()) {
		inodeLock.unlock();
		write_cb_acquire_blocks(journal.size());
		inodeLock.lock();

		chunkData_->getParent()->totalCachedBlocks += journal.size();
		chunkData_->dataChain.splice(chunkData_->dataChain.begin(), std::move(journal));
	}
}

bool ChunkJobWriter::haveAnyBlockInCurrentChunk(Lock &) { return !chunkData_->dataChain.empty(); }

bool ChunkJobWriter::haveBlockWorthWriting(uint32_t unfinishedOperationCount, Lock &inodeLock) {
	if (!haveAnyBlockInCurrentChunk(inodeLock)) { return false; }
	const auto &block = chunkData_->dataChain.front();
	if (block.type != WriteCacheBlock::kWritableBlock) {
		// Always write data, that was previously written
		return true;
	} else if (unfinishedOperationCount >= gWriteWindowSize) {
		// Don't start new operations if there is already a lot of pending writes
		return false;
	} else {
		// Always start full blocks; start partial blocks only if we have to flush the data
		// or the block won't be expanded (only the last one can be) to a full block
		return (block.size() == SFSBLOCKSIZE || chunkData_->requiresFlushing() ||
		        chunkData_->dataChain.size() > 1);
	}
}

/* main working thread | globalLock:UNLOCKED */
void *write_worker(void *) {
	ChunkJobWriter chunkJobWriter;

	static std::atomic_uint16_t writeWorkersCounter(0);
	std::string threadName = "writeWorker " + std::to_string(writeWorkersCounter++);
	pthread_setname_np(pthread_self(), threadName.c_str());

	for (;;) {
		// get next job
		uint32_t z1, z2, z3;
		uint8_t *data;
		{
			LOG_AVG_TILL_END_OF_SCOPE0("write_worker#idle");
			jobsQueue->get(&z1, &z2, &data, &z3);
		}
		if (data == NO_CHUNKDATA) { return NULL; }

		// process the job
		LOG_AVG_TILL_END_OF_SCOPE0("write_worker#working");
		Timer writeTimer;
		chunkJobWriter.processJob((ChunkData *)data);
		statsAdd(WriteStats::ACTIVE_WORKERS_TIME_ms, writeTimer.elapsed_ms());
	}
	return NULL;
}

/* API | globalLock: INITIALIZED,UNLOCKED */
void write_data_init(uint32_t cachesize, uint32_t retries, uint32_t workers,
                     uint32_t writewindowsize, uint32_t chunkserverTimeout_ms,
                     uint32_t cachePerInodePercentage, uint32_t waveTimeout) {
	uint64_t cachebytecount = uint64_t(cachesize) * 1024 * 1024;
	uint64_t cacheblockcount = (cachebytecount / SFSBLOCKSIZE);
	pthread_attr_t thattr;

	gChunkConnector.setSourceIp(fs_getsrcip());
	gWriteWindowSize = writewindowsize;
	gChunkserverTimeout_ms = chunkserverTimeout_ms;
	maxretries = retries;
	gWriteWaveTimeout = waveTimeout;
	if (cacheblockcount < 10) { cacheblockcount = 10; }

	freecacheblocks = cacheblockcount;
	gCachePerInodePercentage = cachePerInodePercentage;

	writeStatsInit();

	jobsQueue = std::make_unique<ProducerConsumerQueue>(0, deleterByType<ChunkData>);

	pthread_attr_init(&thattr);
	pthread_attr_setstacksize(&thattr, 0x100000);
	pthread_create(&delayed_queue_worker_th, &thattr, delayed_queue_worker, NULL);
	write_worker_th.resize(workers);
	for (auto &th : write_worker_th) { pthread_create(&th, &thattr, write_worker, NULL); }
	pthread_attr_destroy(&thattr);

	gTweaks.registerVariable("WriteMaxRetries", maxretries, "sfsioretries (write)");
	gTweaks.registerVariable("WriteWaveTimeout", gWriteWaveTimeout, "sfschunkserverwavewriteto");

	gIsInitialized = true;
}

void write_data_term(void) {
	uint32_t i;

	{
		Lock globalLock(gMutex);
		delayed_queue_put(NO_CHUNKDATA, 0, globalLock);
	}
	for (i = 0; i < write_worker_th.size(); i++) { jobsQueue->put(0, 0, NO_CHUNKDATA, 0); }
	for (i = 0; i < write_worker_th.size(); i++) { pthread_join(write_worker_th[i], NULL); }
	pthread_join(delayed_queue_worker_th, NULL);
	jobsQueue.reset();
	for (const auto &[_, id] : inodedataMap) { delete id; }
	inodedataMap.clear();
	truncateLocatorsData.clear();

	gIsInitialized = false;
}

/* inodeLock: UNLOCKED */
/* globalLock: UNLOCKED */
int write_block(ChunkData *chunkData, uint16_t pos, uint32_t from, uint32_t to, const uint8_t *data,
                Lock &inodeLock) {
	inodedata *parent = chunkData->getParent();
	parent->lastWriteToDataChain.reset();

	// Try to expand the last block
	if (!chunkData->dataChain.empty()) {
		auto &lastBlock = chunkData->dataChain.back();
		if (lastBlock.blockIndex == pos && lastBlock.type == WriteCacheBlock::kWritableBlock &&
		    lastBlock.expand(from, to, data)) {
			chunkData->wakeUpWorkerIfNecessary();
			return 0;
		}
	}
	inodeLock.unlock();

	// Didn't manage to expand an existing block, so allocate a new one
	write_cb_wait_for_block(parent);
	write_cb_acquire_blocks(1);

	inodeLock.lock();
	chunkData->pushToChain(
	    WriteCacheBlock(chunkData->chunkIndex, pos, WriteCacheBlock::kWritableBlock));
	sassert(chunkData->dataChain.back().expand(from, to, data));
	Lock globalLock(gMutex, std::defer_lock);
	if (chunkData->inQueue) {
		// Consider some speedup if there are no errors and:
		// - there is a lot of blocks in the write chain
		// - there are at least two chunks in the write chain
		if (chunkData->tryCounter == 0 &&
		    (chunkData->dataChain.size() > chunkData->minimumBlocksToWrite)) {
			inodeLock.unlock();

			globalLock.lock();
			if (delayed_queue_remove(chunkData, globalLock)) {
				write_enqueue(chunkData, globalLock);
			}
			globalLock.unlock();

			inodeLock.lock();
		}
		chunkData->wakeUpWorkerIfNecessary();
	} else {
		chunkData->inQueue = true;
		inodeLock.unlock();

		globalLock.lock();
		write_enqueue(chunkData, globalLock);
		globalLock.unlock();

		inodeLock.lock();
	}
	return 0;
}

/* globalLock: UNLOCKED */
int write_blocks(inodedata *id, uint64_t offset, uint32_t size, const uint8_t *data) {
	LOG_AVG_TILL_END_OF_SCOPE0("write_blocks");
	uint32_t chindx = offset >> SFSCHUNKBITS;
	uint16_t pos = (offset & SFSCHUNKMASK) >> SFSBLOCKBITS;
	uint32_t from = offset & SFSBLOCKMASK;
	Lock inodeLock(id->mutex);
	while (size > 0) {
		ChunkData *chunkData = write_get_chunkdata(chindx, id);
		chunkData->writesCount++;

		if (size > SFSBLOCKSIZE - from) {
			if (write_block(chunkData, pos, from, SFSBLOCKSIZE, data, inodeLock) < 0) {
				chunkData->writesCount--;
				return SAUNAFS_ERROR_IO;
			}

			gCachedBytes += (SFSBLOCKSIZE - from);
			size -= (SFSBLOCKSIZE - from);
			data += (SFSBLOCKSIZE - from);
			from = 0;
			pos++;
			if (pos == SFSBLOCKSINCHUNK) {
				pos = 0;
				chindx++;
			}
		} else {
			if (write_block(chunkData, pos, from, from + size, data, inodeLock) < 0) {
				chunkData->writesCount--;
				return SAUNAFS_ERROR_IO;
			}

			gCachedBytes += size;
			size = 0;
		}
		chunkData->writesCount--;

		if (id->status != SAUNAFS_STATUS_OK) {
			return id->status;
		}
	}
	return 0;
}

int write_data(void *vid, uint64_t offset, uint32_t size, const uint8_t *data, size_t currentSize) {
	LOG_AVG_TILL_END_OF_SCOPE0("write_data");
	int status;
	inodedata *id = (inodedata *)vid;
	if (id == NO_INODEDATA) { return SAUNAFS_ERROR_IO; }

	Lock inodeLock(id->mutex);
	status = id->status;
	id->maxfleng = std::max(id->maxfleng, currentSize);
	if (status == SAUNAFS_STATUS_OK) {
		if (offset + size > id->maxfleng) {  // move fleng
			id->maxfleng = offset + size;
		}
		id->writewaiting++;
		while (id->flushwaiting > 0) { id->writecond.wait(inodeLock); }
		id->writewaiting--;
	}
	inodeLock.unlock();

	if (status != SAUNAFS_STATUS_OK) { return status; }

	return write_blocks(id, offset, size, data);
}

/* inode: LOCKED */
static void write_data_flushwaiting_increase(inodedata *id, Lock &) { id->flushwaiting++; }

/* inode: LOCKED */
static void write_data_flushwaiting_decrease(inodedata *id, Lock &) {
	id->flushwaiting--;
	if (id->flushwaiting == 0 && id->writewaiting > 0) { id->writecond.notify_all(); }
}

/* inode: UNLOCKED */
static void write_data_lcnt_increase(inodedata *id) { id->lcnt++; }

/* inode: LOCKED */
static void write_data_lcnt_decrease_check_deleted(inodedata *id, Lock &inodeLock,
                                                   bool &isDeleted) {
	// As long as it is not freed, then we don't consider the inodedata deleted
	isDeleted = false;
	bool almostDone =
	    (id->emptyChunkDataList) && (id->flushwaiting == 0) && (id->writewaiting == 0);
	inodeLock.unlock();

	Lock globalLock(gMutex);
	id->lcnt--;
	if (id->lcnt == 0 && almostDone) {
		write_free_inodedata(id, globalLock);
		isDeleted = true;
		return;
	}
	globalLock.unlock();

	inodeLock.lock();
}

/* inode: LOCKED */
inline void write_data_lcnt_decrease(inodedata *id, Lock &inodeLock) {
	bool dummy_isDeleted;
	write_data_lcnt_decrease_check_deleted(id, inodeLock, dummy_isDeleted);
	(void)dummy_isDeleted;
}

void *write_data_new(inode_t inode) {
	inodedata *id;
	Lock globalLock(gMutex);
	id = write_get_inodedata(inode, globalLock);
	if (id == NO_INODEDATA) { return NO_INODEDATA; }

	write_data_lcnt_increase(id);
	return id;
}

/* globalLock: LOCKED */
static int write_data_flush(void *vid, Lock &globalLock) {
	Timer flushTimer;
	inodedata *id = (inodedata *)vid;
	if (id == NO_INODEDATA) { return SAUNAFS_ERROR_IO; }
	globalLock.unlock();

	Lock inodeLock(id->mutex);
	write_data_flushwaiting_increase(id, inodeLock);
	inodeLock.unlock();

	globalLock.lock();
	// If there are no errors (status == SAUNAFS_STATUS_OK) and inode is waiting
	// in the delayed queue, speed it up
	auto delayedEnqueuedChunkData = delayed_queue_remove(id, globalLock);
	if (id->status == SAUNAFS_STATUS_OK && !delayedEnqueuedChunkData.empty()) {
		write_enqueue(delayedEnqueuedChunkData, globalLock);
	}

	// Wait for the data to be flushed
	while (!id->emptyChunkDataList && id->status == SAUNAFS_STATUS_OK) {
		id->flushcond.wait(globalLock);
	}
	globalLock.unlock();

	inodeLock.lock();
	write_data_flushwaiting_decrease(id, inodeLock);
	inodeLock.unlock();

	globalLock.lock();
	statsAdd(WriteStats::TOTAL_FLUSH_TIME_ms, flushTimer.elapsed_ms());
	return id->status;
}

int write_data_flush(void *vid) {
	Lock globalLock(gMutex);
	return write_data_flush(vid, globalLock);
}

uint64_t write_data_getmaxfleng(inode_t inode) {
	uint64_t maxfleng;
	inodedata *id;
	Lock globalLock(gMutex);
	id = write_find_inodedata(inode, globalLock);
	if (id) {
		Lock inodeLock(id->mutex);
		maxfleng = id->maxfleng;
	} else {
		maxfleng = 0;
	}
	return maxfleng;
}

int write_data_flush_inode(inode_t inode) {
	Lock globalLock(gMutex);
	inodedata *id = write_find_inodedata(inode, globalLock);
	if (id == NO_INODEDATA) { return 0; }
	return write_data_flush(id, globalLock);
}

int write_data_truncate(inode_t inode, bool opened, uint32_t uid, uint32_t gid, uint64_t length,
                        Attributes &attr) {
	Lock globalLock(gMutex);

	// 1. Flush writes but don't finish it completely - it'll be done at the end of truncate
	inodedata *id = write_get_inodedata(inode, globalLock);
	globalLock.unlock();
	if (id == NO_INODEDATA) { return SAUNAFS_ERROR_IO; }

	Lock inodeLock(id->mutex);
	write_data_lcnt_increase(id);
	write_data_flushwaiting_increase(id, inodeLock);  // this will block any writing to this inode
	inodeLock.unlock();

	globalLock.lock();
	int err = write_data_flush(id, globalLock);
	globalLock.unlock();
	if (err != 0) {
		inodeLock.lock();
		write_data_flushwaiting_decrease(id, inodeLock);
		write_data_lcnt_decrease(id, inodeLock);
		return err;
	}

	// 2. Send the request to master
	uint8_t status;
	bool writeNeeded;
	uint64_t oldLength;
	uint32_t lockId;
	int retrySleepTime_us = 200000;
	uint32_t retries = 0;
	do {
		status = fs_truncate(inode, opened, uid, gid, length, writeNeeded, attr, oldLength, lockId);
		if (status != SAUNAFS_STATUS_OK) {
			safs::log_info("truncate file {} to length {}: {} (try {}/{})", inode, length,
			               saunafs_error_string(status), int(retries + 1), int(maxretries));
		}
		if (retries >= maxretries) { break; }
		if (status == SAUNAFS_ERROR_LOCKED) {
			sleep(1);
		} else if (status == SAUNAFS_ERROR_CHUNKLOST || status == SAUNAFS_ERROR_NOTDONE) {
			usleep(retrySleepTime_us);
			retrySleepTime_us = std::min(2 * retrySleepTime_us, 60 * 1000000);
			++retries;
		}
	} while (status == SAUNAFS_ERROR_LOCKED || status == SAUNAFS_ERROR_CHUNKLOST ||
	         status == SAUNAFS_ERROR_NOTDONE);
	inodeLock.lock();
	if (status != 0 || !writeNeeded) {
		// Something failed or we have nothing to do more (master server managed to do the truncate)
		bool isDeleted = false;
		write_data_flushwaiting_decrease(id, inodeLock);
		write_data_lcnt_decrease_check_deleted(id, inodeLock, isDeleted);
		if (status == SAUNAFS_STATUS_OK) {
			if (!isDeleted) { id->maxfleng = length; }
			return 0;
		} else {
			return status;
		}
	}

	// We have to write zeros in suitable region to update xor/ec parity parts.
	// Let's calculate size of the region to be zeroed
	uint64_t endOffset = std::min({
	    oldLength,  // no further than to the end of the file
	    length + slice_traits::ec::kMaxDataCount * SFSBLOCKSIZE,  // no more than the maximal stripe
	    (length + SFSCHUNKSIZE - 1) / SFSCHUNKSIZE * SFSCHUNKSIZE  // no beyond the end of chunk
	});

	if (endOffset > length) {
		// Make maxfleng big enough for the upcoming writes
		id->maxfleng = endOffset;

		// Something has to be written, so pass our lock to writing threads
		sassert(id->totalCachedBlocks == 0);

		Lock truncateLocatorsDataLock(truncateLocatorsDataMutex);
		truncateLocatorsData[id->inode] = {lockId, endOffset};
		truncateLocatorsDataLock.unlock();

		inodeLock.unlock();
		// And now pass block of zeros to writing threads
		std::vector<uint8_t> zeros(endOffset - length, 0);
		err = write_blocks(id, length, zeros.size(), zeros.data());

		inodeLock.lock();
		if (err != 0) {
			truncateLocatorsDataLock.lock();
			truncateLocatorsData.erase(id->inode);
			truncateLocatorsDataLock.unlock();
			write_data_flushwaiting_decrease(id, inodeLock);
			write_data_lcnt_decrease(id, inodeLock);
			return err;
		}
		inodeLock.unlock();

		globalLock.lock();
		// Wait for writing threads to finish
		err = write_data_flush(id, globalLock);
		globalLock.unlock();

		inodeLock.lock();

		truncateLocatorsDataLock.lock();
		truncateLocatorsData.erase(id->inode);
		truncateLocatorsDataLock.unlock();

		if (err != 0) {
			// unlock the chunk here?
			write_data_flushwaiting_decrease(id, inodeLock);
			write_data_lcnt_decrease(id, inodeLock);
			return err;
		}
	}

	id->maxfleng = length;
	// Now we can tell the master server to finish the truncate operation and
	// then unblock the inode
	inodeLock.unlock();

	status = fs_truncateend(inode, uid, gid, length, lockId, attr);

	inodeLock.lock();
	write_data_flushwaiting_decrease(id, inodeLock);
	write_data_lcnt_decrease(id, inodeLock);

	if (status != SAUNAFS_STATUS_OK) {
		safs::log_warn("truncateend on file {} to length {}: {}", inode, length,
		               saunafs_error_string(status));
		return status;
	}
	return 0;
}

int write_data_end(void *vid) {
	Lock globalLock(gMutex);
	inodedata *id = (inodedata *)vid;
	if (id == NO_INODEDATA) { return SAUNAFS_ERROR_IO; }
	int status = write_data_flush(id, globalLock);
	globalLock.unlock();

	bool almostDone = false;
	{
		Lock inodeLock(id->mutex);
		almostDone = (id->emptyChunkDataList) && (id->flushwaiting == 0) && (id->writewaiting == 0);
	}

	globalLock.lock();
	id->lcnt--;
	if (id->lcnt == 0 && almostDone) { write_free_inodedata(id, globalLock); }

	return status;
}

}  // namespace ChunkBasedWriteAlgorithm

void write_data_init(uint32_t cachesize, uint32_t retries, uint32_t workers,
                     uint32_t writewindowsize, uint32_t chunkserverTimeout_ms,
                     uint32_t cachePerInodePercentage, uint32_t waveTimeout) {
	if (gUseInodeBasedWriteAlgorithm) {
		if (InodeBasedWriteAlgorithm::gIsInitialized) { return; }

		InodeBasedWriteAlgorithm::write_data_init(cachesize, retries, workers, writewindowsize,
		                                          chunkserverTimeout_ms, cachePerInodePercentage,
		                                          waveTimeout);
	} else {
		if (ChunkBasedWriteAlgorithm::gIsInitialized) { return; }

		ChunkBasedWriteAlgorithm::write_data_init(cachesize, retries, workers, writewindowsize,
		                                          chunkserverTimeout_ms, cachePerInodePercentage,
		                                          waveTimeout);
	}
}

void write_data_term(void) {
	if (gUseInodeBasedWriteAlgorithm) {
		InodeBasedWriteAlgorithm::write_data_term();
	} else {
		ChunkBasedWriteAlgorithm::write_data_term();
	}
}

void *write_data_new(inode_t inode) {
	if (gUseInodeBasedWriteAlgorithm) { return InodeBasedWriteAlgorithm::write_data_new(inode); }
	return ChunkBasedWriteAlgorithm::write_data_new(inode);
}

int write_data_end(void *vid) {
	if (gUseInodeBasedWriteAlgorithm) { return InodeBasedWriteAlgorithm::write_data_end(vid); }
	return ChunkBasedWriteAlgorithm::write_data_end(vid);
}

int write_data_flush(void *vid) {
	if (gUseInodeBasedWriteAlgorithm) { return InodeBasedWriteAlgorithm::write_data_flush(vid); }
	return ChunkBasedWriteAlgorithm::write_data_flush(vid);
}

uint64_t write_data_getmaxfleng(inode_t inode) {
	if (gUseInodeBasedWriteAlgorithm) {
		return InodeBasedWriteAlgorithm::write_data_getmaxfleng(inode);
	}
	return ChunkBasedWriteAlgorithm::write_data_getmaxfleng(inode);
}

int write_data_flush_inode(inode_t inode) {
	if (gUseInodeBasedWriteAlgorithm) {
		return InodeBasedWriteAlgorithm::write_data_flush_inode(inode);
	}
	return ChunkBasedWriteAlgorithm::write_data_flush_inode(inode);
}

int write_data_truncate(inode_t inode, bool opened, uint32_t uid, uint32_t gid, uint64_t length,
                        Attributes &attr) {
	if (gUseInodeBasedWriteAlgorithm) {
		return InodeBasedWriteAlgorithm::write_data_truncate(inode, opened, uid, gid, length, attr);
	}
	return ChunkBasedWriteAlgorithm::write_data_truncate(inode, opened, uid, gid, length, attr);
}

int write_data(void *vid, uint64_t offset, uint32_t size, const uint8_t *buff, size_t currentSize) {
	if (gUseInodeBasedWriteAlgorithm) {
		return InodeBasedWriteAlgorithm::write_data(vid, offset, size, buff, currentSize);
	}
	return ChunkBasedWriteAlgorithm::write_data(vid, offset, size, buff, currentSize);
}

bool isChunkBasedWriteAlgorithmInitialized() {
	return ChunkBasedWriteAlgorithm::gIsInitialized;
}

bool isInodeBasedWriteAlgorithmInitialized() {
	return InodeBasedWriteAlgorithm::gIsInitialized;
}

void setUseInodeBasedWriteAlgorithm(bool useInodeBasedWriteAlgorithm) {
	gUseInodeBasedWriteAlgorithm = useInodeBasedWriteAlgorithm;
}

bool getUseInodeBasedWriteAlgorithm() {
	return gUseInodeBasedWriteAlgorithm;
}
