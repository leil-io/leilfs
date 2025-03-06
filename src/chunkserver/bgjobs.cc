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


// for ChunkOp
struct chunk_chunkop_args {
	uint64_t chunkid,copychunkid;
	uint32_t version,newversion,copyversion;
	uint32_t length;
	ChunkPartType chunkType;
};

// for Open and Close
struct chunk_open_and_close_args {
	uint64_t chunkid;
	ChunkPartType chunkType;
};

// for Read
struct chunk_read_args {
	uint64_t chunkid;
	uint32_t version;
	ChunkPartType chunkType;
	uint32_t offset,size;
	uint8_t *crcbuff;
	uint32_t maxBlocksToBeReadBehind;
	uint32_t blocksToBeReadAhead;
	OutputBuffer* outputBuffer;
	bool performHddOpen;
};

// for Prefetch
struct chunk_prefetch_args {
	uint64_t chunkid;
	uint32_t version;
	ChunkPartType chunkType;
	uint32_t firstBlock;
	uint32_t nrOfBlocks;
};

// for Write
 struct chunk_write_args {
	uint64_t chunkId;
	uint32_t chunkVersion;
	ChunkPartType chunkType;
	uint16_t blocknum;
	uint32_t offset, size;
	uint32_t crc;
	const uint8_t *buffer;
};

struct chunk_get_blocks_args {
	uint64_t chunkId;
	uint32_t chunkVersion;
	ChunkPartType chunkType;
	uint16_t* blocks;
};

struct chunk_legacy_replication_args {
	uint64_t chunkid;
	uint32_t version;
	uint8_t srccnt;
};

struct chunk_replication_args {
	uint64_t chunkId;
	uint32_t chunkVersion;
	ChunkPartType chunkType;
	uint32_t sourcesBufferSize;
	uint8_t* sourcesBuffer;
};

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

uint32_t JobPool::addJob(ChunkOperation operation, void *args, JobCallback callback, void *extra) {
	std::unique_lock lock(jobsMutex);
	uint32_t jobId = nextJobId++;
	auto job = std::make_unique<Job>();
	job->jobId = jobId;
	job->callback = std::move(callback);
	job->extra = extra;
	job->args = args;
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
		switch (operation) {
		case ChunkOperation::Invalid:
		{
			status = SAUNAFS_ERROR_EINVAL;
			break;
		}
		case ChunkOperation::Delete:
		{
			auto args =
			    std::unique_ptr<chunk_chunkop_args>(static_cast<chunk_chunkop_args *>(job->args));
			status = hddInternalDelete(args->chunkid, args->version, args->chunkType);
			break;
		}
		case ChunkOperation::Create:
		{
			auto args =
			    std::unique_ptr<chunk_chunkop_args>(static_cast<chunk_chunkop_args *>(job->args));
			status = hddInternalCreate(args->chunkid, args->version, args->chunkType);
			break;
		}
		case ChunkOperation::ChangeVersion:
		{
			auto args =
			    std::unique_ptr<chunk_chunkop_args>(static_cast<chunk_chunkop_args *>(job->args));
			status = hddInternalUpdateVersion(args->chunkid, args->version, args->newversion,
			                                  args->chunkType);
			break;
		}
		case ChunkOperation::Truncate:
		{
			auto args =
			    std::unique_ptr<chunk_chunkop_args>(static_cast<chunk_chunkop_args *>(job->args));
			status = hddTruncate(args->chunkid, args->version, args->chunkType, args->newversion,
			                     args->length);
			break;
		}
		case ChunkOperation::Duplicate:
		{
			auto args =
			    std::unique_ptr<chunk_chunkop_args>(static_cast<chunk_chunkop_args *>(job->args));
			status = hddDuplicate(args->chunkid, args->version, args->newversion, args->chunkType,
			                      args->copychunkid, args->copyversion);
			break;
		}
		case ChunkOperation::DuplicateTruncate:
		{
			auto args =
			    std::unique_ptr<chunk_chunkop_args>(static_cast<chunk_chunkop_args *>(job->args));
			status = hddDuplicateTruncate(args->chunkid, args->version, args->newversion,
			                              args->chunkType, args->copychunkid, args->copyversion,
			                              args->length);
			break;
		}
		case ChunkOperation::Open:
		{
			auto args = std::unique_ptr<chunk_open_and_close_args>(
			    static_cast<chunk_open_and_close_args *>(job->args));
			status = hddOpen(args->chunkid, args->chunkType);
			break;
		}
		case ChunkOperation::Close:
		{
			auto args = std::unique_ptr<chunk_open_and_close_args>(
			    static_cast<chunk_open_and_close_args *>(job->args));
			status = hddClose(args->chunkid, args->chunkType);
			break;
		}
		case ChunkOperation::Read:
		{
			auto args = std::unique_ptr<chunk_read_args>(static_cast<chunk_read_args *>(job->args));
			LOG_AVG_TILL_END_OF_SCOPE0("job_read");
			if (args->performHddOpen) {
				status = hddOpen(args->chunkid, args->chunkType);
				if (status != SAUNAFS_STATUS_OK) { break; }
			}

			status = hddRead(args->chunkid, args->version, args->chunkType, args->offset,
			                 args->size, args->maxBlocksToBeReadBehind, args->blocksToBeReadAhead,
			                 args->outputBuffer);

			if (args->performHddOpen && status != SAUNAFS_STATUS_OK) {
				int ret = hddClose(args->chunkid, args->chunkType);
				if (ret != SAUNAFS_STATUS_OK) {
					safs_silent_syslog(LOG_ERR,
					                   "read job: cannot close chunk after read error (%s): %s",
					                   saunafs_error_string(status), saunafs_error_string(ret));
				}
			}
			break;
		}
		case ChunkOperation::Prefetch:
		{
			auto args =
			    std::unique_ptr<chunk_prefetch_args>(static_cast<chunk_prefetch_args *>(job->args));
			status = hddPrefetchBlocks(args->chunkid, args->chunkType, args->firstBlock,
			                           args->nrOfBlocks);
			break;
		}
		case ChunkOperation::Write:
		{
			auto args =
			    std::unique_ptr<chunk_write_args>(static_cast<chunk_write_args *>(job->args));
			status = hddChunkWriteBlock(args->chunkId, args->chunkVersion, args->chunkType,
			                            args->blocknum, args->offset, args->size, args->crc,
			                            args->buffer);
			if (status != SAUNAFS_STATUS_OK) {
				safs::log_err("Failed to write chunk id {}: {}", args->chunkId,
				              saunafs_error_string(status));
			}
			break;
		}
		case ChunkOperation::Replicate:
		{
			auto args = std::unique_ptr<chunk_replication_args>(
			    static_cast<chunk_replication_args *>(job->args));
			try {
				std::vector<ChunkTypeWithAddress> sources;
				deserialize(args->sourcesBuffer, args->sourcesBufferSize, sources);
				ChunkFileCreator creator(args->chunkId, args->chunkVersion, args->chunkType);
				gReplicator.replicate(creator, sources);
				status = SAUNAFS_STATUS_OK;
			} catch (Exception &ex) {
				safs_pretty_syslog(LOG_WARNING, "replication error: %s", ex.what());
				status = ex.status();
			}
			break;
		}
		case ChunkOperation::GetBlocks:
		{
			auto args = std::unique_ptr<chunk_get_blocks_args>(
			    static_cast<chunk_get_blocks_args *>(job->args));
			status = hddChunkGetNumberOfBlocks(args->chunkId, args->chunkType, args->chunkVersion,
			                                   args->blocks);
			break;
		}
		default:
			status = SAUNAFS_ERROR_EINVAL;
			break;
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
	auto args = std::make_unique<chunk_open_and_close_args>();
	args->chunkid = chunkId;
	args->chunkType = chunkType;
	return jobPool.addJob(JobPool::ChunkOperation::Open, args.release(), std::move(callback),
	                      extra);
}

uint32_t job_close(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
	ChunkPartType chunkType) {
	auto args = std::make_unique<chunk_open_and_close_args>();
	args->chunkid = chunkId;
	args->chunkType = chunkType;
	return jobPool.addJob(JobPool::ChunkOperation::Close, args.release(), std::move(callback),
	                      extra);
}

uint32_t job_read(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                  uint32_t version, ChunkPartType chunkType, uint32_t offset, uint32_t size,
                  uint32_t maxBlocksToBeReadBehind, uint32_t blocksToBeReadAhead,
                  OutputBuffer *outputBuffer, bool performHddOpen) {
	auto args = std::make_unique<chunk_read_args>();
	args->chunkid = chunkId;
	args->version = version;
	args->chunkType = chunkType;
	args->offset = offset;
	args->size = size;
	args->maxBlocksToBeReadBehind = maxBlocksToBeReadBehind;
	args->blocksToBeReadAhead = blocksToBeReadAhead;
	args->outputBuffer = outputBuffer;
	args->performHddOpen = performHddOpen;
	return jobPool.addJob(JobPool::ChunkOperation::Read, args.release(), std::move(callback),
	                      extra);
}

uint32_t job_prefetch(JobPool &jobPool, uint64_t chunkId, uint32_t version, ChunkPartType chunkType,
	uint32_t firstBlockToBePrefetched, uint32_t blocksToBePrefetched) {
	auto args = std::make_unique<chunk_prefetch_args>();
	args->chunkid = chunkId;
	args->version = version;
	args->chunkType = chunkType;
	args->firstBlock = firstBlockToBePrefetched;
	args->nrOfBlocks = blocksToBePrefetched;
	return jobPool.addJob(JobPool::ChunkOperation::Prefetch, args.release(), nullptr, nullptr);
}

uint32_t job_write(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                   uint32_t chunkVersion, ChunkPartType chunkType, uint16_t blockNum,
                   uint32_t offset, uint32_t size, uint32_t crc, const uint8_t *buffer) {
	auto args = std::make_unique<chunk_write_args>();
	args->chunkId = chunkId;
	args->chunkVersion = chunkVersion;
	args->chunkType = chunkType;
	args->blocknum = blockNum;
	args->offset = offset;
	args->size = size;
	args->crc = crc;
	args->buffer = buffer;
	return jobPool.addJob(JobPool::ChunkOperation::Write, args.release(), std::move(callback),
	                      extra);
}

uint32_t job_get_blocks(JobPool &jobPool, JobPool::JobCallback callback, void *extra,
                        uint64_t chunkId, uint32_t version, ChunkPartType chunkType,
                        uint16_t *blocks) {
	auto args = std::make_unique<chunk_get_blocks_args>();
	args->chunkId = chunkId;
	args->chunkVersion = version;
	args->chunkType = chunkType;
	args->blocks = blocks;
	return jobPool.addJob(JobPool::ChunkOperation::GetBlocks, args.release(), std::move(callback),
	                      extra);
}

uint32_t job_replicate(JobPool &jobPool, JobPool::JobCallback callback, void *extra,
                       uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                       uint32_t sourcesBufferSize, const uint8_t *sourcesBuffer) {
	auto args = std::make_unique<chunk_replication_args>();
	args->chunkId = chunkId;
	args->chunkVersion = chunkVersion;
	args->chunkType = chunkType;
	args->sourcesBufferSize = sourcesBufferSize;
	args->sourcesBuffer = new uint8_t[sourcesBufferSize];
	std::memcpy(args->sourcesBuffer, sourcesBuffer, sourcesBufferSize);
	return jobPool.addJob(JobPool::ChunkOperation::Replicate, args.release(), std::move(callback),
	                      extra);
}

uint32_t job_invalid(JobPool &jobPool, JobPool::JobCallback callback, void *extra) {
	return jobPool.addJob(JobPool::ChunkOperation::Invalid, nullptr, std::move(callback), extra);
}

uint32_t job_delete(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
	uint32_t chunkVersion, ChunkPartType chunkType) {
	auto args = std::make_unique<chunk_chunkop_args>();
	args->chunkid = chunkId;
	args->version = chunkVersion;
	args->chunkType = chunkType;
	return jobPool.addJob(JobPool::ChunkOperation::Delete, args.release(), std::move(callback),
	                      extra);
}

uint32_t job_create(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
	uint32_t chunkVersion, ChunkPartType chunkType) {
	auto args = std::make_unique<chunk_chunkop_args>();
	args->chunkid = chunkId;
	args->version = chunkVersion;
	args->chunkType = chunkType;
	return jobPool.addJob(JobPool::ChunkOperation::Create, args.release(), std::move(callback),
	                      extra);
}

uint32_t job_version(JobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                     uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                     uint32_t newChunkVersion) {
	if (newChunkVersion > 0) {
		auto args = std::make_unique<chunk_chunkop_args>();
		args->chunkid = chunkId;
		args->version = chunkVersion;
		args->newversion = newChunkVersion;
		args->chunkType = chunkType;
		return jobPool.addJob(JobPool::ChunkOperation::ChangeVersion, args.release(), callback,
		                      extra);
	}
	return job_invalid(jobPool, callback, extra);
}

uint32_t job_truncate(JobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
	uint64_t chunkId, ChunkPartType chunkType, uint32_t chunkVersion,
	uint32_t newChunkVersion, uint32_t length) {
	if (newChunkVersion > 0) {
		auto args = std::make_unique<chunk_chunkop_args>();
		args->chunkid = chunkId;
		args->version = chunkVersion;
		args->newversion = newChunkVersion;
		args->length = length;
		args->chunkType = chunkType;
		return jobPool.addJob(JobPool::ChunkOperation::Truncate, args.release(), callback, extra);
	}
	return job_invalid(jobPool, callback, extra);
}

uint32_t job_duplicate(JobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                       uint64_t chunkId, uint32_t chunkVersion, uint32_t newChunkVersion,
                       ChunkPartType chunkType, uint64_t chunkIdCopy, uint32_t chunkVersionCopy) {
	if (newChunkVersion > 0) {
		auto args = std::make_unique<chunk_chunkop_args>();
		args->chunkid = chunkId;
		args->version = chunkVersion;
		args->newversion = newChunkVersion;
		args->chunkType = chunkType;
		args->copychunkid = chunkIdCopy;
		args->copyversion = chunkVersionCopy;
		return jobPool.addJob(JobPool::ChunkOperation::Duplicate, args.release(), callback, extra);
	}
	return job_invalid(jobPool, callback, extra);
}

uint32_t job_duptrunc(JobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                      uint64_t chunkId, uint32_t chunkVersion, uint32_t newChunkVersion,
                      ChunkPartType chunkType, uint64_t chunkIdCopy, uint32_t chunkVersionCopy,
                      uint32_t length) {
	if (newChunkVersion > 0) {
		auto args = std::make_unique<chunk_chunkop_args>();
		args->chunkid = chunkId;
		args->version = chunkVersion;
		args->newversion = newChunkVersion;
		args->copychunkid = chunkIdCopy;
		args->copyversion = chunkVersionCopy;
		args->length = length;
		args->chunkType = chunkType;
		return jobPool.addJob(JobPool::ChunkOperation::DuplicateTruncate, args.release(), callback,
		                      extra);
	}
	return job_invalid(jobPool, callback, extra);
}
