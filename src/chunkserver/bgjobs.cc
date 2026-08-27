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

#include "common/platform.h"

#include "chunkserver/bgjobs.h"

#include "chunkserver/test_job_faults.h"
#include "chunkserver/chunk_replicator.h"
#include "chunkserver/hddspacemgr.h"
#include "common/chunk_part_type.h"
#include "common/chunk_type_with_address.h"
#include "common/massert.h"
#include "common/output_packet.h"
#include "common/pcqueue.h"
#include "common/test_event_stream.h"
#include "devtools/TracePrinter.h"
#include "devtools/request_log.h"
#include "slogger/slogger.h"

#include <sys/eventfd.h>
#include <sys/syslog.h>
#include <unistd.h>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

constexpr auto kInvalidJob = nullptr;

JobPool::JobPool(const std::string &name, uint8_t workers, uint32_t maxJobs, uint32_t nrListeners,
                 std::vector<int> &wakeupFDs, uint8_t numPriorities)
    // EFD_NONBLOCK to prevent blocking reads/writes
    : listenerInfos_(std::max(nrListeners, 1U)), name_(name), workers(workers) {
	nrListeners = std::max(nrListeners, 1U);  // Ensure at least one listener

	if (wakeupFDs.size() != nrListeners) {
		safs::log_warn(
		    "JobPool: wakeupFDs size {} does not match nrListeners {}, resizing to match.",
		    wakeupFDs.size(), nrListeners);
		wakeupFDs.resize(nrListeners);
	}

	for (uint32_t i = 0; i < nrListeners; ++i) {
		listenerInfos_[i].notifierFD = ::eventfd(0, EFD_NONBLOCK);
		if (listenerInfos_[i].notifierFD < 0) {
			throw std::runtime_error("JobPool: eventfd() failed for listener" +
			                         std::to_string(i) + ": " + std::string(strerror(errno)));
		}
		wakeupFDs[i] = listenerInfos_[i].notifierFD;

		listenerInfos_[i].nextJobId = 1;
	}

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

	for (auto &listenerInfo : listenerInfos_) { close(listenerInfo.notifierFD); }
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

	for (size_t i = 0; i < listenerInfos_.size(); ++i) {
		if (!listenerInfos_[i].statusQueue.empty()) { processCompletedJobs(i); }
	}
}

uint32_t JobPool::addJob(ChunkOperation operation, JobCallback callback, void *extra,
                         ProcessJobCallback processJob, uint32_t listenerId) {
	// Check if the listenerId is valid
	if (listenerId >= listenerInfos_.size()) {
		safs::log_warn("JobPool: {}: Invalid listenerId {} for operation {}, resetting to 0",
		               __func__, listenerId, static_cast<int>(operation));
		listenerId = 0;  // Reset to the first listener
	}

	auto &listenerInfo = listenerInfos_[listenerId];
	std::unique_lock lock(listenerInfo.jobsMutex);
	uint32_t jobId = listenerInfo.nextJobId++;
	auto job = std::make_unique<Job>();
	job->jobId = jobId;
	job->callback = std::move(callback);
	job->processJob = std::move(processJob);
	job->extra = extra;
	job->state = JobPool::State::Enabled;
	job->listenerId = listenerId;
	listenerInfo.jobHash[jobId] = std::move(job);

	putToJobQueue(jobId, operation, reinterpret_cast<uint8_t *>(listenerInfo.jobHash[jobId].get()));
	unprocessedJobs_.fetch_add(1, std::memory_order_relaxed);
	return jobId;
}

bool JobPool::allJobsProcessed() const {
	return unprocessedJobs_.load(std::memory_order_relaxed) == 0;
}

bool JobPool::isEmpty() {
	if (jobsQueue->elements() > 0) { return false; }

	for (auto &listenerInfo : listenerInfos_) {
		std::lock_guard lock(listenerInfo.notifierMutex);
		if (!listenerInfo.statusQueue.empty()) { return false; }
	}
	return true;
}

uint32_t JobPool::getJobCount() const {
	TRACETHIS();
	return jobsQueue->elements();
}

bool JobPool::isFull() const {
	return jobsQueue->isFull();
}

void JobPool::disableAndChangeCallbackAll(const JobCallback &callback, uint32_t listenerId) {
	// Check if the listenerId is valid
	if (listenerId >= listenerInfos_.size()) {
		safs::log_warn("JobPool: disableAndChangeCallbackAll: Invalid listenerId {}, returning",
		               listenerId);
		return;
	}

	auto &listenerInfo = listenerInfos_[listenerId];
	std::lock_guard jobsLockGuard(listenerInfo.jobsMutex);
	for (auto &[jobId, job] : listenerInfo.jobHash) {
		if (job->state == JobPool::State::Enabled) { job->state = JobPool::State::Disabled; }
		job->callback = callback;
	}
}

void JobPool::disableJob(uint32_t jobId, uint32_t listenerId) {
	// Check if the listenerId is valid
	if (listenerId >= listenerInfos_.size()) {
		safs::log_warn("JobPool: disableJob: Invalid listenerId {} for jobId {}, returning",
		               listenerId, jobId);
		return;
	}

	auto &listenerInfo = listenerInfos_[listenerId];
	std::unique_lock jobsUniqueLock(listenerInfo.jobsMutex, std::defer_lock);
	auto jobIterator = listenerInfo.jobHash.find(jobId);
	if (jobIterator != listenerInfo.jobHash.end()) {
		jobsUniqueLock.lock();
		if (jobIterator->second->state == JobPool::State::Enabled) {
			jobIterator->second->state = JobPool::State::Disabled;
		}
		jobsUniqueLock.unlock();
	}
}

std::list<uint32_t> JobPool::disableJobs(const std::list<uint32_t> &jobIds, uint32_t listenerId) {
	// Check if the listenerId is valid
	std::list<uint32_t> disabledJobIds;
	if (listenerId >= listenerInfos_.size()) {
		safs::log_warn("JobPool: disableJobs: Invalid listenerId {}, returning",
		               listenerId);
		return disabledJobIds;
	}

	if (jobIds.empty()) { return disabledJobIds; }  // to save the locking
	auto &listenerInfo = listenerInfos_[listenerId];
	std::unique_lock jobsUniqueLock(listenerInfo.jobsMutex);
	for (auto jobId : jobIds) {
		auto jobIterator = listenerInfo.jobHash.find(jobId);
		if (jobIterator != listenerInfo.jobHash.end()) {
			if (jobIterator->second->state == JobPool::State::Enabled) {
				jobIterator->second->state = JobPool::State::Disabled;
				disabledJobIds.push_back(jobId);
			}
		}
	}
	return disabledJobIds;
}

void JobPool::processCompletedJobs(uint32_t listenerId) {
	// Check if the listenerId is valid
	if (listenerId >= listenerInfos_.size()) {
		safs::log_warn("JobPool: processCompletedJobs: Invalid listenerId {}, returning",
		               listenerId);
		return;
	}

	uint32_t jobId{};
	uint8_t status{};
	bool notLastJob = true;
	auto &listenerInfo = listenerInfos_[listenerId];
	while (notLastJob) {
		notLastJob = receiveStatus(jobId, status, listenerId);
		auto jobIterator = listenerInfo.jobHash.find(jobId);
		if (jobIterator != listenerInfo.jobHash.end()) {
			auto callback = jobIterator->second->callback;
			if (callback) { callback(status, jobIterator->second->extra); }
			listenerInfo.jobHash.erase(jobIterator);
			unprocessedJobs_.fetch_sub(1, std::memory_order_relaxed);
		}
	}
}

void JobPool::changeCallback(uint32_t jobId, JobCallback callback, void *extra,
                             uint32_t listenerId) {
	// Check if the listenerId is valid
	if (listenerId >= listenerInfos_.size()) {
		safs::log_warn("JobPool: changeCallback: Invalid listenerId {} for jobId {}, returning",
		               listenerId, jobId);
		return;
	}

	auto &listenerInfo = listenerInfos_[listenerId];
	auto jobIterator = listenerInfo.jobHash.find(jobId);
	if (jobIterator != listenerInfo.jobHash.end()) {
		jobIterator->second->callback = std::move(callback);
		jobIterator->second->extra = extra;
	}
}

void JobPool::changeCallback(std::list<uint32_t> &jobIds, const JobCallback &callback,
                             uint32_t listenerId) {
	// Check if the listenerId is valid
	if (listenerId >= listenerInfos_.size()) {
		safs::log_warn("JobPool: changeCallback: Invalid listenerId {}, returning", listenerId);
		return;
	}

	auto &listenerInfo = listenerInfos_[listenerId];
	for (auto jobId : jobIds) {
		auto jobIterator = listenerInfo.jobHash.find(jobId);
		if (jobIterator != listenerInfo.jobHash.end()) { jobIterator->second->callback = callback; }
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
		std::unique_lock jobsUniqueLock(listenerInfos_[listenerId].jobsMutex);
		jobState = job->state;
		if (job->state == State::Enabled) { job->state = State::InProgress; }

		if (jobState == State::Disabled) {
			status = SAUNAFS_ERROR_NOTDONE;
			jobsUniqueLock.unlock();
			sendStatus(jobId, status, listenerId);
			continue;
		}

		jobsUniqueLock.unlock();

		if (test_job_faults::enabled()) { test_job_faults::holdIfArmed(jobId, clientFacing_); }

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
	// Check if the listenerId is valid
	if (listenerId >= listenerInfos_.size()) {
		safs::log_warn("JobPool: SendStatus: Invalid listenerId {} for jobId {}, returning",
		               listenerId, jobId);
		return;
	}

	auto &listenerInfo = listenerInfos_[listenerId];
	std::lock_guard statusLock(listenerInfo.notifierMutex);

	if (listenerInfo.statusQueue.empty()) {
		static constexpr eventfd_t dummyValue = 1;  // Dummy value to wake up the eventfd
		eassert(::eventfd_write(listenerInfo.notifierFD, dummyValue) == 0 &&
		        "JobPool: SendStatus: Failed to write to eventfd");
	}
	listenerInfo.statusQueue.emplace(jobId, status);
}

bool JobPool::receiveStatus(uint32_t &jobId, uint8_t &status, uint32_t listenerId) {
	// Check if the listenerId is valid
	if (listenerId >= listenerInfos_.size()) {
		safs::log_warn("JobPool: receiveStatus: Invalid listenerId {} for jobId {}, returning",
		               listenerId, jobId);
		return false; // Return false to indicate that we should stop processing
	}

	auto &listenerInfo = listenerInfos_[listenerId];
	std::lock_guard statusLock(listenerInfo.notifierMutex);

	std::tie(jobId, status) = listenerInfo.statusQueue.front();
	listenerInfo.statusQueue.pop();
	if (listenerInfo.statusQueue.empty()) {
		eventfd_t dummyEvent;  // Only to clear the eventfd
		eassert(::eventfd_read(listenerInfo.notifierFD, &dummyEvent) == 0 &&
		        "JobPool: ReceiveStatus: Failed to read from eventfd");
		return false;
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
	std::unique_lock lockedChunkLock(chunkToJobReplyMapMutex_);
	auto it = chunkToJobReplyMap_.find(chunkWithType);
	if (it != chunkToJobReplyMap_.end() && it->second.writeInitReceived) {
		// Chunk is locked, store the job to be added later
		auto &lockedChunkData = it->second;

		lockedChunkData.pendingAddJobs.emplace_back(
		    [this, operation, extra, listenerId, callback = std::move(callback),
		     processJob = std::move(processJob)]() mutable -> uint32_t {
			    return addJob(operation, std::move(callback), extra, std::move(processJob),
			                  listenerId);
		    });

		return lockedChunkData.lockJobId;  // Lock guard is released here
	}
	lockedChunkLock.unlock();  // Release the lock before adding the job to avoid deadlocks

	// Chunk is not locked, add the job immediately
	return addJob(operation, std::move(callback), extra, std::move(processJob), listenerId);
}

uint32_t MasterJobPool::addLockJob(JobCallback callback, void *extra, uint32_t listenerId) {
	// Check if the listenerId is valid
	if (listenerId >= listenerInfos_.size()) {
		safs::log_warn("{} job pool: {}: Invalid listenerId {} for operation LOCK, resetting to 0",
		               name_, __func__, listenerId);
		listenerId = 0;  // Reset to the first listener
	}

	auto &listenerInfo = listenerInfos_[listenerId];
	std::unique_lock lock(listenerInfo.jobsMutex);
	uint32_t jobId = listenerInfo.nextJobId++;
	auto job = std::make_unique<Job>();
	job->jobId = jobId;
	job->callback = std::move(callback);
	// No processJob for lock jobs, as they are just markers for locked chunks
	job->extra = extra;
	job->state = JobPool::State::Enabled;
	job->listenerId = listenerId;
	listenerInfo.jobHash[jobId] = std::move(job);
	// Not an actual job, but a marker for a locked chunk, so never inserted into the job queue.
	return jobId;
}

void MasterJobPool::changeLockJobsCallback(const LockJobCallbackMaker &lockJobCallbackMaker,
                                           uint32_t listenerId) {
	// Check if the listenerId is valid
	if (listenerId >= listenerInfos_.size()) {
		safs::log_warn(
		    "{} job pool: {}: Invalid listenerId {} for changing lock jobs callback, resetting to 0",
		    name_, __func__, listenerId);
		listenerId = 0;  // Reset to the first listener
	}

	auto &listenerInfo = listenerInfos_[listenerId];
	std::scoped_lock lock(chunkToJobReplyMapMutex_, listenerInfo.jobsMutex);
	for (auto &[chunkWithType, lockedChunkData] : chunkToJobReplyMap_) {
		if (lockedChunkData.listenerId == listenerId) {
			auto jobIterator = listenerInfo.jobHash.find(lockedChunkData.lockJobId);
			if (jobIterator != listenerInfo.jobHash.end()) {
				jobIterator->second->callback = lockJobCallbackMaker(chunkWithType, listenerId);
			}
		}
	}
}

bool MasterJobPool::startChunkLock(const JobPool::JobCallback &callback, void *packet,
                                   uint64_t chunkId, ChunkPartType chunkType, uint32_t listenerId) {
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
	auto &listenerInfo = listenerInfos_[listenerId];
	std::unique_lock jobsUniqueLock(listenerInfo.jobsMutex);
	auto lockJobIterator = listenerInfo.jobHash.find(lockJobId);

	if (lockJobIterator != listenerInfo.jobHash.end()) {
		auto *outputPacket = reinterpret_cast<OutputPacket *>(lockJobIterator->second->extra);
		delete outputPacket;
		listenerInfo.jobHash.erase(lockJobIterator);
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

/// Tells the metadata servers that a part they sent a reader to is not here.
///
/// A read only reaches a chunkserver because a metadata server named it as holding the part, so
/// answering "no such chunk" is a statement about this server, not a guess about the cluster:
/// this process has no record of the part at all. That distinction is the whole point. Membership
/// may only shrink on an explicit statement, because a server that merely went quiet has said
/// nothing, and a copy removed on silence is a copy removed on every slow reconnect.
///
/// Only the read path reports. A write chain open can race a create the metadata server has
/// already commanded, so its absence is not yet a fact; a read has no such window, because a
/// reader is sent to a member of a set that was already published.
static void reportReadOfAbsentChunk(uint64_t chunkId, ChunkPartType chunkType, uint8_t status) {
	if (status != SAUNAFS_STATUS_OK && test_event_stream::enabled()) {
		test_event_stream::emit("read_part_failed",
		                        {{"chunk", chunkId},
		                         {"part_type", static_cast<uint64_t>(chunkType.getId())},
		                         {"status", status}});
	}
	if (status != SAUNAFS_ERROR_NOCHUNK) { return; }
	hddReportLostChunk(chunkId, chunkType);
	if (test_event_stream::enabled()) {
		test_event_stream::emit("read_absence_report_queued",
		                        {{"chunk", chunkId},
		                         {"part_type", static_cast<uint64_t>(chunkType.getId())}});
	}
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
			if (status != SAUNAFS_STATUS_OK) {
				reportReadOfAbsentChunk(chunkId, chunkType, status);
				return status;
			}
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
		reportReadOfAbsentChunk(chunkId, chunkType, status);
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

uint32_t job_verify_part(MasterJobPool &jobPool, JobPool::JobCallback callback, void *extra,
                         uint64_t chunkId, ChunkPartType chunkType, uint32_t listenerId) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		return hddVerifyPartPresence(chunkId, chunkType);
	};
	// Not addJobIfNotLocked: a chunk locked for work in progress is not an answer to whether the
	// part is here, and a refusal would be read as an absence.
	return jobPool.addJob(JobPool::ChunkOperation::VerifyPart, std::move(callback), extra,
	                      processJob, listenerId);
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
