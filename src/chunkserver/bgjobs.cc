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

#include <cerrno>
#include <cinttypes>
#include <climits>
#include <pthread.h>
#include <cstdlib>
#include <cstring>
#include <syslog.h>
#include <unistd.h>
#include <cassert>
#include <cstdint>
#include <mutex>

#include "chunkserver/chunk_replicator.h"
#include "chunkserver/hddspacemgr.h"
#include "common/chunk_part_type.h"
#include "common/chunk_type_with_address.h"
#include "common/massert.h"
#include "common/pcqueue.h"
#include "devtools/TracePrinter.h"
#include "devtools/request_log.h"

#define JHASHSIZE 0x400
#define JHASHPOS(id) ((id)&0x3FF)

enum {
	JSTATE_DISABLED,
	JSTATE_ENABLED,
	JSTATE_INPROGRESS
};

enum {
	OP_EXIT,
	OP_INVAL,
	OP_CHUNKOP,
	OP_OPEN,
	OP_CLOSE,
	OP_READ,
	OP_PREFETCH,
	OP_WRITE,
	OP_REPLICATE,
	OP_GET_BLOCKS
};

// for OP_CHUNKOP
struct chunk_chunkop_args {
	uint64_t chunkid,copychunkid;
	uint32_t version,newversion,copyversion;
	uint32_t length;
	ChunkPartType chunkType;
};

// for OP_OPEN and OP_CLOSE
struct chunk_open_and_close_args {
	uint64_t chunkid;
	ChunkPartType chunkType;
};

// for OP_READ
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

// for OP_PREFETCH
struct chunk_prefetch_args {
	uint64_t chunkid;
	uint32_t version;
	ChunkPartType chunkType;
	uint32_t firstBlock;
	uint32_t nrOfBlocks;
};

// for OP_WRITE
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

struct job {
	uint32_t jobid;
	void (*callback)(uint8_t status,void *extra);
	void *extra;
	void *args;
	uint8_t jstate;
	job *next;
};

struct jobpool {
	int rpipe,wpipe;
	uint8_t workers;
	pthread_t *workerthreads;
	std::mutex pipeMutex;
	std::mutex jobsMutex;
	std::unique_ptr<ProducerConsumerQueue> jobsQueue;
	std::unique_ptr<ProducerConsumerQueue> statusQueue;
	job* jobhash[JHASHSIZE];
	uint32_t nextjobid = 1;

	jobpool(int rpipe, int wpipe, uint8_t workers, uint32_t maxJobs)
	    : rpipe(rpipe), wpipe(wpipe), workers(workers) {
		workerthreads = (pthread_t *)malloc(sizeof(pthread_t) * workers);
		passert(workerthreads);

		jobsQueue = std::make_unique<ProducerConsumerQueue>(maxJobs);
		statusQueue = std::make_unique<ProducerConsumerQueue>();

		for (auto &job : jobhash) { job = nullptr; }
	};
};

static inline void job_send_status(jobpool *jp, uint32_t jobid, uint8_t status) {
	TRACETHIS2(jobid, (int)status);

	std::lock_guard pipeLockGuard(jp->pipeMutex);

	if (jp->statusQueue->isEmpty()) {   // first status
		eassert(write(jp->wpipe,&status,1)==1); // write anything to wake up select
	}
	jp->statusQueue->put(jobid, status, nullptr, 1);
}

static inline int job_receive_status(jobpool *jp,uint32_t *jobid,uint8_t *status) {
	TRACETHIS();
	uint32_t qstatus;

	std::lock_guard pipeLockGuard(jp->pipeMutex);

	jp->statusQueue->get(jobid, &qstatus, nullptr, nullptr);
	*status = qstatus;
	PRINTTHIS(*jobid);
	PRINTTHIS((int)*status);
	if (jp->statusQueue->isEmpty()) {
		eassert(read(jp->rpipe,&qstatus,1)==1); // make pipe empty
		return 0;       // last element
	}
	return 1;       // not last
}

void* job_worker(void *th_arg) {
	TRACETHIS();

	static std::atomic_uint16_t workersCounter(0);
	std::string threadName = "jobWorker " + std::to_string(workersCounter++);
	pthread_setname_np(pthread_self(), threadName.c_str());

	jobpool *jp = (jobpool*)th_arg;
	job *jptr;
	uint8_t *jptrarg;
	uint8_t status, jstate;
	uint32_t jobid;
	uint32_t op;

	std::unique_lock jobsUniqueLock(jp->jobsMutex, std::defer_lock);

	for (;;) {
		jp->jobsQueue->get(&jobid, &op, &jptrarg, nullptr);
		jptr = (job*)jptrarg;
		PRINTTHIS(op);
		jobsUniqueLock.lock();
		if (jptr!=NULL) {
			jstate=jptr->jstate;
			if (jptr->jstate==JSTATE_ENABLED) {
				jptr->jstate=JSTATE_INPROGRESS;
			}
		} else {
			jstate=JSTATE_DISABLED;
		}
		jobsUniqueLock.unlock();
		switch (op) {
			case OP_INVAL:
				status = SAUNAFS_ERROR_EINVAL;
				break;
			case OP_CHUNKOP:
			{
				auto opargs = (chunk_chunkop_args*)(jptr->args);
				if (jstate==JSTATE_DISABLED) {
					status = SAUNAFS_ERROR_NOTDONE;
				} else {
					status = hddChunkOperation(opargs->chunkid, opargs->version, opargs->chunkType,
							opargs->newversion, opargs->copychunkid, opargs->copyversion,
							opargs->length);
				}
				break;
			}
			case OP_OPEN:
			{
				auto ocargs = (chunk_open_and_close_args*)(jptr->args);
				if (jstate==JSTATE_DISABLED) {
					status = SAUNAFS_ERROR_NOTDONE;
				} else {
					status = hddOpen(ocargs->chunkid, ocargs->chunkType);
				}
				break;
			}
			case OP_CLOSE:
			{
				auto ocargs = (chunk_open_and_close_args*)(jptr->args);
				if (jstate==JSTATE_DISABLED) {
					status = SAUNAFS_ERROR_NOTDONE;
				} else {
					status = hddClose(ocargs->chunkid, ocargs->chunkType);
				}
				break;
			}
			case OP_READ:
			{
				auto rdargs = (chunk_read_args*)(jptr->args);
				if (jstate==JSTATE_DISABLED) {
					status = SAUNAFS_ERROR_NOTDONE;
					break;
				}
				LOG_AVG_TILL_END_OF_SCOPE0("job_read");
				if (rdargs->performHddOpen) {
					status = hddOpen(rdargs->chunkid, rdargs->chunkType);
					if (status != SAUNAFS_STATUS_OK) {
						break;
					}
				}

				status = hddRead(rdargs->chunkid, rdargs->version, rdargs->chunkType,
						rdargs->offset, rdargs->size, rdargs->maxBlocksToBeReadBehind,
						rdargs->blocksToBeReadAhead, rdargs->outputBuffer);

				rdargs->outputBuffer->isReadCompleted = true;

				if (rdargs->performHddOpen && status != SAUNAFS_STATUS_OK) {
					int ret = hddClose(rdargs->chunkid, rdargs->chunkType);
					if (ret != SAUNAFS_STATUS_OK) {
						safs_silent_syslog(LOG_ERR,
								"read job: cannot close chunk after read error (%s): %s",
								saunafs_error_string(status),
								saunafs_error_string(ret));
					}
				}
				break;
			}
			case OP_PREFETCH:
			{
				auto prefetchArgs = (chunk_prefetch_args*)(jptr->args);
				status = hddPrefetchBlocks(prefetchArgs->chunkid, prefetchArgs->chunkType,
						prefetchArgs->firstBlock, prefetchArgs->nrOfBlocks);
				break;
			}
			case OP_WRITE:
			{
				auto wrargs = (chunk_write_args*)(jptr->args);
				if (jstate==JSTATE_DISABLED) {
					status = SAUNAFS_ERROR_NOTDONE;
				} else {
				    status = hddChunkWriteBlock(
				        wrargs->chunkId, wrargs->chunkVersion,
				        wrargs->chunkType, wrargs->blocknum, wrargs->offset,
				        wrargs->size, wrargs->crc, wrargs->buffer);
				}
				if (status != SAUNAFS_STATUS_OK) {
						safs::log_err("Failed to write chunk id {}: {}", wrargs->chunkId, saunafs_error_string(status));
				}
				break;
			}
			case OP_GET_BLOCKS:
			{
				auto gbargs = (chunk_get_blocks_args*)(jptr->args);
				if (jstate == JSTATE_DISABLED) {
					status = SAUNAFS_ERROR_NOTDONE;
				} else {
					status = hddChunkGetNumberOfBlocks(gbargs->chunkId, gbargs->chunkType,
							gbargs->chunkVersion, gbargs->blocks);
				}
				break;
			}
			case OP_REPLICATE:
			{
				auto rpargs = (chunk_replication_args*)(jptr->args);
				if (jstate==JSTATE_DISABLED) {
					status = SAUNAFS_ERROR_NOTDONE;
				} else {
					try {
						std::vector<ChunkTypeWithAddress> sources;
						deserialize(rpargs->sourcesBuffer, rpargs->sourcesBufferSize, sources);
						ChunkFileCreator creator(
								rpargs->chunkId, rpargs->chunkVersion, rpargs->chunkType);
						gReplicator.replicate(creator, sources);
						status = SAUNAFS_STATUS_OK;
					} catch (Exception& ex) {
						safs_pretty_syslog(LOG_WARNING, "replication error: %s", ex.what());
						status = ex.status();
					}
				}
				break;
			}
			default:
				return nullptr;
		}
		job_send_status(jp,jobid,status);
	}
}

static inline uint32_t job_new(jobpool *jp,uint32_t op,void *args,void (*callback)(uint8_t status,void *extra),void *extra) {
	TRACETHIS();
	uint32_t jobid = jp->nextjobid;
	uint32_t jhpos = JHASHPOS(jobid);
	job *jptr;
	jptr = (job*) malloc(sizeof(job));
	passert(jptr);
	jptr->jobid = jobid;
	jptr->callback = callback;
	jptr->extra = extra;
	jptr->args = args;
	jptr->jstate = JSTATE_ENABLED;
	jptr->next = jp->jobhash[jhpos];
	jp->jobhash[jhpos] = jptr;
	jp->jobsQueue->put(jobid, op, reinterpret_cast<uint8_t*>(jptr), 1);
	jp->nextjobid++;
	if (jp->nextjobid==0) {
		jp->nextjobid=1;
	}
	return jobid;
}

/* interface */

void* job_pool_new(uint8_t workers,uint32_t jobs,int *wakeupdesc) {
	TRACETHIS();
	int fd[2];
	uint32_t i;
	pthread_attr_t thattr;
	jobpool* jp;

	if (pipe(fd)<0) {
		return NULL;
	}
	jp = new jobpool(fd[0], fd[1], workers, jobs);
	passert(jp);
	*wakeupdesc = fd[0];
	zassert(pthread_attr_init(&thattr));
	zassert(pthread_attr_setstacksize(&thattr,0x100000));
	zassert(pthread_attr_setdetachstate(&thattr,PTHREAD_CREATE_JOINABLE));
	for (i=0 ; i<workers ; i++) {
		zassert(pthread_create(jp->workerthreads + i, &thattr, job_worker, jp));
	}
	zassert(pthread_attr_destroy(&thattr));
	return jp;
}

uint32_t job_pool_jobs_count(void *jpool) {
	TRACETHIS();
	jobpool* jp = (jobpool*)jpool;
	return jp->jobsQueue->elements();
}

void job_pool_disable_and_change_callback_all(void *jpool,void (*callback)(uint8_t status,void *extra)) {
	TRACETHIS();
	jobpool* jp = (jobpool*)jpool;
	uint32_t jhpos;
	job *jptr;

	std::lock_guard jobsLockGuard(jp->jobsMutex);

	for (jhpos = 0 ; jhpos<JHASHSIZE ; jhpos++) {
		for (jptr = jp->jobhash[jhpos] ; jptr ; jptr=jptr->next) {
			if (jptr->jstate==JSTATE_ENABLED) {
				jptr->jstate=JSTATE_DISABLED;
			}
			jptr->callback=callback;
		}
	}
}

void job_pool_disable_job(void *jpool,uint32_t jobid) {
	TRACETHIS();
	jobpool* jp = (jobpool*)jpool;
	uint32_t jhpos = JHASHPOS(jobid);
	job *jptr;

	std::unique_lock jobsUniqueLock(jp->jobsMutex, std::defer_lock);

	for (jptr = jp->jobhash[jhpos] ; jptr ; jptr=jptr->next) {
		if (jptr->jobid==jobid) {
			jobsUniqueLock.lock();
			if (jptr->jstate==JSTATE_ENABLED) {
				jptr->jstate=JSTATE_DISABLED;
			}
			jobsUniqueLock.unlock();
		}
	}
}

void job_pool_disable_jobs(void *jpool, std::list<uint32_t> &jobIds) {
	TRACETHIS();
	jobpool *jp = (jobpool *)jpool;
	job *jptr;

	std::unique_lock jobsUniqueLock(jp->jobsMutex, std::defer_lock);
	for (auto jobid : jobIds) {
		uint32_t jhpos = JHASHPOS(jobid);

		for (jptr = jp->jobhash[jhpos]; jptr; jptr = jptr->next) {
			if (jptr->jobid == jobid) {
				jobsUniqueLock.lock();
				if (jptr->jstate == JSTATE_ENABLED) { jptr->jstate = JSTATE_DISABLED; }
				jobsUniqueLock.unlock();
			}
		}
	}
}

void job_pool_change_callback(void *jpool,uint32_t jobid,void (*callback)(uint8_t status,void *extra),void *extra) {
	TRACETHIS();
	jobpool* jp = (jobpool*)jpool;
	uint32_t jhpos = JHASHPOS(jobid);
	job *jptr;
	for (jptr = jp->jobhash[jhpos] ; jptr ; jptr=jptr->next) {
		if (jptr->jobid==jobid) {
			jptr->callback=callback;
			jptr->extra=extra;
		}
	}
}

void job_pool_change_callback(void *jpool, std::list<uint32_t> &jobIds,
                              void (*callback)(uint8_t status, void *extra), void *extra) {
	TRACETHIS();
	jobpool *jp = (jobpool *)jpool;
	job *jptr;
	for (auto jobid : jobIds) {
		uint32_t jhpos = JHASHPOS(jobid);
		for (jptr = jp->jobhash[jhpos]; jptr; jptr = jptr->next) {
			if (jptr->jobid == jobid) {
				jptr->callback = callback;
				jptr->extra = extra;
			}
		}
	}
}

void job_pool_check_jobs(void *jpool) {
	TRACETHIS();
	jobpool* jp = (jobpool*)jpool;
	uint32_t jobid,jhpos;
	uint8_t status;
	int notlast;
	job **jhandle,*jptr;
	do {
		notlast = job_receive_status(jp,&jobid,&status);
		jhpos = JHASHPOS(jobid);
		jhandle = jp->jobhash+jhpos;
		while ((jptr = *jhandle)) {
			if (jptr->jobid==jobid) {
				if (jptr->callback) {
					jptr->callback(status,jptr->extra);
				}
				*jhandle = jptr->next;
				if (jptr->args) {
					free(jptr->args);
				}
				free(jptr);
				break;
			} else {
				jhandle = &(jptr->next);
			}
		}
	} while (notlast);
}

void job_pool_delete(void *jpool) {
	TRACETHIS();
	jobpool* jp = (jobpool*)jpool;
	uint32_t i;

	for (i = 0; i < jp->workers; i++) {
		jp->jobsQueue->put(0, OP_EXIT, nullptr, 1);
	}

	for (i = 0; i < jp->workers; i++) {
		zassert(pthread_join(jp->workerthreads[i], NULL));
	}

	sassert(jp->jobsQueue->isEmpty());

	if (!jp->statusQueue->isEmpty()) {
		job_pool_check_jobs(jp);
	}

	jp->jobsQueue.reset();
	jp->statusQueue.reset();
	free(jp->workerthreads);
	close(jp->rpipe);
	close(jp->wpipe);
	delete jp;
}

uint32_t job_inval(void *jpool,void (*callback)(uint8_t status,void *extra),void *extra) {
	TRACETHIS();
	jobpool* jp = (jobpool*)jpool;
	return job_new(jp,OP_INVAL,NULL,callback,extra);
}

uint32_t job_chunkop(void *jpool, void (*callback)(uint8_t status, void *extra), void *extra,
		uint64_t chunkid, uint32_t version, ChunkPartType chunkType, uint32_t newversion,
		uint64_t copychunkid, uint32_t copyversion, uint32_t length) {
	TRACETHIS();
	jobpool* jp = (jobpool*)jpool;
	chunk_chunkop_args *args;
	args = (chunk_chunkop_args*) malloc(sizeof(chunk_chunkop_args));
	passert(args);
	args->chunkid = chunkid;
	args->version = version;
	args->newversion = newversion;
	args->copychunkid = copychunkid;
	args->copyversion = copyversion;
	args->length = length;
	args->chunkType = chunkType;
	return job_new(jp,OP_CHUNKOP,args,callback,extra);
}

uint32_t job_open(void *jpool, void (*callback)(uint8_t status,void *extra), void *extra,
		uint64_t chunkid, ChunkPartType chunkType) {
	TRACETHIS();
	jobpool* jp = (jobpool*)jpool;
	chunk_open_and_close_args *args;
	args = (chunk_open_and_close_args*) malloc(sizeof(chunk_open_and_close_args));
	passert(args);
	args->chunkid = chunkid;
	args->chunkType = chunkType;
	return job_new(jp,OP_OPEN,args,callback,extra);
}

uint32_t job_close(void *jpool, void (*callback)(uint8_t status,void *extra), void *extra,
		uint64_t chunkid, ChunkPartType chunkType) {
	TRACETHIS();
	jobpool* jp = (jobpool*)jpool;
	chunk_open_and_close_args *args;
	args = (chunk_open_and_close_args*) malloc(sizeof(chunk_open_and_close_args));
	passert(args);
	args->chunkid = chunkid;
	args->chunkType = chunkType;
	return job_new(jp,OP_CLOSE,args,callback,extra);
}

uint32_t job_read(void *jpool, void (*callback)(uint8_t status, void *extra), void *extra,
		uint64_t chunkid, uint32_t version, ChunkPartType chunkType, uint32_t offset, uint32_t size,
		uint32_t maxBlocksToBeReadBehind, uint32_t blocksToBeReadAhead,
		OutputBuffer* outputBuffer, bool performHddOpen) {
	TRACETHIS();
	jobpool* jp = (jobpool*)jpool;
	chunk_read_args *args;
	args = (chunk_read_args*) malloc(sizeof(chunk_read_args));
	passert(args);
	args->chunkid = chunkid;
	args->version = version;
	args->chunkType = chunkType;
	args->offset = offset;
	args->size = size;
	args->maxBlocksToBeReadBehind = maxBlocksToBeReadBehind;
	args->blocksToBeReadAhead = blocksToBeReadAhead;
	args->outputBuffer = outputBuffer;
	args->performHddOpen = performHddOpen;
	return job_new(jp,OP_READ,args,callback,extra);
}

uint32_t job_prefetch(void *jpool, uint64_t chunkid, uint32_t version, ChunkPartType chunkType,
		uint32_t firstBlockToBePrefetched, uint32_t nrOfBlocksToBePrefetched) {
	TRACETHIS();
	jobpool* jp = (jobpool*)jpool;
	chunk_prefetch_args* args;
	args = (chunk_prefetch_args*) malloc(sizeof(chunk_prefetch_args));
	passert(args);
	args->chunkid = chunkid;
	args->version = version;
	args->chunkType = chunkType;
	args->firstBlock = firstBlockToBePrefetched;
	args->nrOfBlocks = nrOfBlocksToBePrefetched;
	return job_new(jp,OP_PREFETCH, args, nullptr, nullptr);
}


uint32_t job_write(void *jpool, void (*callback)(uint8_t status, void *extra), void *extra,
		uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
		uint16_t blocknum, uint32_t offset, uint32_t size, uint32_t crc, const uint8_t *buffer) {
	TRACETHIS();
	jobpool* jp = (jobpool*)jpool;
	chunk_write_args *args;
	args = (chunk_write_args*) malloc(sizeof(chunk_write_args));
	passert(args);
	args->chunkId = chunkId;
	args->chunkVersion = chunkVersion;
	args->chunkType = chunkType,
	args->blocknum = blocknum;
	args->offset = offset;
	args->size = size;
	args->crc = crc;
	args->buffer = buffer;
	return job_new(jp, OP_WRITE, args, callback, extra);
}

uint32_t job_get_blocks(void *jpool, void (*callback)(uint8_t status, void *extra), void *extra,
		uint64_t chunkId, uint32_t version, ChunkPartType chunkType, uint16_t* blocks) {
	TRACETHIS();
	jobpool* jp = (jobpool*)jpool;
	chunk_get_blocks_args *args;
	args = (chunk_get_blocks_args*) malloc(sizeof(chunk_get_blocks_args));
	passert(args);
	args->chunkId = chunkId;
	args->chunkVersion = version;
	args->chunkType = chunkType;
	args->blocks = blocks;
	return job_new(jp, OP_GET_BLOCKS, args, callback, extra);
}

uint32_t job_replicate(void *jpool, void (*callback)(uint8_t status, void *extra), void *extra,
		uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
		uint32_t sourcesBufferSize, const uint8_t* sourcesBuffer) {
	TRACETHIS();
	jobpool* jp = (jobpool*)jpool;
	chunk_replication_args *args;
	// It's an ugly hack to allocate the memory for the structure and for the "sources" in a single
	// call, but as long as the whole 'args' are allocated with malloc I can't do much about it
	args = (chunk_replication_args*) malloc(sizeof(chunk_replication_args) + sourcesBufferSize);
	passert(args);
	args->chunkId = chunkId;
	args->chunkVersion = chunkVersion;
	args->chunkType = chunkType;
	args->sourcesBufferSize = sourcesBufferSize;

	// Ugly.
	args->sourcesBuffer = (uint8_t*)args + sizeof(chunk_replication_args);
	memcpy((void*)args->sourcesBuffer, (void*)sourcesBuffer, sourcesBufferSize);

	return job_new(jp, OP_REPLICATE, args, callback, extra);
}
