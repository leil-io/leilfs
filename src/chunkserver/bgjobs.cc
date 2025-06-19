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
#include "chunkserver-common/hdd_utils.h"
#include "chunkserver/chunk_replicator.h"
#include "chunkserver/hddspacemgr.h"
#include "common/chunk_part_type.h"
#include "common/chunk_type_with_address.h"
#include "devtools/request_log.h"
#include "slogger/slogger.h"

#include <sys/eventfd.h>
#include <sys/syslog.h>
#include <unistd.h>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

void disableJob(uint32_t jobId, uint32_t listenerId, uint64_t chunkId, ChunkPartType chunkType) {
	auto disk = hddChunkFindDisk(chunkId, chunkType);
	auto jobPool = static_cast<JobPool *>(disk->getWorkerPool());
	jobPool->disableJob(jobId, listenerId);
}

void disableJobs(std::list<uint32_t> &jobIds, uint32_t listenerId, uint64_t chunkId,
                 ChunkPartType chunkType) {
	auto disk = hddChunkFindDisk(chunkId, chunkType);
	auto jobPool = static_cast<JobPool *>(disk->getWorkerPool());
	jobPool->disableJobs(jobIds, listenerId);
}

void changeCallback(std::list<uint32_t> &jobIds, JobPool::JobCallback callback, void *extra,
                    uint32_t listenerId, uint64_t chunkId, ChunkPartType chunkType) {
	auto disk = hddChunkFindDisk(chunkId, chunkType);
	auto jobPool = static_cast<JobPool *>(disk->getWorkerPool());
	jobPool->changeCallback(jobIds, std::move(callback), extra, listenerId);
}

void changeCallback(uint32_t jobId, JobPool::JobCallback callback, void *extra, uint32_t listenerId,
                    uint64_t chunkId, ChunkPartType chunkType) {
	auto disk = hddChunkFindDisk(chunkId, chunkType);
	auto jobPool = static_cast<JobPool *>(disk->getWorkerPool());
	jobPool->changeCallback(jobId, std::move(callback), extra, listenerId);
}

uint32_t job_open(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                  ChunkPartType chunkType, uint32_t listenerId) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		return hddOpen(chunkId, chunkType);
	};
	return jobPool.addJob(JobPool::ChunkOperation::Open, std::move(callback), extra, processJob,
	                      listenerId);
}

uint32_t job_open(uint32_t listenerId, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                  ChunkPartType chunkType) {
	auto disk = hddChunkFindDisk(chunkId, chunkType);
	auto ret = job_open(*(static_cast<JobPool *>(disk->getWorkerPool())),
	                    std::move(callback), extra, chunkId, chunkType, listenerId);
	return ret;
}

uint32_t job_close(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                   ChunkPartType chunkType, uint32_t listenerId) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		return hddClose(chunkId, chunkType);
	};
	return jobPool.addJob(JobPool::ChunkOperation::Close, std::move(callback), extra, processJob,
	                      listenerId);
}

uint32_t job_close(uint32_t sourceId, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                   ChunkPartType chunkType) {
	auto disk = hddChunkFindDisk(chunkId, chunkType);
	auto ret = job_close(*(static_cast<JobPool *>(disk->getWorkerPool())),
	                     std::move(callback), extra, chunkId, chunkType, sourceId);
	return ret;
}

uint32_t job_read(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
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
		outputBuffer->setStatus(status);

		if (performHddOpen && status != SAUNAFS_STATUS_OK) {
			int ret = hddClose(chunkId, chunkType);
			if (ret != SAUNAFS_STATUS_OK) {
				safs::log_err("read job: cannot close chunk after read error ({}): {}",
				              saunafs_error_string(status), saunafs_error_string(ret));
			}
		}
		return status;
	};
	return jobPool.addJob(JobPool::ChunkOperation::Read, std::move(callback), extra, processJob,
	                      listenerId);
}

uint32_t job_read(uint32_t listenerId, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                  uint32_t version, ChunkPartType chunkType, uint32_t offset, uint32_t size,
                  uint32_t maxBlocksToBeReadBehind, uint32_t blocksToBeReadAhead,
                  OutputBuffer *outputBuffer, bool performHddOpen) {
	auto disk = hddChunkFindDisk(chunkId, chunkType);
	auto ret =
	    job_read(*(static_cast<JobPool *>(disk->getWorkerPool())), std::move(callback),
	             extra, chunkId, version, chunkType, offset, size, maxBlocksToBeReadBehind,
	             blocksToBeReadAhead, outputBuffer, performHddOpen, listenerId);
	return ret;
}

uint32_t job_prefetch(JobPool &jobPool, uint64_t chunkId, ChunkPartType chunkType,
                      uint32_t firstBlockToBePrefetched, uint32_t blocksToBePrefetched,
                      uint32_t listenerId) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		return hddPrefetchBlocks(chunkId, chunkType, firstBlockToBePrefetched,
		                         blocksToBePrefetched);
	};
	return jobPool.addJob(JobPool::ChunkOperation::Prefetch, kEmptyCallback, kEmptyExtra,
	                      processJob, listenerId);
}

uint32_t job_prefetch(uint32_t listenerId, uint64_t chunkId, ChunkPartType chunkType,
                      uint32_t firstBlockToBePrefetched, uint32_t blocksToBePrefetched) {
	auto disk = hddChunkFindDisk(chunkId, chunkType);
	auto ret = job_prefetch(*(static_cast<JobPool *>(disk->getWorkerPool())), chunkId,
	                        chunkType, firstBlockToBePrefetched, blocksToBePrefetched, listenerId);
	return ret;
}

uint32_t job_write(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                   uint32_t chunkVersion, ChunkPartType chunkType, uint16_t blockNum,
                   uint32_t offset, uint32_t size, uint32_t crc, const uint8_t *buffer,
                   uint32_t listenerId) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		auto status = hddChunkWriteBlock(chunkId, chunkVersion, chunkType, blockNum, offset, size,
		                                 crc, buffer);

		if (status != SAUNAFS_STATUS_OK) {
			safs::log_err("Failed to write chunk id {}: {}", chunkId, saunafs_error_string(status));
		}

		return status;
	};
	return jobPool.addJob(JobPool::ChunkOperation::Write, std::move(callback), extra, processJob,
	                      listenerId);
}

uint32_t job_write(uint32_t listenerId, JobPool::JobCallback callback, void *extra,
                   uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                   uint16_t blockNum, uint32_t offset, uint32_t size, uint32_t crc,
                   const uint8_t *buffer) {
	auto disk = hddChunkFindDisk(chunkId, chunkType);
	auto ret = job_write(*(static_cast<JobPool *>(disk->getWorkerPool())),
	                     std::move(callback), extra, chunkId, chunkVersion, chunkType, blockNum,
	                     offset, size, crc, buffer, listenerId);
	return ret;
}

uint32_t job_get_blocks(JobPool &jobPool, JobPool::JobCallback callback, void *extra,
                        uint64_t chunkId, uint32_t version, ChunkPartType chunkType,
                        uint16_t *blocks, uint32_t listenerId) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		return hddChunkGetNumberOfBlocks(chunkId, chunkType, version, blocks);
	};
	return jobPool.addJob(JobPool::ChunkOperation::GetBlocks, std::move(callback), extra,
	                      processJob, listenerId);
}

uint32_t job_get_blocks(uint32_t listenerId, JobPool::JobCallback callback, void *extra,
                        uint64_t chunkId, uint32_t version, ChunkPartType chunkType,
                        uint16_t *blocks) {
	auto disk = hddChunkFindDisk(chunkId, chunkType);
	auto ret = job_get_blocks(*(static_cast<JobPool *>(disk->getWorkerPool())),
	                           std::move(callback), extra, chunkId, version, chunkType, blocks,
	                           listenerId);
	return ret;
}

uint32_t job_replicate(JobPool &jobPool, JobPool::JobCallback callback, void *extra,
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

uint32_t job_invalid(JobPool &jobPool, JobPool::JobCallback callback, void *extra,
                     uint32_t listenerId) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t { return SAUNAFS_ERROR_EINVAL; };
	return jobPool.addJob(JobPool::ChunkOperation::Invalid, std::move(callback), extra, processJob,
	                      listenerId);
}

uint32_t job_delete(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                    uint32_t chunkVersion, ChunkPartType chunkType, uint32_t listenerId) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		return hddInternalDelete(chunkId, chunkVersion, chunkType);
	};
	return jobPool.addJob(JobPool::ChunkOperation::Delete, std::move(callback), extra, processJob,
	                      listenerId);
}

uint32_t job_create(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                    uint32_t chunkVersion, ChunkPartType chunkType, uint32_t listenerId) {
	JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
		return hddInternalCreate(chunkId, chunkVersion, chunkType);
	};
	return jobPool.addJob(JobPool::ChunkOperation::Create, std::move(callback), extra, processJob,
	                      listenerId);
}

uint32_t job_version(JobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                     uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                     uint32_t newChunkVersion, uint32_t listenerId) {
	if (newChunkVersion > 0) {
		JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
			return hddInternalUpdateVersion(chunkId, chunkVersion, newChunkVersion, chunkType);
		};
		return jobPool.addJob(JobPool::ChunkOperation::ChangeVersion, callback, extra, processJob,
		                      listenerId);
	}
	return job_invalid(jobPool, callback, extra, listenerId);
}

uint32_t job_truncate(JobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                      uint64_t chunkId, ChunkPartType chunkType, uint32_t chunkVersion,
                      uint32_t newChunkVersion, uint32_t length, uint32_t listenerId) {
	if (newChunkVersion > 0) {
		JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
			return hddTruncate(chunkId, chunkVersion, chunkType, newChunkVersion, length);
		};
		return jobPool.addJob(JobPool::ChunkOperation::Truncate, callback, extra, processJob,
		                      listenerId);
	}
	return job_invalid(jobPool, callback, extra, listenerId);
}

uint32_t job_duplicate(JobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                       uint64_t chunkId, uint32_t chunkVersion, uint32_t newChunkVersion,
                       ChunkPartType chunkType, uint64_t chunkIdCopy, uint32_t chunkVersionCopy,
                       uint32_t listenerId) {
	if (newChunkVersion > 0) {
		JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
			return hddDuplicate(chunkId, chunkVersion, newChunkVersion, chunkType, chunkIdCopy,
			                    chunkVersionCopy);
		};
		return jobPool.addJob(JobPool::ChunkOperation::Duplicate, callback, extra, processJob,
		                      listenerId);
	}
	return job_invalid(jobPool, callback, extra, listenerId);
}

uint32_t job_duptrunc(JobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                      uint64_t chunkId, uint32_t chunkVersion, uint32_t newChunkVersion,
                      ChunkPartType chunkType, uint64_t chunkIdCopy, uint32_t chunkVersionCopy,
                      uint32_t length, uint32_t listenerId) {
	if (newChunkVersion > 0) {
		JobPool::ProcessJobCallback processJob = [=]() -> uint8_t {
			return hddDuplicateTruncate(chunkId, chunkVersion, newChunkVersion, chunkType,
			                            chunkIdCopy, chunkVersionCopy, length);
		};
		return jobPool.addJob(JobPool::ChunkOperation::DuplicateTruncate, callback, extra,
		                      processJob, listenerId);
	}
	return job_invalid(jobPool, callback, extra, listenerId);
}
