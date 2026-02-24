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

#include "common/platform.h"

#include "chunkserver/chunk_high_level_ops.h"

#include "chunkserver/bgjobs.h"
#include "chunkserver/chunkserver_entry.h"
#include "chunkserver/hdd_readahead.h"
#include "chunkserver/masterconn.h"
#include "chunkserver/network_stats.h"
#include "protocol/cstocl.h"
#include "protocol/cstocs.h"

void GetBlocksHighLevelOp::delayedCloseCallback(uint8_t /*status*/, void * /*entry*/) {
	assert(getParentState() == ChunkserverEntry::State::CloseWait);
	assert(pendingDelayedJobs_ > 0);

	pendingDelayedJobs_--;
	checkAndApplyClosedOnParent();
}

void GetBlocksHighLevelOp::getChunkBlocksCallback(uint8_t status, void * /*entry*/) {
	getBlocksJobId_ = 0;
	std::vector<uint8_t> buffer;
	cstocs::getChunkBlocksStatus::serialize(buffer, chunkId_, chunkVersion_, chunkType_,
	                                        getBlocksJobResult_, status);
	createAttachedPacket(buffer);
	setParentState(ChunkserverEntry::State::Idle);
}

void GetBlocksHighLevelOp::setup(uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType) {
	chunkId_ = chunkId;
	chunkVersion_ = chunkVersion;
	chunkType_ = chunkType;

	getBlocksJobId_ = job_get_blocks(
	    *workerJobPool(),
	    [this](uint8_t status, void *entry) { this->getChunkBlocksCallback(status, entry); },
	    chunkId_, chunkVersion_, chunkType_, &getBlocksJobResult_);
}

bool GetBlocksHighLevelOp::isRunning() const { return getBlocksJobId_ > 0; }

void GetBlocksHighLevelOp::delayedClose() {
	workerJobPool()->disableJob(getBlocksJobId_);
	workerJobPool()->changeCallback(
	    getBlocksJobId_,
	    [this](uint8_t status, void *entry) { this->delayedCloseCallback(status, entry); },
	    kEmptyExtra);

	pendingDelayedJobs_++;
}

void ReadHighLevelOp::delayedCloseCallback(uint8_t status, void *buffer) {
	assert(getParentState() == ChunkserverEntry::State::CloseWait);
	auto outputBuffer = static_cast<OutputBuffer *>(buffer);
	passert(outputBuffer);
	outputBuffer->setIsCallbackStarted(true);

	if (status == SAUNAFS_STATUS_OK) { isChunkOpen_ = true; }

	while (!toDiscardReadDataBuffers_.empty() &&
	       toDiscardReadDataBuffers_.front()->isCallbackStarted()) {
		getReadOutputBufferPool().put(std::move(toDiscardReadDataBuffers_.front()));
		toDiscardReadDataBuffers_.pop_front();
		toDiscardReadJobIds_.pop_front();
	}

	assert(pendingDelayedJobs_ > 0);
	pendingDelayedJobs_--;
	checkAndApplyClosedOnParent();
}

void ReadHighLevelOp::readDiscardCallback(uint8_t status, void *buffer) {
	(void)status;
	auto outputBuffer = static_cast<OutputBuffer *>(buffer);
	passert(outputBuffer);
	outputBuffer->setIsCallbackStarted(true);

	// jobId <--> related packet, maintaining the order
	assert(toDiscardReadJobIds_.size() == toDiscardReadDataBuffers_.size());

	auto jobsIt = toDiscardReadJobIds_.begin();
	for (auto buffersIt = toDiscardReadDataBuffers_.begin();
	     buffersIt != toDiscardReadDataBuffers_.end();) {
		if ((*buffersIt)->isCallbackStarted()) {
			getReadOutputBufferPool().put(std::move(*buffersIt));
			buffersIt = toDiscardReadDataBuffers_.erase(buffersIt);
			jobsIt = toDiscardReadJobIds_.erase(jobsIt);
		} else {
			buffersIt++;
			jobsIt++;
		}
	}
}

void ReadHighLevelOp::prepareDiscardReadJobs() {
	// We need to:
	// - change state of packets not taken by any hdd worker
	// - change callback in already taken ones
	// - move packets from pending to toDiscard lists

	// The jobs which are not going to be processed: all but the ones in progress
	auto disabledJobIds = workerJobPool()->disableJobs(pendingReadJobIds_);
	workerJobPool()->changeCallback(pendingReadJobIds_, [this](uint8_t status, void *entry) {
		this->readDiscardCallback(status, entry);
	});
	workerJobPool()->changeCallback(disabledJobIds, kEmptyCallback);
	while (!pendingReadJobIds_.empty()) {
		// pendingReadJobIds and pendingReadDataBuffers should have the related elements in the
		// correct order
		if (!pendingReadDataBuffers_.front()->isCallbackStarted() &&
		    (disabledJobIds.empty() || disabledJobIds.front() != pendingReadJobIds_.front())) {
			toDiscardReadJobIds_.push_back(pendingReadJobIds_.front());
			toDiscardReadDataBuffers_.emplace_back(std::move(pendingReadDataBuffers_.front()));
		} else {
			// Already processed packets, can be moved to the pool
			getReadOutputBufferPool().put(std::move(pendingReadDataBuffers_.front()));
			if (!disabledJobIds.empty() && disabledJobIds.front() == pendingReadJobIds_.front()) {
				disabledJobIds.pop_front();
			}
		}

		pendingReadJobIds_.pop_front();
		pendingReadDataBuffers_.pop_front();
	}

	assert(disabledJobIds.empty());
	assert(pendingReadJobIds_.empty());
	assert(pendingReadDataBuffers_.empty());
}

void ReadHighLevelOp::readFinishedCallback(uint8_t status, void *buffer) {
	auto outputBuffer = static_cast<OutputBuffer *>(buffer);
	passert(outputBuffer);
	outputBuffer->setIsCallbackStarted(true);

	if (status == SAUNAFS_STATUS_OK) {
		isChunkOpen_ = true;
		readContinue(maxParallelHddReadJobs_);
	} else {
		// - prepare discard
		// - send status
		// - close chunk
		// - change state
		prepareDiscardReadJobs();

		std::vector<uint8_t> buffer;
		cstocl::readStatus::serialize(buffer, chunkId_, status);
		createAttachedPacket(buffer);

		if (isChunkOpen_) {
			job_close(*workerJobPool(), kEmptyCallback, chunkId_, chunkType_);
			isChunkOpen_ = false;
		}

		// Send status and close connection
		setParentState(ChunkserverEntry::State::IOFinish);
		LOG_AVG_STOP(readOperationTimer_);
	}
}

std::shared_ptr<OutputBuffer> ReadHighLevelOp::prepareReadDataPacket(
    std::vector<uint8_t> &readDataPrefix, uint32_t jobSize, uint32_t jobOffset) {
	const uint32_t numBlocks = (jobSize + SFSBLOCKSIZE - 1) / SFSBLOCKSIZE;

	readDataPrefix.clear();
	cstocl::readData::serializePrefix(readDataPrefix, chunkId_, offset_,
	                                  std::min<uint32_t>(jobSize, SFSBLOCKSIZE - jobOffset));

	auto buffer = getReadOutputBufferPool().get(readDataPrefix.size(), numBlocks);

	for (uint32_t i = 0; i < numBlocks; i++) {
		if (i > 0) {  // first block is already serialized
			readDataPrefix.clear();
			cstocl::readData::serializePrefix(
			    readDataPrefix, chunkId_, offset_ - jobOffset + (i * SFSBLOCKSIZE), SFSBLOCKSIZE);
		}

		if (buffer->copyIntoBuffer(OutputBuffer::BufferType::Header, readDataPrefix) !=
		    static_cast<ssize_t>(readDataPrefix.size())) {
			if (buffer) { getReadOutputBufferPool().put(std::move(buffer)); }

			return kInvalidPacket;
		}
	}

	return buffer;
}

void ReadHighLevelOp::readContinue(uint16_t callMaxParallelHddReadJobs) {
	while (!pendingReadDataBuffers_.empty() &&
	       pendingReadDataBuffers_.front()->isCallbackStarted()) {
		attachBuffer(std::move(pendingReadDataBuffers_.front()));
		pendingReadDataBuffers_.pop_front();
		pendingReadJobIds_.pop_front();
	}

	if (pendingReadDataBuffers_.empty() && size_ == 0) {  // everything has been read
		std::vector<uint8_t> buffer;
		cstocl::readStatus::serialize(buffer, chunkId_, SAUNAFS_STATUS_OK);
		createAttachedPacket(buffer);

		sassert(isChunkOpen_);
		job_close(*workerJobPool(), kEmptyCallback, chunkId_, chunkType_);
		isChunkOpen_ = false;
		// no error - do not disconnect - go direct to the IDLE state, ready for
		// requests on the same connection
		if (workerJobPool()->isFull()) {
			// If the worker job pool is full (best-effort check), try not to accept
			// more requests until it has free slots. Note: the pool state may change
			// after this check, but this serves as backpressure heuristic.
			setParentState(ChunkserverEntry::State::IOFinish);
		} else {
			// Ready for new requests, reset state
			setParentState(ChunkserverEntry::State::Idle);
		}
		LOG_AVG_STOP(readOperationTimer_);
	} else {
		std::vector<uint8_t> readDataPrefix;

		while (size_ > 0 && pendingReadDataBuffers_.size() < callMaxParallelHddReadJobs) {
			const uint32_t totalRequestSize = size_;
			const uint32_t thisPartOffset = offset_ % SFSBLOCKSIZE;
			const uint32_t thisPartSize = std::min<uint32_t>(
			    totalRequestSize, maxBlocksPerHddReadJob_ * SFSBLOCKSIZE - thisPartOffset);
			const uint16_t totalRequestBlocks =
			    (totalRequestSize + thisPartOffset + SFSBLOCKSIZE - 1) / SFSBLOCKSIZE;

			auto buffer = prepareReadDataPacket(readDataPrefix, thisPartSize, thisPartOffset);
			if (buffer == kInvalidPacket) {
				setParentState(ChunkserverEntry::State::Close);
				return;
			}

			pendingReadDataBuffers_.emplace_back(std::move(buffer));

			uint32_t readAheadBlocks = 0;
			uint32_t maxReadBehindBlocks = 0;

			if (!isChunkOpen_) {
				if (gHDDReadAhead.blocksToBeReadAhead() > 0) {
					readAheadBlocks = totalRequestBlocks + gHDDReadAhead.blocksToBeReadAhead();
				}
				// Try not to influence slow streams too much:
				maxReadBehindBlocks =
				    std::min(totalRequestBlocks, gHDDReadAhead.maxBlocksToBeReadBehind());
			}

			uint32_t readJobId = job_read(
			    *workerJobPool(),
			    [this](uint8_t status, void *entry) { this->readFinishedCallback(status, entry); },
			    chunkId_, chunkVersion_, chunkType_, offset_, thisPartSize, maxReadBehindBlocks,
			    readAheadBlocks, pendingReadDataBuffers_.back().get(), !isChunkOpen_);

			if (readJobId == 0) {
				getReadOutputBufferPool().put(std::move(pendingReadDataBuffers_.back()));
				pendingReadDataBuffers_.pop_back();
				setParentState(ChunkserverEntry::State::Close);
				return;
			}
			pendingReadJobIds_.push_back(readJobId);

			offset_ += thisPartSize;
			size_ -= thisPartSize;
		}
	}
}

void ReadHighLevelOp::setup(uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                            uint32_t offset, uint32_t size) {
	stats_hlopr++;
	LOG_AVG_START0(readOperationTimer_, "csserv_read");

	chunkId_ = chunkId;
	chunkVersion_ = chunkVersion;
	chunkType_ = chunkType;
	offset_ = offset;
	size_ = size;

	readContinue(1);
}

void ReadHighLevelOp::continueReadingIfPossible() {
	if (!pendingReadDataBuffers_.empty() || size_ > 0) { readContinue(maxParallelHddReadJobs_); }
}

bool ReadHighLevelOp::prepareForDelayedClose() {
	if (!pendingReadJobIds_.empty()) { prepareDiscardReadJobs(); }

	return !toDiscardReadJobIds_.empty();
}

void ReadHighLevelOp::delayedClose() {
	// Already disabled jobs
	workerJobPool()->changeCallback(toDiscardReadJobIds_, [this](uint8_t status, void *entry) {
		this->delayedCloseCallback(status, entry);
	});

	pendingDelayedJobs_ += toDiscardReadJobIds_.size();
}

void ReadHighLevelOp::cleanup() {
	if (isChunkOpen_) {
		job_close(*workerJobPool(), kEmptyCallback, chunkId_, chunkType_);
		isChunkOpen_ = false;
	}

	chunkId_ = 0;
	chunkVersion_ = 0;
	chunkType_ = slice_traits::standard::ChunkPartType();
}

bool WriteHighLevelOp::isOpenWriteJobBeingProcessed() const {
	return writeJobId_ != 0 && writeJobWriteId_ == 0;
}

bool WriteHighLevelOp::isWriteJobBeingProcessed() const { return writeJobId_ != 0; }

void WriteHighLevelOp::setNoWriteJobBeingProcessed() { writeJobId_ = 0; }

void WriteHighLevelOp::startOpenWriteJob() {
	writeJobWriteId_ = 0;
	writeJobId_ = job_open(
	    *workerJobPool(),
	    [this](uint8_t status, void *entry) { this->openWriteFinishedCallback(status, entry); },
	    chunkId_, chunkType_);
}

void WriteHighLevelOp::updateUsingWriteStatusAndReply(uint8_t status, uint32_t writeId) {
	if (status != SAUNAFS_STATUS_OK) {
		createAttachedWriteStatus(chunkId_, status, writeId);
		setParentState(ChunkserverEntry::State::IOFinish);
		return;
	}

	// We can consider that the write was successful
	if (getParentState() == ChunkserverEntry::State::WriteLast) {
		createAttachedWriteStatus(chunkId_, status, writeId);
		return;
	}

	// state is WriteForward or IOFinish
	if (partiallyCompletedWrites_.count(writeId) > 0) {
		// found - it means that it was added by status_receive, ie. next
		// chunkserver from a chain finished writing before our worker
		createAttachedWriteStatus(chunkId_, status, writeId);
		partiallyCompletedWrites_.erase(writeId);
	} else {
		// not found - so add it
		partiallyCompletedWrites_.insert(writeId);
	}
}

void WriteHighLevelOp::delayedCloseCallback(uint8_t status, void * /*entry*/) {
	assert(getParentState() == ChunkserverEntry::State::CloseWait);

	if (isOpenWriteJobBeingProcessed() && status == SAUNAFS_STATUS_OK) {
		// this was job_open (write)
		isChunkOpen_ = true;
		setNoWriteJobBeingProcessed();
	}

	assert(pendingDelayedJobs_ > 0);
	pendingDelayedJobs_--;
	checkAndApplyClosedOnParent();
}

void WriteHighLevelOp::startNextWriteJob() {
	if (writeDataBuffers_.empty()) {
		safs::log_warn("({}) Called with no write data buffers.", __func__);
		return;
	}

	if (isWriteJobBeingProcessed()) {
		safs::log_warn("({}) Called with write job already in progress.", __func__);
		return;
	}

	/// Start the next write job: it is always the first write data buffer
	writeJobWriteId_ = writeDataBuffers_.front()->getLastWriteId();
	writeJobId_ = job_write(
	    *workerJobPool(),
	    [this](uint8_t status, void *entry) { this->writeFinishedCallback(status, entry); },
	    chunkId_, chunkVersion_, chunkType_, writeDataBuffers_.front().get());
}

void WriteHighLevelOp::writeCurrentInputPacket() {
	writeDataBuffers_.emplace_back(std::move(inputBuffer_));
	if (!isWriteJobBeingProcessed()) { startNextWriteJob(); }
}

void WriteHighLevelOp::continueWritingIfPossible() {
	if (!writeDataBuffers_.empty()) {
		// there is a write buffer ready to be written, there should not be any
		// write jobs being processed.
		startNextWriteJob();
		return;
	}

	if (inputBuffer_ != nullptr && !inputBuffer_->isBeingUpdated()) { writeCurrentInputPacket(); }
}

void WriteHighLevelOp::writeFinishedCallback(uint8_t status, void * /*entry*/) {
	if (isOpenWriteJobBeingProcessed()) {
		safs::log_warn("({}) Inconsistent state: writeJobWriteId: {}, chunkId: {}, status: {}.",
		               __func__, writeJobWriteId_, chunkId_, status);
	}
	setNoWriteJobBeingProcessed();

	auto statusWithWriteIdToReply = writeDataBuffers_.front()->getStatuses();
	getWriteInputBufferPool().put(std::move(writeDataBuffers_.front()));
	writeDataBuffers_.pop_front();

	for (const auto &[status, writeId] : statusWithWriteIdToReply) {
		updateUsingWriteStatusAndReply(status, writeId);
		if (status != SAUNAFS_STATUS_OK) { return; }
	}

	continueWritingIfPossible();
}

void WriteHighLevelOp::openWriteFinishedCallback(uint8_t status, void * /*entry*/) {
	if (!isOpenWriteJobBeingProcessed()) {
		safs::log_warn("({}) Inconsistent state: writeJobWriteId: {}, chunkId: {}, status: {}.",
		               __func__, writeJobWriteId_, chunkId_, status);
	}
	setNoWriteJobBeingProcessed();

	// We should assume that writeJobWriteId is 0 here, because this callback
	// is called after job_open, which should have set writeJobWriteId to 0.

	updateUsingWriteStatusAndReply(status, writeJobWriteId_);
	if (status != SAUNAFS_STATUS_OK) { return; }

	isChunkOpen_ = true;

	continueWritingIfPossible();
}

void WriteHighLevelOp::prepareInputBufferForWrite(bool isForward) {
	if (inputBuffer_ != nullptr && inputBuffer_->isFull()) {
		safs::log_warn("({}) Called with full inputBuffer, isForward: {}. Writing existing buffer.",
		               __func__, isForward);
		writeCurrentInputPacket();
	}

	if (inputBuffer_ == nullptr) {
		inputBuffer_ = getWriteInputBufferPool().get(
		    isForward ? kSauWriteDataPrefixSizeForward : kSauWriteDataPrefixSize,
		    nextInputBufferBlockCount_);

		nextInputBufferBlockCount_ =
		    std::min<uint16_t>(nextInputBufferBlockCount_ * 2, maxBlocksPerHddWriteJob_);
	}

	inputBuffer_->addNewWriteOperation();
}

void WriteHighLevelOp::setup(uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType) {
	stats_hlopw++;

	chunkId_ = chunkId;
	chunkVersion_ = chunkVersion;
	chunkType_ = chunkType;

	isChunkLocked_ = masterconn_get_job_pool()->enforceChunkLock(chunkId_, chunkType_);
	startOpenWriteJob();
}

void WriteHighLevelOp::prepareForNewWriteData(bool mustForward, uint8_t *headerBuffer) {
	prepareInputBufferForWrite(mustForward);

	if (mustForward) {
		inputBuffer_->copyIntoBuffer(InputBuffer::BufferType::Header, headerBuffer,
		                             PacketHeader::kSize);
	}
}

void WriteHighLevelOp::processWriteDataBlock(uint16_t blocknum, uint32_t opOffset, uint32_t opSize,
                                             uint32_t writeId, uint32_t crc) {
	inputBuffer_->setupLastWriteOperation(blocknum, opOffset, opSize, writeId, crc);

	// No write jobs in progress or current input buffer is full - write it
	if (!isWriteJobBeingProcessed() || inputBuffer_->isFull()) { writeCurrentInputPacket(); }
}

bool WriteHighLevelOp::isLastHeaderSizeValid() const { return inputBuffer_->isHeaderSizeValid(); }

uint8_t *WriteHighLevelOp::getLastOperationHeader() {
	return const_cast<uint8_t *>(inputBuffer_->getStartLastWriteOperationHeader());
}

ssize_t WriteHighLevelOp::readData(int sock, size_t bytesToRead) {
	return inputBuffer_->readFromSocket(sock, bytesToRead);
}

ssize_t WriteHighLevelOp::writeData(int sock, size_t bytesToWrite) {
	return inputBuffer_->writeToSocket(sock, bytesToWrite);
}

bool WriteHighLevelOp::isCompleted() const {
	// Conditions:
	// - no write job being processed
	// - no partially completed writes (forward case)
	// - no write data buffers waiting to be enqueued
	// - no input buffer being filled (all data has been processed)
	return !isWriteJobBeingProcessed() && partiallyCompletedWrites_.empty() &&
	       writeDataBuffers_.empty() && inputBuffer_ == nullptr;
}

void WriteHighLevelOp::delayedClose() {
	workerJobPool()->disableJob(writeJobId_);
	workerJobPool()->changeCallback(
	    writeJobId_,
	    [this](uint8_t status, void *entry) { this->delayedCloseCallback(status, entry); },
	    kEmptyExtra);

	if (inputBuffer_ != nullptr) {
		/// Drop the input buffer, it won't be used anymore
		getWriteInputBufferPool().put(std::move(inputBuffer_));
	}

	pendingDelayedJobs_++;
}

void WriteHighLevelOp::cleanup() {
	if (chunkId_ == 0) {
		safs::log_info("(WriteHighLevelOp::{}) Called with no chunk associated.", __func__);
		return;
	}

	while (!writeDataBuffers_.empty()) {
		getWriteInputBufferPool().put(std::move(writeDataBuffers_.front()));
		writeDataBuffers_.pop_front();
	}

	if (inputBuffer_ != nullptr) {
		/// Drop the input buffer, it won't be used anymore
		getWriteInputBufferPool().put(std::move(inputBuffer_));
	}

	if (isChunkOpen_) {
		if (isChunkLocked_) {
			// We need to wait for the metadata to be synced before releasing the lock, so we use a
			// callback to release the lock afterward
			job_close(*workerJobPool(), jobCloseWriteCallback(chunkId_, chunkType_, SAUNAFS_STATUS_OK),
			          chunkId_, chunkType_);
		} else {
			job_close(*workerJobPool(), kEmptyCallback, chunkId_, chunkType_);
		}
		isChunkOpen_ = false;
	} else if (isChunkLocked_) {
		masterconn_get_job_pool()->endChunkLock(chunkId_, chunkType_, SAUNAFS_STATUS_OK);
	}

	isChunkLocked_ = false;
	partiallyCompletedWrites_.clear();
	chunkId_ = 0;
	chunkVersion_ = 0;
	nextInputBufferBlockCount_ =
	    std::min(kDefaultInitialNextInputBufferBlockCount, maxBlocksPerHddWriteJob_);
	chunkType_ = slice_traits::standard::ChunkPartType();
}

std::function<void(uint8_t status, void *packet)> jobCloseWriteCallback(uint64_t chunkId,
                                                                        ChunkPartType chunkType,
                                                                        uint8_t untoldStatus) {
	return [chunkId, chunkType, untoldStatus](uint8_t status, void * /*entry*/) {
		if (untoldStatus == SAUNAFS_STATUS_OK && status != SAUNAFS_STATUS_OK) {
			masterconn_get_job_pool()->endChunkLock(chunkId, chunkType, status);
		} else {
			masterconn_get_job_pool()->endChunkLock(chunkId, chunkType, untoldStatus);
		}
	};
}
