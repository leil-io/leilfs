/*
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include <cstdint>
#include <list>

#include "chunkserver/chunkserver_entry.h"
#include "chunkserver/io_buffers.h"
#include "common/chunk_part_type.h"
#include "common/slice_traits.h"
#include "devtools/request_log.h"
#include "protocol/cltocs.h"

class ClientJobPool;

inline constexpr uint8_t kSauWriteDataPrefixSize = cltocs::writeData::kPrefixSize;
// For forwarding: size of SAU_CLTOCS_WRITE_DATA prefix plus the packet header.
inline constexpr uint8_t kSauWriteDataPrefixSizeForward =
    cltocs::writeData::kPrefixSize + PacketHeader::kSize;

/// @brief Base class for high-level chunk operations.
class HighLevelOp {
public:
	HighLevelOp(ChunkserverEntry *parent) : parent_(parent) {}

	/// Returns the number of pending delayed jobs.
	uint16_t pendingDelayedJobs() const { return pendingDelayedJobs_; }

	/// Returns whether the chunk is currently open.
	bool isChunkOpen() const { return isChunkOpen_; }

	/// Returns the ID of the chunk associated with the operation.
	uint64_t chunkId() const { return chunkId_; }

	/// Returns the type of the chunk associated with the operation.
	ChunkPartType chunkType() const { return chunkType_; }

protected:
	/// Gets the state of the parent ChunkserverEntry.
	ChunkserverEntry::State getParentState() const { return parent_->state; }

	/// Sets the state of the parent ChunkserverEntry.
	void setParentState(ChunkserverEntry::State newState) { parent_->state = newState; }

	/// Job pool of the network worker handling the parent ChunkserverEntry.
	ClientJobPool *workerJobPool() const { return parent_->workerJobPool; }

	/// Gets the parent pending write jobs counter.
	uint32_t &parentPendingWriteJobs() { return parent_->pendingWriteJobs; }

	/// Checks and applies closing on the parent ChunkserverEntry.
	void checkAndApplyClosedOnParent() const { parent_->checkAndApplyClosed(); }

	/// Whether the serving era that admitted this operation is still the era serving now.
	bool parentServingEraIsCurrent() const { return parent_->servingEraIsCurrent(); }

	/// The era this operation was admitted under, for the record a refusal leaves behind.
	uint64_t parentServingEra() const { return parent_->servingEra_; }

	/// Creates an attached packet on the parent ChunkserverEntry.
	void createAttachedPacket(MessageBuffer &packet) { parent_->createAttachedPacket(packet); }

	/// Attaches an output buffer to the parent ChunkserverEntry.
	void attachBuffer(std::shared_ptr<OutputBuffer> &&buffer) {
		parent_->attachBuffer(std::move(buffer));
	}

	/// Creates and attach write status packet on the parent ChunkserverEntry.
	void createAttachedWriteStatus(uint64_t targetChunkId, uint8_t status, uint32_t writeId) {
		parent_->createAttachedWriteStatus(targetChunkId, status, writeId);
	}

	uint64_t chunkId_ = 0;       ///< ID of the chunk associated with the operation
	uint32_t chunkVersion_ = 0;  ///< Version of the chunk associated with the operation
	/// Type of the chunk associated with the operation
	ChunkPartType chunkType_ = slice_traits::standard::ChunkPartType();
	uint16_t pendingDelayedJobs_ = 0;  ///< Number of remaining delayed jobs running
	bool isChunkOpen_ = false;         ///< Indicates if the chunk is open.

private:
	ChunkserverEntry *parent_;
};

/// @brief High-level operation for retrieving the number of chunk blocks.
class GetBlocksHighLevelOp : public HighLevelOp {
public:
	GetBlocksHighLevelOp(ChunkserverEntry *parent) : HighLevelOp(parent) {}

	/// Sets up and enqueues the get blocks high level operation.
	///
	/// @param chunkId ID of the chunk.
	/// @param chunkVersion Version of the chunk.
	/// @param chunkType Type of the chunk.
	void setup(uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType);

	/// Checks if the get blocks job is currently running.
	bool isRunning() const;

	/// Closes the get blocks job, assuming it is running.
	void delayedClose();

	/// Disables the queued get blocks job without closing anything. @see
	/// ReadHighLevelOp::cancelQueuedJobs.
	size_t cancelQueuedJobs();

protected:
	/// Callback after delayed close operations.
	void delayedCloseCallback(uint8_t status, void * /*entry*/);

	/// Callback for chunk block retrieval completion.
	void getChunkBlocksCallback(uint8_t status, void * /*entry*/);

	uint16_t getBlocksJobResult_ = 0;  ///< Result of the get blocks job
	uint32_t getBlocksJobId_ = 0;      ///< Current job ID for retrieving chunk blocks
};

/// @brief High-level operation for reading data from a chunk.
class ReadHighLevelOp : public HighLevelOp {
public:
	ReadHighLevelOp(ChunkserverEntry *parent, uint16_t maxBlocksPerHddReadJob,
	                uint16_t maxParallelHddReadJobs)
	    : HighLevelOp(parent),
	      maxBlocksPerHddReadJob_(maxBlocksPerHddReadJob),
	      maxParallelHddReadJobs_(maxParallelHddReadJobs) {}

	/// Checks if it can continue reading more data, and continues if so.
	void continueReadingIfPossible();

	/// Sets up and starts the read high level operation.
	/// @param chunkId ID of the chunk.
	/// @param chunkVersion Version of the chunk.
	/// @param chunkType Type of the chunk.
	/// @param offset Offset within the chunk to start reading from.
	/// @param size Size of data to read.
	void setup(uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType, uint32_t offset,
	           uint32_t size);

	/// Disables every queued read job, leaving the connection and this operation alone.
	///
	/// Unlike the close paths, this changes no callback and closes nothing: a disabled job
	/// comes back through the callback it already had, reporting that it was not done, and
	/// the read ends the way any failed read ends. That is what lets the cutoff stop work
	/// already inside the pool without taking the connection down with it, so the frame gates
	/// are still there to refuse whatever the client sends next.
	///
	/// @return How many jobs were disabled.
	size_t cancelQueuedJobs();

	/// Checks if there are jobs that need a delayed close.
	/// Discards pending read jobs.
	/// @return True if there are jobs that need a delayed close, false otherwise.
	bool prepareForDelayedClose();

	/// Performs the delayed close of already discarded read jobs.
	void delayedClose();

	/// Closes chunk if open.
	void cleanup();

protected:
	/// Callback after delayed close operations.
	void delayedCloseCallback(uint8_t status, void *buffer);

	/// Callback for when a discarded read operation finishes.
	void readDiscardCallback(uint8_t status, void *buffer);

	/// Callback for when a read operation finishes.
	void readFinishedCallback(uint8_t status, void *buffer);

	/// Prepares the discard of the current ongoing read operations.
	///
	/// It disables the jobs, changes the callback and moves the jobs from pending
	/// to discard lists.
	void prepareDiscardReadJobs();

	/// Prepares a read data buffer.
	///
	/// Creates the OutputBuffer to be used in the read operation.
	/// It is then provided with the headers of the blocks to be read.
	///
	/// @param readDataPrefix A buffer to store the read data prefix.
	/// @param jobSize The size of the job.
	/// @param jobOffset The offset of the job.
	/// @return A shared pointer to the prepared OutputBuffer.
	std::shared_ptr<OutputBuffer> prepareReadDataPacket(std::vector<uint8_t> &readDataPrefix,
	                                                    uint32_t jobSize, uint32_t jobOffset);

	/// Continues a previously started read operation.
	///
	/// Processes the remaining data to be read from the chunkserver. If all
	/// data has been read, it sends a read status message and closes the chunk.
	/// Otherwise, it prepares the next part of the read operation.
	///
	/// @param callMaxParallelHddReadJobs The maximum number of parallel HDD read for this call.
	/// @see ChunkserverEntry::readInit
	void readContinue(uint16_t callMaxParallelHddReadJobs);

	/// Number of blocks to read from the device in one read job.
	uint16_t maxBlocksPerHddReadJob_;
	uint16_t maxParallelHddReadJobs_;  ///< Maximum size of pendingReadDataBuffers.

	/// List of output buffers waiting for the HDD worker to finish, and then be sent.
	std::list<std::shared_ptr<OutputBuffer>> pendingReadDataBuffers_;
	std::list<uint32_t> pendingReadJobIds_;  ///< Job IDs for pending read operations.
	/// List of output buffers within a failing read operation, which are to be discarded.
	std::list<std::shared_ptr<OutputBuffer>> toDiscardReadDataBuffers_;
	std::list<uint32_t> toDiscardReadJobIds_;  ///< Job IDs for read operations to discard.
	uint32_t offset_ = 0;                      ///< Offset within the chunk for the operation.
	uint32_t size_ = 0;                        ///< Size of the current operation.

	LOG_AVG_TYPE readOperationTimer_;
};

/// @brief High-level operation for writing data to a chunk.
///
/// Contains the core write-path logic and state for all write variants. In its default form it
/// implements the standard, "eager" write path: write jobs are started as soon as a buffer is
/// ready (no client-side WRITE_FLUSH is expected).
///
/// One concrete derivation exists:
/// - `WriteHighLevelOpExpectFlush`: batched mode. Write jobs are deferred until the client sends
///   an explicit WRITE_FLUSH packet, at which point `flushData()` releases a batch of buffered
///   input packets for writing.
///
/// The differentiating behavior is expressed through a small set of virtual hooks (see the
/// `protected` section). Default implementations match the no-flush (eager) variant.
class WriteHighLevelOp : public HighLevelOp {
public:
	/// Disables the queued write job without closing anything. @see
	/// ReadHighLevelOp::cancelQueuedJobs.
	size_t cancelQueuedJobs();

	/// @brief Default initial number of blocks for the next input buffer, which can be adjusted
	/// based on the max blocks per HDD write job.
	constexpr static uint16_t kDefaultInitialNextInputBufferBlockCount = 4;

	WriteHighLevelOp(ChunkserverEntry *parent, uint16_t maxBlocksPerHddWriteJob)
	    : HighLevelOp(parent),
	      maxBlocksPerHddWriteJob_(maxBlocksPerHddWriteJob),
	      nextInputBufferBlockCount_(
	          std::min(kDefaultInitialNextInputBufferBlockCount, maxBlocksPerHddWriteJob)) {}

	virtual ~WriteHighLevelOp() = default;

	/// Sets up and starts the write high level operation.
	/// Enqueues a job_open for the chunk.
	/// @param chunkId ID of the chunk.
	/// @param chunkVersion Version of the chunk.
	/// @param chunkType Type of the chunk.
	void setup(uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType);

	/// Prepares for new write data to be added to the input buffer.
	/// @param mustForward Indicates if the data must be forwarded to another chunkserver.
	/// @param headerBuffer Buffer containing the header of the write data packet.
	void prepareForNewWriteData(bool mustForward, uint8_t *headerBuffer);

	/// Checks if there is a write job being processed.
	bool isWriteJobBeingProcessed() const;

	/// Update the structures considering the write status generated by a self write or a chain
	/// write. Schedules sending the write status to the client if needed.
	void updateUsingWriteStatusAndReply(uint8_t status, uint32_t writeId);

	/// Processes an already read write data block considering the provided parameters.
	/// @param blocknum Block number in the chunk.
	/// @param opOffset Offset in the block where the data starts.
	/// @param opSize Size of the data to be written.
	/// @param writeId Write identifier from client.
	/// @param crc CRC of the block.
	void processWriteDataBlock(uint16_t blocknum, uint32_t opOffset, uint32_t opSize,
	                           uint32_t writeId, uint32_t crc);

	/// Checks if the size of the last write operation header is valid.
	bool isLastHeaderSizeValid() const;

	/// Returns a pointer to the start of the last write operation header.
	uint8_t *getLastOperationHeader();

	/// Reads data into the input buffer from the socket.
	/// @param sock Socket to read from.
	/// @param bytesToRead Number of bytes to read.
	/// @return Number of bytes read.
	ssize_t readData(int sock, size_t bytesToRead);

	/// Writes data from the input buffer to the socket.
	/// @param sock Socket to write to.
	/// @param bytesToWrite Number of bytes to write.
	/// @return Number of bytes written.
	ssize_t writeData(int sock, size_t bytesToWrite);

	/// Attempts to seal the write operation. In order to seal the operation, all the write data
	/// blocks must have been replied (considering also the chain writes). If there is a not-nullptr
	/// input buffer it must also be replied, in which case it is moved to the list of write data
	/// buffers waiting to be written to the chunk.
	/// @return True if the operation was sealed, false otherwise.
	bool trySeal();

	/// Checks if the write operation is completed, considering that it is sealed and there are no
	/// write data buffers waiting to be written.
	bool isCompleted() const;

	/// Performs a delayed close of the write operation.
	/// Moves input buffer to its pool if not null.
	void delayedClose();

	/// Closes chunk if open.
	void cleanup();

	/// Tries to send an instant reply to the client if possible, considering the current state of
	/// the write operation and the amount of write buffering blocks available.
	/// Does not work if the operation is sealed, in delayed close, the open write job is still
	/// being processed or the chunk is not locked for writing.
	/// Does not violate the order of replies for the write data blocks received from the client,
	/// neither the amount of write buffering blocks available.
	/// @param fromFlushDataCall Indicates if the function is called from flushData.
	void tryInstantReply(bool fromFlushDataCall = false);

	/// Makes the current input buffer available for write jobs. Starts write jobs if not running
	/// one or schedules the write job to run ASAP.
	/// Default (no-flush) behavior logs a warning and drains the current input buffer
	/// opportunistically. The expect-flush variant overrides this with the real batching logic.
	virtual void flushData();

protected:
	// ---------------------------------------------------------------------------
	// Differentiating hooks. Defaults implement the no-flush ("eager") behavior.
	// Implementations live in chunk_high_level_ops.cc, paired with the matching
	// `WriteHighLevelOpExpectFlush` overrides for easy side-by-side comparison.
	// ---------------------------------------------------------------------------

	/// Whether a write job may be started right now. Encapsulates the full precondition set used
	/// by `writeCurrentInputBuffer()` and `continueWritingIfPossible()` to decide whether to launch the
	/// next job from queued buffers: there must be at least one queued buffer in
	/// `writeDataBuffers_` and no write job currently in flight. The expect-flush variant adds the
	/// extra requirement that a flush trigger has been received (unless batching has been turned
	/// off after a delayed close).
	virtual bool canStartWriteJobNow() const;

	/// Populates `inputBuffers` and sets `enqueuedInputBuffers_` for the next write job.
	/// Default takes only the first front buffer (single-buffer write).
	virtual void collectInputBuffersForNextJob(std::vector<InputBuffer *> &inputBuffers);

	/// Whether `processWriteDataBlock()` should freeze the current input buffer onto
	/// `writeDataBuffers_` (and possibly start a write job) immediately after appending the just-
	/// received block. Encapsulates the full precondition set: there must be a current input
	/// buffer (`inputBuffer_ != nullptr`) that is not still being updated. Default (eager) policy:
	/// freeze as soon as the buffer is full, or whenever there is no in-flight write job that
	/// would naturally drain it later.
	virtual bool shouldWriteCurrentInputBuffer() const;

	/// Whether `tryInstantReply()` should defer (no-op) the reply. Used by the expect-flush
	/// variant to delay instant replies until an explicit `flushData()` trigger.
	virtual bool shouldDeferInstantReply(bool fromFlushDataCall) const;

	/// Hook called from `delayedClose()` right before re-evaluating whether to start a write job.
	/// Derived classes use this to switch off flush batching and fall back to eager behavior, so
	/// that any remaining buffered data is drained one-by-one.
	virtual void onSwitchingToEagerWriteOnDelayedClose();

	/// Hook called from `cleanup()` to reset any flush-batching state.
	virtual void resetFlushBatchingState();

	/// Moves the current `inputBuffer_` onto `writeDataBuffers_`. Unlike `writeCurrentInputBuffer()`,
	/// this does NOT start a new write job; callers are responsible for triggering one if
	/// appropriate. Derived classes may override to perform additional bookkeeping (e.g. updating
	/// flush-batching counters).
	virtual void freezeCurrentInputBuffer();

	// ---------------------------------------------------------------------------
	// Shared protected methods.
	// ---------------------------------------------------------------------------

	/// Decreases the pending write jobs in the parent ChunkserverEntry to check and apply closed on
	/// parent. This is used while in delayed close mode whenever a callback is processed.
	void decreasePendingWriteJobsToCheckAndApplyClosedOnParent();

	/// Checks if there is an open write job being processed.
	bool isOpenWriteJobBeingProcessed() const;

	/// Sets that no write job is being processed.
	void setNoWriteJobBeingProcessed();

	/// Checks whether the current input buffer can be frozen
	bool isInputBufferFreezable() const;

	/// Starts an open write job for the current chunk.
	void startOpenWriteJob();

	/// Starts the next write job: no write job is being processed and there is at least
	/// one write data buffer waiting.
	void startNextWriteJob();

	/// Callback after delayed close operations.
	void delayedCloseCallback(uint8_t status, void * /*entry*/);

	/// Preserves the inputPacket buffer into writePackets (to avoid copying it).
	/// Creates a new write if no running write job.
	/// Used for write operations, where the data comes from the network.
	void writeCurrentInputBuffer();

	/// Checks and processes the next packet in the input buffer.
	void continueWritingIfPossible();

	/// Callback for when a write operation finishes.
	void writeFinishedCallback(uint8_t status, void * /*entry*/);

	/// Callback for when a job_open associated to a write operation finishes.
	void openWriteFinishedCallback(uint8_t status, void * /*entry*/);

	/// Prepares the input buffer for a write operation.
	void prepareInputBufferForWrite(bool isForward);

	// ---------------------------------------------------------------------------
	// Shared protected state.
	// ---------------------------------------------------------------------------

	/// Number of blocks to write to the device in one write job.
	uint16_t maxBlocksPerHddWriteJob_;

	/// Size in blocks of the next input buffer.
	uint16_t nextInputBufferBlockCount_;

	/// Indicates if the operation is in delayed close:
	/// - no new replies issued
	/// - decrease pending write jobs in parent on delayed close completion
	bool inDelayedClose_ = false;
	/// Indicates if the chunk is locked for writing. If true, master will be waiting for the lock
	/// to be released when the write operation finishes.
	bool isChunkLocked_ = false;
	/// Indicates if the write operation has ended from the client side, i.e all data has been
	/// received and replied to.
	bool isSealed_ = false;

	uint8_t untoldStatus_ = SAUNAFS_STATUS_OK;  ///< Untold status to be sent to master at cleanup.
	uint32_t writeJobId_ = 0;                   ///< ID of the current write job being processed
	uint32_t writeJobWriteId_ = 0;              ///< Specific write operation from client
	/// Number of input buffers currently enqueued in write jobs
	uint16_t enqueuedInputBuffers_ = 0;
	std::shared_ptr<InputBuffer> inputBuffer_ = nullptr;  ///< Buffer for the current write job
	/// writeJobWriteId's which:
	/// - have been completed by our worker, but need ack from the next
	///   chunkserver from the chain.
	/// - have been acked by the next chunkserver from the chain, but are still
	///   being written by us.
	std::set<uint32_t> partiallyCompletedWrites_;
	/// List of write data buffers waiting to be written to the chunk.
	std::list<std::shared_ptr<InputBuffer>> writeDataBuffers_;
};

/// @brief Concrete write high-level operation that batches writes until a WRITE_FLUSH arrives.
///
/// While `expectsWriteFlush_` is true, input packets accumulate in `writeDataBuffers_` without
/// triggering a write job. The client signals batch boundaries via WRITE_FLUSH, which translates
/// to `flushData()` calls that release a batch of buffers to a single `job_write`. Instant replies
/// are also deferred to flush points.
///
/// On `delayedClose()`, the variant switches off flush batching (`expectsWriteFlush_ = false`) so
/// any remaining buffered data is drained one-by-one, matching the base eager behavior.
class WriteHighLevelOpExpectFlush : public WriteHighLevelOp {
public:
	using WriteHighLevelOp::WriteHighLevelOp;

	void flushData() override;

protected:
	bool canStartWriteJobNow() const override;
	void collectInputBuffersForNextJob(std::vector<InputBuffer *> &inputBuffers) override;
	void freezeCurrentInputBuffer() override;
	bool shouldWriteCurrentInputBuffer() const override;
	bool shouldDeferInstantReply(bool fromFlushDataCall) const override;
	void onSwitchingToEagerWriteOnDelayedClose() override;
	void resetFlushBatchingState() override;

private:
	/// Indicates whether the operation is still in flush-batching mode. Cleared by
	/// `delayedClose()` so the remaining buffered data is drained using the base (eager) behavior.
	bool expectsWriteFlush_ = true;
	/// Queue of number of input buffers per pending flush batch.
	std::queue<uint32_t> inputBuffersForNextFlush_;
	/// Number of input buffers that became ready since the last flush.
	uint32_t inputBuffersReadySinceLastFlush_ = 0;
};

/// @brief Creates a callback function for closing a write operation.
/// This is only used for write operations, such that the chunk is locked. This callback will be
/// called after the close operation is completed, to release the chunk lock. The close job syncs
/// the metadata part of the chunk, so it is important for the master to know if also that operation
/// succeeded, and not only the write data jobs.
/// @param chunkId ID of the chunk.
/// @param chunkType Type of the chunk.
/// @param untoldStatus Error status untold to the client, to be sent to the master.
/// @return A function to be used as a callback for closing a write operation.
std::function<void(uint8_t status, void *packet)> jobCloseWriteCallback(uint64_t chunkId,
                                                                        ChunkPartType chunkType,
                                                                        uint8_t untoldStatus);
