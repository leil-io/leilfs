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
	uint8_t status =
	    (state == State::Connecting ? SAUNAFS_ERROR_CANTCONNECT : SAUNAFS_ERROR_DISCONNECTED);
	createAttachedWriteStatus(chunkId, status, 0);
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

void ChunkserverEntry::applyClosed() {
	while (!writeDataBuffers.empty()) {
		getWriteInputBufferPool().put(std::move(writeDataBuffers.back()));
		writeDataBuffers.pop_back();
	}
	state = State::Closed;
}

void ChunkserverEntry::delayedCloseCallback(uint8_t status, void *entry) {
	TRACETHIS();
	auto *eptr = static_cast<ChunkserverEntry*>(entry);

	if (eptr->isOpenWriteJobBeingProcessed() && status == SAUNAFS_STATUS_OK) {
		// this was job_open (write)
		eptr->isChunkOpen = 1;
		eptr->setNoWriteJobBeingProcessed();
	} else if (eptr->isReadJobBeingProcessed() && status == SAUNAFS_STATUS_OK) {
		// this was job_read -> should have opened chunk
		eptr->isChunkOpen = 1;
		eptr->setNoReadJobBeingProcessed();
	}

	if (eptr->isChunkOpen) {
		job_close(*eptr->workerJobPool, kEmptyCallback, kEmptyExtra, eptr->chunkId,
		          eptr->chunkType);
		eptr->isChunkOpen = 0;
	}

	eptr->applyClosed();
}

// Read related

bool ChunkserverEntry::isReadJobBeingProcessed() {
	return readJobId != 0;
}

void ChunkserverEntry::setNoReadJobBeingProcessed() {
	readJobId = 0;
}

void ChunkserverEntry::readFinishedCallback(uint8_t status, void *entry) {
	TRACETHIS();
	auto *eptr = static_cast<ChunkserverEntry*>(entry);

	eptr->setNoReadJobBeingProcessed();
	if (status == SAUNAFS_STATUS_OK) {
		eptr->isChunkOpen = 1;
		eptr->attachBuffer(std::move(eptr->readDataBuffer));
		eptr->readContinue();
	} else {
		// - send status
		// - close chunk
		// - change state

		std::vector<uint8_t> buffer;
		cstocl::readStatus::serialize(buffer, eptr->chunkId, status);
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
	cstocl::readData::serializePrefix(readDataPrefix, chunkId, offset,
	                                  std::min<uint32_t>(jobSize, SFSBLOCKSIZE - jobOffset));

	auto buffer = getReadOutputBufferPool().get(readDataPrefix.size(), numBlocks);

	for (uint32_t i = 0; i < numBlocks; i++) {
		if (i > 0) {  // first block is already serialized
			readDataPrefix.clear();
			cstocl::readData::serializePrefix(
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

void ChunkserverEntry::readContinue() {
	TRACETHIS2(offset, size);

	if (size == 0) {  // everything has been read
		std::vector<uint8_t> buffer;
		cstocl::readStatus::serialize(buffer, chunkId, SAUNAFS_STATUS_OK);
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
		const uint32_t totalRequestSize = size;
		const uint32_t thisPartOffset = offset % SFSBLOCKSIZE;
		const uint32_t thisPartSize = std::min<uint32_t>(
		    totalRequestSize, maxBlocksPerHddReadJob * SFSBLOCKSIZE - thisPartOffset);
		const uint16_t totalRequestBlocks =
		    (totalRequestSize + thisPartOffset + SFSBLOCKSIZE - 1) / SFSBLOCKSIZE;

		readDataBuffer = prepareReadDataPacket(readDataPrefix, thisPartSize, thisPartOffset);
		if (readDataBuffer == kInvalidPacket) {
			state = State::Close;
			return;
		}

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

		readJobId = job_read(*workerJobPool, readFinishedCallback, this, chunkId, chunkVersion,
		                     chunkType, offset, thisPartSize, maxReadBehindBlocks, readAheadBlocks,
		                     readDataBuffer.get(), !isChunkOpen);

		if (!isReadJobBeingProcessed()) { // readJobId == 0
			getReadOutputBufferPool().put(std::move(readDataBuffer));
			state = State::Close;
			return;
		}

		offset += thisPartSize;
		size -= thisPartSize;
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
	} catch (Exception &) {
		safs::log_info("({}) Cannot deserialize READ message (type:{:X}, length:{})", __func__,
		               type, length);
		state = State::Close;
		return;
	}
	// Check if the request is valid
	std::vector<uint8_t> instantResponseBuffer;
	if (size == 0) {
		cstocl::readStatus::serialize(instantResponseBuffer, chunkId, SAUNAFS_STATUS_OK);
	} else if (size > SFSCHUNKSIZE) {
		cstocl::readStatus::serialize(instantResponseBuffer, chunkId, SAUNAFS_ERROR_WRONGSIZE);
	} else if (offset >= SFSCHUNKSIZE || offset + size > SFSCHUNKSIZE) {
		cstocl::readStatus::serialize(instantResponseBuffer, chunkId, SAUNAFS_ERROR_WRONGOFFSET);
	}
	if (!instantResponseBuffer.empty()) {
		createAttachedPacket(instantResponseBuffer);
		return;
	}
	// Process the request
	stats_hlopr++;
	state = State::Read;
	LOG_AVG_START0(readOperationTimer, "csserv_read");
	readContinue();
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

bool ChunkserverEntry::isLastHeaderTypeWriteData() {
	uint32_t type;
	const uint8_t *ptr = headerBuffer;
	get32bit(&ptr, type);
	return type == SAU_CLTOCS_WRITE_DATA;
}

void ChunkserverEntry::updateUsingWriteStatusAndReply(uint8_t status, uint32_t writeId) {
	if (status != SAUNAFS_STATUS_OK) {
		createAttachedWriteStatus(chunkId, status, writeId);
		state = State::WriteFinish;
		return;
	}

	// We can consider that the write was successful
	if (state == State::WriteLast) {
		createAttachedWriteStatus(chunkId, status, writeId);
		return;
	}

	// state is WriteForward or WriteFinish
	if (partiallyCompletedWrites.count(writeId) > 0) {
		// found - it means that it was added by status_receive, ie. next
		// chunkserver from a chain finished writing before our worker
		createAttachedWriteStatus(chunkId, status, writeId);
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

void ChunkserverEntry::createAttachedWriteStatus(uint64_t targetChunkId, uint8_t status,
                                                 uint32_t writeId) {
	std::vector<uint8_t> buffer;
	cstocl::writeStatus::serialize(buffer, targetChunkId, writeId, status);
	createAttachedPacket(buffer);
}

void ChunkserverEntry::writeFinishedCallback(uint8_t status, void *entry) {
	TRACETHIS();
	auto *eptr = static_cast<ChunkserverEntry *>(entry);

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
	} catch (Exception &) {
		safs::log_info("Received malformed WRITE_INIT message (length: {})", length);
		state = State::Close;
		return;
	}

	if (!chain.empty()) {
		// Create a chain -- connect to the next chunkserver
		fwdServer = chain[0].address;
		chain.erase(chain.begin());
		cltocs::writeInit::serialize(fwdInitPacket, chunkId, chunkVersion, chunkType, chain);
		fwdStartPtr = fwdInitPacket.data();
		fwdBytesLeft = fwdInitPacket.size();
		connectRetryCounter = 0;

		if (initConnection() < kInitConnectionOK) {
			createAttachedWriteStatus(chunkId, SAUNAFS_ERROR_CANTCONNECT, 0);
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
		createAttachedWriteStatus(opChunkId, status, writeId);
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
	try {
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
	if (state == State::Read && !isReadJobBeingProcessed()) {
		readContinue();
	}
}

void ChunkserverEntry::closeJobs() {
	TRACETHIS();

	if (isReadJobBeingProcessed()) {
		workerJobPool->disableJob(readJobId);
		workerJobPool->changeCallback(readJobId, delayedCloseCallback, this);
		state = State::CloseWait;
	} else if (isWriteJobBeingProcessed()) {
		workerJobPool->disableJob(writeJobId);
		workerJobPool->changeCallback(writeJobId, delayedCloseCallback, this);

		if (inputBuffer != nullptr) {
			/// Drop the input buffer, it won't be used anymore
			getWriteInputBufferPool().put(std::move(inputBuffer));
		}

		state = State::CloseWait;
	} else if (getBlocksJobId > 0) {
		workerJobPool->disableJob(getBlocksJobId);
		workerJobPool->changeCallback(getBlocksJobId, delayedCloseCallback, this);
		state = State::CloseWait;
	} else {
		if (isChunkOpen) {
			job_close(*workerJobPool, kEmptyCallback, kEmptyExtra, chunkId, chunkType);
			isChunkOpen = 0;
		}

		applyClosed();
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
	int32_t bytesRead;
	uint32_t type;
	uint32_t opSize;
	const uint8_t *ptr;

	if (fwdMode == Mode::Header) {
		bytesRead = read(fwdSocket, fwdInputPacket.startPtr, fwdInputPacket.bytesLeft);
		if (bytesRead == 0) {
			fwdError();
			return;
		}
		if (bytesRead < 0) {
			if (errno != EAGAIN) {
				safs::log_info_with_error_code(errno, "({}) read error", __func__);
				fwdError();
			}
			return;
		}
		stats_bytesin += bytesRead;
		fwdInputPacket.startPtr += bytesRead;
		fwdInputPacket.bytesLeft -= bytesRead;
		if (fwdInputPacket.bytesLeft > 0) {
			return;
		}

		ptr = fwdHeaderBuffer;
		get32bit(&ptr, type);
		get32bit(&ptr, opSize);

		if (opSize > kMaxPacketSize) {
			safs::log_warn("({}) packet too long ({}/{})", __func__, opSize, kMaxPacketSize);
			fwdError();
			return;
		}

		if (opSize > 0) {
			fwdInputPacket.packet.resize(opSize);
			passert(fwdInputPacket.packet.data());
			fwdInputPacket.startPtr = fwdInputPacket.packet.data();
		}
		fwdInputPacket.bytesLeft = opSize;
		fwdMode = Mode::Data;
	}

	if (fwdMode == Mode::Data) {
		if (fwdInputPacket.bytesLeft > 0) {
			bytesRead = read(fwdSocket, fwdInputPacket.startPtr, fwdInputPacket.bytesLeft);
			if (bytesRead == 0) {
				fwdError();
				return;
			}
			if (bytesRead < 0) {
				if (errno != EAGAIN) {
					safs::log_info_with_error_code(errno, "({}) read error", __func__);
					fwdError();
				}
				return;
			}
			stats_bytesin += bytesRead;
			fwdInputPacket.startPtr += bytesRead;
			fwdInputPacket.bytesLeft -= bytesRead;
			if (fwdInputPacket.bytesLeft > 0) {
				return;
			}
		}
		ptr = fwdHeaderBuffer;
		get32bit(&ptr, type);
		get32bit(&ptr, opSize);

		fwdMode = Mode::Header;
		fwdInputPacket.bytesLeft = PacketHeader::kSize;
		fwdInputPacket.startPtr = fwdHeaderBuffer;

		gotPacket(type, fwdInputPacket.packet.data(), opSize);
	}
}

void ChunkserverEntry::fwdWrite() {
	TRACETHIS();
	int32_t bytesWritten;

	if (fwdBytesLeft > 0) {
		bytesWritten = ::write(fwdSocket, fwdStartPtr, fwdBytesLeft);
		if (bytesWritten == 0) {
			fwdError();
			return;
		}

		if (bytesWritten < 0) {
			if (errno != EAGAIN) {
				safs::log_info_with_error_code(errno, "({}) write error", __func__);
				fwdError();
			}
			return;
		}

		stats_bytesout += bytesWritten;
		fwdStartPtr += bytesWritten;
		fwdBytesLeft -= bytesWritten;
	}

	if (fwdBytesLeft == 0) {
		fwdInitPacket.clear();
		fwdStartPtr = nullptr;
		fwdMode = Mode::Header;
		fwdInputPacket.bytesLeft = PacketHeader::kSize;
		fwdInputPacket.startPtr = fwdHeaderBuffer;
		state = State::WriteForward;
	}
}

void ChunkserverEntry::forward() {
	TRACETHIS();
	ssize_t bytesReadOrWritten{0};

	if (mode == Mode::Header) {
		bytesReadOrWritten = ::read(sock, inputPacket.startPtr, inputPacket.bytesLeft);

		if (bytesReadOrWritten == 0) {
			state = State::Close;
			return;
		}

		if (bytesReadOrWritten < 0) {
			if (errno != EAGAIN) {
				safs::log_info_with_error_code(errno, "({}) read error", __func__);
				state = State::Close;
			}
			return;
		}

		stats_bytesin += bytesReadOrWritten;
		inputPacket.startPtr += bytesReadOrWritten;
		inputPacket.bytesLeft -= bytesReadOrWritten;

		if (inputPacket.bytesLeft > 0) {
			return;
		}

		PacketHeader header;

		try {
			deserializePacketHeader(headerBuffer, sizeof(headerBuffer), header);
		} catch (IncorrectDeserializationException &) {
			safs::log_warn("({}) Received malformed network packet", __func__);
			state = State::Close;
			return;
		}

		if (header.length > kMaxPacketSize) {
			safs::log_warn("({}) packet too long ({}/{})", __func__, header.length, kMaxPacketSize);
			state = State::Close;
			return;
		}

		uint32_t totalPacketLength = PacketHeader::kSize + header.length;

		// Check if we can use aligned memory directly
		if (header.type == SAU_CLTOCS_WRITE_DATA) {
			prepareInputBufferForWrite(true);
			inputBuffer->copyIntoBuffer(InputBuffer::BufferType::Header, headerBuffer,
			                            PacketHeader::kSize);
			inputPacket.startPtr = const_cast<uint8_t *>(
			    inputBuffer->getStartLastWriteOperationHeader() + PacketHeader::kSize);

			inputPacket.bytesLeft = header.length;
		} else {
			inputPacket.packet.resize(totalPacketLength);
			passert(inputPacket.packet.data());
			std::copy(headerBuffer, headerBuffer + PacketHeader::kSize, inputPacket.packet.begin());
			inputPacket.startPtr = inputPacket.packet.data() + PacketHeader::kSize;
			inputPacket.bytesLeft = header.length;
		}

		if (header.type == SAU_CLTOCS_WRITE_DATA || header.type == SAU_CLTOCS_WRITE_END) {
			fwdBytesLeft = PacketHeader::kSize;
			// Use the correct buffer for forwarding
			if (header.type == SAU_CLTOCS_WRITE_DATA) {
				fwdStartPtr =
				    const_cast<uint8_t *>(inputBuffer->getStartLastWriteOperationHeader());
			} else {
				fwdStartPtr = inputPacket.packet.data();
			}
		}

		mode = Mode::Data;
	}

	if (inputPacket.bytesLeft > 0) {
		if (isLastHeaderTypeWriteData()) {
			bytesReadOrWritten = inputBuffer->readFromSocket(sock, inputPacket.bytesLeft);
		} else {
			bytesReadOrWritten = ::read(sock, inputPacket.startPtr, inputPacket.bytesLeft);
		}

		if (bytesReadOrWritten == 0) {
			state = State::Close;
			return;
		}
		if (bytesReadOrWritten < 0) {
			if (errno != EAGAIN) {
				safs::log_info_with_error_code(errno, "({}) read error", __func__);
				state = State::Close;
			}
			return;
		}

		stats_bytesin += bytesReadOrWritten;
		// Note startPtr could point to anywhere in some cases
		inputPacket.startPtr += bytesReadOrWritten;
		inputPacket.bytesLeft -= bytesReadOrWritten;
		if (fwdStartPtr != nullptr) {
			fwdBytesLeft += bytesReadOrWritten;
		}
	}

	if (fwdBytesLeft > 0) {
		sassert(fwdStartPtr != nullptr);
		if (isLastHeaderTypeWriteData()) {
			bytesReadOrWritten = inputBuffer->writeToSocket(fwdSocket, fwdBytesLeft);
		} else {
			bytesReadOrWritten = ::write(fwdSocket, fwdStartPtr, fwdBytesLeft);
		}

		if (bytesReadOrWritten == 0) {
			fwdError();
			return;
		}
		if (bytesReadOrWritten < 0) {
			if (errno != EAGAIN) {
				safs::log_info_with_error_code(errno, "({}) write error", __func__);
				fwdError();
			}
			return;
		}
		stats_bytesout += bytesReadOrWritten;
		// Note fwdStartPtr could point to anywhere in some cases
		fwdStartPtr += bytesReadOrWritten;
		fwdBytesLeft -= bytesReadOrWritten;
	}

	if (inputPacket.bytesLeft == 0 && fwdBytesLeft == 0) {
		PacketHeader header;
		try {
			deserializePacketHeader(headerBuffer, sizeof(headerBuffer), header);
		} catch (IncorrectDeserializationException &) {
			safs::log_warn("({}) Received malformed network packet", __func__);
			state = State::Close;
			return;
		}
		mode = Mode::Header;
		inputPacket.bytesLeft = PacketHeader::kSize;
		inputPacket.startPtr = headerBuffer;

		uint8_t *packetData{nullptr};
		if (isLastHeaderTypeWriteData()) {
			packetData = const_cast<uint8_t *>(inputBuffer->getStartLastWriteOperationHeader() +
			                                   PacketHeader::kSize);
		} else {
			packetData = inputPacket.packet.data() + PacketHeader::kSize;
		}
		gotPacket(header.type, packetData, header.length);
		fwdStartPtr = nullptr;
	}
}

void ChunkserverEntry::readFromSocket() {
	TRACETHIS();
	int32_t bytesRead;
	uint32_t type;
	uint32_t opSize;
	const uint8_t *ptr;

	if (mode == Mode::Header) {
		sassert(inputPacket.startPtr + inputPacket.bytesLeft == headerBuffer + PacketHeader::kSize);
		bytesRead = ::read(sock, inputPacket.startPtr, inputPacket.bytesLeft);
		if (bytesRead == 0) {
			state = State::Close;
			return;
		}
		if (bytesRead < 0) {
			if (errno != EAGAIN) {
				safs::log_info_with_error_code(errno, "({}) read error", __func__);
				state = State::Close;
			}
			return;
		}
		stats_bytesin += bytesRead;
		inputPacket.startPtr += bytesRead;
		inputPacket.bytesLeft -= bytesRead;

		if (inputPacket.bytesLeft > 0) {
			return;
		}

		ptr = headerBuffer;
		get32bit(&ptr, type);
		get32bit(&ptr, opSize);

		if (opSize > 0) {
			if (opSize > kMaxPacketSize) {
				safs::log_warn("({}) packet too long ({}/{})", __func__, opSize, kMaxPacketSize);
				state = State::Close;
				return;
			}

			if (type == SAU_CLTOCS_WRITE_DATA) {
				prepareInputBufferForWrite(false);
				inputPacket.startPtr =
				    const_cast<uint8_t *>(inputBuffer->getStartLastWriteOperationHeader());
			} else {
				inputPacket.packet.resize(opSize);
				passert(inputPacket.packet.data());
				inputPacket.startPtr = inputPacket.packet.data();
			}
		}
		inputPacket.bytesLeft = opSize;
		mode = Mode::Data;
	}

	if (mode == Mode::Data) {
		if (inputPacket.bytesLeft > 0) {
			if (isLastHeaderTypeWriteData()) {
				bytesRead = inputBuffer->readFromSocket(sock, inputPacket.bytesLeft);
			} else {
				bytesRead = ::read(sock, inputPacket.startPtr, inputPacket.bytesLeft);
			}

			if (bytesRead == 0) {
				state = State::Close;
				return;
			}
			if (bytesRead < 0) {
				if (errno != EAGAIN) {
					safs::log_info_with_error_code(errno, "({}) read error", __func__);
					state = State::Close;
				}
				return;
			}
			stats_bytesin += bytesRead;
			// Note startPtr could point to anywhere in some cases
			inputPacket.startPtr += bytesRead;
			inputPacket.bytesLeft -= bytesRead;

			if (inputPacket.bytesLeft > 0) { return; }
		}

		ptr = headerBuffer;
		get32bit(&ptr, type);
		get32bit(&ptr, opSize);

		mode = Mode::Header;
		inputPacket.bytesLeft = PacketHeader::kSize;
		inputPacket.startPtr = headerBuffer;

		if (isLastHeaderTypeWriteData()) {
			gotPacket(type, inputBuffer->getStartLastWriteOperationHeader(), opSize);
		} else {
			gotPacket(type, inputPacket.packet.data(), opSize);
		}
	}
}

void ChunkserverEntry::writeToSocket() {
	TRACETHIS();
	PacketStruct *pack = nullptr;
	int32_t bytesWritten;

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
		} else {
			bytesWritten = ::write(sock, pack->startPtr, pack->bytesLeft);
			if (bytesWritten == 0) {
				state = State::Close;
				return;
			}
			if (bytesWritten < 0) {
				if (errno != EAGAIN) {
					safs::log_info_with_error_code(errno, "({}) write error", __func__);
					state = State::Close;
				}
				return;
			}
			stats_bytesout += bytesWritten;
			pack->startPtr += bytesWritten;
			pack->bytesLeft -= bytesWritten;
			if (pack->bytesLeft > 0) {
				return;
			}
		}
		// packet has been sent
		if (pack->outputBuffer) {
			getReadOutputBufferPool().put(std::move(pack->outputBuffer));
		}
		outputPackets.pop_front();
		outputCheckReadFinished();
	}
}
