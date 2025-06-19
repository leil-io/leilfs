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

#include "chunkserver-common/jobpool.h"
#include "common/massert.h"
#include "common/pcqueue.h"
#include "devtools/TracePrinter.h"
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
                 std::vector<int> &wakeupFDs)
    // EFD_NONBLOCK to prevent blocking reads/writes
    : listenerInfos_(nrListeners), name_(name), workers(workers) {
	nrListeners = std::max(nrListeners, 1u);  // Ensure at least one listener

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

		listenerInfos_[i].statusQueue = std::make_unique<ProducerConsumerQueue>();
		listenerInfos_[i].nextJobId = 1;
	}

	jobsQueue = std::make_unique<ProducerConsumerQueue>(maxJobs);

	for (uint8_t i = 0; i < workers; ++i) {
		workerThreads.emplace_back(&JobPool::workerThread, this, name_, i);
	}
}

JobPool::~JobPool() {
	for (uint8_t i = 0; i < workers; ++i) {
		jobsQueue->put(0, JobPool::ChunkOperation::Exit, nullptr, 1);
	}

	for (auto &thread : workerThreads) {
		if (thread.joinable()) { thread.join(); }
	}

	for (size_t i = 0; i < listenerInfos_.size(); ++i) {
		if (!listenerInfos_[i].statusQueue->isEmpty()) { processCompletedJobs(i); }
	}

	jobsQueue.reset();
	for (auto &listenerInfo : listenerInfos_) { listenerInfo.statusQueue.reset(); }

	workerThreads.clear();

	for (auto &listenerInfo : listenerInfos_) { close(listenerInfo.notifierFD); }
}

uint32_t JobPool::addJob(ChunkOperation operation, JobCallback callback, void *extra,
                         ProcessJobCallback processJob, uint32_t listenerId) {
	// Check if the listenerId is valid
	if (listenerId >= listenerInfos_.size()) {
		safs::log_warn("JobPool: addJob: Invalid listenerId {} for operation {}, resetting to 0",
		               listenerId, static_cast<int>(operation));
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
	jobsQueue->put(jobId, operation, reinterpret_cast<uint8_t *>(listenerInfo.jobHash[jobId].get()),
	               1);
	return jobId;
}

uint32_t JobPool::getJobCount() const {
	TRACETHIS();
	return jobsQueue->elements();
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

void JobPool::disableJobs(std::list<uint32_t> &jobIds, uint32_t listenerId) {
	// Check if the listenerId is valid
	if (listenerId >= listenerInfos_.size()) {
		safs::log_warn("JobPool: disableJobs: Invalid listenerId {}, returning",
		               listenerId);
		return;
	}

	if (jobIds.empty()) { return; }  // to save the locking
	auto &listenerInfo = listenerInfos_[listenerId];
	std::unique_lock jobsUniqueLock(listenerInfo.jobsMutex);
	for (auto jobId : jobIds) {
		auto jobIterator = listenerInfo.jobHash.find(jobId);
		if (jobIterator != listenerInfo.jobHash.end()) {
			if (jobIterator->second->state == JobPool::State::Enabled) {
				jobIterator->second->state = JobPool::State::Disabled;
			}
		}
	}
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

void JobPool::changeCallback(std::list<uint32_t> &jobIds, JobCallback callback, void *extra,
                             uint32_t listenerId) {
	// Check if the listenerId is valid
	if (listenerId >= listenerInfos_.size()) {
		safs::log_warn("JobPool: changeCallback: Invalid listenerId {}, returning",
		               listenerId);
		return;
	}

	auto &listenerInfo = listenerInfos_[listenerId];
	for (auto jobId : jobIds) {
		auto jobIterator = listenerInfo.jobHash.find(jobId);
		if (jobIterator != listenerInfo.jobHash.end()) {
			jobIterator->second->callback = std::move(callback);
			jobIterator->second->extra = extra;
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
		jobsQueue->get(&jobId, &operation, &jobPtrArg, nullptr);

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

	if (listenerInfo.statusQueue->isEmpty()) {
		static constexpr eventfd_t dummyValue = 1;  // Dummy value to wake up the eventfd
		eassert(::eventfd_write(listenerInfo.notifierFD, dummyValue) == 0 &&
		        "JobPool: SendStatus: Failed to write to eventfd");
	}
	listenerInfo.statusQueue->put(jobId, status, nullptr, 1);
}

bool JobPool::receiveStatus(uint32_t &jobId, uint8_t &status, uint32_t listenerId) {
	// Check if the listenerId is valid
	if (listenerId >= listenerInfos_.size()) {
		safs::log_warn("JobPool: receiveStatus: Invalid listenerId {} for jobId {}, returning",
		               listenerId, jobId);
		return false; // Return false to indicate that we should stop processing
	}

	auto &listenerInfo = listenerInfos_[listenerId];
	uint32_t qstatus = 0;
	std::lock_guard statusLock(listenerInfo.notifierMutex);

	listenerInfo.statusQueue->get(&jobId, &qstatus, nullptr, nullptr);
	status = qstatus;
	if (listenerInfo.statusQueue->isEmpty()) {
		eventfd_t dummyEvent;  // Only to clear the eventfd
		eassert(::eventfd_read(listenerInfo.notifierFD, &dummyEvent) == 0 &&
		        "JobPool: ReceiveStatus: Failed to read from eventfd");
		return false;
	}

	return true;
}
