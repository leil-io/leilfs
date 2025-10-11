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

#include "chunkserver/chunkserver_entry.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <syslog.h>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <set>

#include "chunkserver-common/global_shared_resources.h"
#include "chunkserver/bgjobs.h"
#include "chunkserver/hdd_readahead.h"
#include "chunkserver/hddspacemgr.h"
#include "chunkserver/network_stats.h"
#include "chunkserver/io_buffers.h"
#include "common/charts.h"
#include "common/datapack.h"
#include "common/event_loop.h"
#include "common/legacy_vector.h"
#include "common/massert.h"
#include "common/saunafs_version.h"
#include "common/sockets.h"
#include "devtools/TracePrinter.h"
#include "devtools/request_log.h"
#include "protocol/SFSCommunication.h"
#include "protocol/cltocs.h"
#include "protocol/cstocl.h"
#include "protocol/cstocs.h"
#include "protocol/packet.h"
#include "slogger/slogger.h"

constexpr uint32_t kMaxPacketSize = 100000 + SFSBLOCKSIZE;
constexpr uint8_t kConnectRetries = 10;

constexpr uint8_t kSauWriteDataPreffixSize = cltocs::writeData::kPrefixSize;
// For forwarding: size of SAU_CLTOCS_WRITE_DATA prefix plus the packet header.
constexpr uint8_t kSauWriteDataPreffixSizeForward =
    cltocs::writeData::kPrefixSize + PacketHeader::kSize;

class MessageSerializer {
public:
	static MessageSerializer *getSerializer(PacketHeader::Type type);

	virtual void serializePrefixOfCstoclReadData(std::vector<uint8_t> &buffer,
	                                             uint64_t chunkId,
	                                             uint32_t offset,
	                                             uint32_t size) = 0;
	virtual void serializeCstoclReadStatus(std::vector<uint8_t> &buffer,
	                                       uint64_t chunkId,
	                                       uint8_t status) = 0;
	virtual void serializeCstoclWriteStatus(std::vector<uint8_t> &buffer,
	                                        uint64_t chunkId, uint32_t writeId,
	                                        uint8_t status) = 0;
	virtual ~MessageSerializer() {}
};

class SaunaFsMessageSerializer : public MessageSerializer {
public:
	void serializePrefixOfCstoclReadData(std::vector<uint8_t> &buffer,
	                                     uint64_t chunkId, uint32_t offset,
	                                     uint32_t size) override {
		cstocl::readData::serializePrefix(buffer, chunkId, offset, size);
	}

	void serializeCstoclReadStatus(std::vector<uint8_t> &buffer,
	                               uint64_t chunkId, uint8_t status) override {
		cstocl::readStatus::serialize(buffer, chunkId, status);
	}

	void serializeCstoclWriteStatus(std::vector<uint8_t> &buffer,
	                                uint64_t chunkId, uint32_t writeId,
	                                uint8_t status) override {
		cstocl::writeStatus::serialize(buffer, chunkId, writeId, status);
	}
};

MessageSerializer *MessageSerializer::getSerializer(PacketHeader::Type type) {
	sassert(type >= PacketHeader::kMinSauPacketType &&
	         type <= PacketHeader::kMaxSauPacketType);

	static SaunaFsMessageSerializer singleton;
	return &singleton;
}

ChunkserverEntry::~ChunkserverEntry() {
	if (sock >= 0) { tcpclose(sock); }
	if (fwdSocket >= 0) { tcpclose(fwdSocket); }
}

// Packet/connection related

void ChunkserverEntry::attachPacket(std::unique_ptr<PacketStruct> &&packet) {
	outputPackets.push_back(std::move(packet));
}

void ChunkserverEntry::attachBuffer(std::shared_ptr<OutputBuffer> &&buffer) {
	auto packet = std::make_unique<PacketStruct>();
	passert(packet);
	packet->outputBuffer = std::move(buffer);
	outputPackets.push_back(std::move(packet));
}

void ChunkserverEntry::createAttachedPacket(std::vector<uint8_t> &packet) {
	TRACETHIS();
	std::unique_ptr<PacketStruct> outpacket = std::make_unique<PacketStruct>();
	passert(outpacket);

	outpacket->packet = std::move(packet);
	passert(outpacket->packet.data());

	outpacket->bytesLeft = outpacket->packet.size();
	outpacket->startPtr = outpacket->packet.data();

	attachPacket(std::move(outpacket));
}

uint8_t *ChunkserverEntry::createAttachedPacket(uint32_t type,
                                                uint32_t operationSize) {
	TRACETHIS();

	std::unique_ptr<PacketStruct> outPacket = std::make_unique<PacketStruct>();
	passert(outPacket);

	uint32_t packetSize = operationSize + PacketHeader::kSize;
	outPacket->packet.resize(packetSize);
	passert(outPacket->packet.data());

	outPacket->bytesLeft = packetSize;
	uint8_t *ptr = outPacket->packet.data();
	put32bit(&ptr, type);
	put32bit(&ptr, operationSize);
	outPacket->startPtr = outPacket->packet.data();

	attachPacket(std::move(outPacket));

	return ptr;
}

void ChunkserverEntry::fwdError() {
	TRACETHIS();
	sassert(messageSerializer != nullptr);
	std::vector<uint8_t> buffer;
	uint8_t status = (state == State::Connecting ? SAUNAFS_ERROR_CANTCONNECT
	                                             : SAUNAFS_ERROR_DISCONNECTED);
	messageSerializer->serializeCstoclWriteStatus(buffer, chunkId, 0, status);
	createAttachedPacket(buffer);
	state = State::WriteFinish;
}

int ChunkserverEntry::initConnection() {
	TRACETHIS();
	int status;
	// TODO(msulikowski) If we want to use a ConnectionPool, this is the right
	// place to get a connection from it
	fwdSocket = tcpsocket();
	if (fwdSocket < 0) {
		safs::log_warn_with_error_code(errno, "create socket, error");
		return kInitConnectionFailed;
	}

	if (tcpnonblock(fwdSocket) < 0) {
		safs::log_warn_with_error_code(errno, "set nonblock, error");
		tcpclose(fwdSocket);
		fwdSocket = kInvalidSocket;
		return kInitConnectionFailed;
	}

	status = tcpnumconnect(fwdSocket, fwdServer.ip, fwdServer.port);
	if (status < 0) {
		safs::log_warn_with_error_code(errno, "connect failed, error");
		tcpclose(fwdSocket);
		fwdSocket = kInvalidSocket;
		return kInitConnectionFailed;
	}

	if (status == 0) { // connected immediately
		tcpnodelay(fwdSocket);
		state = State::WriteInit;
	} else {
		state = State::Connecting;
		connectStartTimeUSec = eventloop_utime();
	}

	return kInitConnectionOK;
}

void ChunkserverEntry::retryConnect() {
	TRACETHIS();
	tcpclose(fwdSocket);
	fwdSocket = kInvalidSocket;
	connectRetryCounter++;

	if (connectRetryCounter < kConnectRetries) {
		if (initConnection() < kInitConnectionOK) {
			fwdError();
			return;
		}
	} else {
		fwdError();
		return;
	}
}

// common - delayed close

void ChunkserverEntry::checkAndApplyClosed() {
	if (pendingDelayedJobs == 0) {
		while (!writeDataBuffers.empty()) {
			getWriteInputBufferPool().put(std::move(writeDataBuffers.back()));
			writeDataBuffers.pop_back();
		}

		if (isChunkOpen) {
			job_close(*workerJobPool, kEmptyCallback, kEmptyExtra, chunkId, chunkType);
			isChunkOpen = 0;
		}
		state = State::Closed;
	}
}

void ChunkserverEntry::delayedCloseCallback(uint8_t status, void *entry) {
	TRACETHIS();
	auto *eptr = static_cast<ChunkserverEntry*>(entry);
	assert(eptr->state == State::CloseWait);

	if (eptr->isOpenWriteJobBeingProcessed() && status == SAUNAFS_STATUS_OK) {
		// this was job_open (write)
		eptr->isChunkOpen = 1;
		eptr->setNoWriteJobBeingProcessed();
	}

	assert(eptr->pendingDelayedJobs > 0);
	eptr->pendingDelayedJobs--;
	eptr->checkAndApplyClosed();
}

void ChunkserverEntry::delayedDiscardCallback(uint8_t status, void *entry) {
	TRACETHIS();
	(void)status;
	auto *eptr = static_cast<ChunkserverEntry *>(entry);
	assert(eptr->state == State::CloseWait);

	if (status == SAUNAFS_STATUS_OK) {
		eptr->isChunkOpen = 1;
	}

	while (!eptr->toDiscardReadDataBuffers.empty() &&
	       eptr->toDiscardReadDataBuffers.front()->getStatus() != kNotSaunafsStatus) {
		getReadOutputBufferPool().put(std::move(eptr->toDiscardReadDataBuffers.front()));
		eptr->toDiscardReadDataBuffers.pop_front();
		eptr->toDiscardReadJobIds.pop_front();
	}

	assert(eptr->pendingDelayedJobs > 0);
	eptr->pendingDelayedJobs--;
	eptr->checkAndApplyClosed();
}

// Read related

void ChunkserverEntry::readDiscardCallback(uint8_t status, void *entry) {
	TRACETHIS();
	(void)status;
	auto *eptr = static_cast<ChunkserverEntry *>(entry);

	// jobId <--> related packet, maintaining the order
	assert(eptr->toDiscardReadJobIds.size() == eptr->toDiscardReadDataBuffers.size());

	auto jobsIt = eptr->toDiscardReadJobIds.begin();
	for (auto buffersIt = eptr->toDiscardReadDataBuffers.begin();
	     buffersIt != eptr->toDiscardReadDataBuffers.end();) {
		if ((*buffersIt)->getStatus() != kNotSaunafsStatus) {
			getReadOutputBufferPool().put(std::move(*buffersIt));
			buffersIt = eptr->toDiscardReadDataBuffers.erase(buffersIt);
			jobsIt = eptr->toDiscardReadJobIds.erase(jobsIt);
		} else {
			buffersIt++;
			jobsIt++;
		}
	}
}

void ChunkserverEntry::prepareDiscardReadJobs() {
	// We need to:
	// - change state of packets not taken by any hdd worker
	// - change callback in already taken ones
	// - move packets from pending to toDiscard lists

	// The jobs which are not going to be processed: all but the ones in progress
	auto disabledJobIds = workerJobPool->disableJobs(pendingReadJobIds);
	workerJobPool->changeCallback(pendingReadJobIds, readDiscardCallback, this);
	workerJobPool->changeCallback(disabledJobIds, kEmptyCallback, kEmptyExtra);
	while (!pendingReadJobIds.empty()) {
		// pendingReadJobIds and pendingReadDataBuffers should have the related elements in the
		// correct order
		if (pendingReadDataBuffers.front()->getStatus() == kNotSaunafsStatus &&
		    (disabledJobIds.empty() || disabledJobIds.front() != pendingReadJobIds.front())) {
			toDiscardReadJobIds.push_back(pendingReadJobIds.front());
			toDiscardReadDataBuffers.emplace_back(std::move(pendingReadDataBuffers.front()));
		} else {
			// Already processed packets, can be moved to the pool
			getReadOutputBufferPool().put(std::move(pendingReadDataBuffers.front()));
			if (!disabledJobIds.empty() && disabledJobIds.front() == pendingReadJobIds.front()) {
				disabledJobIds.pop_front();
			}
		}

		pendingReadJobIds.pop_front();
		pendingReadDataBuffers.pop_front();
	}
	assert(disabledJobIds.empty());
	assert(pendingReadJobIds.empty());
	assert(pendingReadDataBuffers.empty());
}

void ChunkserverEntry::readFinishedCallback(uint8_t status, void *entry) {
	TRACETHIS();
	auto *eptr = static_cast<ChunkserverEntry*>(entry);

	if (status == SAUNAFS_STATUS_OK) {
		eptr->isChunkOpen = 1;
		eptr->readContinue(eptr->maxParallelHddReadJobs);
	} else {
		// - prepare discard
		// - send status
		// - close chunk
		// - change state
		eptr->prepareDiscardReadJobs();

		std::vector<uint8_t> buffer;
		eptr->messageSerializer->serializeCstoclReadStatus(
		    buffer, eptr->chunkId, status);
		eptr->createAttachedPacket(buffer);

		if (eptr->isChunkOpen) {
			job_close(*eptr->workerJobPool, kEmptyCallback, kEmptyExtra, eptr->chunkId,
			          eptr->chunkType);
			eptr->isChunkOpen = 0;
		}

		// after sending status even if there was an error it's possible to
		// receive new requests on the same connection
		eptr->state = State::Idle;
		LOG_AVG_STOP(readOperationTimer);
	}
}

std::shared_ptr<OutputBuffer> ChunkserverEntry::prepareReadDataPacket(
    std::vector<uint8_t> &readDataPrefix, uint32_t jobSize, uint32_t jobOffset) {
	const uint32_t numBlocks = (jobSize + SFSBLOCKSIZE - 1) / SFSBLOCKSIZE;

	readDataPrefix.clear();
	messageSerializer->serializePrefixOfCstoclReadData(
	    readDataPrefix, chunkId, offset, std::min<uint32_t>(jobSize, SFSBLOCKSIZE - jobOffset));

	auto buffer = getReadOutputBufferPool().get(readDataPrefix.size(), numBlocks);

	for (uint32_t i = 0; i < numBlocks; i++) {
		if (i > 0) {  // first block is already serialized
			readDataPrefix.clear();
			messageSerializer->serializePrefixOfCstoclReadData(
			    readDataPrefix, chunkId, offset - jobOffset + (i * SFSBLOCKSIZE), SFSBLOCKSIZE);
		}

		if (buffer->copyIntoBuffer(OutputBuffer::BufferType::Header, readDataPrefix) !=
		    static_cast<ssize_t>(readDataPrefix.size())) {
			if (buffer) { getReadOutputBufferPool().put(std::move(buffer)); }

			return kInvalidPacket;
		}
	}

	return buffer;
}

void ChunkserverEntry::readContinue(uint16_t callMaxParallelHddReadJobs) {
	TRACETHIS2(offset, size);

	while (!pendingReadDataBuffers.empty() &&
	       pendingReadDataBuffers.front()->getStatus() == SAUNAFS_STATUS_OK) {
		attachBuffer(std::move(pendingReadDataBuffers.front()));
		pendingReadDataBuffers.pop_front();
		workerJobPool->changeCallback(pendingReadJobIds.front(), kEmptyCallback, kEmptyExtra);
		pendingReadJobIds.pop_front();
	}

	if (pendingReadDataBuffers.empty() && size == 0) {  // everything has been read
		std::vector<uint8_t> buffer;
		messageSerializer->serializeCstoclReadStatus(
		    buffer, chunkId, SAUNAFS_STATUS_OK);
		createAttachedPacket(buffer);
		sassert(isChunkOpen);

		job_close(*workerJobPool, kEmptyCallback, kEmptyExtra, chunkId, chunkType);
		isChunkOpen = 0;
		// no error - do not disconnect - go direct to the IDLE state, ready for
		// requests on the same connection
		state = State::Idle;
		LOG_AVG_STOP(readOperationTimer);
	} else {
		std::vector<uint8_t> readDataPrefix;

		while (size > 0 && pendingReadDataBuffers.size() < callMaxParallelHddReadJobs) {
			const uint32_t totalRequestSize = size;
			const uint32_t thisPartOffset = offset % SFSBLOCKSIZE;
			const uint32_t thisPartSize = std::min<uint32_t>(
			    totalRequestSize, maxBlocksPerHddReadJob * SFSBLOCKSIZE - thisPartOffset);
			const uint16_t totalRequestBlocks =
			    (totalRequestSize + thisPartOffset + SFSBLOCKSIZE - 1) / SFSBLOCKSIZE;

			auto buffer = prepareReadDataPacket(readDataPrefix, thisPartSize, thisPartOffset);
			if (buffer == kInvalidPacket) {
				state = State::Close;
				return;
			}

			pendingReadDataBuffers.emplace_back(std::move(buffer));

			uint32_t readAheadBlocks = 0;
			uint32_t maxReadBehindBlocks = 0;

			if (!static_cast<bool>(isChunkOpen)) {
				if (gHDDReadAhead.blocksToBeReadAhead() > 0) {
					readAheadBlocks = totalRequestBlocks + gHDDReadAhead.blocksToBeReadAhead();
				}
				// Try not to influence slow streams too much:
				maxReadBehindBlocks =
				    std::min(totalRequestBlocks, gHDDReadAhead.maxBlocksToBeReadBehind());
			}

			uint32_t readJobId =
			    job_read(*workerJobPool, readFinishedCallback, this, chunkId, chunkVersion,
			             chunkType, offset, thisPartSize, maxReadBehindBlocks, readAheadBlocks,
			             pendingReadDataBuffers.back().get(), !isChunkOpen);

			if (readJobId == 0) {
				getReadOutputBufferPool().put(std::move(pendingReadDataBuffers.back()));
				pendingReadDataBuffers.pop_back();
				state = State::Close;
				return;
			}
			pendingReadJobIds.push_back(readJobId);

			offset += thisPartSize;
			size -= thisPartSize;
		}
	}
}

void ChunkserverEntry::ping(const uint8_t *data, PacketHeader::Length length) {
	static constexpr uint32_t kExpectedPingSize = sizeof(uint32_t);

	if (length != kExpectedPingSize) {
		state = State::Close;
		return;
	}

	uint32_t opSize;
	deserialize(data, length, opSize);
	createAttachedPacket(ANTOAN_PING_REPLY, opSize);
}

void ChunkserverEntry::readInit(const uint8_t *data, PacketHeader::Type type,
                                PacketHeader::Length length) {
	TRACETHIS2(type, length);

	// Deserialize request
	sassert(type == SAU_CLTOCS_READ);
	try {
		PacketVersion v;
		deserializePacketVersionNoHeader(data, length, v);
		sassert(v == cltocs::read::kECChunks);
		cltocs::read::deserialize(data, length, chunkId, chunkVersion, chunkType, offset, size);
		messageSerializer = MessageSerializer::getSerializer(type);
	} catch (Exception &) {
		safs::log_info("({}) Cannot deserialize READ message (type:{:X}, length:{})", __func__,
		               type, length);
		state = State::Close;
		return;
	}
	// Check if the request is valid
	std::vector<uint8_t> instantResponseBuffer;
	if (size == 0) {
		messageSerializer->serializeCstoclReadStatus(
		    instantResponseBuffer, chunkId, SAUNAFS_STATUS_OK);
	} else if (size > SFSCHUNKSIZE) {
		messageSerializer->serializeCstoclReadStatus(
		    instantResponseBuffer, chunkId, SAUNAFS_ERROR_WRONGSIZE);
	} else if (offset >= SFSCHUNKSIZE || offset + size > SFSCHUNKSIZE) {
		messageSerializer->serializeCstoclReadStatus(
		    instantResponseBuffer, chunkId, SAUNAFS_ERROR_WRONGOFFSET);
	}
	if (!instantResponseBuffer.empty()) {
		createAttachedPacket(instantResponseBuffer);
		return;
	}
	// Process the request
	stats_hlopr++;
	state = State::Read;
	LOG_AVG_START0(readOperationTimer, "csserv_read");
	readContinue(1);
}

void ChunkserverEntry::prefetch(const uint8_t *data, PacketHeader::Type type,
                                PacketHeader::Length length) {
	sassert(type == SAU_CLTOCS_PREFETCH);
	PacketVersion v;
	try {
		deserializePacketVersionNoHeader(data, length, v);
		sassert(v == cltocs::prefetch::kECChunks);
		cltocs::prefetch::deserialize(data, length, chunkId, chunkVersion, chunkType, offset, size);
	} catch (Exception &) {
		safs::log_info("({}) Cannot deserialize PREFETCH message (type:{:X}, length:{})", __func__,
		               type, length);
		state = State::Close;
		return;
	}
	// Start prefetching in background, don't wait for it to complete
	auto firstBlock = offset / SFSBLOCKSIZE;
	auto lastByte = offset + size - 1;
	auto lastBlock = lastByte / SFSBLOCKSIZE;
	auto nrOfBlocks = lastBlock - firstBlock + 1;
	job_prefetch(*workerJobPool, chunkId, chunkType, firstBlock, nrOfBlocks);
}

// Write helpers

/// @brief Returns a pair of type and length obtained from a header pointer.
/// @param headerPtr The pointer to the header (must point to at least 8 bytes).
/// @return A pair where the first element is the type and the second is the length.
static std::pair<uint32_t, uint32_t> getTypeAndLengthFromHeader(const uint8_t *headerPtr) {
	uint32_t type;
	uint32_t length;
	get32bit(&headerPtr, type);
	get32bit(&headerPtr, length);
	return {type, length};
}

bool ChunkserverEntry::isLastHeaderTypeWriteData() {
	return getTypeAndLengthFromHeader(headerBuffer).first == SAU_CLTOCS_WRITE_DATA;
}

void ChunkserverEntry::updateUsingWriteStatusAndReply(uint8_t status, uint32_t writeId) {
	if (status != SAUNAFS_STATUS_OK) {
		createAttachedWriteStatus(status, writeId);
		state = State::WriteFinish;
		return;
	}

	// We can consider that the write was successful
	if (state == State::WriteLast) {
		createAttachedWriteStatus(status, writeId);
		return;
	}

	// state is WriteForward or WriteFinish
	if (partiallyCompletedWrites.count(writeId) > 0) {
		// found - it means that it was added by status_receive, ie. next
		// chunkserver from a chain finished writing before our worker
		createAttachedWriteStatus(status, writeId);
		partiallyCompletedWrites.erase(writeId);
	} else {
		// not found - so add it
		partiallyCompletedWrites.insert(writeId);
	}
}

bool ChunkserverEntry::isOpenWriteJobBeingProcessed() {
	return writeJobId != 0 && writeJobWriteId == 0;
}

bool ChunkserverEntry::isWriteJobBeingProcessed() {
	return writeJobId != 0;
}

void ChunkserverEntry::setNoWriteJobBeingProcessed() {
	writeJobId = 0;
}

void ChunkserverEntry::startOpenWriteJob() {
	writeJobWriteId = 0;
	writeJobId = job_open(*workerJobPool, openWriteFinishedCallback, this, chunkId, chunkType);
}

void ChunkserverEntry::startNextWriteJob() {
	if (writeDataBuffers.empty()) {
		safs::log_warn("({}) Called with no write data buffers.", __func__);
		return;
	}

	if (isWriteJobBeingProcessed()) {
		safs::log_warn("({}) Called with write job already in progress.", __func__);
		return;
	}

	/// Start the next write job: it is always the first write data buffer
	writeJobWriteId = writeDataBuffers.front()->getLastWriteId();
	writeJobId = job_write(*workerJobPool, writeFinishedCallback, this, chunkId, chunkVersion,
	                       chunkType, writeDataBuffers.front().get());
}

void ChunkserverEntry::writeCurrentInputPacket() {
	TRACETHIS();

	writeDataBuffers.emplace_back(std::move(inputBuffer));
	if (!isWriteJobBeingProcessed()) {
		startNextWriteJob();
	}
}

void ChunkserverEntry::checkNextPacket() {
	TRACETHIS();

	if (!writeDataBuffers.empty()) {
		// there is a write buffer ready to be written, there should not be any
		// write jobs being processed.
		startNextWriteJob();
		return;
	}

	if (inputBuffer != nullptr && !inputBuffer->isBeingUpdated()) {
		writeCurrentInputPacket();
	}
}

// Write operations

void ChunkserverEntry::createAttachedWriteStatus(uint8_t status, uint32_t writeId) {
	sassert(messageSerializer != nullptr);
	std::vector<uint8_t> buffer;
	messageSerializer->serializeCstoclWriteStatus(buffer, chunkId, writeId, status);
	createAttachedPacket(buffer);
}

void ChunkserverEntry::writeFinishedCallback(uint8_t status, void *entry) {
	TRACETHIS();
	auto *eptr = static_cast<ChunkserverEntry *>(entry);
	sassert(eptr->messageSerializer != nullptr);

	if (eptr->isOpenWriteJobBeingProcessed()) {
		safs::log_warn("({}) Inconsistent state: writeJobWriteId: {}, chunkId: {}, status: {}.",
		               __func__, eptr->writeJobWriteId, eptr->chunkId, status);
	}
	eptr->setNoWriteJobBeingProcessed();

	auto statusWithWriteIdToReply = eptr->writeDataBuffers.front()->getStatuses();
	getWriteInputBufferPool().put(std::move(eptr->writeDataBuffers.front()));
	eptr->writeDataBuffers.pop_front();

	for (const auto &[status, writeId] : statusWithWriteIdToReply) {
		eptr->updateUsingWriteStatusAndReply(status, writeId);
		if (status != SAUNAFS_STATUS_OK) { return; }
	}

	eptr->checkNextPacket();
}

void ChunkserverEntry::openWriteFinishedCallback(uint8_t status, void *entry) {
	TRACETHIS();
	auto *eptr = static_cast<ChunkserverEntry *>(entry);
	sassert(eptr->messageSerializer != nullptr);

	if (!eptr->isOpenWriteJobBeingProcessed()) {
		safs::log_warn("Inconsistent state in {}: writeJobWriteId: {}, chunkId: {}, status: {}.",
		               __func__, eptr->writeJobWriteId, eptr->chunkId, status);
	}
	eptr->setNoWriteJobBeingProcessed();

	// We should assume that writeJobWriteId is 0 here, because this callback
	// is called after job_open, which should have set writeJobWriteId to 0.

	eptr->updateUsingWriteStatusAndReply(status, eptr->writeJobWriteId);
	if (status != SAUNAFS_STATUS_OK) { return; }

	eptr->isChunkOpen = 1;

	eptr->checkNextPacket();
}

void ChunkserverEntry::prepareInputBufferForWrite(bool isForward) {
	if (inputBuffer != nullptr && inputBuffer->isFull()) {
		safs::log_warn("({}) Called with full inputBuffer, isForward: {}. Writing existing buffer.",
		               __func__, isForward);
		writeCurrentInputPacket();
	}

	if (inputBuffer == nullptr) {
		inputBuffer = getWriteInputBufferPool().get(
		    isForward ? kSauWriteDataPreffixSizeForward : kSauWriteDataPreffixSize,
		    maxBlocksPerHddWriteJob);
	}

	inputBuffer->addNewWriteOperation();
}

void serializeCltocsWriteInit(std::vector<uint8_t> &buffer, uint64_t chunkId, uint32_t chunkVersion,
                              ChunkPartType chunkType,
                              const std::vector<ChunkTypeWithAddress> &chain) {
	cltocs::writeInit::serialize(buffer, chunkId, chunkVersion, chunkType, chain);
}

void ChunkserverEntry::writeInit(const uint8_t *data, PacketHeader::Type type,
                                 PacketHeader::Length length) {
	TRACETHIS();
	std::vector<ChunkTypeWithAddress> chain;

	sassert(type == SAU_CLTOCS_WRITE_INIT);
	try {
		PacketVersion v;
		deserializePacketVersionNoHeader(data, length, v);
		sassert(v == cltocs::writeInit::kECChunks);
		cltocs::writeInit::deserialize(data, length, chunkId, chunkVersion, chunkType, chain);
		messageSerializer = MessageSerializer::getSerializer(type);
	} catch (Exception &) {
		safs::log_info("Received malformed WRITE_INIT message (length: {})", length);
		state = State::Close;
		return;
	}

	if (!chain.empty()) {
		// Create a chain -- connect to the next chunkserver
		fwdServer = chain[0].address;
		chain.erase(chain.begin());
		serializeCltocsWriteInit(fwdInitPacket, chunkId, chunkVersion, chunkType, chain);
		fwdOutputPacket.startPtr = fwdInitPacket.data();
		fwdOutputPacket.bytesLeft = fwdInitPacket.size();
		connectRetryCounter = 0;
		if (initConnection() < kInitConnectionOK) {
			std::vector<uint8_t> buffer;
			messageSerializer->serializeCstoclWriteStatus(
			    buffer, chunkId, 0, SAUNAFS_ERROR_CANTCONNECT);
			createAttachedPacket(buffer);
			state = State::WriteFinish;
			return;
		}
	} else {
		state = State::WriteLast;
	}

	stats_hlopw++;
	startOpenWriteJob();
}

void ChunkserverEntry::writeData(const uint8_t *data, PacketHeader::Type type,
                                 PacketHeader::Length length) {
	TRACETHIS();
	uint64_t opChunkId;
	uint32_t writeId;
	uint16_t blocknum;
	uint32_t opOffset;
	uint32_t opSize;
	uint32_t crc;

	sassert(type == SAU_CLTOCS_WRITE_DATA);
	try {
		const auto *serializer = MessageSerializer::getSerializer(type);
		if (messageSerializer != serializer) {
			safs::log_info("Received WRITE_DATA message incompatible with WRITE_INIT");
			state = State::Close;
			return;
		}
		cltocs::writeData::deserializePrefix(data, kSauWriteDataPreffixSize, opChunkId, writeId,
		                                     blocknum, opOffset, opSize, crc);
	} catch (IncorrectDeserializationException &) {
		safs::log_info("Received malformed WRITE_DATA message (length: {})", length);
		state = State::Close;
		return;
	}

	uint8_t status = SAUNAFS_STATUS_OK;
	if (!inputBuffer->isHeaderSizeValid()) {
		status = SAUNAFS_ERROR_WRONGSIZE;
	} else if (opChunkId != chunkId) {
		status = SAUNAFS_ERROR_WRONGCHUNKID;
	}

	if (status != SAUNAFS_STATUS_OK) {
		std::vector<uint8_t> buffer;
		messageSerializer->serializeCstoclWriteStatus(buffer, opChunkId,
		                                              writeId, status);
		createAttachedPacket(buffer);
		state = State::WriteFinish;
		return;
	}

	inputBuffer->setupLastWriteOperation(blocknum, opOffset, opSize, writeId, crc);

	// No write jobs in progress or current input buffer is full - write it
	if (!isWriteJobBeingProcessed() || inputBuffer->isFull()) {
		writeCurrentInputPacket();
	}
}

void ChunkserverEntry::writeStatus(const uint8_t *data, PacketHeader::Type type,
                                   PacketHeader::Length length) {
	TRACETHIS();
	uint64_t opChunkId;
	uint32_t writeId;
	uint8_t status;

	sassert(type == SAU_CSTOCL_WRITE_STATUS);
	sassert(messageSerializer != nullptr);
	try {
		const auto *serializer = MessageSerializer::getSerializer(type);
		if (messageSerializer != serializer) {
			safs::log_info("Received WRITE_DATA message incompatible with WRITE_INIT");
			state = State::Close;
			return;
		}
		std::vector<uint8_t> message(data, data + length);
		cstocl::writeStatus::deserialize(message, opChunkId, writeId, status);
	} catch (IncorrectDeserializationException &) {
		safs::log_info("Received malformed WRITE_STATUS message (length: {})", length);
		state = State::Close;
		return;
	}

	if (chunkId != opChunkId) {
		status = SAUNAFS_ERROR_WRONGCHUNKID;
		writeId = 0;
	}

	updateUsingWriteStatusAndReply(status, writeId);
}

void ChunkserverEntry::writeEnd(const uint8_t *data, uint32_t length) {
	TRACETHIS();
	uint64_t opChunkId;
	messageSerializer = nullptr;

	try {
		cltocs::writeEnd::deserialize(data, length, opChunkId);
	} catch (IncorrectDeserializationException&) {
		safs::log_info("Received malformed WRITE_END message (length: {})", length);
		state = State::WriteFinish;
		return;
	}

	if (opChunkId != chunkId) {
		safs::log_info(
		    "Received malformed WRITE_END message (got chunkId={:016X}, expected {:016X})",
		    opChunkId, chunkId);
		state = State::WriteFinish;
		return;
	}

	if (isWriteJobBeingProcessed() || !partiallyCompletedWrites.empty() || !outputPackets.empty()) {
		/*
		 * WRITE_END received too early:
		 * isWriteJobBeingProcessed -- hdd worker is working (writing some data)
		 * !eptr->partiallyCompletedWrites.empty() -- there are write tasks
		 * which have not been acked by our hdd worker EX-or next chunkserver
		 * from a chain !outputPackets.empty() -- there is a status being
		 * send
		 */
		// TODO(msulikowski) temporary syslog message. May be useful until this
		// code is fully tested
		safs::log_info("Received WRITE_END message too early");
		state = State::WriteFinish;
		return;
	}

	if (isChunkOpen) {
		job_close(*workerJobPool, nullptr, nullptr, chunkId, chunkType);
		isChunkOpen = 0;
	}

	if (fwdSocket > 0) {
		// TODO(msulikowski) if we want to use a ConnectionPool, this the right
		// place to put the connection to the pool.
		tcpclose(fwdSocket);
		fwdSocket = kInvalidSocket;
	}

	if (inputBuffer != nullptr) {
		safs::log_info("Received WRITE_END message while there is still non-null input buffer. Returning it to pool.");
		getWriteInputBufferPool().put(std::move(inputBuffer));
	}

	state = State::Idle;
}

void ChunkserverEntry::sauGetChunkBlocksFinishedCallback(uint8_t status,
                                                         void *entry) {
	TRACETHIS();
	auto *eptr = static_cast<ChunkserverEntry*>(entry);
	eptr->getBlocksJobId = 0;
	std::vector<uint8_t> buffer;
	cstocs::getChunkBlocksStatus::serialize(buffer, eptr->chunkId,
	                                        eptr->chunkVersion, eptr->chunkType,
	                                        eptr->getBlocksJobResult, status);
	eptr->createAttachedPacket(buffer);
	eptr->state = State::Idle;
}

void ChunkserverEntry::sauGetChunkBlocks(const uint8_t *data, uint32_t length) {
	try {
		PacketVersion v;
		deserializePacketVersionNoHeader(data, length, v);
		sassert(v == cstocs::getChunkBlocks::kECChunks);
		cstocs::getChunkBlocks::deserialize(data, length, chunkId, chunkVersion, chunkType);
	} catch (Exception &) {
		safs::log_info("Received malformed SAU_CSTOCS_GET_CHUNK_BLOCKS message (length: {})",
		               length);
		state = State::Close;
		return;
	}

	getBlocksJobId = job_get_blocks(*workerJobPool, sauGetChunkBlocksFinishedCallback, this,
	                                chunkId, chunkVersion, chunkType, &getBlocksJobResult);
	state = State::GetBlock;
}

/* IDLE operations */

void ChunkserverEntry::hddListV2([[maybe_unused]] const uint8_t *data, uint32_t length) {
	TRACETHIS();

	if (length != 0) {  // This packet should not have any data
		safs::log_info("CLTOCS_HDD_LIST_V2 - wrong size ({}/0)", length);
		state = State::Close;
		return;
	}

	std::lock_guard disksLock(gDisksMutex);

	uint32_t opSize = hddGetSerializedSizeOfAllDiskInfosV2();
	uint8_t *ptr = createAttachedPacket(CSTOCL_HDD_LIST_V2, opSize);
	hddSerializeAllDiskInfosV2(ptr);
}

void ChunkserverEntry::listDiskGroups([[maybe_unused]] const uint8_t *data,
                                      [[maybe_unused]] uint32_t length) {
	TRACETHIS();

	std::string diskGroups = hddGetDiskGroups();

	// 4 bytes for the size of the string + 1 byte for the null character
	static constexpr uint8_t kSerializedSizePlusNullChar = 5;

	uint8_t *ptr =
	    createAttachedPacket(CSTOCL_ADMIN_LIST_DISK_GROUPS,
	                         diskGroups.size() + kSerializedSizePlusNullChar);
	serialize(&ptr, diskGroups);
}

void ChunkserverEntry::generateChartPNGorCSV(const uint8_t *data,
                                             uint32_t length) {
	TRACETHIS();
	uint32_t chartid;
	uint8_t *ptr;
	uint32_t len;

	if (length != kGenerateChartExpectedPacketSize) {
		safs::log_info("CLTOAN_CHART - wrong size ({}/{})", length,
		               kGenerateChartExpectedPacketSize);
		state = State::Close;
		return;
	}
	get32bit(&data, chartid);
	if(chartid <= CHARTS_CSV_CHARTID_BASE) {
		len = charts_make_png(chartid);
		ptr = createAttachedPacket(ANTOCL_CHART, len);
		if (len > 0) {
			charts_get_png(ptr);
		}
	} else {
		len = charts_make_csv(chartid % CHARTS_CSV_CHARTID_BASE);
		ptr = createAttachedPacket(ANTOCL_CHART, len);
		if (len > 0) {
			charts_get_csv(ptr);
		}
	}
}

void ChunkserverEntry::generateChartData(const uint8_t *data, uint32_t length) {
	TRACETHIS();
	uint32_t chartid;
	uint8_t *ptr;
	uint32_t len;

	if (length != kGenerateChartExpectedPacketSize) {
		safs::log_info("CLTOAN_CHART_DATA - wrong size ({}/{})", length,
		               kGenerateChartExpectedPacketSize);
		state = State::Close;
		return;
	}
	get32bit(&data, chartid);
	len = charts_datasize(chartid);
	ptr = createAttachedPacket(ANTOCL_CHART_DATA, len);
	if (len > 0) {
		charts_makedata(ptr, chartid);
	}
}

void ChunkserverEntry::testChunk(const uint8_t *data, uint32_t length) {
	try {
		PacketVersion v;
		deserializePacketVersionNoHeader(data, length, v);
		ChunkWithVersionAndType chunk;
		sassert(v == cltocs::testChunk::kECChunks);
		cltocs::testChunk::deserialize(data, length, chunk.id, chunk.version, chunk.type);
		hddAddChunkToTestQueue(chunk);
	} catch (Exception &e) {
		safs::log_info("SAU_CLTOCS_TEST_CHUNK - bad packet: {} (length: {})", e.what(), length);
		state = State::Close;
		return;
	}
}

void ChunkserverEntry::outputCheckReadFinished() {
	TRACETHIS();
	if (state == State::Read && (!pendingReadDataBuffers.empty() || size > 0)) {
		readContinue(maxParallelHddReadJobs);
	}
}

void ChunkserverEntry::closeJobs() {
	TRACETHIS();

	if (!pendingReadJobIds.empty()) {
		prepareDiscardReadJobs();
	}
	if (!toDiscardReadJobIds.empty()) {
		// Already disabled jobs
		workerJobPool->changeCallback(toDiscardReadJobIds, delayedDiscardCallback, this);
		pendingDelayedJobs += toDiscardReadJobIds.size();
		state = State::CloseWait;
	}

	if (isWriteJobBeingProcessed()) {
		workerJobPool->disableJob(writeJobId);
		workerJobPool->changeCallback(writeJobId, delayedCloseCallback, this);

		if (inputBuffer != nullptr) {
			/// Drop the input buffer, it won't be used anymore
			getWriteInputBufferPool().put(std::move(inputBuffer));
		}

		pendingDelayedJobs++;
		state = State::CloseWait;
	} else if (getBlocksJobId > 0) {
		workerJobPool->disableJob(getBlocksJobId);
		workerJobPool->changeCallback(getBlocksJobId, delayedCloseCallback, this);
		pendingDelayedJobs++;
		state = State::CloseWait;
	} else {
		// Not necessary to close chunk - checkAndApplyClosed will do it
		checkAndApplyClosed();
	}
}

void ChunkserverEntry::gotPacket(uint32_t type, const uint8_t *data,
                                 uint32_t length) {
	TRACETHIS();

	if (type == ANTOAN_NOP) {
		return;
	}
	if (type == ANTOAN_UNKNOWN_COMMAND) { // for future use
		return;
	}
	if (type == ANTOAN_BAD_COMMAND_SIZE) { // for future use
		return;
	}
	if (state == State::Idle) {
		switch (type) {
		case ANTOAN_PING:
			ping(data, length);
			break;
		case SAU_CLTOCS_READ:
			readInit(data, type, length);
			break;
		case SAU_CLTOCS_PREFETCH:
			prefetch(data, type, length);
			break;
		case SAU_CLTOCS_WRITE_INIT:
			writeInit(data, type, length);
			break;
		case SAU_CSTOCS_GET_CHUNK_BLOCKS:
			sauGetChunkBlocks(data, length);
			break;
		case CLTOCS_HDD_LIST_V2:
			hddListV2(data, length);
			break;
		case CLTOCS_ADMIN_LIST_DISK_GROUPS:
			listDiskGroups(data, length);
			break;
		case CLTOAN_CHART:
			generateChartPNGorCSV(data, length);
			break;
		case CLTOAN_CHART_DATA:
			generateChartData(data, length);
			break;
		case SAU_CLTOCS_TEST_CHUNK:
			testChunk(data, length);
			break;
		default:
			safs::log_info("Got invalid message in Idle state (type:{})", type);
			state = State::Close;
			break;
		}
	} else if (state == State::WriteLast) {
		switch (type) {
		case SAU_CLTOCS_WRITE_DATA:
			writeData(data, type, length);
			break;
		case SAU_CLTOCS_WRITE_END:
			writeEnd(data, length);
			break;
		default:
			safs::log_info("Got invalid message in WriteLast state (type:{})", type);
			state = State::Close;
			break;
		}
	} else if (state == State::WriteForward) {
		switch (type) {
		case SAU_CLTOCS_WRITE_DATA:
			writeData(data, type, length);
			break;
		case SAU_CSTOCL_WRITE_STATUS:
			writeStatus(data, type, length);
			break;
		case SAU_CLTOCS_WRITE_END:
			writeEnd(data, length);
			break;
		default:
			safs::log_info("Got invalid message in WriteForward state (type:{})", type);
			state = State::Close;
			break;
		}
	} else if (state == State::WriteFinish) {
		switch (type) {
		case SAU_CLTOCS_WRITE_DATA:
		case SAU_CLTOCS_WRITE_END:
			return;
		default:
			safs::log_info("Got invalid message in WriteFinish state (type:{})", type);
			state = State::Close;
		}
	} else {
		safs::log_info("Got invalid message (type:{})", type);
		state = State::Close;
	}
}

bool ChunkserverEntry::processRWBytes(int bytesRW, PacketStruct &packet, bool shouldForwardError,
                                      const char *callerName, bool isRead) {
	if (bytesRW == 0) {
		if (shouldForwardError) {
			fwdError();
		} else {
			state = State::Close;
		}

		return false;
	}

	if (bytesRW < 0) {
		if (errno != EAGAIN) {
			safs::log_info_with_error_code(errno, "({}) {} error", callerName,
			                               isRead ? "read" : "write");

			if (shouldForwardError) {
				fwdError();
			} else {
				state = State::Close;
			}
		}
		return false;
	}

	if (isRead) {
		stats_bytesin += bytesRW;
	} else {
		stats_bytesout += bytesRW;
	}
	packet.startPtr += bytesRW;
	packet.bytesLeft -= bytesRW;

	return true;
}

bool ChunkserverEntry::readHeader(int socket, PacketStruct &packet, uint8_t *headerBuf,
                                  Mode &targetMode) {
	// At this point, packet.startPtr points to the current position in the header buffer,
	// and packet.bytesLeft is the number of bytes remaining to read to complete the header.
	// Therefore, packet.startPtr + packet.bytesLeft should equal headerBuf + PacketHeader::kSize,
	// ensuring that the header buffer will be fully filled after reading the remaining bytes.
	sassert(packet.startPtr + packet.bytesLeft == headerBuf + PacketHeader::kSize);

	bool fromForward = (socket == fwdSocket);
	bool mustForward = (state == State::WriteForward && !fromForward);
	sassert(targetMode == Mode::Header);

	auto bytesRead = ::read(socket, packet.startPtr, packet.bytesLeft);

	if (!processRWBytes(bytesRead, packet, fromForward, __func__, true)) { return false; }

	if (packet.bytesLeft > 0) { return false; }

	auto [type, length] = getTypeAndLengthFromHeader(headerBuf);

	if (length > kMaxPacketSize) {
		safs::log_warn("({}) packet too long ({}/{})", __func__, length, kMaxPacketSize);

		if (fromForward) {
			fwdError();
		} else {
			state = State::Close;
		}
		return false;
	}

	if (type == SAU_CLTOCS_WRITE_DATA) {
		prepareInputBufferForWrite(mustForward);

		if (mustForward) {
			inputBuffer->copyIntoBuffer(InputBuffer::BufferType::Header, headerBuffer,
			                            PacketHeader::kSize);
		}
		// Setting up packet.startPtr is not needed, inputBuffer will be used for reading data
	} else {
		if (mustForward) {
			packet.packet.resize(PacketHeader::kSize + length);
			passert(packet.packet.data());
			std::copy(headerBuffer, headerBuffer + PacketHeader::kSize, packet.packet.begin());
			packet.startPtr = packet.packet.data() + PacketHeader::kSize;
		} else if (length > 0) {  // asserts might fail if length is 0
			packet.packet.resize(length);
			passert(packet.packet.data());
			packet.startPtr = packet.packet.data();
		}
	}
	packet.bytesLeft = length;

	if (mustForward && (type == SAU_CLTOCS_WRITE_DATA || type == SAU_CLTOCS_WRITE_END)) {
		fwdOutputPacket.bytesLeft = PacketHeader::kSize;
		// Use the correct buffer for forwarding
		if (type == SAU_CLTOCS_WRITE_DATA) {
			fwdOutputPacket.startPtr =
			    const_cast<uint8_t *>(inputBuffer->getStartLastWriteOperationHeader());
		} else {
			fwdOutputPacket.startPtr = packet.packet.data();
		}
	}

	targetMode = Mode::Data;
	return true;
}

bool ChunkserverEntry::readData(int socket, PacketStruct &packet) {
	bool fromForward = (socket == fwdSocket);
	bool mustForward = (state == State::WriteForward && !fromForward);
	sassert((mode == Mode::Data && !fromForward) || (fwdMode == Mode::Data && fromForward));

	if (packet.bytesLeft == 0) { return true; }

	int bytesRead{0};
	if (!fromForward && isLastHeaderTypeWriteData()) {
		bytesRead = inputBuffer->readFromSocket(socket, packet.bytesLeft);
	} else {
		bytesRead = ::read(socket, packet.startPtr, packet.bytesLeft);
	}

	if (!processRWBytes(bytesRead, packet, fromForward, __func__, true)) { return false; }

	if (mustForward && fwdOutputPacket.startPtr != nullptr) {
		fwdOutputPacket.bytesLeft += bytesRead;
	}
	if (!mustForward && packet.bytesLeft > 0) { return false; }

	return true;
}

bool ChunkserverEntry::writePacket(int socket, PacketStruct &packet) {
	bool toForward = (socket == fwdSocket);
	bool isWriteInit = (state == State::WriteInit);

	if (packet.bytesLeft == 0) { return true; }

	int bytesWritten{0};
	if (!isWriteInit && toForward && isLastHeaderTypeWriteData()) {
		bytesWritten = inputBuffer->writeToSocket(socket, packet.bytesLeft);
	} else {
		sassert(packet.startPtr != nullptr);
		bytesWritten = ::write(socket, packet.startPtr, packet.bytesLeft);
	}

	if (!processRWBytes(bytesWritten, packet, toForward, __func__, false)) { return false; }

	if (packet.bytesLeft > 0) { return false; }

	return true;
}

void ChunkserverEntry::processPacket(PacketStruct &packet, uint8_t *headerBuf, Mode &targetMode,
                                     bool fromForward) {
	sassert(targetMode == Mode::Data);
	bool mustForward = (state == State::WriteForward && !fromForward);

	auto [type, length] = getTypeAndLengthFromHeader(headerBuf);

	targetMode = Mode::Header;
	packet.bytesLeft = PacketHeader::kSize;
	packet.startPtr = headerBuf;

	uint32_t offsetFromSkipHeaderInForward = mustForward ? PacketHeader::kSize : 0;
	const uint8_t *packetData{nullptr};
	if (!fromForward && isLastHeaderTypeWriteData()) {
		packetData =
		    inputBuffer->getStartLastWriteOperationHeader() + offsetFromSkipHeaderInForward;
	} else {
		packetData = packet.packet.data() + offsetFromSkipHeaderInForward;
	}

	gotPacket(type, packetData, length);
}

void ChunkserverEntry::fwdConnected() {
	TRACETHIS();
	int status = tcpgetstatus(fwdSocket);
	if (status) {
		safs::log_warn_with_error_code(errno, "connection failed, error");
		fwdError();
		return;
	}
	tcpnodelay(fwdSocket);
	state = State::WriteInit;
}

void ChunkserverEntry::fwdRead() {
	TRACETHIS();

	if (fwdMode == Mode::Header &&
	    !readHeader(fwdSocket, fwdInputPacket, fwdHeaderBuffer, fwdMode)) {
		return;
	}

	if (fwdMode == Mode::Data) {
		if (!readData(fwdSocket, fwdInputPacket)) { return; }

		processPacket(fwdInputPacket, fwdHeaderBuffer, fwdMode, true);
	}
}

void ChunkserverEntry::fwdWrite() {
	TRACETHIS();

	if (!writePacket(fwdSocket, fwdOutputPacket)) { return; }

	if (fwdOutputPacket.bytesLeft == 0) {
		fwdInitPacket.clear();
		fwdOutputPacket.startPtr = nullptr;
		fwdMode = Mode::Header;
		fwdInputPacket.bytesLeft = PacketHeader::kSize;
		fwdInputPacket.startPtr = fwdHeaderBuffer;
		state = State::WriteForward;
	}
}

void ChunkserverEntry::forward() {
	TRACETHIS();

	if (mode == Mode::Header && !readHeader(sock, inputPacket, headerBuffer, mode)) { return; }

	if (!readData(sock, inputPacket)) { return; }

	if (!writePacket(fwdSocket, fwdOutputPacket)) { return; }

	if (inputPacket.bytesLeft == 0 && fwdOutputPacket.bytesLeft == 0) {
		processPacket(inputPacket, headerBuffer, mode, false);
		fwdOutputPacket.startPtr = nullptr;
	}
}

void ChunkserverEntry::readFromSocket() {
	TRACETHIS();

	if (mode == Mode::Header && !readHeader(sock, inputPacket, headerBuffer, mode)) { return; }

	if (mode == Mode::Data) {
		if (!readData(sock, inputPacket)) { return; }

		processPacket(inputPacket, headerBuffer, mode, false);
	}
}

void ChunkserverEntry::writeToSocket() {
	TRACETHIS();
	PacketStruct *pack = nullptr;

	for (;;) {
		if (outputPackets.empty()) { return; }

		pack = outputPackets.front().get();

		if (pack->outputBuffer) {
			size_t bytesInBufferBefore = pack->outputBuffer->bytesInABuffer();
			OutputBuffer::WriteStatus ret =
			    pack->outputBuffer->writeOutToAFileDescriptor(sock);
			size_t bytesInBufferAfter = pack->outputBuffer->bytesInABuffer();
			massert(bytesInBufferAfter <= bytesInBufferBefore,
					"New bytes in pack->outputBuffer after sending some data");
			stats_bytesout += (bytesInBufferBefore - bytesInBufferAfter);
			if (ret == OutputBuffer::WriteStatus::Error) {
				safs::log_info_with_error_code(errno, "({}) write error", __func__);
				state = State::Close;
				return;
			} else if (ret == OutputBuffer::WriteStatus::Again) {
				return;
			}
		} else if (!writePacket(sock, *pack)) {
			return;
		}
		// packet has been sent
		if (pack->outputBuffer) {
			getReadOutputBufferPool().put(std::move(pack->outputBuffer));
		}
		outputPackets.pop_front();
		outputCheckReadFinished();
	}
}
