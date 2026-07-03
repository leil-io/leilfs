/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
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
#include "mount/readdata.h"

#include <cerrno>
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

#include "common/connection_pool.h"
#include "common/exceptions.h"
#include "common/read_plan_executor.h"
#include "common/time_utils.h"
#include "mount/chunk_reader.h"
#include "mount/mastercomm.h"
#include "mount/memory_info.h"
#include "mount/mount_info.h"
#include "mount/notification_area_logging.h"
#include "mount/readahead_adviser.h"
#include "mount/readdata_cache.h"
#include "mount/stats.h"
#include "mount/tweaks.h"
#include "protocol/SFSCommunication.h"
#include "slogger/slogger.h"

#define REFRESHTICKS 15

#define EMPTY_REQUEST nullptr

inline std::condition_variable readOperationsAvailable;
inline std::mutex gReadaheadRequestsContainerMutex;
inline int kMaxThresholdTicks = 360;
uint32_t maxWindowConsideringMaxReadCacheSize;
uint64_t timesRequestedMemory = 0;
uint64_t successfulTimesRequestedMemory = 0;
constexpr double kTimesRequestedMemoryLowerSuccessRate = 0.3;
constexpr double kTimesRequestedMemoryUpperSuccessRate = 0.8;
constexpr uint32_t kMinCacheExpirationTime = 1;
constexpr uint32_t kMinTryCounterToShowReadErrorMessage = 9;
constexpr int64_t kDelayedOpsTimeout_us = 166667;
constexpr int64_t kMinDelayedOpsSleepTime_us = 50000;

static std::atomic_bool gUsingReadPrefetch = false;
static constexpr uint32_t kInactiveReadRecordsTimeout_ms = 10000;  // 10s
static std::mutex gInodesWithFailedReadsMutex;
static std::map<inode_t, Timer> gInodesWithFailedReads;
static constexpr uint32_t kInodesWithFailedReadsTimeout_ms = 60000;  // 1 minute

enum class ReadStats : uint8_t {
	SERVED_READ_BYTES,       ///< Total number of bytes read (served from cache or requested)
	REQ_READ_BYTES,          ///< Total number of bytes actually read from chunkservers
	CACHE_HITS_BYTES,        ///< Number of bytes read from cache
	REQ_TO_SATISFY_BYTES,    ///< Number of bytes that had to be requested to satisfy read
	READ_FROM_CACHE,         ///< Number of read operations satisfied from cache
	READ_FROM_REQUESTED,     ///< Number of read operations satisfied with upcoming requests
	READ_FROM_NEW_REQ,       ///< Number of read operations satisfied with a new request
	NON_EXPIRED_RREC,        ///< Number of current non-expired read records
	TOTAL_WAIT_TIME_ms,      ///< Total time spent waiting for read operations to complete
	ACTIVE_WORKERS_TIME_ms,  ///< Sum of time read workers were actively reading
	STATNODES
};

static std::vector<uint64_t *> statsPtr(static_cast<uint8_t>(ReadStats::STATNODES));

void readStatsInit() {
	statsnode *s = stats_get_subnode(nullptr, "read_details", 0);
	auto init_stat = [&](ReadStats stat, const char *name) {
		statsPtr[static_cast<uint8_t>(stat)] = stats_get_counterptr(stats_get_subnode(s, name, 0));
	};

	init_stat(ReadStats::SERVED_READ_BYTES, "served_read_bytes");
	init_stat(ReadStats::REQ_READ_BYTES, "req_read_bytes");
	init_stat(ReadStats::CACHE_HITS_BYTES, "cache_hits_bytes");
	init_stat(ReadStats::REQ_TO_SATISFY_BYTES, "req_to_satisfy_bytes");
	init_stat(ReadStats::READ_FROM_CACHE, "read_from_cache");
	init_stat(ReadStats::READ_FROM_REQUESTED, "read_from_requested");
	init_stat(ReadStats::READ_FROM_NEW_REQ, "read_from_new_req");
	init_stat(ReadStats::NON_EXPIRED_RREC, "non_expired_rrec");
	init_stat(ReadStats::TOTAL_WAIT_TIME_ms, "total_wait_time_ms");
	init_stat(ReadStats::ACTIVE_WORKERS_TIME_ms, "active_workers_time_ms");
}

void statsAdd(ReadStats type, int64_t value = 1) {
	if (value < 0) {
		::stats_dec(static_cast<uint8_t>(type), statsPtr, static_cast<uint64_t>(-value));
		return;
	}
	::stats_inc(static_cast<uint8_t>(type), statsPtr, static_cast<uint64_t>(value));
}

std::unique_ptr<IMemoryInfo> createMemoryInfo() {
    std::unique_ptr<IMemoryInfo> memoryInfo;
    #ifdef _WIN32
        memoryInfo = std::make_unique<WindowsMemoryInfo>();
    #elif __linux__
        memoryInfo = std::make_unique<LinuxMemoryInfo>();
    #endif
    return memoryInfo;
}
std::unique_ptr<IMemoryInfo> gMemoryInfo = createMemoryInfo();

bool readShouldWaitForSystemMemory(size_t bytesToReadLeft) {
	uint64_t avalaibleSystemMemory = gMemoryInfo->getAvailableMemory();
	uint64_t virtualReadCacheAvailableMemory =
		gReadCacheMaxSize.load() - gUsedReadCacheMemory;
	return bytesToReadLeft > virtualReadCacheAvailableMemory ||
			bytesToReadLeft > avalaibleSystemMemory;
}

inline void updateCacheExpirationTime() {
	if (gOriginalCacheExpirationTime_ms.load() == 0) {
		gCacheExpirationTime_ms.store(0);
	} else if (gCacheExpirationTime_ms.load() == 0) {
		gCacheExpirationTime_ms.store(gOriginalCacheExpirationTime_ms.load());
	} else if (timesRequestedMemory > 0) {
		double successRate =
		    static_cast<double>(successfulTimesRequestedMemory) /
		    static_cast<double>(timesRequestedMemory);
		if (successRate < kTimesRequestedMemoryLowerSuccessRate) {
			gCacheExpirationTime_ms = std::max<uint32_t>(
			    gCacheExpirationTime_ms / 3, kMinCacheExpirationTime);
		} else if (successRate > kTimesRequestedMemoryUpperSuccessRate) {
			gCacheExpirationTime_ms =
			    std::min<uint32_t>(gCacheExpirationTime_ms * 2,
			                       gOriginalCacheExpirationTime_ms.load());
		}
	}
	timesRequestedMemory = 0;
	successfulTimesRequestedMemory = 0;
}

uint32_t getBytesToBeReadFromCS(uint32_t index, uint32_t offset, uint32_t size,
                                const uint64_t fileLength) {
	uint64_t offsetInFile = static_cast<uint64_t>(index) * SFSCHUNKSIZE + offset;
	uint32_t availableSize = size;  // requested data may lie beyond end of file
	if (offsetInFile >= fileLength) {
		// read request entirely beyond EOF, can't read anything
		availableSize = 0;
	} else if (offsetInFile + availableSize > fileLength) {
		// read request partially beyond EOF, truncate request to EOF
		availableSize = fileLength - offsetInFile;
	}
	if (availableSize == 0) {
		return 0;
	}
	return availableSize;
}

RequestConditionVariablePair *ReadaheadRequests::append(
    ReadaheadRequestPtr readaheadRequestPtr) {
	readaheadRequestPtr->entry->isPendingNotify = true;
	pendingRequests_.emplace_back(readaheadRequestPtr);
	return &pendingRequests_.back();
}

void ReadaheadRequests::tryNotify() {
	while (!pendingRequests_.empty() &&
	       pendingRequests_.front().requestPtr->state ==
	           ReadaheadRequestState::kFinished) {
		pendingRequests_.front().notify();
		pendingRequests_.pop_front();
	}
}

void ReadaheadRequests::clearAndNotify(const ReadaheadRequestPtr &reqPtr) {
	// find the request
	auto it = pendingRequests_.begin();
	uint32_t index = 0;
	while (it != pendingRequests_.end() && it->requestPtr != reqPtr) {
		it++;
		index++;
	}

	// this request already is not there
	if (it == pendingRequests_.end()) {
		return;
	}

	// remove all the requests that were considering this request successful
	while (pendingRequests_.size() > index) {
		pendingRequests_.back().requestPtr->error_code = reqPtr->error_code;
		pendingRequests_.back().notify();
		pendingRequests_.pop_back();
	}
}

uint64_t ReadaheadRequests::continuousOffsetRequested(
    uint64_t offset, uint64_t endOffset, ReadCache::Result &result,
    RequestConditionVariablePair *&rcvpPtr) {
	if (pendingRequests_.empty()) {
		return offset;
	}

	auto it = pendingRequests_.begin();
	while (it != pendingRequests_.end() && offset < endOffset) {
		if (it->requestPtr->request_offset() <= offset && it->requestPtr->endOffset() > offset) {
			result.add(*(it->requestPtr->entry));
			offset = it->requestPtr->endOffset();
			rcvpPtr = &(*it);
		}
		it++;
	}
	offset = std::min(offset, endOffset);
	return offset;
}

std::stringstream ReadaheadRequests::toString() {
	std::stringstream text;
	int index = 0;
	for (const auto &req : pendingRequests_) {
		text << (std::to_string(index) + ": (" +
		         std::to_string(req.requestPtr->request_offset()) + "," +
		         std::to_string(req.requestPtr->bytes_to_read_left()) + "," +
		         std::to_string(req.requestPtr->endOffset()) + ")" + "\n");
		index++;
	}
	return text;
}

void ReadaheadRequests::discardAllPendingRequests() {
	for (const auto &req : pendingRequests_) {
		if (req.requestPtr->state == ReadaheadRequestState::kInqueued &&
		    req.requestPtr->entry->refcount <= 1) {
			req.requestPtr->state = ReadaheadRequestState::kDiscarded;
		}
		req.requestPtr->entry->isPendingNotify = false;
	}
	pendingRequests_.clear();
}

ReadaheadRequests::~ReadaheadRequests() {
	if (!pendingRequests_.empty()) {
		safs::log_warn(
		    "ReadaheadRequests destructor called with pending requests, "
		    "this should not happen, pending requests: {}",
		    toString().str());
		discardAllPendingRequests();
	}
}

bool ReadaheadOperationsManager::request(ReadRecord *rrec, off_t fuseOffset, size_t fuseSize,
                                         uint64_t offset, uint32_t size, ReadCache::Result &result,
                                         RequestConditionVariablePair *&rcvpPtr) {
	bool isSequential = true;
	// Feed the adviser with original FUSE offset and size (before alignment)
	rrec->readahead_adviser.feed(fuseOffset, fuseSize, isSequential);

	rrec->cache.query(fuseOffset, fuseSize, result, false);

	uint64_t recommendedSize = round_up_to_blocksize(std::max<uint64_t>(
	    size, std::min<uint32_t>(rrec->readahead_adviser.window(),
	                             maxWindowConsideringMaxReadCacheSize)));

	if (!result.empty() && result.frontOffset() <= static_cast<uint64_t>(fuseOffset) &&
	    fuseOffset + fuseSize <= result.endOffset()) {
		// this query can be directly served from cache
		statsAdd(ReadStats::READ_FROM_CACHE);
		statsAdd(ReadStats::CACHE_HITS_BYTES, fuseSize);
		addExtraRequests_(rrec, offset, recommendedSize, result.endOffset());
		return false;
	}

	if (!isSequential) {
		rrec->resetSuggestedReadaheadReqs();
	} else {
		rrec->increaseSuggestedReadaheadReqs();
	}

	uint64_t cachedOffset = result.empty() ? offset : result.endOffset();
	if (cachedOffset > static_cast<uint64_t>(fuseOffset)) {
		statsAdd(ReadStats::CACHE_HITS_BYTES, cachedOffset - fuseOffset);
	}

	// check if this data is already in process
	uint64_t maximumRequestedOffset =
	    rrec->readaheadRequests.continuousOffsetRequested(
	        cachedOffset, offset + size, result, rcvpPtr);

	if (maximumRequestedOffset == offset + size) {
		// this query can be directly served after succeeding at some
		// pending read requests
		statsAdd(ReadStats::READ_FROM_REQUESTED);
		addExtraRequests_(rrec, offset, recommendedSize,
		                  maximumRequestedOffset);
		return true;
	}

	if (!isSequential && maximumRequestedOffset == cachedOffset) {
		rrec->readaheadRequests.discardAllPendingRequests();
	}

	// this query cannot be directly served after succeeding at some pending
	// read requests, it is necessary to add specific request for it
	auto remainingSize = recommendedSize - (maximumRequestedOffset - offset);

	rrec->cache.query(maximumRequestedOffset, remainingSize, result, true);

	uint64_t requestOffset = result.remainingOffset();
	uint64_t bytesToReadLeft = round_up_to_blocksize(
	    remainingSize - (requestOffset - maximumRequestedOffset));

	// Let's check if we very recently finished the request we've been waiting
	// for
	if (!result.back()->done) {
		statsAdd(ReadStats::READ_FROM_NEW_REQ);
		if (maximumRequestedOffset < offset + size) {
			statsAdd(ReadStats::REQ_TO_SATISFY_BYTES, offset + size - maximumRequestedOffset);
		}
		rcvpPtr = addRequest_(rrec, result.back(), 0);
	}

	addExtraRequests_(rrec, offset, recommendedSize,
	                  requestOffset + bytesToReadLeft);

	bool mustWait = false;
	for (auto const &entry : result.entries) {
		mustWait |= !(entry->done);
	}

	return mustWait;
}

void ReadaheadOperationsManager::prefetch(ReadRecord *rrec, uint64_t offset, uint32_t size) {
	ReadCache::Result dummyResult;
	rrec->cache.query(offset, size, dummyResult, false);
	if (!dummyResult.empty() && dummyResult.frontOffset() <= offset &&
	    offset + size <= dummyResult.endOffset()) {
		// Data is already in cache
		return;
	}
	RequestConditionVariablePair *rcvpPtr = nullptr;
	uint64_t cachedOffset = dummyResult.empty() ? offset : dummyResult.endOffset();

	// check if this data is already in process
	uint64_t maximumRequestedOffset = rrec->readaheadRequests.continuousOffsetRequested(
	    cachedOffset, offset + size, dummyResult, rcvpPtr);
	if (maximumRequestedOffset == offset + size) {
		// this query can be directly served after succeeding at some
		// pending read requests
		return;
	}

	// Remaining size to be prefetched
	massert(offset + size > maximumRequestedOffset,
	        "Remaining size to be prefetched should be greater than 0");
	size = offset + size - maximumRequestedOffset;
	rrec->cache.query(maximumRequestedOffset, size, dummyResult, true);

	// Let's check if we very recently finished the request we've been waiting
	// for
	if (dummyResult.back()->done) { return; }

	// Add the request with very low priority, 10 seconds from now
	constexpr int64_t kVeryLowPriority_us = 10000000;
	addRequest_(rrec, dummyResult.back(), kVeryLowPriority_us);
}

Request ReadaheadOperationsManager::nextRequest() {
	massert(!readaheadRequestContainer_.empty(), "There is no request to be processed");
	Request request = readaheadRequestContainer_.top().second;
	readaheadRequestContainer_.pop();
	return request;
}

void ReadaheadOperationsManager::putTerminateRequest() {
	std::unique_lock requestsLock(gReadaheadRequestsContainerMutex);
	putRequestWithPriority(std::numeric_limits<int64_t>::max(), EMPTY_REQUEST, EMPTY_REQUEST);
	readOperationsAvailable.notify_one();
}

RequestConditionVariablePair *ReadaheadOperationsManager::addRequest_(
    ReadRecord *rrec, ReadCache::Entry *entry, int64_t extraPriority) {
	ReadaheadRequestPtr readaheadRequestPtr =
	    ReadaheadRequestPtr(new ReadaheadRequestEntry(entry));
	std::unique_lock requestsLock(gReadaheadRequestsContainerMutex);
	int64_t priority = requestsTimer_.elapsed_us() + extraPriority;
	putRequestWithPriority(priority, rrec, readaheadRequestPtr);
	rrec->requestsNotDone++;
	entry->acquire();

	readOperationsAvailable.notify_one();
	requestsLock.unlock();

	return rrec->readaheadRequests.append(readaheadRequestPtr);
}

void ReadaheadOperationsManager::addExtraRequests_(
    ReadRecord *rrec, uint64_t currentOffset, uint64_t satisfyingSize,
    uint64_t maximumRequestedOffset) {
	if (gReadCacheMemoryAlmostExceeded.load()) {
		return;
	}

	if (!rrec->readaheadRequests.empty()) {
		maximumRequestedOffset =
		    std::max(maximumRequestedOffset,
		             rrec->readaheadRequests.lastPendingRequest()->requestPtr->endOffset());
	}

	uint64_t throughputWindow = rrec->readahead_adviser.throughputWindow();
	uint64_t readaheadSize = std::min<uint64_t>(
	    gMaxReadaheadRequests * satisfyingSize, throughputWindow);

	while (rrec->readaheadRequests.size() < rrec->suggestedReadaheadReqs() &&
	       maximumRequestedOffset < currentOffset + readaheadSize) {

		auto it = rrec->cache.find(maximumRequestedOffset);
		if (it != MISSING_OFFSET_PTR) {
			maximumRequestedOffset += it->requested_size;
			continue;
		}

		// Try to align extra requests to SFSCHUNKSIZE
		uint64_t extraRequestSize = std::min<uint64_t>(
		    satisfyingSize, SFSCHUNKSIZE - (maximumRequestedOffset % SFSCHUNKSIZE));

		if (maximumRequestedOffset < currentOffset) {
			safs::log_warn(
			    "ReadaheadOperationsManager::addExtraRequests_: Maximum requested offset should be "
			    "greater than or equal current offset (currentOffset: {}, "
			    "maximumRequestedOffset: {}, extraRequestSize: {})",
			    currentOffset, maximumRequestedOffset, extraRequestSize);
			// Next subtraction will overflow, so let's just return here
			return;
		}

		ReadCache::Entry *entry = rrec->cache.forceInsert(maximumRequestedOffset, extraRequestSize);

		int64_t extraPriority =
		    rrec->readahead_adviser.expectedNeededTime_us(maximumRequestedOffset - currentOffset);

		// we are not going to use the return value from the addRequest_ call
		addRequest_(rrec, entry, extraPriority);

		maximumRequestedOffset += extraRequestSize;
	}
}

using ReadRecords = std::unordered_multimap<inode_t, ReadRecord *>;
using ReadRecordRange = std::pair<ReadRecords::iterator, ReadRecords::iterator>;

inline ConnectionPool gReadConnectionPool;
inline ChunkConnectorUsingPool gChunkConnector(gReadConnectionPool);
inline ReadaheadOperationsManager gReadaheadOperationsManager;
inline std::mutex gMutex;

inline ReadRecords gInactiveReadRecords;
inline ReadRecords gActiveReadRecords;
inline pthread_t delayedOpsThread;
inline std::vector<pthread_t> readOpsThreads;
inline std::atomic<uint32_t> gChunkserverConnectTimeout_ms;
inline std::atomic<uint32_t> gChunkserverWaveReadTimeout_ms;
inline std::atomic<uint32_t> gChunkserverTotalReadTimeout_ms;
inline uint32_t gReadWorkers;
inline std::atomic<bool> gPrefetchXorStripes;
inline bool readDataTerminate;
inline std::atomic<uint32_t> maxRetries;
inline double gBandwidthOveruse;

const unsigned ReadaheadAdviser::kInitWindowSize;
const int ReadaheadAdviser::kRandomThreshold;
const int ReadaheadAdviser::kHistoryEntryLifespan_ns;
const int ReadaheadAdviser::kHistoryCapacity;
const unsigned ReadaheadAdviser::kHistoryValidityThreshold;

inline uint64_t round_up_to_blocksize(uint64_t bytes) {
	return (bytes + SFSBLOCKSIZE - 1) / SFSBLOCKSIZE * SFSBLOCKSIZE;
}

uint32_t read_data_get_wave_read_timeout_ms() {
	return gChunkserverWaveReadTimeout_ms;
}

uint32_t read_data_get_connect_timeout_ms() {
	return gChunkserverConnectTimeout_ms;
}

uint32_t read_data_get_total_read_timeout_ms() {
	return gChunkserverTotalReadTimeout_ms;
}

bool read_data_get_prefetchxorstripes() {
	return gPrefetchXorStripes;
}

ChunkConnector& read_data_get_chunk_connector() {
	return gChunkConnector;
}

inline void clear_read_records() {
	std::unique_lock gMutexLock(gMutex);

	for (ReadRecords::value_type& readRecord : gActiveReadRecords) {
		delete readRecord.second;
	}
	for (ReadRecords::value_type& readRecord : gInactiveReadRecords) {
		delete readRecord.second;
	}

	gActiveReadRecords.clear();
	gInactiveReadRecords.clear();
}

void* read_data_delayed_ops(void *arg) {
	(void)arg;

	pthread_setname_np(pthread_self(), "readDelayedOps");

	std::unique_lock gMutexLock(gMutex, std::defer_lock);
	std::unique_lock gUsedReadCacheMemoryLock(gReadCacheMemoryMutex,
	                                          std::defer_lock);
	int ticksSinceLastUpdate = 0;

	for (;;) {
		gReadConnectionPool.cleanup();
		gMutexLock.lock();
		Timeout sleep_timeout = Timeout(std::chrono::microseconds(kDelayedOpsTimeout_us));
		if (readDataTerminate) {
			return EMPTY_REQUEST;
		}
		auto readRecordIt = gActiveReadRecords.begin();
		std::vector<ReadRecord *> toCollectGarbage;
		while (readRecordIt != gActiveReadRecords.end()) {
			if (readRecordIt->second->refreshCounter < REFRESHTICKS) {
				++(readRecordIt->second->refreshCounter);
			}

			if (readRecordIt->second->expired) {
				assert(readRecordIt->second->readaheadRequests.empty());
				if (readRecordIt->second->requestsNotDone == 0) {
					// If there are no requests enqueued/in process then delete the ReadRecord
					delete readRecordIt->second;
					readRecordIt = gActiveReadRecords.erase(readRecordIt);
				} else {
					// Otherwise just try to clear the cache
					std::unique_lock inodeLock(readRecordIt->second->mutex);
					readRecordIt->second->cache.clear();
					++readRecordIt;
				}
			} else {
				toCollectGarbage.push_back(readRecordIt->second);

				++readRecordIt;
			}
		}

		if (gUsingReadPrefetch) {
			readRecordIt = gInactiveReadRecords.begin();
			while (readRecordIt != gInactiveReadRecords.end()) {
				if (readRecordIt->second->refreshCounter < REFRESHTICKS) {
					++(readRecordIt->second->refreshCounter);
				}

				if (readRecordIt->second->lastPrefetch.elapsed_ms() >
				    kInactiveReadRecordsTimeout_ms) {
					if (readRecordIt->second->requestsNotDone == 0) {
						// If there are no requests enqueued/in process then delete the ReadRecord
						delete readRecordIt->second;
						readRecordIt = gInactiveReadRecords.erase(readRecordIt);
					} else {
						// Otherwise just try to clear the cache
						std::unique_lock inodeLock(readRecordIt->second->mutex);
						readRecordIt->second->cache.clear();
						++readRecordIt;
					}
				} else {
					toCollectGarbage.push_back(readRecordIt->second);

					++readRecordIt;
				}
			}

			std::lock_guard iwfrLock(gInodesWithFailedReadsMutex);
			auto iwfrIt = gInodesWithFailedReads.begin();
			while (iwfrIt != gInodesWithFailedReads.end()) {
				if (iwfrIt->second.elapsed_ms() > kInodesWithFailedReadsTimeout_ms) {
					iwfrIt = gInodesWithFailedReads.erase(iwfrIt);
				} else {
					++iwfrIt;
				}
			}
		}

		ticksSinceLastUpdate++;

		for (auto *readRecord : toCollectGarbage) {
			std::unique_lock inodeLock(readRecord->mutex);
			readRecord->cache.collectGarbage();
		}

		if (ticksSinceLastUpdate == kMaxThresholdTicks) {
			gUsedReadCacheMemoryLock.lock();
			updateCacheExpirationTime();
			gUsedReadCacheMemoryLock.unlock();
			ticksSinceLastUpdate = 0;
		}

		gMutexLock.unlock();

		usleep(std::max(sleep_timeout.remaining_us(), kMinDelayedOpsSleepTime_us));
	}
}

void* read_worker(void *arg) {
	(void)arg;

	static std::atomic_uint16_t readWorkersCounter(0);
	std::string threadName = "readWorker " + std::to_string(readWorkersCounter++);
	pthread_setname_np(pthread_self(), threadName.c_str());

	for (;;) {
		std::unique_lock requestsLock(gReadaheadRequestsContainerMutex);
		if (gReadaheadOperationsManager.empty()) {
			readOperationsAvailable.wait(requestsLock, [] {
				return !gReadaheadOperationsManager.empty();
			});
		}

		auto nextRequest = gReadaheadOperationsManager.nextRequest();
		ReadRecord *readRecord = std::move(nextRequest.first);
		ReadaheadRequestPtr request = std::move(nextRequest.second);
		requestsLock.unlock();

		if (!request) {
			return EMPTY_REQUEST;
		}

		ReadCache::Entry *entry = request->entry;

		// request no longer valid or no longer needed
		if (request->error_code != SAUNAFS_STATUS_OK
		    || request->state == ReadaheadRequestState::kDiscarded) {
			{
				std::unique_lock inodeLock(readRecord->mutex);
				request.reset();
			}

			std::unique_lock entryLock(entry->mutex);  // Make helgrind happy
			entry->release();
			entry->done = true;
			entry->requested_size = 0;
			readRecord->requestsNotDone--;
			continue;
		}

		std::unique_lock entryLock(entry->mutex);
		ChunkReader reader(gChunkConnector, gBandwidthOveruse);
		request->state = ReadaheadRequestState::kProcessing;

		uint64_t bytes_read = 0;
		Timer readTimer;
		int error_code =
		    read_to_buffer(readRecord, request->request_offset(),
		                   request->bytes_to_read_left(), entry->buffer,
		                   &bytes_read, reader, entryLock);
		statsAdd(ReadStats::ACTIVE_WORKERS_TIME_ms, readTimer.elapsed_ms());

		entry->release();
		entry->reset_timer();

		if (error_code != SAUNAFS_STATUS_OK
		    || request->error_code != SAUNAFS_STATUS_OK) {
			// discard any leftover bytes from incorrect read
			entry->buffer.clear();
			entry->requested_size = 0;
			entry->done = true;
			entryLock.unlock();

			std::unique_lock inodeLock(readRecord->mutex);
			// clear the list of read requests for this inode and notify waiting
			// threads of this error
			if (request->error_code == SAUNAFS_STATUS_OK) {
				request->error_code = error_code;
				readRecord->readaheadRequests.clearAndNotify(request);
			}
			request.reset();

			readRecord->requestsNotDone--;
			continue;
		}

		entry->requested_size = entry->buffer.size();
		entry->done = true;
		entryLock.unlock();

		request->state = ReadaheadRequestState::kFinished;

		std::unique_lock inodeLock(readRecord->mutex);
		readRecord->readaheadRequests.tryNotify();
		request.reset();

		readRecord->requestsNotDone--;
	}

	return EMPTY_REQUEST;
}

ReadRecord *read_data_new(inode_t inode) {
	std::unique_lock gMutexLock(gMutex);
	if (gUsingReadPrefetch) {
		auto it = gInactiveReadRecords.find(inode);
		if (it != gInactiveReadRecords.end()) {
			// Move it from inactive to active
			ReadRecord *rrec = it->second;
			gInactiveReadRecords.erase(it);
			gActiveReadRecords.emplace(inode, rrec);
			return rrec;
		}
	}
	ReadRecord *rrec = new ReadRecord(inode);

	gActiveReadRecords.emplace(inode, rrec);

	statsAdd(ReadStats::NON_EXPIRED_RREC, +1);
	return rrec;
}

void read_data_end(ReadRecord *rrec) {
	std::unique_lock inodeLock(rrec->mutex);
	rrec->readaheadRequests.discardAllPendingRequests();
	rrec->cache.clear();
	inodeLock.unlock();

	std::unique_lock gMutexLock(gMutex);
	rrec->expired = true;
	statsAdd(ReadStats::NON_EXPIRED_RREC, -1);
}

void read_data_init(uint32_t retries,
		uint32_t chunkserverRoundTripTime_ms,
		uint32_t chunkserverConnectTimeout_ms,
		uint32_t chunkServerWaveReadTimeout_ms,
		uint32_t chunkserverTotalReadTimeout_ms,
		uint32_t cache_expiration_time_ms,
		uint32_t read_buffers_expiration_time_ms,
		uint32_t readahead_max_window_size_kB,
		uint32_t read_chache_max_size_percentage,
		uint32_t read_workers,
		uint32_t max_readahead_requests,
		bool prefetchXorStripes,
		double bandwidth_overuse) {
	pthread_attr_t thattr;

	readDataTerminate = false;

	// Initialize the global read cache entries pool. This pool manages cached read entries
	// and is used throughout the program. It is created during initialization and should
	// remain valid until the program terminates.
	gReadCacheEntriesPool = std::make_unique<ReadCacheEntriesPool>(read_buffers_expiration_time_ms);
	clear_read_records();

	readStatsInit();

	maxRetries = retries;
	gChunkserverConnectTimeout_ms = chunkserverConnectTimeout_ms;
	gChunkserverWaveReadTimeout_ms = chunkServerWaveReadTimeout_ms;
	gChunkserverTotalReadTimeout_ms = chunkserverTotalReadTimeout_ms;
	gOriginalCacheExpirationTime_ms = cache_expiration_time_ms;
	gCacheExpirationTime_ms = cache_expiration_time_ms;
	gReadaheadMaxWindowSize = readahead_max_window_size_kB * 1024;
	gReadCacheMaxSize.store((read_chache_max_size_percentage * 0.01) *
	                        gMemoryInfo->getTotalMemory());
	gReadWorkers = read_workers;
	maxWindowConsideringMaxReadCacheSize = gReadCacheMaxSize.load() / gReadWorkers;
	gMaxReadaheadRequests = max_readahead_requests;
	gPrefetchXorStripes = prefetchXorStripes;
	gBandwidthOveruse = bandwidth_overuse;
	gChunkConnector.setRoundTripTime(chunkserverRoundTripTime_ms);
	gChunkConnector.setSourceIp(fs_getsrcip());
	pthread_attr_init(&thattr);
	pthread_attr_setstacksize(&thattr,0x100000);
	pthread_create(&delayedOpsThread,&thattr,read_data_delayed_ops,NULL);
	readOpsThreads.resize(gReadWorkers);
	for (auto &th : readOpsThreads)
		pthread_create(&th, &thattr, read_worker, NULL);
	pthread_attr_destroy(&thattr);

	std::lock_guard lock(gMountInfoMtx);
	gTweaks.registerVariable("PrefetchXorStripes", gPrefetchXorStripes, "sfsprefetchxorstripes");
	gTweaks.registerVariable("ReadMaxRetries", maxRetries, "sfsioretries (read)");
	gTweaks.registerVariable("ReadConnectTimeout", gChunkserverConnectTimeout_ms, "sfschunkserverrtt");
	gTweaks.registerVariable("ReadWaveTimeout", gChunkserverWaveReadTimeout_ms, "sfschunkserverwavereadto");
	gTweaks.registerVariable("ReadTotalTimeout", gChunkserverTotalReadTimeout_ms, "sfschunkservertotalreadto");
	gTweaks.registerVariable("CacheExpirationTime", gOriginalCacheExpirationTime_ms, "cacheexpirationtime");
	gTweaks.registerVariable("ReadaheadMaxWindowSize", gReadaheadMaxWindowSize, "readaheadmaxwindowsize");
	gTweaks.registerVariable("ReadCacheMaxSize", gReadCacheMaxSize, "readcachemaxsizepercentage (bytes)");
	gTweaks.registerVariable("MaxReadaheadRequests", gMaxReadaheadRequests, "maxreadaheadrequests");
	gTweaks.registerVariable("ReadChunkPrepare", ChunkReader::preparations);
	gTweaks.registerVariable("ReqExecutedTotal", ReadPlanExecutor::executions_total_);
	gTweaks.registerVariable("ReqExecutedUsingAll", ReadPlanExecutor::executions_with_additional_operations_);
	gTweaks.registerVariable("ReqFinishedUsingAll", ReadPlanExecutor::executions_finished_by_additional_operations_);
}

void read_data_term(void) {
	{
		std::unique_lock gMutexLock(gMutex);
		readDataTerminate = true;
	}

	pthread_join(delayedOpsThread, NULL);
	for (uint32_t i = 0; i < gReadWorkers; i++) {
		gReadaheadOperationsManager.putTerminateRequest();
	}

	for (auto &thread : readOpsThreads) {
		pthread_join(thread, NULL);
	}

	clear_read_records();
	gMemoryInfo.reset();
	gReadCacheEntriesPool.reset();
	gInodesWithFailedReads.clear();
}

void read_inode_ops(inode_t inode) { // attributes of inode have been changed - force reconnect and clear cache
	std::unique_lock gMutexLock(gMutex);

	for (ReadRecords *records : {&gActiveReadRecords, &gInactiveReadRecords}) {
		ReadRecordRange range = records->equal_range(inode);

		for (auto it = range.first; it != range.second; ++it) {
			it->second->refreshCounter = REFRESHTICKS;  // force reconnect on forthcoming access
			if (!it->second->expired) {
				std::unique_lock inodeLock(it->second->mutex);
				it->second->cache.clear();
			}
		}
	}
}

// Ongoing write on this inode and chunkIndex, force reconnect and clear cache for this chunk
void read_inode_reconnect_and_clear_cache(inode_t inode, uint32_t chunkIndex) {
	std::unique_lock gMutexLock(gMutex);

	ReadRecordRange range = gActiveReadRecords.equal_range(inode);

	for (auto it = range.first; it != range.second; ++it) {
		if (!it->second->expired) {
			it->second->refreshCounter = REFRESHTICKS;  // force reconnect on forthcoming access
			std::unique_lock inodeLock(it->second->mutex);
			it->second->cache.selective_clear(chunkIndex);  // clear cache for this chunk
		}
	}
}

int read_data_sleep_time_ms(int tryCounter) {
	if (tryCounter <= 13) {            // 2^13 = 8192
		return (1 << tryCounter);  // 2^tryCounter milliseconds
	} else {
		return 1000 * 10;          // 10 seconds
	}
}

static void print_error_msg(ChunkReader& reader, uint32_t try_counter, const Exception &ex) {
	if (reader.isChunkLocated()) {
		safs_pretty_syslog(LOG_WARNING,
		                   "read file error, inode: %" PRIiNode ", index: %u, chunk: %" PRIu64 ", version: %u - %s "
		                   "(try counter: %u)", reader.inode(), reader.index(),
		                   reader.chunkId(), reader.version(), ex.what(), try_counter);
	} else {
		safs_pretty_syslog(LOG_WARNING,
		                   "read file error, inode: %" PRIiNode ", index: %u, chunk: failed to locate - %s "
		                   "(try counter: %u)", reader.inode(), reader.index(),
		                   ex.what(), try_counter);
	}
}

static void addInodeToFailedReads(inode_t inode) {
	if (gUsingReadPrefetch) {
		std::lock_guard iwfrLock(gInodesWithFailedReadsMutex);
		gInodesWithFailedReads[inode] = Timer();
	}
}

int read_to_buffer(ReadRecord *rrec, uint64_t current_offset, uint64_t bytes_to_read,
                   std::vector<uint8_t> &read_buffer, uint64_t *bytes_read, ChunkReader &reader,
                   std::unique_lock<std::mutex> &entryLock) {
	uint32_t try_counter = 0;
	inode_t prepared_inode = 0;  // this is always different than any real inode
	uint32_t prepared_chunk_id = 0;
	assert(*bytes_read == 0);

	// forced sleep between retries caused by recoverable failures
	uint32_t sleep_time_ms = 0;

	bool force_prepare = (rrec->refreshCounter == REFRESHTICKS);

	uint32_t total_read_cache_bytes_to_reserve = 0;
	uint32_t last_read_cache_bytes_to_reserve = 0;

	while (bytes_to_read > 0) {
		Timeout sleep_timeout = Timeout(std::chrono::milliseconds(sleep_time_ms));
		// Increase communicationTimeout to sleepTime; longer poll() can't be worse
		// than short poll() followed by nonproductive usleep().
		uint32_t timeout_ms = std::max(gChunkserverTotalReadTimeout_ms.load(), sleep_time_ms);
		Timeout communication_timeout = Timeout(std::chrono::milliseconds(timeout_ms));
		sleep_time_ms = 0;

		try {
			uint32_t chunk_id = current_offset / SFSCHUNKSIZE;
			if (force_prepare || prepared_inode != rrec->inode || prepared_chunk_id != chunk_id) {
				reader.prepareReadingChunk(rrec->inode, chunk_id, force_prepare);
				prepared_chunk_id = chunk_id;
				prepared_inode = rrec->inode;
				force_prepare = false;
				rrec->refreshCounter = 0;
			}

			uint64_t offset_of_chunk = static_cast<uint64_t>(chunk_id) * SFSCHUNKSIZE;
			uint32_t offset_in_chunk = current_offset - offset_of_chunk;
			uint32_t size_in_chunk = SFSCHUNKSIZE - offset_in_chunk;
			if (size_in_chunk > bytes_to_read) { size_in_chunk = bytes_to_read; }

			uint32_t read_cache_bytes_to_reserve = getBytesToBeReadFromCS(
			    reader.index(), offset_in_chunk, size_in_chunk, reader.fileLength());
			std::unique_lock usedMemoryLock(gReadCacheMemoryMutex);
			timesRequestedMemory++;
			if (readShouldWaitForSystemMemory(read_cache_bytes_to_reserve)) {
				throw RecoverableReadException(
				    "Not enough read cache memory available for reading");
			}
			successfulTimesRequestedMemory++;
			increaseUsedReadCacheMemory(read_cache_bytes_to_reserve);
			last_read_cache_bytes_to_reserve = read_cache_bytes_to_reserve;
			total_read_cache_bytes_to_reserve += read_cache_bytes_to_reserve;
			usedMemoryLock.unlock();

			uint32_t bytes_read_from_chunk = reader.readData(
			    read_buffer, offset_in_chunk, size_in_chunk, gChunkserverConnectTimeout_ms,
			    gChunkserverWaveReadTimeout_ms, communication_timeout, gPrefetchXorStripes);
			// No exceptions thrown. We can increase the counters and go to the next chunk
			*bytes_read += bytes_read_from_chunk;
			current_offset += bytes_read_from_chunk;
			bytes_to_read -= bytes_read_from_chunk;
			if (bytes_read_from_chunk < size_in_chunk) {
				// end of file
				break;
			}
			try_counter = 0;
		} catch (UnrecoverableReadException &ex) {
			print_error_msg(reader, try_counter, ex);
			addPathByInodeBasedNotificationMessage(
			    "Unrecoverable read error: " + std::string(ex.what()), rrec->inode);

			std::unique_lock usedMemoryLock(gReadCacheMemoryMutex);
			decreaseUsedReadCacheMemory(total_read_cache_bytes_to_reserve);
			usedMemoryLock.unlock();

			addInodeToFailedReads(rrec->inode);
			if (ex.status() == SAUNAFS_ERROR_ENOENT) {
				return SAUNAFS_ERROR_EBADF;  // stale handle
			} else {
				return SAUNAFS_ERROR_IO;
			}
		} catch (Exception &ex) {
			if (try_counter > 0) { print_error_msg(reader, try_counter, ex); }
			force_prepare = true;

			if (try_counter > maxRetries) {
				addPathByInodeBasedNotificationMessage(
				    "Read error: Exceeded max retries:" + std::string(ex.what()), rrec->inode);

				std::unique_lock usedMemoryLock(gReadCacheMemoryMutex);
				decreaseUsedReadCacheMemory(total_read_cache_bytes_to_reserve);
				usedMemoryLock.unlock();

				addInodeToFailedReads(rrec->inode);
				return SAUNAFS_ERROR_IO;
			} else {
				if (try_counter > kMinTryCounterToShowReadErrorMessage) {
					addPathByInodeBasedNotificationMessage("Read error: " + std::string(ex.what()),
					                                       rrec->inode);
				}

				std::unique_lock usedMemoryLock(gReadCacheMemoryMutex);
				decreaseUsedReadCacheMemory(last_read_cache_bytes_to_reserve);
				total_read_cache_bytes_to_reserve -= last_read_cache_bytes_to_reserve;
				usedMemoryLock.unlock();
				entryLock.unlock();

				usleep(sleep_timeout.remaining_us());
				entryLock.lock();
				sleep_time_ms = read_data_sleep_time_ms(try_counter);
			}
			try_counter++;
		}
	}
	statsAdd(ReadStats::REQ_READ_BYTES, *bytes_read);
	return SAUNAFS_STATUS_OK;
}

int read_data(ReadRecord *rrec, off_t fuseOffset, size_t fuseSize,
              uint64_t offset, uint32_t size, ReadCache::Result &ret) {
	assert(size % SFSBLOCKSIZE == 0);
	assert(offset % SFSBLOCKSIZE == 0);

	if (size == 0) {
		return SAUNAFS_STATUS_OK;
	}

	ReadCache::Result result;

	if (gCacheExpirationTime_ms == 0
	    || !rrec->readahead_adviser.shouldUseReadahead()) {
		// no cache case
		std::unique_lock inodeLock(rrec->mutex);
		rrec->readahead_adviser.feed(fuseOffset, fuseSize);

		auto insertedEntry = rrec->cache.query(offset, size, result, true);
		inodeLock.unlock();

		if (insertedEntry != nullptr) {
			statsAdd(ReadStats::READ_FROM_NEW_REQ);
			std::unique_lock entryLock(insertedEntry->mutex);
			uint64_t requestOffset = result.remainingOffset();
			uint64_t bytesToReadLeft = round_up_to_blocksize(
			    size - (requestOffset - offset));
			statsAdd(ReadStats::REQ_TO_SATISFY_BYTES, bytesToReadLeft);
			if (requestOffset > offset) {
				statsAdd(ReadStats::CACHE_HITS_BYTES, requestOffset - fuseOffset);
			}

			ChunkReader reader(gChunkConnector, gBandwidthOveruse);

			uint64_t bytesRead = 0;

			Timer readTimer;
			int errorCode = read_to_buffer(rrec,
			                               requestOffset,
			                               bytesToReadLeft,
			                               result.inputBuffer(),
			                               &bytesRead,
			                               reader, entryLock);
			statsAdd(ReadStats::TOTAL_WAIT_TIME_ms, readTimer.elapsed_ms());
			result.back()->done = true;

			if (errorCode != SAUNAFS_STATUS_OK) {
				result.inputBuffer().clear();
				return errorCode;
			}
		} else {
			statsAdd(ReadStats::READ_FROM_CACHE);
			statsAdd(ReadStats::CACHE_HITS_BYTES, fuseSize);
		}
	} else {
		// use the read operations manager to process the request
		RequestConditionVariablePair *rcvpPtr = nullptr;
		std::unique_lock inodeLock(rrec->mutex);
		bool mustWait = gReadaheadOperationsManager.request(
		    rrec, fuseOffset, fuseSize, offset, size, result, rcvpPtr);

		if (mustWait) {
			assert(rcvpPtr != nullptr);

			auto requestPtr = rcvpPtr->requestPtr;
			auto waitingCVPtr = rcvpPtr->cvPtr;

			Timer waitTimer;
			waitingCVPtr->wait(inodeLock);
			statsAdd(ReadStats::TOTAL_WAIT_TIME_ms, waitTimer.elapsed_ms());

			int error_code = requestPtr->error_code;
			if (error_code != SAUNAFS_STATUS_OK) {
				return error_code;
			}
		}
	}

	ret = std::move(result);
	statsAdd(ReadStats::SERVED_READ_BYTES, fuseSize);
	return SAUNAFS_STATUS_OK;
}

void read_prefetch(inode_t inode, uint64_t offset, uint32_t size) {
	std::unique_lock iwfrLock(gInodesWithFailedReadsMutex);
	if (gInodesWithFailedReads.find(inode) != gInodesWithFailedReads.end()) {
		// previous read failed, do not prefetch
		return;
	}
	iwfrLock.unlock();

	// Determine if it is present in the active read records
	std::unique_lock gMutexLock(gMutex);
	if (gActiveReadRecords.contains(inode)) {
		// Do not prefetch on some inode being read
		return;
	}

	// Create it if not already present
	ReadRecord *rrec;
	auto it = gInactiveReadRecords.find(inode);
	if (it != gInactiveReadRecords.end()) {
		rrec = it->second;
	} else {
		rrec = new ReadRecord(inode);
		gInactiveReadRecords.emplace(inode, rrec);
	}
	rrec->lastPrefetch = Timer();
	gMutexLock.unlock();

	std::unique_lock inodeLock(rrec->mutex);
	if (!rrec->readahead_adviser.shouldUseReadahead()) {
		// Feed the adviser with a sequential access pattern
		for (int i = 0; i < ReadaheadAdviser::kOppositeRequestThreshold; i++) {
			rrec->readahead_adviser.updateShouldUseReadahead(true);
		}
	}

	// Do the prefetch: queue the requests to the readahead operations manager
	gReadaheadOperationsManager.prefetch(rrec, offset, size);
}
