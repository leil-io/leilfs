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
#include "chunkserver/hddspacemgr.h"
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

void WriteHighLevelOp::decreasePendingWriteJobsToCheckAndApplyClosedOnParent() {
	parentPendingWriteJobs()--;
	checkAndApplyClosedOnParent();
}

void WriteHighLevelOp::delayedCloseCallback(uint8_t status, void * /*entry*/) {
	assert(getParentState() == ChunkserverEntry::State::CloseWait);

	if (isOpenWriteJobBeingProcessed() && status == SAUNAFS_STATUS_OK) {
		// this was job_open (write)
		isChunkOpen_ = true;
		setNoWriteJobBeingProcessed();
	}

	assert(parentPendingWriteJobs() > 0);
	decreasePendingWriteJobsToCheckAndApplyClosedOnParent();
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

	if (expectWriteFlush_ && inputBuffersForNextFlush_.empty()) {
		safs::log_warn(
		    "({}) Called with no input buffers ready to be flushed while expecting write flush.",
		    __func__);
		return;
	}

	/// Start the next write job: it is always the first write data buffer
	writeJobWriteId_ = writeDataBuffers_.front()->getLastWriteId();
	std::vector<InputBuffer *> inputBuffers;

	if (expectWriteFlush_) {
		enqueuedInputBuffers_ = inputBuffersForNextFlush_.front();
		inputBuffersForNextFlush_.pop();

		if (writeDataBuffers_.size() < enqueuedInputBuffers_) {
			safs::log_err("({}) Fewer available write data buffers than expected ({} < {}).",
			              __func__, writeDataBuffers_.size(), enqueuedInputBuffers_);
			enqueuedInputBuffers_ = writeDataBuffers_.size();
		}

		auto inputBuffersIt = writeDataBuffers_.begin();
		for (uint32_t i = 0; i < enqueuedInputBuffers_; i++) {
			if (inputBuffersIt->get()->currentBlocks() == 0) {
				safs::log_warn("({}) Called with no blocks in front InputBuffer.", __func__);
			}

			inputBuffers.push_back(inputBuffersIt->get());
			inputBuffersIt++;
		}
	} else {
		if (writeDataBuffers_.front()->currentBlocks() == 0) {
			safs::log_warn("({}) Called with no blocks in front InputBuffer.", __func__);
		}

		enqueuedInputBuffers_ = 1;
		inputBuffers.push_back(writeDataBuffers_.front().get());
	}

	writeJobId_ = job_write(
	    *workerJobPool(),
	    [this](uint8_t status, void *entry) { this->writeFinishedCallback(status, entry); },
	    chunkId_, chunkVersion_, chunkType_, inputBuffers);
}

void WriteHighLevelOp::writeCurrentInputPacket() {
	writeDataBuffers_.emplace_back(std::move(inputBuffer_));
	inputBuffersReadySinceLastFlush_++;
	if (!isWriteJobBeingProcessed() && !expectWriteFlush_) { startNextWriteJob(); }
}

void WriteHighLevelOp::continueWritingIfPossible() {
	tryInstantReply();
	// There should not be any write jobs being processed.

	if (!writeDataBuffers_.empty() && (!expectWriteFlush_ || !inputBuffersForNextFlush_.empty())) {
		// There is a write buffer ready to be written and either we do not expect write flushes (so
		// we can write it immediately) or we expect write flushes but there are already scheduled
		// flushes from previous flushData calls, so we can write it immediately.
		startNextWriteJob();
		return;
	}

	if (!expectWriteFlush_ && inputBuffer_ != nullptr && !inputBuffer_->isBeingUpdated()) {
		assert(inputBuffer_->currentBlocks() > 0);
		writeCurrentInputPacket();
	}
}

void WriteHighLevelOp::flushData() {
	if (!expectWriteFlush_) {
		safs::log_warn("({}) Called flushData on a write operation that does not expect write flush.",
		               __func__);
	}

	tryInstantReply(true);

	if (inputBuffer_ != nullptr && inputBuffer_->currentBlocks() > 0 && !inputBuffer_->isBeingUpdated()) {
		writeDataBuffers_.emplace_back(std::move(inputBuffer_));
		inputBuffersReadySinceLastFlush_++;
	}

	if (inputBuffersReadySinceLastFlush_ > 0) {
		inputBuffersForNextFlush_.push(inputBuffersReadySinceLastFlush_);
		inputBuffersReadySinceLastFlush_ = 0;
	
		if (!isWriteJobBeingProcessed() && !writeDataBuffers_.empty()) {
			startNextWriteJob();
		}
	} else {
		// This can happen if the connection is created but very little data is sent, which implies
		// some WriteHighLevelOp receive no write data packets.
		safs::log_debug("({}) Called flushData with no input buffers ready to be flushed.", __func__);
	}
}

void WriteHighLevelOp::writeFinishedCallback(uint8_t status, void * /*entry*/) {
	if (isOpenWriteJobBeingProcessed()) {
		safs::log_warn("({}) Inconsistent state: writeJobWriteId: {}, chunkId: {}, status: {}.",
		               __func__, writeJobWriteId_, chunkId_, status);
	}
	setNoWriteJobBeingProcessed();

	std::vector<std::pair<uint8_t, uint32_t>> statusWithWriteIdToReply;
	uint16_t alreadyRepliedBlocks = 0;
	while (enqueuedInputBuffers_ > 0) {
		assert(!writeDataBuffers_.empty());

		auto statusWithWriteIdToReplyCurrentBuffer = writeDataBuffers_.front()->getStatuses();
		statusWithWriteIdToReply.insert(statusWithWriteIdToReply.end(),
		                                statusWithWriteIdToReplyCurrentBuffer.begin(),
		                                statusWithWriteIdToReplyCurrentBuffer.end());
		uint16_t alreadyRepliedBlocksBuffer = writeDataBuffers_.front()->repliedBlocks;
		alreadyRepliedBlocks += alreadyRepliedBlocksBuffer;

		if (alreadyRepliedBlocksBuffer > 0) {
			// This means that this write data buffer has already been replied to for some blocks,
			// so we need to remove the input buffer from the container that patches read
			// operations.
			hddRemoveAlreadyRepliedInputBuffer(chunkId_, chunkType_, writeDataBuffers_.front());
		}

		getWriteInputBufferPool().put(std::move(writeDataBuffers_.front()));
		writeDataBuffers_.pop_front();
		enqueuedInputBuffers_--;
	}

	for (const auto &[status, writeId] : statusWithWriteIdToReply) {
		if (alreadyRepliedBlocks == 0) {
			if (!inDelayedClose_) {
				updateUsingWriteStatusAndReply(status, writeId);
				if (status != SAUNAFS_STATUS_OK) { return; }
			} // else: in delayed close, do not reply, just consume the status
		} else {
			// Already replied
			if (untoldStatus_ == SAUNAFS_STATUS_OK && status != SAUNAFS_STATUS_OK) {
				untoldStatus_ = status;
			}
			alreadyRepliedBlocks--;
		}
	}

	continueWritingIfPossible();

	if (inDelayedClose_) {
		decreasePendingWriteJobsToCheckAndApplyClosedOnParent();
	}
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

void WriteHighLevelOp::setup(uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                             bool expectWriteFlush) {
	stats_hlopw++;

	chunkId_ = chunkId;
	chunkVersion_ = chunkVersion;
	chunkType_ = chunkType;
	expectWriteFlush_ = expectWriteFlush;

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
	tryInstantReply();

	// No write jobs in progress or current input buffer is full - write it
	if (inputBuffer_->isFull() || (!isWriteJobBeingProcessed() && !expectWriteFlush_)) {
		assert(inputBuffer_->currentBlocks() > 0);
		writeCurrentInputPacket();
	}
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

bool WriteHighLevelOp::trySeal() {
	if (inputBuffer_ != nullptr) {
		// If there is an input buffer, we can only seal if all its data has been replied
		if (inputBuffer_->isBeingUpdated()) { return false; }
		if (inputBuffer_->currentBlocks() > inputBuffer_->repliedBlocks) {
			return false;
		}

		for (const auto &buffer : writeDataBuffers_) {
			if (buffer->currentBlocks() > buffer->repliedBlocks) {
				// The log warn is because this case does not make any sense, there could be any
				// issue somewhere else
				safs::log_err(
				    "({}) Inconsistent state: write data buffer has un-replied blocks "
				    "while input buffer is fully replied (chunkId: {:016X}).",
				    __func__, chunkId_);
				assert(
				    false &&
				    "write data buffer has un-replied blocks while input buffer is fully replied");
				return false;
			}
		}

		// There is an input buffer, all its data has been processed and replied
		// so we can write it out to complete the operation
		assert(inputBuffer_->currentBlocks() > 0);
		writeCurrentInputPacket();
	} else {
		// There is no input buffer, we can only seal if all write data buffers have been replied
		for (const auto &buffer : writeDataBuffers_) {
			if (buffer->currentBlocks() > buffer->repliedBlocks) {
				// There is a write data buffer with un-replied blocks and no input buffer
				return false;
			}
		}
	}

	// All buffers have been replied, check partially completed writes (chain writes)
	isSealed_ = partiallyCompletedWrites_.empty();

	return isSealed_;
}

bool WriteHighLevelOp::isCompleted() const {
	return writeDataBuffers_.empty() && isSealed_;
}

void WriteHighLevelOp::delayedClose() {
	inDelayedClose_ = true;

	// Only write init received - disable open write job
	if (isOpenWriteJobBeingProcessed()) {
		workerJobPool()->disableJob(writeJobId_);
		workerJobPool()->changeCallback(
		    writeJobId_,
		    [this](uint8_t status, void *entry) { this->delayedCloseCallback(status, entry); },
		    kEmptyExtra);

		parentPendingWriteJobs()++;
		// When open write job is being processed no writes can be instantly replied, so that's it.
		return;
	}

	// Some write data jobs received, handle input buffer
	if (inputBuffer_ != nullptr) {
		if (inputBuffer_->repliedBlocks > 0) {
			/// There are replied blocks in the input buffer, move it to write data buffers
			if (inputBuffer_->isBeingUpdated()) {
				// Unfinished update - we need to drop that last block
				inputBuffer_->getWriteInfoVector().pop_back();
			}
			writeDataBuffers_.emplace_back(std::move(inputBuffer_));
		} else {
			/// No replied blocks - drop the input buffer, it won't be used anymore
			getWriteInputBufferPool().put(std::move(inputBuffer_));
		}
	}
	// Input buffer now is null
	assert(inputBuffer_ == nullptr);

	// Drop write data buffers with no replied blocks, and keep at least the amount input buffers
	// that are currently enqueued in write jobs
	while (writeDataBuffers_.size() > enqueuedInputBuffers_ &&
	       writeDataBuffers_.back()->repliedBlocks == 0) {
		getWriteInputBufferPool().put(std::move(writeDataBuffers_.back()));
		writeDataBuffers_.pop_back();
	}

	// We do not expect write flush anymore, so all the remaining buffers should be written out, but
	// one-by-one
	expectWriteFlush_ = false;
	if (!isWriteJobBeingProcessed() && !writeDataBuffers_.empty()) {
		// No write job in progress, start one to trigger the processing of the remaining buffers,
		// which will be replied in writeFinishedCallback
		startNextWriteJob();
	}

	// Pending write jobs will be handled in writeFinishedCallback, they need to write out the
	// buffers
	if (!writeDataBuffers_.empty()) {
		parentPendingWriteJobs() += writeDataBuffers_.size() - enqueuedInputBuffers_ + 1;
	}
}

void WriteHighLevelOp::cleanup() {
	if (chunkId_ == 0) {
		safs::log_warn("(WriteHighLevelOp::{}) Called with no chunk associated.", __func__);
		return;
	}

	while (!writeDataBuffers_.empty()) {
		if (writeDataBuffers_.front()->repliedBlocks > 0) {
			hddRemoveAlreadyRepliedInputBuffer(chunkId_, chunkType_, writeDataBuffers_.front());
		}
		getWriteInputBufferPool().put(std::move(writeDataBuffers_.front()));
		writeDataBuffers_.pop_front();
	}
	enqueuedInputBuffers_ = 0;
	inputBuffersReadySinceLastFlush_ = 0;
	inputBuffersForNextFlush_ = std::queue<uint32_t>();
	nextInputBufferBlockCount_ =
	    std::min(kDefaultInitialNextInputBufferBlockCount, maxBlocksPerHddWriteJob_);

	if (inputBuffer_ != nullptr) {
		/// Drop the input buffer, it won't be used anymore
		if (inputBuffer_->repliedBlocks > 0) {
			hddRemoveAlreadyRepliedInputBuffer(chunkId_, chunkType_, inputBuffer_);
		}
		getWriteInputBufferPool().put(std::move(inputBuffer_));
	}

	if (isChunkOpen_) {
		if (isChunkLocked_) {
			// We need to wait for the metadata to be synced before releasing the lock, so we use a
			// callback to release the lock afterward
			job_close(*workerJobPool(), jobCloseWriteCallback(chunkId_, chunkType_, untoldStatus_),
			          chunkId_, chunkType_);
		} else {
			job_close(*workerJobPool(), kEmptyCallback, chunkId_, chunkType_);
		}
	} else if (isChunkLocked_) {
		masterconn_get_job_pool()->endChunkLock(chunkId_, chunkType_, untoldStatus_);
	}
}

void WriteHighLevelOp::tryInstantReply(bool fromFlushDataCall) {
	// No need to try instant reply if:
	// - sealed: everything has been replied already.
	// - in delayed close: we won't accept new write data and we will reply to pending ones in
	//   writeFinishedCallback, so no need to try here.
	// - open write job being processed: we haven't received of the initial open write job.
	// - chunk not locked: we cannot perform instant reply if the chunk is not locked because
	//   master will not receive the status if there is some failure.
	// - chunk type is standard: we only support instant reply for non-standard slices, because for
	//   standard slice we may lost data due to a single IO error.
	if (isSealed_ || inDelayedClose_ || isOpenWriteJobBeingProcessed() || !isChunkLocked_ ||
	    chunkType_ == slice_traits::standard::ChunkPartType()) {
		return;
	}

	if (expectWriteFlush_ && !fromFlushDataCall) {
		// If we expect write flushes, we only try instant reply when flushData is called, because
		// that's the only time when we know that there won't be more data to be flushed for the
		// current write operation.
		return;
	}

	// Instant reply for a single buffer, it is assumed that we can reply for all blocks in
	// the buffer.
	auto bufferLevelInstantReply = [this](std::shared_ptr<InputBuffer> &buffer) {
		auto &writeInfoVec = buffer->getWriteInfoVector();

		if (writeInfoVec.size() > 0) {
			hddInsertAlreadyRepliedInputBuffer(chunkId_, chunkType_, buffer,
			                                   buffer->repliedBlocks == 0);
		}

		modifyAvailableWriteBufferingBlocks(
		    -static_cast<int32_t>(writeInfoVec.size() - buffer->repliedBlocks));
		while (buffer->repliedBlocks < writeInfoVec.size()) {
			const auto &writeInfo = writeInfoVec[buffer->repliedBlocks];
			updateUsingWriteStatusAndReply(SAUNAFS_STATUS_OK, writeInfo.writeId);
			buffer->repliedBlocks++;
		}
	};

	// Try for write data buffers, in the expected order of reply
	uint32_t inputBufferCount = 0;
	for (auto &buff : writeDataBuffers_) {
		// Everything replied
		inputBufferCount++;
		if (buff->repliedBlocks == buff->currentBlocks()) { continue; }

		if (getAvailableWriteBufferingBlocks() + static_cast<int32_t>(buff->repliedBlocks) <
		    static_cast<int32_t>(buff->currentBlocks())) {
			// Cannot reply the remaining blocks
			return;
		}

		if (inputBufferCount <= enqueuedInputBuffers_) {
			// We can only reply buffers that are not already enqueued in write jobs, because for
			// enqueued ones the access to the status fields is not thread safe.
			return;
		}

		bufferLevelInstantReply(buff);
	}

	// Try for the input buffer
	if (inputBuffer_ == nullptr || inputBuffer_->isBeingUpdated() ||
	    inputBuffer_->repliedBlocks == inputBuffer_->currentBlocks() ||
	    getAvailableWriteBufferingBlocks() + static_cast<int32_t>(inputBuffer_->repliedBlocks) <
	        static_cast<int32_t>(inputBuffer_->currentBlocks())) {
		return;
	}

	bufferLevelInstantReply(inputBuffer_);
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
