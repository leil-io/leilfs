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
#include "chunkserver/chunk_high_level_ops.h"
#include "chunkserver/hddspacemgr.h"
#include "chunkserver/io_buffers.h"
#include "chunkserver/network_stats.h"
#include "common/charts.h"
#include "common/chunk_connector.h"
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

// Connection timeout in seconds
constexpr uint32_t kDefaultConnectionTimeout_s = 3;
// Connection pool for forward writes
static ConnectionPool gForwardConnectionPool;

constexpr uint32_t kMaxPacketSize = 100000 + SFSBLOCKSIZE;
constexpr uint8_t kConnectRetries = 10;

ChunkserverEntry::ChunkserverEntry(int socket, JobPool *workerJobPool,
                                   uint16_t maxBlocksPerHddReadJob, uint16_t maxParallelHddReadJobs,
                                   uint16_t maxBlocksPerHddWriteJob)
    : workerJobPool(workerJobPool), sock(socket) {
	inputPacket.bytesLeft = PacketHeader::kSize;
	inputPacket.startPtr = headerBuffer;
	writeHLO = std::make_unique<WriteHighLevelOp>(this, maxBlocksPerHddWriteJob);
	readHLO =
	    std::make_unique<ReadHighLevelOp>(this, maxBlocksPerHddReadJob, maxParallelHddReadJobs);
	getBlocksHLO = std::make_unique<GetBlocksHighLevelOp>(this);
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
	uint8_t status =
	    (state == State::Connecting ? SAUNAFS_ERROR_CANTCONNECT : SAUNAFS_ERROR_DISCONNECTED);
	createAttachedWriteStatus(chunkId, status, 0);
	state = State::WriteFinish;
}

int ChunkserverEntry::initConnection() {
	TRACETHIS();
	int status;
	fwdSocket = gForwardConnectionPool.getConnection(fwdServer);
	if (fwdSocket >= 0) {
		// reused connection
		state = State::WriteInit;
		return kInitConnectionOK;
	}

	// new connection
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
	// Not yet stable connection, retry
	// No need to put connection back to pool, as it failed
	tcpclose(fwdSocket);
	fwdSocket = kInvalidSocket;
	connectRetryCounter++;

	if (connectRetryCounter < kConnectRetries) {
		if (initConnection() < kInitConnectionOK) {
			safs::log_info("({}) Failed initializing connection.", __func__);
			fwdError();
			return;
		}
	} else {
		safs::log_info("({}) Connect retry counter reached limit.", __func__);
		fwdError();
		return;
	}
}

// common - delayed close

void ChunkserverEntry::checkAndApplyClosed() {
	if (writeHLO->pendingDelayedJobs() == 0 && readHLO->pendingDelayedJobs() == 0 &&
	    getBlocksHLO->pendingDelayedJobs() == 0) {
		writeHLO->cleanup();
		readHLO->cleanup();

		state = State::Closed;
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
	uint32_t offset;
	uint32_t size;

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
	state = State::Read;
	readHLO->setup(chunkId, chunkVersion, chunkType, offset, size);
}

void ChunkserverEntry::prefetch(const uint8_t *data, PacketHeader::Type type,
                                PacketHeader::Length length) {
	sassert(type == SAU_CLTOCS_PREFETCH);
	PacketVersion v;
	uint32_t offset;
	uint32_t size;

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

void ChunkserverEntry::createAttachedWriteStatus(uint64_t targetChunkId, uint8_t status,
                                                 uint32_t writeId) {
	std::vector<uint8_t> buffer;
	cstocl::writeStatus::serialize(buffer, targetChunkId, writeId, status);
	createAttachedPacket(buffer);
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
		fwdOutputPacket.startPtr = fwdInitPacket.data();
		fwdOutputPacket.bytesLeft = fwdInitPacket.size();
		connectRetryCounter = 0;

		if (initConnection() < kInitConnectionOK) {
			createAttachedWriteStatus(chunkId, SAUNAFS_ERROR_CANTCONNECT, 0);
			state = State::WriteFinish;
			return;
		}
	} else {
		state = State::WriteLast;
	}

	writeHLO->setup(chunkId, chunkVersion, chunkType);
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
		cltocs::writeData::deserializePrefix(data, kSauWriteDataPrefixSize, opChunkId, writeId,
		                                     blocknum, opOffset, opSize, crc);
	} catch (IncorrectDeserializationException &) {
		safs::log_info("Received malformed WRITE_DATA message (length: {})", length);
		state = State::Close;
		return;
	}

	uint8_t status = SAUNAFS_STATUS_OK;
	if (!writeHLO->isLastHeaderSizeValid()) {
		status = SAUNAFS_ERROR_WRONGSIZE;
	} else if (opChunkId != chunkId) {
		status = SAUNAFS_ERROR_WRONGCHUNKID;
	}

	if (status != SAUNAFS_STATUS_OK) {
		createAttachedWriteStatus(opChunkId, status, writeId);
		state = State::WriteFinish;
		return;
	}

	writeHLO->processWriteDataBlock(blocknum, opOffset, opSize, writeId, crc);
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

	writeHLO->updateUsingWriteStatusAndReply(status, writeId);
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

	if (!writeHLO->isCompleted() || !outputPackets.empty()) {
		/*
		 * WRITE_END received too early:
		 * !writeHLO->isCompleted() -- some write data not yet processed
		 * !outputPackets.empty() -- there is a status being sent
		 */
		// TODO(msulikowski) temporary syslog message. May be useful until this
		// code is fully tested
		safs::log_info("Received WRITE_END message too early");
		state = State::WriteFinish;
		return;
	}

	if (fwdSocket > 0) {
		gForwardConnectionPool.putConnection(fwdSocket, fwdServer, kDefaultConnectionTimeout_s);
		fwdSocket = kInvalidSocket;
	}

	// All went fine, cleanup
	writeHLO->cleanup();
	state = State::Idle;
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

	getBlocksHLO->setup(chunkId, chunkVersion, chunkType);
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
	if (state == State::Read) {
		readHLO->continueReadingIfPossible();
	}
}

bool ChunkserverEntry::isChunkOpen() {
	if (writeHLO->isChunkOpen() && readHLO->isChunkOpen()) {
		safs::log_warn(
		    "({}) Both write and read chunk handles are open for chunk {:016X} and {:016X}",
		    __func__, writeHLO->chunkId(), readHLO->chunkId());
	}

	return writeHLO->isChunkOpen() || readHLO->isChunkOpen();
}

void ChunkserverEntry::closeJobs() {
	TRACETHIS();

	if (readHLO->prepareForDelayedClose()) {
		readHLO->delayedClose();
		state = State::CloseWait;
	} else if (writeHLO->isWriteJobBeingProcessed()) {
		writeHLO->delayedClose();
		state = State::CloseWait;
	} else if (getBlocksHLO->isRunning()) {
		getBlocksHLO->delayedClose();
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
			safs::log_info("({}) {} returned 0 bytes", callerName, isRead ? "read" : "write");
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
		writeHLO->prepareForNewWriteData(mustForward, headerBuffer);
		// No need to set up packet.startPtr here; writeHLO's input buffer will be used to receive
		// the data instead of packet's buffer.
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
			fwdOutputPacket.startPtr = writeHLO->getLastOperationHeader();
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
		bytesRead = writeHLO->readData(sock, packet.bytesLeft);
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
		bytesWritten = writeHLO->writeData(socket, packet.bytesLeft);
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
		packetData = writeHLO->getLastOperationHeader() + offsetFromSkipHeaderInForward;
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
