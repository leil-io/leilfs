/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include "chunkserver/bgjobs.h"

#include <sys/eventfd.h>
#include <sys/syslog.h>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "chunkserver/chunk_replicator.h"
#include "chunkserver/hddspacemgr.h"
#include "common/chunk_part_type.h"
#include "common/chunk_type_with_address.h"
#include "common/massert.h"
#include "common/metadata_cluster_member.h"
#include "common/output_packet.h"
#include "common/pcqueue.h"
#include "devtools/TracePrinter.h"
#include "devtools/request_log.h"
#include "slogger/slogger.h"

constexpr auto kInvalidJob = nullptr;

int JobPool::allocateListener(uint32_t listenerId) {
	if (listenerId >= kMaxMetadataConnections) {
		safs::log_err("JobPool: {}: listenerId {} exceeds max {}", __func__, listenerId,
		              kMaxMetadataConnections);
		return -1;
	}

	auto *listener = listenerInfos_[listenerId].load(std::memory_order_acquire);
	if (listener != nullptr) { return listener->notifierFD; }

	auto *newListener = new ListenerInfo();
	newListener->notifierFD = ::eventfd(0, EFD_NONBLOCK);
	if (newListener->notifierFD < 0) {
		const int err = errno;
		delete newListener;
		throw std::runtime_error("JobPool: eventfd() failed for listener " +
		                         std::to_string(listenerId) + ": " + std::string(strerror(err)));
	}
	newListener->nextJobId = 1;

	ListenerInfo *expected = nullptr;
	if (!listenerInfos_[listenerId].compare_exchange_strong(
	        expected, newListener, std::memory_order_acq_rel, std::memory_order_acquire)) {
		close(newListener->notifierFD);
		delete newListener;
		return expected->notifierFD;
	}

	return newListener->notifierFD;
}

uint32_t JobPool::allocatedListenerCount() const {
	uint32_t count = 0;
	for (const auto &atomicSlot : listenerInfos_) {
		if (atomicSlot.load(std::memory_order_acquire) != nullptr) { ++count; }
	}
	return count;
}

JobPool::JobPool(const std::string &name, uint8_t workers, uint32_t maxJobs, uint8_t numPriorities)
    : name_(name), workers(workers) {
	for (auto &slot : listenerInfos_) { slot.store(nullptr, std::memory_order_relaxed); }
	allocateListener(0);

	// Initialize the job queue with a maximum size
	if (numPriorities > 1) {
		jobsQueue = std::make_unique<ProducerConsumerQueueWithPriority>(numPriorities, maxJobs);
	} else {
		jobsQueue = std::make_unique<ProducerConsumerQueue>(maxJobs);
	}

	// NOTE: worker threads are NOT started here. Call start() after the fully-derived
	// object is constructed to avoid a vtable-pointer race condition.
}

void JobPool::startWorkers() {
	for (uint8_t i = 0; i < workers; ++i) {
		workerThreads.emplace_back(&JobPool::workerThread, this, name_, i);
	}
}

JobPool::~JobPool() {
	// stop() could already have been called (e.g. from a derived-class destructor).
	// The call here is a safety net; if the pool was never stopped the virtual
	// putExitJobToQueue() will resolve to JobPool's version.
	// If stop() was already called, this will be a no-op due to the stopped_ guard.
	stop();

	jobsQueue.reset();

	workerThreads.clear();

	for (auto &atomicSlot : listenerInfos_) {
		auto *listener = atomicSlot.load(std::memory_order_acquire);
		if (listener != nullptr) {
			if (listener->notifierFD >= 0) { close(listener->notifierFD); }
			delete listener;
			atomicSlot.store(nullptr, std::memory_order_relaxed);
		}
	}
}

void JobPool::stop() {
	if (stopped_.exchange(true, std::memory_order_acq_rel)) {
		return;  // Already stopped.
	}

	for (uint8_t i = 0; i < workers; ++i) {
		putExitJobToQueue();
	}

	for (auto &thread : workerThreads) {
		if (thread.joinable()) { thread.join(); }
	}

	for (uint32_t i = 0; i < kMaxMetadataConnections; ++i) {
		auto *listener = listenerInfos_[i].load(std::memory_order_acquire);
		if (listener != nullptr && !listener->statusQueue.empty()) { processCompletedJobs(i); }
	}
}

JobPool::ListenerInfo *JobPool::listenerForJob(uint32_t &listenerId, const char *caller) {
	// Listener zero exists for the whole life of the pool, so a job always has somewhere to land.
	// Every other slot is allocated when its connection is admitted, never here: allocating from
	// the job path could throw into a caller that has no way to undo the work it already did.
	if (listenerId >= kMaxMetadataConnections) {
		safs::log_warn("{} job pool: {}: invalid listenerId {}, resetting to 0", name_, caller,
		               listenerId);
		listenerId = 0;
	}

	auto *listener = listenerInfos_[listenerId].load(std::memory_order_acquire);
	if (listener == nullptr) {
		safs::log_warn("{} job pool: {}: listener {} was never allocated, resetting to 0", name_,
		               caller, listenerId);
		listenerId = 0;
		listener = listenerInfos_[0].load(std::memory_order_acquire);
	}

	sassert(listener != nullptr);
	return listener;
}

uint32_t JobPool::addJob(ChunkOperation operation, JobCallback callback, void *extra,
                         ProcessJobCallback processJob, uint32_t listenerId) {
	auto *listenerInfo = listenerForJob(listenerId, __func__);

	std::unique_lock lock(listenerInfo->jobsMutex);
	uint32_t jobId = listenerInfo->nextJobId++;
	auto job = std::make_unique<Job>();
	job->jobId = jobId;
	job->callback = std::move(callback);
	job->processJob = std::move(processJob);
	job->extra = extra;
	job->state = JobPool::State::Enabled;
	job->listenerId = listenerId;
	listenerInfo->jobHash[jobId] = std::move(job);

	putToJobQueue(jobId, operation,
	              reinterpret_cast<uint8_t *>(listenerInfo->jobHash[jobId].get()));
	unprocessedJobs_.fetch_add(1, std::memory_order_relaxed);
	return jobId;
}

bool JobPool::allJobsProcessed() const {
	return unprocessedJobs_.load(std::memory_order_relaxed) == 0;
}

bool JobPool::isEmpty() {
	if (jobsQueue->elements() > 0) { return false; }

	for (const auto &atomicSlot : listenerInfos_) {
		auto *listener = atomicSlot.load(std::memory_order_acquire);
		if (listener == nullptr) { continue; }
		std::lock_guard lock(listener->notifierMutex);
		if (!listener->statusQueue.empty()) { return false; }
	}
	return true;
}

uint32_t JobPool::getJobCount() const {
	TRACETHIS();
	return jobsQueue->elements();
}

bool JobPool::isListenerIdle(uint32_t listenerId) {
	if (listenerId >= kMaxMetadataConnections) { return true; }
	auto *listener = listenerInfos_[listenerId].load(std::memory_order_acquire);
	if (listener == nullptr) { return true; }
	std::lock_guard lock(listener->jobsMutex);
	return listener->jobHash.empty() && listener->deferredJobs.load() == 0;
}

bool JobPool::isFull() const {
	return jobsQueue->isFull();
}

void JobPool::disableAndChangeCallbackAll(const JobCallback &callback, uint32_t listenerId) {
	if (listenerId >= kMaxMetadataConnections) {
		safs::log_warn("JobPool: disableAndChangeCallbackAll: Invalid listenerId {}, returning",
		               listenerId);
		return;
	}

	auto *listenerInfo = listenerInfos_[listenerId].load(std::memory_order_acquire);
	if (listenerInfo == nullptr) { return; }

	std::lock_guard jobsLockGuard(listenerInfo->jobsMutex);
	for (auto &[jobId, job] : listenerInfo->jobHash) {
		if (job->state == JobPool::State::Enabled) { job->state = JobPool::State::Disabled; }
		job->callback = callback;
	}
}

void JobPool::disableJob(uint32_t jobId, uint32_t listenerId) {
	if (listenerId >= kMaxMetadataConnections) {
		safs::log_warn("JobPool: disableJob: Invalid listenerId {} for jobId {}, returning",
		               listenerId, jobId);
		return;
	}

	auto *listenerInfo = listenerInfos_[listenerId].load(std::memory_order_acquire);
	if (listenerInfo == nullptr) { return; }

	std::lock_guard jobsLockGuard(listenerInfo->jobsMutex);
	auto jobIterator = listenerInfo->jobHash.find(jobId);
	if (jobIterator != listenerInfo->jobHash.end() &&
	    jobIterator->second->state == JobPool::State::Enabled) {
		jobIterator->second->state = JobPool::State::Disabled;
	}
}

std::list<uint32_t> JobPool::disableJobs(const std::list<uint32_t> &jobIds, uint32_t listenerId) {
	// Check if the listenerId is valid
	std::list<uint32_t> disabledJobIds;
	if (listenerId >= kMaxMetadataConnections) {
		safs::log_warn("JobPool: disableJobs: Invalid listenerId {}, returning", listenerId);
		return disabledJobIds;
	}

	if (jobIds.empty()) { return disabledJobIds; }  // to save the locking

	auto *listenerInfo = listenerInfos_[listenerId].load(std::memory_order_acquire);
	if (listenerInfo == nullptr) { return disabledJobIds; }

	std::unique_lock jobsUniqueLock(listenerInfo->jobsMutex);
	for (auto jobId : jobIds) {
		auto jobIterator = listenerInfo->jobHash.find(jobId);
		if (jobIterator != listenerInfo->jobHash.end()) {
			if (jobIterator->second->state == JobPool::State::Enabled) {
				jobIterator->second->state = JobPool::State::Disabled;
				disabledJobIds.push_back(jobId);
			}
		}
	}
	return disabledJobIds;
}

void JobPool::processCompletedJobs(uint32_t listenerId) {
	if (listenerId >= kMaxMetadataConnections) {
		safs::log_warn("JobPool: processCompletedJobs: Invalid listenerId {}, returning",
		               listenerId);
		return;
	}

	auto *listenerInfo = listenerInfos_[listenerId].load(std::memory_order_acquire);
	if (listenerInfo == nullptr) { return; }

	uint32_t jobId{};
	uint8_t status{};
	while (receiveStatus(jobId, status, listenerId)) {
		std::unique_lock jobsUniqueLock(listenerInfo->jobsMutex);
		auto jobIterator = listenerInfo->jobHash.find(jobId);
		if (jobIterator == listenerInfo->jobHash.end()) { continue; }
		auto job = std::move(jobIterator->second);
		listenerInfo->jobHash.erase(jobIterator);
		jobsUniqueLock.unlock();

		// A callback may add a job to this pool, so it runs with the table unlocked.
		if (job->callback) { job->callback(status, job->extra); }
		unprocessedJobs_.fetch_sub(1, std::memory_order_relaxed);
	}
}

void JobPool::changeCallback(uint32_t jobId, JobCallback callback, void *extra,
                             uint32_t listenerId) {
	if (listenerId >= kMaxMetadataConnections) {
		safs::log_warn("JobPool: changeCallback: Invalid listenerId {} for jobId {}, returning",
		               listenerId, jobId);
		return;
	}

	auto *listenerInfo = listenerInfos_[listenerId].load(std::memory_order_acquire);
	if (listenerInfo == nullptr) { return; }

	std::lock_guard jobsLockGuard(listenerInfo->jobsMutex);
	auto jobIterator = listenerInfo->jobHash.find(jobId);
	if (jobIterator != listenerInfo->jobHash.end()) {
		jobIterator->second->callback = std::move(callback);
		jobIterator->second->extra = extra;
	}
}

void JobPool::changeCallback(std::list<uint32_t> &jobIds, const JobCallback &callback,
                             uint32_t listenerId) {
	if (listenerId >= kMaxMetadataConnections) {
		safs::log_warn("JobPool: changeCallback: Invalid listenerId {}, returning", listenerId);
		return;
	}

	auto *listenerInfo = listenerInfos_[listenerId].load(std::memory_order_acquire);
	if (listenerInfo == nullptr) { return; }

	std::lock_guard jobsLockGuard(listenerInfo->jobsMutex);
	for (auto jobId : jobIds) {
		auto jobIterator = listenerInfo->jobHash.find(jobId);
		if (jobIterator != listenerInfo->jobHash.end()) {
			jobIterator->second->callback = callback;
		}
	}
}

void JobPool::workerThread(const std::string &poolName, uint8_t workerId) {
	std::string threadName = poolName + "_worker_" + std::to_string(workerId);
	pthread_setname_np(pthread_self(), threadName.c_str());

	uint32_t jobId;
	uint32_t operation;
	uint8_t *jobPtrArg;
	State jobState;
	uint8_t status = SAUNAFS_STATUS_OK;

	while (true) {
		getFromJobQueue(&jobId, &operation, &jobPtrArg);

		if (operation == ChunkOperation::Exit) { break; }

		Job *job = reinterpret_cast<Job *>(jobPtrArg);

		if (job == kInvalidJob) {
			jobState = State::Disabled;
			continue;
		}

		// job exists
		uint32_t listenerId = job->listenerId;
		// A job only exists once its listener does, and listeners outlive every job in the pool.
		auto *listenerInfo = listenerInfos_[listenerId].load(std::memory_order_acquire);
		sassert(listenerInfo != nullptr);
		std::unique_lock jobsUniqueLock(listenerInfo->jobsMutex);
		jobState = job->state;
		if (job->state == State::Enabled) { job->state = State::InProgress; }

		if (jobState == State::Disabled) {
			status = SAUNAFS_ERROR_NOTDONE;
			jobsUniqueLock.unlock();
			sendStatus(jobId, status, listenerId);
			continue;
		}

		jobsUniqueLock.unlock();

		auto processJobCallback = job->processJob;
		if (processJobCallback) {
			status = processJobCallback();
		} else {
			status = SAUNAFS_ERROR_NOTDONE;
		}

		sendStatus(jobId, status, listenerId);
	}
}

void JobPool::sendStatus(uint32_t jobId, uint8_t status, uint32_t listenerId) {
	if (listenerId >= kMaxMetadataConnections) {
		safs::log_warn("JobPool: SendStatus: Invalid listenerId {} for jobId {}, returning",
		               listenerId, jobId);
		return;
	}

	auto *listenerInfo = listenerInfos_[listenerId].load(std::memory_order_acquire);
	if (listenerInfo == nullptr) {
		safs::log_warn("JobPool: SendStatus: Listener {} is null for jobId {}, returning",
		               listenerId, jobId);
		return;
	}

	std::lock_guard statusLock(listenerInfo->notifierMutex);

	if (listenerInfo->statusQueue.empty()) {
		static constexpr eventfd_t dummyValue = 1;
		eassert(::eventfd_write(listenerInfo->notifierFD, dummyValue) == 0 &&
		        "JobPool: SendStatus: Failed to write to eventfd");
	}
	listenerInfo->statusQueue.emplace(jobId, status);
}

bool JobPool::receiveStatus(uint32_t &jobId, uint8_t &status, uint32_t listenerId) {
	if (listenerId >= kMaxMetadataConnections) {
		safs::log_warn("JobPool: receiveStatus: Invalid listenerId {} for jobId {}, returning",
		               listenerId, jobId);
		return false;  // Return false to indicate that we should stop processing
	}

	auto *listenerInfo = listenerInfos_[listenerId].load(std::memory_order_acquire);
	if (listenerInfo == nullptr) {
		safs::log_warn("JobPool: receiveStatus: Listener {} is null, returning", listenerId);
		return false;
	}

	std::lock_guard statusLock(listenerInfo->notifierMutex);

	if (listenerInfo->statusQueue.empty()) { return false; }

	std::tie(jobId, status) = listenerInfo->statusQueue.front();
	listenerInfo->statusQueue.pop();
	if (listenerInfo->statusQueue.empty()) {
		eventfd_t dummyEvent;
		eassert(::eventfd_read(listenerInfo->notifierFD, &dummyEvent) == 0 &&
		        "JobPool: ReceiveStatus: Failed to read from eventfd");
	}

	return true;
}

void JobPool::putExitJobToQueue() {
	jobsQueue->put(0, JobPool::ChunkOperation::Exit, nullptr, 1);
}

void JobPool::putToJobQueue(uint32_t jobId, uint32_t operation, uint8_t *jobPtrArg) {
	jobsQueue->put(jobId, operation, jobPtrArg, 1);
}

void JobPool::getFromJobQueue(uint32_t *jobId, uint32_t *operation, uint8_t **jobPtrArg) {
	jobsQueue->get(jobId, operation, jobPtrArg, nullptr);
}

uint32_t MasterJobPool::addJobIfNotLocked(ChunkWithType chunkWithType, ChunkOperation operation,
                                          JobCallback callback, void *extra,
                                          ProcessJobCallback processJob, uint32_t listenerId) {
	auto *listener = listenerForJob(listenerId, __func__);

	std::unique_lock lockedChunkLock(chunkToJobReplyMapMutex_);
	auto it = chunkToJobReplyMap_.find(chunkWithType);
	if (it != chunkToJobReplyMap_.end() && it->second.writeInitReceived) {
		// Chunk is locked, store the job to be added later
		auto &lockedChunkData = it->second;
		listener->deferredJobs.fetch_add(1);

		lockedChunkData.pendingAddJobs.emplace_back(
		    [this, operation, extra, listenerId, callback = std::move(callback),
		     processJob = std::move(processJob)]() mutable -> uint32_t {
			    const auto jobId = addJob(operation, std::move(callback), extra,
			                              std::move(processJob), listenerId);
			    auto *l = listenerInfos_[listenerId].load(std::memory_order_acquire);
			    if (l != nullptr) { l->deferredJobs.fetch_sub(1); }
			    return jobId;
		    });

		return lockedChunkData.lockJobId;  // Lock guard is released here
	}
	lockedChunkLock.unlock();  // Release the lock before adding the job to avoid deadlocks

	// Chunk is not locked, add the job immediately
	return addJob(operation, std::move(callback), extra, std::move(processJob), listenerId);
}

uint32_t MasterJobPool::addLockJob(JobCallback callback, void *extra, uint32_t listenerId) {
	auto *listenerInfo = listenerForJob(listenerId, __func__);

	std::unique_lock lock(listenerInfo->jobsMutex);
	uint32_t jobId = listenerInfo->nextJobId++;
	auto job = std::make_unique<Job>();
	job->jobId = jobId;
	job->callback = std::move(callback);
	// No processJob for lock jobs, as they are just markers for locked chunks
	job->extra = extra;
	job->state = JobPool::State::Enabled;
	job->listenerId = listenerId;
	listenerInfo->jobHash[jobId] = std::move(job);
	// Not an actual job, but a marker for a locked chunk, so never inserted into the job queue.
	return jobId;
}

void MasterJobPool::changeLockJobsCallback(const LockJobCallbackMaker &lockJobCallbackMaker,
                                           uint32_t listenerId) {
	if (listenerId >= kMaxMetadataConnections) {
		safs::log_warn(
		    "{} job pool: {}: Invalid listenerId {} for changing lock jobs callback, resetting to 0",
		    name_, __func__, listenerId);
		listenerId = 0;
	}

	auto *listenerInfo = listenerInfos_[listenerId].load(std::memory_order_acquire);
	if (listenerInfo == nullptr) { return; }

	std::scoped_lock lock(chunkToJobReplyMapMutex_, listenerInfo->jobsMutex);
	for (auto &[chunkWithType, lockedChunkData] : chunkToJobReplyMap_) {
		if (lockedChunkData.listenerId == listenerId) {
			auto jobIterator = listenerInfo->jobHash.find(lockedChunkData.lockJobId);
			if (jobIterator != listenerInfo->jobHash.end()) {
				jobIterator->second->callback = lockJobCallbackMaker(chunkWithType, listenerId);
			}
		}
	}
}

void MasterJobPool::detachLockJobs(const LockJobCallbackMaker &lockJobCallbackMaker,
                                   uint32_t listenerId, uint32_t callbackListenerId) {
	if (listenerId >= kMaxMetadataConnections) {
		safs::log_warn("{} job pool: {}: Invalid listenerId {}, returning", name_, __func__,
		               listenerId);
		return;
	}
	if (callbackListenerId >= kMaxMetadataConnections) {
		safs::log_warn("{} job pool: {}: Invalid callback listener {}, resetting to 0", name_,
		               __func__, callbackListenerId);
		callbackListenerId = 0;
	}

	if (listenerId == callbackListenerId) { return; }

	auto *listenerInfo = listenerInfos_[listenerId].load(std::memory_order_acquire);
	auto *targetInfo = listenerInfos_[callbackListenerId].load(std::memory_order_acquire);
	if (listenerInfo == nullptr || targetInfo == nullptr) { return; }

	std::scoped_lock lock(chunkToJobReplyMapMutex_, listenerInfo->jobsMutex, targetInfo->jobsMutex);
	for (auto &[chunkWithType, lockedChunkData] : chunkToJobReplyMap_) {
		if (lockedChunkData.listenerId != listenerId) { continue; }

		auto jobIterator = listenerInfo->jobHash.find(lockedChunkData.lockJobId);
		if (jobIterator == listenerInfo->jobHash.end()) { continue; }
		auto callback = lockJobCallbackMaker(chunkWithType, callbackListenerId);
		if (!callback) {
			safs::log_err("{} job pool: {}: Refusing to detach lock job {} without a callback",
			              name_, __func__, lockedChunkData.lockJobId);
			continue;
		}

		// The lock job moves to the stable listener whole, so its completion still travels
		// through that listener's status queue and runs on the thread that drains it.
		auto job = std::move(jobIterator->second);
		listenerInfo->jobHash.erase(jobIterator);
		// A lock job never reaches the queue, so no worker can read the fields rewritten here.
		sassert(!job->processJob);
		job->jobId = targetInfo->nextJobId++;
		job->listenerId = callbackListenerId;
		job->callback = std::move(callback);
		lockedChunkData.lockJobId = job->jobId;
		lockedChunkData.listenerId = callbackListenerId;
		targetInfo->jobHash[job->jobId] = std::move(job);
	}
}

bool MasterJobPool::startChunkLock(const JobPool::JobCallback &callback, void *packet,
                                   uint64_t chunkId, ChunkPartType chunkType, uint32_t listenerId) {
	// Resolve the listener before recording it. addLockJob corrects its own copy of the argument,
	// so the map would otherwise name a listener that never receives the reply, stranding the
	// lock job on the one that does.
	listenerForJob(listenerId, __func__);

	std::unique_lock lock(chunkToJobReplyMapMutex_);
	if (chunkToJobReplyMap_.contains(ChunkWithType{chunkId, chunkType})) {
		lock.unlock();  // Release the lock before logging to avoid extra contention

		safs::log_warn(
		    "{}: Chunk lock job already exists for chunkId {:016X}, chunkType {}. Treating request "
		    "as retransmission; not adding a new lock job.",
		    __func__, chunkId, chunkType.toString());
		return false;
	}

	chunkToJobReplyMap_[ChunkWithType{chunkId, chunkType}] =
	    LockedChunkData(addLockJob(callback, packet, listenerId), listenerId);
	return true;
}

bool MasterJobPool::enforceChunkLock(uint64_t chunkId, ChunkPartType chunkType) {
	std::unique_lock lock(chunkToJobReplyMapMutex_);
	auto entry = chunkToJobReplyMap_.find(ChunkWithType{chunkId, chunkType});
	if (entry == chunkToJobReplyMap_.end()) {
		lock.unlock();  // Release the lock before logging to avoid extra contention

		// Master was not waiting for this lock, just log and return
		safs::log_trace("{}: No chunk lock job found for chunkId {:016X}, chunkType {}. Ignoring.",
		                __func__, chunkId, chunkType.toString());
		return false;
	}

	entry->second.writeInitReceived = true;
	return true;
}

bool MasterJobPool::releaseChunkLockEntry(uint64_t chunkId, ChunkPartType chunkType,
                                          const char *callerName, uint32_t &lockJobId,
                                          uint32_t &listenerId,
                                          std::vector<AddJobFunc> &pendingAddJobs) {
	std::unique_lock lock(chunkToJobReplyMapMutex_);
	auto entry = chunkToJobReplyMap_.find(ChunkWithType{chunkId, chunkType});
	if (entry == chunkToJobReplyMap_.end()) {
		lock.unlock();  // Release the lock before logging to avoid extra contention

		// Master was not waiting for this lock, just log and return
		safs::log_warn("{}: No chunk lock job found for chunkId {:016X}, chunkType {}. Ignoring.",
		               callerName, chunkId, chunkType.toString());
		return false;
	}

	lockJobId = entry->second.lockJobId;
	listenerId = entry->second.listenerId;
	pendingAddJobs = std::move(entry->second.pendingAddJobs);
	chunkToJobReplyMap_.erase(entry);
	return true;
}

void MasterJobPool::endChunkLock(uint64_t chunkId, ChunkPartType chunkType, uint8_t status) {
	uint32_t lockJobId;
	uint32_t listenerId;
	std::vector<AddJobFunc> pendingAddJobs;

	if (!releaseChunkLockEntry(chunkId, chunkType, __func__, lockJobId, listenerId,
	                           pendingAddJobs)) {
		return;  // No lock job found, just return
	}

	for (const auto &addJobFunc : pendingAddJobs) { addJobFunc(); }

	sendStatus(lockJobId, status, listenerId);
}

void MasterJobPool::eraseChunkLock(uint64_t chunkId, ChunkPartType chunkType) {
	uint32_t lockJobId;
	uint32_t listenerId;
	std::vector<AddJobFunc> pendingAddJobs;

	if (!releaseChunkLockEntry(chunkId, chunkType, __func__, lockJobId, listenerId,
	                           pendingAddJobs)) {
		return;  // No lock job found, just return
	}

	for (const auto &addJobFunc : pendingAddJobs) { addJobFunc(); }

	// Remove the lock job and the related packet
	auto *listenerInfo = listenerInfos_[listenerId].load(std::memory_order_acquire);
	if (listenerInfo == nullptr) { return; }
	std::unique_lock jobsUniqueLock(listenerInfo->jobsMutex);
	auto lockJobIterator = listenerInfo->jobHash.find(lockJobId);

	if (lockJobIterator != listenerInfo->jobHash.end()) {
		auto *outputPacket = reinterpret_cast<OutputPacket *>(lockJobIterator->second->extra);
		delete outputPacket;
		listenerInfo->jobHash.erase(lockJobIterator);
	}
}

ClientJobPool::~ClientJobPool() {
	// Call stop() while this derived object is fully alive so that virtual
	// dispatch resolves to ClientJobPool::putExitJobToQueue(), enqueuing Exit
	// at the correct lowest priority and allowing workers to drain the queue.
	stop();
}

void ClientJobPool::putExitJobToQueue() {
	// Enqueue EXIT at the lowest priority for the active I/O priority mode
	jobsQueue->put(0, ChunkOperation::Exit, nullptr, 1, getJobPriority(ChunkOperation::Exit));
}

uint8_t ClientJobPool::getJobPriority(ChunkOperation operation) {
	switch (operation) {
	case ChunkOperation::Open:
	case ChunkOperation::Close:
	case ChunkOperation::GetBlocks:
		// Use higher priority (0) for Open, Close and GetBlocks operations
		return 0;
	case ChunkOperation::Read:
	case ChunkOperation::Prefetch:
		return kReadLevel;  // Read jobs
	case ChunkOperation::Exit:
	case ChunkOperation::Write:
		// Use the lowest priority for Write and Exit operations, with an extra level if in Switch
		// mode
		return (ioPriorityMode_ == IOPriorityMode::Switch) ? kWriteLevelSwitchMode
		                                                   : kWriteLevelFifoMode;
	default:
		return 0;  // Default priority for other jobs
	}
}

void ClientJobPool::putToJobQueue(uint32_t jobId, uint32_t operation, uint8_t *jobPtrArg) {
	jobsQueue->put(jobId, operation, jobPtrArg, 1,
	               getJobPriority(static_cast<ChunkOperation>(operation)));
}

void ClientJobPool::getFromJobQueue(uint32_t *jobId, uint32_t *operation, uint8_t **jobPtrArg) {
	if (ioPriorityMode_ == IOPriorityMode::Fifo || stopped_.load()) {
		// If we don't have a preferred IO type or if we're stopping, just get the next job without
		// custom priority considerations. In switch mode, the priority levels in the queue will
		// still ensure that Exit and Write jobs are processed correctly.
		jobsQueue->get(jobId, operation, jobPtrArg, nullptr);
		return;
	}

	// The IOPriorityMode must be Switch if preferredIOType_ is not kPreferAny, so we can use the
	// priority feature of the queue to prefer the specified IO type.
	uint8_t preferredIOType = preferredIOType_.fetch_xor(kSwitchValue, std::memory_order_relaxed);
	uint8_t otherIOType = preferredIOType ^ kSwitchValue;
	std::array<uint8_t, 3> priorityLevelsToCheck = {0, preferredIOType, otherIOType};
	jobsQueue->getUsingCustomPriority(jobId, operation, jobPtrArg, nullptr, priorityLevelsToCheck);
}

uint32_t job_open(ClientJobPool &jobPool, JobPool::JobCallback callback, uint64_t chunkId,
                  ChunkPartType chunkType, uint32_t listenerId) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		return hddOpen(chunkId, chunkType);
	};
	return jobPool.addJob(JobPool::ChunkOperation::Open, std::move(callback), kEmptyExtra,
	                      processJob, listenerId);
}

uint32_t job_close(ClientJobPool &jobPool, JobPool::JobCallback callback, uint64_t chunkId,
                   ChunkPartType chunkType, uint32_t listenerId) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		return hddClose(chunkId, chunkType);
	};
	return jobPool.addJob(JobPool::ChunkOperation::Close, std::move(callback), kEmptyExtra,
	                      processJob, listenerId);
}

uint32_t job_read(ClientJobPool &jobPool, JobPool::JobCallback callback, uint64_t chunkId,
                  uint32_t version, ChunkPartType chunkType, uint32_t offset, uint32_t size,
                  uint32_t maxBlocksToBeReadBehind, uint32_t blocksToBeReadAhead,
                  OutputBuffer *outputBuffer, bool performHddOpen, uint32_t listenerId) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		LOG_AVG_TILL_END_OF_SCOPE0("job_read");
		uint8_t status = SAUNAFS_STATUS_OK;
		if (performHddOpen) {
			status = hddOpen(chunkId, chunkType);
			if (status != SAUNAFS_STATUS_OK) { return status; }
		}

		status = hddRead(chunkId, version, chunkType, offset, size, maxBlocksToBeReadBehind,
		                 blocksToBeReadAhead, outputBuffer);

		if (performHddOpen && status != SAUNAFS_STATUS_OK) {
			int ret = hddClose(chunkId, chunkType);
			if (ret != SAUNAFS_STATUS_OK) {
				safs::log_err("read job: cannot close chunk after read error ({}): {}",
				              saunafs_error_string(status), saunafs_error_string(ret));
			}
		}
		return status;
	};
	return jobPool.addJob(JobPool::ChunkOperation::Read, std::move(callback), outputBuffer,
	                      processJob, listenerId);
}

uint32_t job_prefetch(ClientJobPool &jobPool, uint64_t chunkId, ChunkPartType chunkType,
                      uint32_t firstBlockToBePrefetched, uint32_t blocksToBePrefetched,
                      uint32_t listenerId) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		return hddPrefetchBlocks(chunkId, chunkType, firstBlockToBePrefetched,
		                         blocksToBePrefetched);
	};
	return jobPool.addJob(JobPool::ChunkOperation::Prefetch, kEmptyCallback, kEmptyExtra,
	                      processJob, listenerId);
}

uint32_t job_write(ClientJobPool &jobPool, JobPool::JobCallback callback, uint64_t chunkId,
                   uint32_t chunkVersion, ChunkPartType chunkType,
                   std::vector<InputBuffer *> inputBuffers, uint32_t listenerId) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		if (inputBuffers.empty()) {
			safs::log_warn("job_write: No input buffers found for chunk id {}", chunkId);
			return SAUNAFS_STATUS_OK;
		}

		uint32_t totalBlocks = 0;
		auto writeOperations = InputBuffer::getWriteOperations(inputBuffers, totalBlocks);

		if (writeOperations.empty()) {
			safs::log_warn("job_write: No write operations found for chunk id {}", chunkId);
			return SAUNAFS_STATUS_OK;
		}

		std::vector<uint8_t> statuses;
		for (auto &op : writeOperations) {
			if (op.size == 0) {
				safs::log_warn("job_write: Skipping zero-size write operation for chunk id {}",
				               chunkId);
				statuses.push_back(SAUNAFS_STATUS_OK);
				continue;
			}

			if (op.offset == 0 && op.size == SFSBLOCKSIZE) {
				// Full blocks write
				uint32_t numBlocks = 0;
				for (auto blocks : op.blocksPerBuffer) { numBlocks += blocks; }

				// Make sure numBlocks does not exceed uint16_t max, as that's the maximum we can
				// handle in one write
				if (numBlocks > std::numeric_limits<uint16_t>::max()) {
					safs::log_err(
					    "job_write: Too many blocks ({}) in write operation for chunk id {}",
					    numBlocks, chunkId);
					statuses.push_back(SAUNAFS_ERROR_EINVAL);
					break;
				}

				auto bytesWritten =
				    hddChunkWriteFullBlocks(chunkId, chunkVersion, chunkType, op.startBlock,
				                            numBlocks, op.crcs, op.blocksPerBuffer, op.buffers);

				if (bytesWritten != static_cast<int>(numBlocks * SFSBLOCKSIZE)) {
					if (bytesWritten < 0) {
						statuses.push_back(-bytesWritten);
						break;
					} else {
						safs::log_warn(
						    "job_write: Partial write for chunk id {}, expected {} bytes, "
						    "wrote {} bytes. Sending EIO for the rest.",
						    chunkId, numBlocks * SFSBLOCKSIZE, bytesWritten);

						// Fill statuses with OK for the blocks that were written
						for (int32_t i = 0; i < bytesWritten / SFSBLOCKSIZE; ++i) {
							statuses.push_back(SAUNAFS_STATUS_OK);
						}
						statuses.push_back(SAUNAFS_ERROR_IO);
						break;
					}
				} else {
					// All blocks written successfully
					for (uint32_t i = 0; i < numBlocks; ++i) {
						statuses.push_back(SAUNAFS_STATUS_OK);
					}
				}
			} else {
				statuses.push_back(hddChunkWriteBlock(chunkId, chunkVersion, chunkType,
				                                      op.startBlock, op.offset, op.size, op.crcs[0],
				                                      op.buffers[0]));

				if (statuses.back() != SAUNAFS_STATUS_OK) { break; }
			}
		}

		if (statuses.empty()) {
			safs::log_warn("job_write: No write operations were processed for chunk id {}", chunkId);
			return SAUNAFS_STATUS_OK;
		}

		if (statuses.size() < totalBlocks) { statuses.resize(totalBlocks, statuses.back()); }
		assert(statuses.size() == totalBlocks);

		InputBuffer::applyStatuses(inputBuffers, statuses);

		if (!statuses.empty() && statuses.back() != SAUNAFS_STATUS_OK) {
			safs::log_warn("Failed to write chunk id {}: {}", chunkId,
			               saunafs_error_string(statuses.back()));
		}

		return statuses.empty() ? static_cast<uint8_t>(SAUNAFS_STATUS_OK) : statuses.back();
	};
	return jobPool.addJob(JobPool::ChunkOperation::Write, std::move(callback), kEmptyExtra,
	                      processJob, listenerId);
}

uint32_t job_get_blocks(ClientJobPool &jobPool, JobPool::JobCallback callback, uint64_t chunkId,
                        uint32_t version, ChunkPartType chunkType, uint16_t *blocks,
                        uint32_t listenerId) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		return hddChunkGetNumberOfBlocks(chunkId, chunkType, version, blocks);
	};
	return jobPool.addJob(JobPool::ChunkOperation::GetBlocks, std::move(callback), kEmptyExtra,
	                      processJob, listenerId);
}

uint32_t job_probe(MasterJobPool &jobPool, JobPool::JobCallback callback, void *extra,
                   uint64_t chunkId, ChunkPartType chunkType, std::shared_ptr<uint32_t> version,
                   uint32_t listenerId) {
	// The worker lambda and the callback share ownership of the buffer: a connection close
	// replaces the callback, and a raw pointer owned by it would dangle under a running worker.
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		return hddChunkGetVersion(chunkId, chunkType, version.get());
	};
	return jobPool.addJob(JobPool::ChunkOperation::ProbeVersion, std::move(callback), extra,
	                      processJob, listenerId);
}

uint32_t job_replicate(MasterJobPool &jobPool, JobPool::JobCallback callback, void *extra,
                       uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                       uint32_t sourcesBufferSize, const uint8_t *sourcesBufferPtr,
                       uint32_t listenerId) {
	// Copy the sources buffer to a vector to ensure it is valid for the lifetime of the job
	std::vector<uint8_t> sourcesBuffer(sourcesBufferSize);
	std::memcpy(sourcesBuffer.data(), sourcesBufferPtr, sourcesBufferSize);
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		uint8_t status = SAUNAFS_STATUS_OK;
		try {
			std::vector<ChunkTypeWithAddress> sources;
			deserialize(sourcesBuffer.data(), sourcesBufferSize, sources);
			ChunkFileCreator creator(chunkId, chunkVersion, chunkType);
			gReplicator.replicate(creator, sources);
		} catch (Exception &ex) {
			safs::log_warn("replication error: {}", ex.what());
			status = ex.status();
		}
		return status;
	};
	return jobPool.addJob(JobPool::ChunkOperation::Replicate, std::move(callback), extra,
	                      processJob, listenerId);
}

uint32_t job_invalid(MasterJobPool &jobPool, JobPool::JobCallback callback, void *extra,
                     uint32_t listenerId) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t { return SAUNAFS_ERROR_EINVAL; };
	return jobPool.addJob(JobPool::ChunkOperation::Invalid, std::move(callback), extra, processJob,
	                      listenerId);
}

uint32_t job_delete(MasterJobPool &jobPool, JobPool::JobCallback callback, void *extra,
                    uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                    uint32_t listenerId) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		return hddInternalDelete(chunkId, chunkVersion, chunkType);
	};
	return jobPool.addJobIfNotLocked(ChunkWithType{chunkId, chunkType},
	                                 JobPool::ChunkOperation::Delete, std::move(callback), extra,
	                                 processJob, listenerId);
}

uint32_t job_create(MasterJobPool &jobPool, JobPool::JobCallback callback, void *extra,
                    uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                    uint32_t listenerId) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		return hddInternalCreate(chunkId, chunkVersion, chunkType);
	};
	return jobPool.addJob(JobPool::ChunkOperation::Create, std::move(callback), extra, processJob,
	                      listenerId);
}

uint32_t job_version(MasterJobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                     uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                     uint32_t newChunkVersion, uint32_t listenerId) {
	if (newChunkVersion > 0) {
		JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
			return hddInternalUpdateVersion(chunkId, chunkVersion, newChunkVersion, chunkType);
		};
		return jobPool.addJobIfNotLocked(ChunkWithType{chunkId, chunkType},
		                                 JobPool::ChunkOperation::ChangeVersion, callback, extra,
		                                 processJob, listenerId);
	}
	return job_invalid(jobPool, callback, extra, listenerId);
}

uint32_t job_truncate(MasterJobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                      uint64_t chunkId, ChunkPartType chunkType, uint32_t chunkVersion,
                      uint32_t newChunkVersion, uint32_t length, uint32_t listenerId) {
	if (newChunkVersion > 0) {
		JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
			return hddTruncate(chunkId, chunkVersion, chunkType, newChunkVersion, length);
		};
		return jobPool.addJobIfNotLocked(ChunkWithType{chunkId, chunkType},
		                                 JobPool::ChunkOperation::Truncate, callback, extra,
		                                 processJob, listenerId);
	}
	return job_invalid(jobPool, callback, extra, listenerId);
}

uint32_t job_duplicate(MasterJobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                       uint64_t chunkId, uint32_t chunkVersion, uint32_t newChunkVersion,
                       ChunkPartType chunkType, uint64_t chunkIdCopy, uint32_t chunkVersionCopy,
                       uint32_t listenerId) {
	if (newChunkVersion > 0) {
		JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
			return hddDuplicate(chunkId, chunkVersion, newChunkVersion, chunkType, chunkIdCopy,
			                    chunkVersionCopy);
		};
		return jobPool.addJobIfNotLocked(ChunkWithType{chunkId, chunkType},
		                                 JobPool::ChunkOperation::Duplicate, callback, extra,
		                                 processJob, listenerId);
	}
	return job_invalid(jobPool, callback, extra, listenerId);
}

uint32_t job_duptrunc(MasterJobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                      uint64_t chunkId, uint32_t chunkVersion, uint32_t newChunkVersion,
                      ChunkPartType chunkType, uint64_t chunkIdCopy, uint32_t chunkVersionCopy,
                      uint32_t length, uint32_t listenerId) {
	if (newChunkVersion > 0) {
		JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
			return hddDuplicateTruncate(chunkId, chunkVersion, newChunkVersion, chunkType,
			                            chunkIdCopy, chunkVersionCopy, length);
		};
		return jobPool.addJobIfNotLocked(ChunkWithType{chunkId, chunkType},
		                                 JobPool::ChunkOperation::DuplicateTruncate, callback,
		                                 extra, processJob, listenerId);
	}
	return job_invalid(jobPool, callback, extra, listenerId);
}
