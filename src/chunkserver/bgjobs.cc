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

#include "chunkserver/chunk_replicator.h"
#include "chunkserver/hddspacemgr.h"
#include "common/chunk_part_type.h"
#include "common/chunk_type_with_address.h"
#include "common/massert.h"
#include "common/pcqueue.h"
#include "devtools/TracePrinter.h"
#include "devtools/request_log.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <sys/syslog.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#define JHASHSIZE 0x400
#define JHASHPOS(id) ((id)&0x3FF)

constexpr auto kInvalidJob = nullptr;

JobPool::JobPool(uint8_t workers, uint32_t maxJobs, int *wakeupDesc) : workers(workers) {
	int fd[2];
	if (pipe(fd) < 0) { throw std::runtime_error("Failed to create pipe"); }
	rpipe = fd[0];
	wpipe = fd[1];

	*wakeupDesc = fd[0];

	jobsQueue = std::make_unique<ProducerConsumerQueue>(maxJobs);
	statusQueue = std::make_unique<ProducerConsumerQueue>();

	for (uint8_t i = 0; i < workers; ++i) {
		workerThreads.emplace_back(&JobPool::workerThread, this);
	}
}

JobPool::~JobPool() {
	for (uint8_t i = 0; i < workers; ++i) {
		jobsQueue->put(0, JobPool::ChunkOperation::Exit, nullptr, 1);
	}

	for (auto &thread : workerThreads) {
		if (thread.joinable()) { thread.join(); }
	}

	if (!statusQueue->isEmpty()) { checkJobs(); }

	jobsQueue.reset();
	statusQueue.reset();

	workerThreads.clear();

	close(rpipe);
	close(wpipe);
}

uint32_t JobPool::addJob(ChunkOperation operation, JobCallback callback, void *extra,
                         ProcessJobCallback processJob) {
	std::unique_lock lock(jobsMutex);
	uint32_t jobId = nextJobId++;
	auto job = std::make_unique<Job>();
	job->jobId = jobId;
	job->callback = std::move(callback);
	job->processJob = std::move(processJob);
	job->extra = extra;
	job->state = JobPool::State::Enabled;
	jobHash[jobId] = std::move(job);
	jobsQueue->put(jobId, operation, reinterpret_cast<uint8_t *>(jobHash[jobId].get()), 1);
	return jobId;
}

uint32_t JobPool::getJobCount() const {
	TRACETHIS();
	return jobsQueue->elements();
}

void JobPool::disableAndChangeCallbackAll(const JobCallback& callback) {
	std::lock_guard jobsLockGuard(jobsMutex);
	for (auto &[jobId, job] : jobHash) {
		if (job->state == JobPool::State::Enabled) { job->state = JobPool::State::Disabled; }
		job->callback = callback;
	}
}

void JobPool::disableJob(uint32_t jobId) {
	std::unique_lock jobsUniqueLock(jobsMutex, std::defer_lock);
	auto jobIterator = jobHash.find(jobId);
	if (jobIterator != jobHash.end()) {
		jobsUniqueLock.lock();
		if (jobIterator->second->state == JobPool::State::Enabled) {
			jobIterator->second->state = JobPool::State::Disabled;
		}
		jobsUniqueLock.unlock();
	}
}

void JobPool::checkJobs() {
	uint32_t jobId;
	uint8_t status;

	receiveStatus(jobId, status);

	auto jobIterator = jobHash.find(jobId);
	if (jobIterator != jobHash.end()) {
		auto callback = jobIterator->second->callback;
		if (callback) {
			callback(status, jobIterator->second->extra);
		}
		jobHash.erase(jobIterator);
	}
}

void JobPool::changeCallback(uint32_t jobId, JobCallback callback, void *extra) {
	std::unique_lock lock(jobsMutex);
	auto jobIterator = jobHash.find(jobId);
	if (jobIterator != jobHash.end()) {
		jobIterator->second->callback = std::move(callback);
		jobIterator->second->extra = extra;
	}
}

void JobPool::workerThread() {
	static std::atomic_uint16_t workersCounter(0);
	std::string threadName = "jobWorker " + std::to_string(workersCounter++);
	pthread_setname_np(pthread_self(), threadName.c_str());

	uint32_t jobId;
	uint32_t operation;
	uint8_t *jobPtrArg;
	State jobState;
	uint8_t status = SAUNAFS_STATUS_OK;

	std::unique_lock jobsUniqueLock(jobsMutex, std::defer_lock);

	while (true) {
		jobsQueue->get(&jobId, &operation, &jobPtrArg, nullptr);
		Job *job = reinterpret_cast<Job *>(jobPtrArg);

		jobsUniqueLock.lock();
		if (job == kInvalidJob) {
			jobState = State::Disabled;
		}
		else {
			jobState = job->state;
			if (job->state == State::Enabled) { job->state = State::InProgress; }
		}

		if (operation == ChunkOperation::Exit) { break; }

		if (jobState == State::Disabled) {
			status = SAUNAFS_ERROR_NOTDONE;
			sendStatus(jobId, status);
			continue;
		}

		jobsUniqueLock.unlock();

		auto processJobCallback = job->processJob;
		if (processJobCallback) {
			status = processJobCallback();
		} else {
			status = SAUNAFS_ERROR_NOTDONE;
		}

		sendStatus(jobId, status);
	}
}

void JobPool::sendStatus(uint32_t jobId, uint8_t status) {
	std::lock_guard pipeLockGuard(pipeMutex);
	if (statusQueue->isEmpty()) { eassert(write(wpipe, &status, 1) == 1); }
	statusQueue->put(jobId, status, nullptr, 1);
}

int JobPool::receiveStatus(uint32_t &jobId, uint8_t &status) {
	uint32_t qstatus = 0;
	std::lock_guard pipeLockGuard(pipeMutex);
	statusQueue->get(&jobId, &qstatus, nullptr, nullptr);
	status = qstatus;
	if (statusQueue->isEmpty()) {
		eassert(read(rpipe, &qstatus, 1) == 1);
		return 0;
	}
	return 1;
}

uint32_t job_open(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                  ChunkPartType chunkType) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		return hddOpen(chunkId, chunkType);
	};
	return jobPool.addJob(JobPool::ChunkOperation::Open, std::move(callback), extra, processJob);
}

uint32_t job_close(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
	ChunkPartType chunkType) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		return hddClose(chunkId, chunkType);
	};
	return jobPool.addJob(JobPool::ChunkOperation::Close, std::move(callback), extra, processJob);
}

uint32_t job_read(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                  uint32_t version, ChunkPartType chunkType, uint32_t offset, uint32_t size,
                  uint32_t maxBlocksToBeReadBehind, uint32_t blocksToBeReadAhead,
                  OutputBuffer *outputBuffer, bool performHddOpen) {
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
				safs_silent_syslog(LOG_ERR,
				                   "read job: cannot close chunk after read error (%s): %s",
				                   saunafs_error_string(status), saunafs_error_string(ret));
			}
		}
		return status;
	};
	return jobPool.addJob(JobPool::ChunkOperation::Read, std::move(callback), extra, processJob);
}

uint32_t job_prefetch(JobPool &jobPool, uint64_t chunkId, uint32_t  /*version*/, ChunkPartType chunkType,
	uint32_t firstBlockToBePrefetched, uint32_t blocksToBePrefetched) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		return hddPrefetchBlocks(chunkId, chunkType, firstBlockToBePrefetched,
		                         blocksToBePrefetched);
	};
	return jobPool.addJob(JobPool::ChunkOperation::Prefetch, nullptr, nullptr, processJob);
}

uint32_t job_write(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                   uint32_t chunkVersion, ChunkPartType chunkType, uint16_t blockNum,
                   uint32_t offset, uint32_t size, uint32_t crc, const uint8_t *buffer) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		auto status = hddChunkWriteBlock(chunkId, chunkVersion, chunkType, blockNum, offset, size,
		                                 crc, buffer);

		if (status != SAUNAFS_STATUS_OK) {
			safs::log_err("Failed to write chunk id {}: {}", chunkId, saunafs_error_string(status));
		}

		return status;
	};
	return jobPool.addJob(JobPool::ChunkOperation::Write, std::move(callback), extra, processJob);
}

uint32_t job_get_blocks(JobPool &jobPool, JobPool::JobCallback callback, void *extra,
                        uint64_t chunkId, uint32_t version, ChunkPartType chunkType,
                        uint16_t *blocks) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		return hddChunkGetNumberOfBlocks(chunkId, chunkType, version, blocks);
	};
	return jobPool.addJob(JobPool::ChunkOperation::GetBlocks, std::move(callback), extra,
	                      processJob);
}

uint32_t job_replicate(JobPool &jobPool, JobPool::JobCallback callback, void *extra,
                       uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                       uint32_t sourcesBufferSize, const uint8_t *sourcesBuffer) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		uint8_t status = SAUNAFS_STATUS_OK;
		try {
			std::vector<ChunkTypeWithAddress> sources;
			deserialize(sourcesBuffer, sourcesBufferSize, sources);
			ChunkFileCreator creator(chunkId, chunkVersion, chunkType);
			gReplicator.replicate(creator, sources);
		} catch (Exception &ex) {
			safs_pretty_syslog(LOG_WARNING, "replication error: %s", ex.what());
			status = ex.status();
		}
		return status;
	};
	return jobPool.addJob(JobPool::ChunkOperation::Replicate, std::move(callback), extra,
	                      processJob);
}

uint32_t job_invalid(JobPool &jobPool, JobPool::JobCallback callback, void *extra) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		return SAUNAFS_ERROR_EINVAL;
	};
	return jobPool.addJob(JobPool::ChunkOperation::Invalid, std::move(callback), extra, processJob);
}

uint32_t job_delete(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
	uint32_t chunkVersion, ChunkPartType chunkType) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		return hddInternalDelete(chunkId, chunkVersion, chunkType);
	};
	return jobPool.addJob(JobPool::ChunkOperation::Delete, std::move(callback), extra, processJob);
}

uint32_t job_create(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
	uint32_t chunkVersion, ChunkPartType chunkType) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		return hddInternalCreate(chunkId, chunkVersion, chunkType);
	};
	return jobPool.addJob(JobPool::ChunkOperation::Create, std::move(callback), extra, processJob);
}

uint32_t job_version(JobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                     uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                     uint32_t newChunkVersion) {
	if (newChunkVersion > 0) {
		JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
			return hddInternalUpdateVersion(chunkId, chunkVersion, newChunkVersion, chunkType);
		};
		return jobPool.addJob(JobPool::ChunkOperation::ChangeVersion, callback, extra, processJob);
	}
	return job_invalid(jobPool, callback, extra);
}

uint32_t job_truncate(JobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
	uint64_t chunkId, ChunkPartType chunkType, uint32_t chunkVersion,
	uint32_t newChunkVersion, uint32_t length) {
	if (newChunkVersion > 0) {
		JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
			return hddTruncate(chunkId, chunkVersion, chunkType, newChunkVersion, length);
		};
		return jobPool.addJob(JobPool::ChunkOperation::Truncate, callback, extra, processJob);
	}
	return job_invalid(jobPool, callback, extra);
}

uint32_t job_duplicate(JobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                       uint64_t chunkId, uint32_t chunkVersion, uint32_t newChunkVersion,
                       ChunkPartType chunkType, uint64_t chunkIdCopy, uint32_t chunkVersionCopy) {
	if (newChunkVersion > 0) {
		JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
			return hddDuplicate(chunkId, chunkVersion, newChunkVersion, chunkType, chunkIdCopy,
			                    chunkVersionCopy);
		};
		return jobPool.addJob(JobPool::ChunkOperation::Duplicate, callback, extra, processJob);
	}
	return job_invalid(jobPool, callback, extra);
}

uint32_t job_duptrunc(JobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                      uint64_t chunkId, uint32_t chunkVersion, uint32_t newChunkVersion,
                      ChunkPartType chunkType, uint64_t chunkIdCopy, uint32_t chunkVersionCopy,
                      uint32_t length) {
	if (newChunkVersion > 0) {
		JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
			return hddDuplicateTruncate(chunkId, chunkVersion, newChunkVersion, chunkType,
			                            chunkIdCopy, chunkVersionCopy, length);
		};
		return jobPool.addJob(JobPool::ChunkOperation::DuplicateTruncate, callback, extra,
		                      processJob);
	}
	return job_invalid(jobPool, callback, extra);
}
