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

#include "chunkserver/output_buffer.h"
#include "common/pcqueue.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

class JobPool {
public:
	enum class State : uint8_t {
		Disabled,
		Enabled,
		InProgress
	};

	enum ChunkOperation {
		Exit,
		Invalid,
		ChunkOp,
		Open,
		Close,
		Read,
		Prefetch,
		Write,
		Replicate,
		GetBlocks
	};

public:
	using JobCallback = std::function<void(uint8_t status, void *extra)>;

	JobPool(uint8_t workers, uint32_t maxJobs, int *wakeupDesc);
	~JobPool();

	uint32_t addJob(ChunkOperation operation, void *args, JobCallback callback, void *extra);
	uint32_t getJobCount() const;
	void disableAndChangeCallbackAll(const JobCallback& callback);
	void disableJob(uint32_t jobId);
	void checkJobs();
	void changeCallback(uint32_t jobId, JobCallback callback, void *extra);

private:
	struct Job {
		uint32_t jobId;
		JobCallback callback;
		void *extra;
		void *args;
		JobPool::State state;
	};

	void workerThread();
	void sendStatus(uint32_t jobId, uint8_t status);
	int receiveStatus(uint32_t &jobId, uint8_t &status);

	int rpipe, wpipe;
	uint8_t workers;
	std::vector<std::thread> workerThreads;
	std::mutex pipeMutex;
	std::mutex jobsMutex;
	std::unique_ptr<ProducerConsumerQueue> jobsQueue;
	std::unique_ptr<ProducerConsumerQueue> statusQueue;
	std::unordered_map<uint32_t, std::unique_ptr<Job>> jobHash;
	uint32_t nextJobId = 1;
};

uint32_t job_open(JobPool &jobPool, JobPool::JobCallback callback, void *extra,
                  uint64_t chunkId, ChunkPartType chunkType);

uint32_t job_close(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                   ChunkPartType chunkType);

uint32_t job_read(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                  uint32_t version, ChunkPartType chunkType, uint32_t offset, uint32_t size,
                  uint32_t maxBlocksToBeReadBehind, uint32_t blocksToBeReadAhead,
                  OutputBuffer *outputBuffer, bool performHddOpen);

uint32_t job_prefetch(JobPool &jobPool, uint64_t chunkId, uint32_t version, ChunkPartType chunkType,
                      uint32_t firstBlockToBePrefetched, uint32_t blocksToBePrefetched);

uint32_t job_write(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                   uint32_t chunkVersion, ChunkPartType chunkType, uint16_t blockNum,
                   uint32_t offset, uint32_t size, uint32_t crc, const uint8_t *buffer);

uint32_t job_get_blocks(JobPool &jobPool, JobPool::JobCallback callback, void *extra,
                        uint64_t chunkId, uint32_t version, ChunkPartType chunkType,
                        uint16_t *blocks);

uint32_t job_replicate(JobPool &jobPool, JobPool::JobCallback callback, void *extra,
                       uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                       uint32_t sourcesBufferSize, const uint8_t *sourcesBuffer);

uint32_t job_chunkop(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                     uint32_t chunkVersion, ChunkPartType chunkType, uint32_t newChunkVersion,
                     uint64_t copyChunkId, uint32_t copyChunkVersion, uint32_t length);

uint32_t job_invalid(JobPool &jobPool, JobPool::JobCallback callback, void *extra);

uint32_t job_delete(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                    uint32_t chunkVersion, ChunkPartType chunkType);

uint32_t job_create(JobPool &jobPool, JobPool::JobCallback callback, void *extra, uint64_t chunkId,
                    uint32_t chunkVersion, ChunkPartType chunkType);

uint32_t job_version(JobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                     uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                     uint32_t newChunkVersion);

uint32_t job_truncate(JobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                      uint64_t chunkId, ChunkPartType chunkType, uint32_t chunkVersion,
                      uint32_t newChunkVersion, uint32_t length);

uint32_t job_duplicate(JobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                       uint64_t chunkId, uint32_t chunkVersion, uint32_t newChunkVersion,
                       ChunkPartType chunkType, uint64_t chunkIdCopy, uint32_t chunkVersionCopy);

uint32_t job_duptrunc(JobPool &jobPool, const JobPool::JobCallback &callback, void *extra,
                      uint64_t chunkId, uint32_t chunkVersion, uint32_t newChunkVersion,
                      ChunkPartType chunkType, uint64_t chunkIdCopy, uint32_t chunkVersionCopy,
                      uint32_t length);

/* srcs: srccnt * (chunkid:64 chunkVersion:32 ip:32 port:16) */
uint32_t job_legacy_replicate(void *jpool,void (*callback)(uint8_t status,void *extra),void *extra,uint64_t chunkid,uint32_t chunkVersion,uint8_t srccnt,const uint8_t *srcs);
uint32_t job_legacy_replicate_simple(void *jpool,void (*callback)(uint8_t status,void *extra),void *extra,uint64_t chunkid,uint32_t chunkVersion,uint32_t ip,uint16_t port);
