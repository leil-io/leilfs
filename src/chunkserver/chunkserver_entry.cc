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
#include <sys/types.h>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <memory>

#include "chunkserver-common/global_shared_resources.h"
#include "chunkserver/bgjobs.h"
#include "chunkserver/chunk_generation_fence.h"
#include "chunkserver/chunk_high_level_ops.h"
#include "chunkserver/hddspacemgr.h"
#include "chunkserver/io_buffers.h"
#include "chunkserver/masterconn.h"
#include "chunkserver/network_stats.h"
#include "common/charts.h"
#include "common/connection_pool.h"
#include "common/datapack.h"
#include "common/event_loop.h"
#include "common/massert.h"
#include "common/sockets.h"
#include "devtools/TracePrinter.h"
#include "protocol/SFSCommunication.h"
#include "protocol/cltocs.h"
#include "protocol/cstocl.h"
#include "protocol/cstocs.h"
#include "protocol/packet.h"
#include "common/test_event_stream.h"
#include "slogger/slogger.h"

/// Records that one client plane frame was refused because the local session cutoff had
/// already fired. A frame can only reach this point after the deadline, so the event is
/// what proves a gate ran rather than the work simply never being requested.
static void noteFrameRefusedAfterCutoff(const char *frame, uint64_t chunkId) {
	if (test_event_stream::enabled()) {
		test_event_stream::emit("client_frame_refused", {{"frame", frame}, {"chunk_id", chunkId}});
	}
}

// Connection timeout in seconds
constexpr uint32_t kDefaultConnectionTimeout_s = 3;
// Connection pool for forward writes
static ConnectionPool gForwardConnectionPool;

static constexpr uint32_t kMaxPacketSize = 100000 + SFSBLOCKSIZE;
static constexpr uint8_t kConnectRetries = 10;

ChunkserverEntry::ChunkserverEntry(int socket, ClientJobPool *workerJobPool,
                                   uint16_t maxBlocksPerHddReadJob, uint16_t maxParallelHddReadJobs,
                                   uint16_t maxBlocksPerHddWriteJob)
    : workerJobPool(workerJobPool),
      sock(socket),
      maxBlocksPerHddWriteJob_(maxBlocksPerHddWriteJob) {
	inputPacket.bytesLeft = PacketHeader::kSize;
	inputPacket.startPtr = headerBuffer;
	readHLO_ =
	    std::make_unique<ReadHighLevelOp>(this, maxBlocksPerHddReadJob, maxParallelHddReadJobs);
	getBlocksHLO_ = std::make_unique<GetBlocksHighLevelOp>(this);
}

ChunkserverEntry::~ChunkserverEntry() {
	if (sock >= 0) { tcpclose(sock); }
	if (fwdSocket >= 0) { tcpclose(fwdSocket); }
	if (!writeHLOs_.empty()) {
		safs::log_err("({}) Destructor called with non-empty writeHLOs_.", __func__);
		writeHLOs_.clear();
	}
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
	state = State::IOFinish;
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
	if (pendingWriteJobs == 0 && readHLO_->pendingDelayedJobs() == 0 &&
	    getBlocksHLO_->pendingDelayedJobs() == 0) {
		while (!writeHLOs_.empty()) {
			assert(writeHLOs_.back()->chunkId() != 0);
			writeHLOs_.back()->cleanup();
			writeHLOs_.pop_back();
		}
		if (readHLO_->chunkId() != 0) { readHLO_->cleanup(); }

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
	if (!bindServingEra()) {
		// Client-plane session cutoff gate (FR-044): no new read after durable
		// authority expired, independently of any MDS-side gate.
		cstocl::readStatus::serialize(instantResponseBuffer, chunkId,
		                              SAUNAFS_ERROR_TEMP_NOTPOSSIBLE);
	} else if (size == 0) {
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
	readHLO_->setup(chunkId, chunkVersion, chunkType, offset, size);
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
	if (masterconn_serving_era() == 0) {
		// Prefetch has no reply, so refusing it is simply declining to start the job.
		noteFrameRefusedAfterCutoff("prefetch", chunkId);
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

void ChunkserverEntry::everyLoopUpdateWrite() {
	// Get rid of completed write HLOs.
	while (!writeHLOs_.empty() && writeHLOs_.front()->isCompleted()) {
		writeHLOs_.front()->cleanup();
		writeHLOs_.pop_front();
	}

	if (writeHLOs_.empty()) { return; }

	// Try fast reply in the last write HLO if possible.
	writeHLOs_.back()->tryInstantReply();
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
	bool expectWriteFlush = false;
	uint64_t grantGeneration = 0;
	uint64_t grantRandom = 0;
	try {
		PacketVersion v;
		deserializePacketVersionNoHeader(data, length, v);
		if (v == cltocs::writeInit::kECChunksWithGrant) {
			cltocs::writeInit::deserialize(data, length, chunkId, chunkVersion, chunkType,
			                               expectWriteFlush, grantGeneration, grantRandom,
			                               chain);
		} else if (v == cltocs::writeInit::kECChunks) {
			cltocs::writeInit::deserialize(data, length, chunkId, chunkVersion, chunkType, chain);
		} else if (v == cltocs::writeInit::kECChunksWithWriteFlush) {
			cltocs::writeInit::deserialize(data, length, chunkId, chunkVersion, chunkType,
			                               expectWriteFlush, chain);
		} else {
			throw IncorrectDeserializationException("Unexpected packet version");
		}
	} catch (Exception &ex) {
		safs::log_info("Received malformed WRITE_INIT message (length: {}): {}", length, ex.what());
		state = State::Close;
		return;
	}

	if (masterconn_is_distributed() && grantRandom == 0) {
		// FR-062/FR-059: a distributed write carries its grant or does not start. An old
		// client cannot silently enter the distributed write path.
		if (test_event_stream::enabled()) {
			test_event_stream::emit("write_grant_missing_refused", {{"chunk_id", chunkId}});
		}
		createAttachedWriteStatus(chunkId, SAUNAFS_ERROR_EPERM, 0);
		state = State::IOFinish;
		return;
	}
	if (grantRandom != 0 && test_event_stream::enabled()) {
		test_event_stream::emit("write_grant_bound", {{"chunk_id", chunkId},
		                                              {"grant_generation", grantGeneration},
		                                              {"grant_random", grantRandom}});
	}

	// A grant a newer round has already fenced starts no write: recovery raises the chunk fence
	// above the revoked grant with its own SET_VERSION, so a grant below the fence is one the
	// cluster moved past. The chain binds the grant generation so every later frame is checked
	// under it too, and a mid-stream revocation stops the writer instead of appending to a
	// version nobody owns.
	if (chunk_generation_fence::isSuperseded(chunkId, grantGeneration)) {
		if (test_event_stream::enabled()) {
			test_event_stream::emit("write_grant_superseded", {{"chunk_id", chunkId},
			                                                   {"grant_generation", grantGeneration},
			                                                   {"stage", "write_init"}});
		}
		createAttachedWriteStatus(chunkId, SAUNAFS_ERROR_TEMP_NOTPOSSIBLE, 0);
		state = State::IOFinish;
		return;
	}
	boundGrantGeneration_ = grantGeneration;

	if (!bindServingEra()) {
		// Client-plane session cutoff gate (FR-044): no new write chain after durable
		// authority expired, independently of any MDS-side gate.
		createAttachedWriteStatus(chunkId, SAUNAFS_ERROR_TEMP_NOTPOSSIBLE, 0);
		state = State::IOFinish;
		return;
	}

	if (!chain.empty()) {
		// Create a chain -- connect to the next chunkserver
		fwdServer = chain[0].address;
		chain.erase(chain.begin());
		if (grantRandom != 0) {
			cltocs::writeInit::serialize(fwdInitPacket, chunkId, chunkVersion, chunkType,
			                             expectWriteFlush, grantGeneration, grantRandom, chain);
		} else if (expectWriteFlush) {
			cltocs::writeInit::serialize(fwdInitPacket, chunkId, chunkVersion, chunkType,
			                             expectWriteFlush, chain);
		} else {
			cltocs::writeInit::serialize(fwdInitPacket, chunkId, chunkVersion, chunkType, chain);
		}
		fwdOutputPacket.startPtr = fwdInitPacket.data();
		fwdOutputPacket.bytesLeft = fwdInitPacket.size();
		connectRetryCounter = 0;

		if (initConnection() < kInitConnectionOK) {
			createAttachedWriteStatus(chunkId, SAUNAFS_ERROR_CANTCONNECT, 0);
			state = State::IOFinish;
			return;
		}
	} else {
		state = State::WriteLast;
	}

	// Setup write HLO
	if (expectWriteFlush) {
		writeHLOs_.emplace_back(
		    std::make_unique<WriteHighLevelOpExpectFlush>(this, maxBlocksPerHddWriteJob_));
	} else {
		writeHLOs_.emplace_back(std::make_unique<WriteHighLevelOp>(this, maxBlocksPerHddWriteJob_));
	}
	writeHLOs_.back()->setup(chunkId, chunkVersion, chunkType);
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

	if (writeHLOs_.empty()) {
		safs::log_warn("Received WRITE_DATA message without prior WRITE_INIT (chunkId={:016X})",
		               opChunkId);
		state = State::Close;
		return;
	}

	uint8_t status = SAUNAFS_STATUS_OK;
	if (!servingEraIsCurrent()) {
		// A chain admitted before the deadline must not keep writing past it, so
		// authority is rechecked on every frame and not only once at WRITE_INIT.
		noteFrameRefusedAfterCutoff("write_data", opChunkId);
		status = SAUNAFS_ERROR_TEMP_NOTPOSSIBLE;
	} else if (writeGrantSuperseded(opChunkId)) {
		// A newer round fenced this chunk after the chain bound its grant, so the grant is stale
		// and this frame must not extend a write the cluster moved past.
		if (test_event_stream::enabled()) {
			test_event_stream::emit(
			    "write_grant_superseded",
			    {{"chunk_id", opChunkId}, {"grant_generation", boundGrantGeneration_},
			     {"stage", "write_data"}});
		}
		status = SAUNAFS_ERROR_TEMP_NOTPOSSIBLE;
	} else if (!writeHLOs_.back()->isLastHeaderSizeValid()) {
		status = SAUNAFS_ERROR_WRONGSIZE;
	} else if (opChunkId != chunkId) {
		status = SAUNAFS_ERROR_WRONGCHUNKID;
	} else if (opOffset >= SFSBLOCKSIZE || opSize > SFSBLOCKSIZE ||
	           opOffset + opSize > SFSBLOCKSIZE) {
		status = SAUNAFS_ERROR_WRONGOFFSET;
	}

	if (status != SAUNAFS_STATUS_OK) {
		createAttachedWriteStatus(opChunkId, status, writeId);
		state = State::IOFinish;
		return;
	}

	writeHLOs_.back()->processWriteDataBlock(blocknum, opOffset, opSize, writeId, crc);
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

	if (writeHLOs_.empty()) {
		safs::log_warn("Received WRITE_STATUS message without prior WRITE_INIT (chunkId={:016X})",
		               opChunkId);
		state = State::Close;
		return;
	}

	// A downstream success can become an upstream client success, so accepting this is an
	// authority-bearing act and not merely bookkeeping. The packet was parsed and the
	// transport state is released above whatever happens next, because refusing to believe an
	// answer is not a reason to leak the resources that carried it. What a retired era may not
	// do is turn that answer into an acknowledgement: the chain fails closed instead, and
	// whatever the downstream server actually did becomes something for reconciliation to
	// settle rather than something this process asserts.
	if (!servingEraIsCurrent()) {
		noteFrameRefusedAfterCutoff("write_status", opChunkId);
		createAttachedWriteStatus(chunkId, SAUNAFS_ERROR_TEMP_NOTPOSSIBLE, 0);
		state = State::IOFinish;
		return;
	}

	writeHLOs_.back()->updateUsingWriteStatusAndReply(status, writeId);
}

void ChunkserverEntry::writeFlush(const uint8_t *data, uint32_t length) {
	TRACETHIS();
	uint64_t opChunkId;

	try {
		cltocs::writeFlush::deserialize(data, length, opChunkId);
	} catch (IncorrectDeserializationException &ex) {
		safs::log_info("Received malformed WRITE_FLUSH message (length: {}): {}", length,
		               ex.what());
		state = State::IOFinish;
		return;
	}
	if (opChunkId != chunkId) {
		safs::log_info(
		    "Received malformed WRITE_FLUSH message (got chunkId={:016X}, expected {:016X})",
		    opChunkId, chunkId);
		state = State::IOFinish;
		return;
	}

	// Sealing buffered work is an effect, so it is refused the same way a new frame is. A
	// chain admitted before the deadline must not be able to finish itself after it.
	if (!servingEraIsCurrent()) {
		noteFrameRefusedAfterCutoff("write_flush", opChunkId);
		createAttachedWriteStatus(opChunkId, SAUNAFS_ERROR_TEMP_NOTPOSSIBLE, 0);
		state = State::IOFinish;
		return;
	}

	if (writeGrantSuperseded(opChunkId)) {
		// Sealing buffered work under a grant a newer round fenced would make durable a version the
		// cluster moved past, so it fails closed the same way an after-cutoff seal does.
		if (test_event_stream::enabled()) {
			test_event_stream::emit(
			    "write_grant_superseded",
			    {{"chunk_id", opChunkId}, {"grant_generation", boundGrantGeneration_},
			     {"stage", "write_flush"}});
		}
		createAttachedWriteStatus(opChunkId, SAUNAFS_ERROR_TEMP_NOTPOSSIBLE, 0);
		state = State::IOFinish;
		return;
	}

	if (writeHLOs_.empty()) {
		safs::log_warn("Received WRITE_FLUSH message without prior WRITE_INIT (chunkId={:016X})",
		               opChunkId);
		state = State::Close;
		return;
	}

	writeHLOs_.back()->flushData();
}

void ChunkserverEntry::writeEnd(const uint8_t *data, uint32_t length) {
	TRACETHIS();
	uint64_t opChunkId;

	try {
		cltocs::writeEnd::deserialize(data, length, opChunkId);
	} catch (IncorrectDeserializationException&) {
		safs::log_info("Received malformed WRITE_END message (length: {})", length);
		state = State::IOFinish;
		return;
	}

	if (opChunkId != chunkId) {
		safs::log_info(
		    "Received malformed WRITE_END message (got chunkId={:016X}, expected {:016X})",
		    opChunkId, chunkId);
		state = State::IOFinish;
		return;
	}

	// Ending a write is where it becomes durable and acknowledged, which is the last moment
	// this chunkserver can still decline to speak for a claim it no longer holds.
	if (!servingEraIsCurrent()) {
		noteFrameRefusedAfterCutoff("write_end", opChunkId);
		createAttachedWriteStatus(opChunkId, SAUNAFS_ERROR_TEMP_NOTPOSSIBLE, 0);
		state = State::IOFinish;
		return;
	}

	if (writeGrantSuperseded(opChunkId)) {
		// Ending a write under a grant a newer round fenced would acknowledge a claim this
		// chunkserver no longer holds, so it declines here, the last moment it still can.
		if (test_event_stream::enabled()) {
			test_event_stream::emit(
			    "write_grant_superseded",
			    {{"chunk_id", opChunkId}, {"grant_generation", boundGrantGeneration_},
			     {"stage", "write_end"}});
		}
		createAttachedWriteStatus(opChunkId, SAUNAFS_ERROR_TEMP_NOTPOSSIBLE, 0);
		state = State::IOFinish;
		return;
	}

	if (writeHLOs_.empty()) {
		safs::log_warn("Received WRITE_END message without prior WRITE_INIT (chunkId={:016X})",
		               opChunkId);
		state = State::Close;
		return;
	}

	if (!writeHLOs_.back()->trySeal() || !outputPackets.empty()) {
		/*
		 * WRITE_END received too early:
		 * !writeHLOs_.back()->trySeal() -- some write data not yet replied
		 * !outputPackets.empty() -- there is a status being sent
		 */
		// TODO(msulikowski) temporary syslog message. May be useful until this
		// code is fully tested
		safs::log_info("Received WRITE_END message too early");
		state = State::IOFinish;
		return;
	}

	if (fwdSocket > 0) {
		gForwardConnectionPool.putConnection(fwdSocket, fwdServer, kDefaultConnectionTimeout_s);
		fwdSocket = kInvalidSocket;
	}

	if (writeHLOs_.back()->isCompleted()) {
		// Everything done, cleanup
		writeHLOs_.back()->cleanup();
		writeHLOs_.pop_back();
	}
	if (workerJobPool->isFull()) {
		// If the worker job pool is full (best-effort check), try not to accept
		// more requests until it has free slots. Note: the pool state may change
		// after this check, but this serves as backpressure heuristic.
		state = State::IOFinish;
	} else {
		// Ready for new requests, reset state
		state = State::Idle;
	}
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

	if (!bindServingEra()) {
		// Replication and repair size their work from this answer, so a server whose
		// durable authority has lapsed must not describe a chunk it may no longer own.
		noteFrameRefusedAfterCutoff("get_chunk_blocks", chunkId);
		std::vector<uint8_t> buffer;
		cstocs::getChunkBlocksStatus::serialize(buffer, chunkId, chunkVersion, chunkType, 0,
		                                        SAUNAFS_ERROR_TEMP_NOTPOSSIBLE);
		createAttachedPacket(buffer);
		state = State::Idle;
		return;
	}

	getBlocksHLO_->setup(chunkId, chunkVersion, chunkType);
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
	if (state != State::Read) { return; }
	// A read admitted before the cutoff is still a read after it. The drain component of the
	// cutoff reserve exists to cover work already in flight, but nothing stopped this one from
	// handing out blocks for as long as the client kept asking for them, so the reserve
	// bounded an assumption rather than the work. Terminate the read with the same refusal a
	// new one would get; the client retries it against a holder that still has authority.
	if (!servingEraIsCurrent()) {
		// Carrying both eras is what makes this observable as the property it is. A refusal
		// while authority is simply gone proves nothing a flag could not also have produced;
		// a refusal while this process is serving again, under a different era, is the whole
		// claim: readmission authorizes new work and does not revive old work.
		if (test_event_stream::enabled()) {
			test_event_stream::emit("read_continuation_refused_stale_era",
			                        {{"chunk", readHLO_->chunkId()},
			                         {"admitted_era", servingEra_},
			                         {"current_era", masterconn_serving_era()}});
		}
		std::vector<uint8_t> refusal;
		cstocl::readStatus::serialize(refusal, readHLO_->chunkId(),
		                              SAUNAFS_ERROR_TEMP_NOTPOSSIBLE);
		createAttachedPacket(refusal);
		state = State::Close;
		return;
	}
	readHLO_->continueReadingIfPossible();
}

bool ChunkserverEntry::isChunkOpen() {
	if (!writeHLOs_.empty() && readHLO_->isChunkOpen()) {
		safs::log_warn("({}) Both write and read chunk handles are open", __func__);
	}

	return !writeHLOs_.empty() || readHLO_->isChunkOpen();
}

void ChunkserverEntry::forceCloseOpenChunks() {
	if (!writeHLOs_.empty()) {
		for (auto &writeHLO : writeHLOs_) {
			hddClose(writeHLO->chunkId(), writeHLO->chunkType());
		}
	}

	if (readHLO_->isChunkOpen()) {
		hddClose(readHLO_->chunkId(), readHLO_->chunkType());
	}
}

bool ChunkserverEntry::bindServingEra() {
	const uint64_t era = masterconn_serving_era();
	if (era == 0) { return false; }
	servingEra_ = era;
	return true;
}

bool ChunkserverEntry::servingEraIsCurrent() const {
	// Work with no era was never admitted under one, which happens on a connection that has
	// not started anything yet. Asking the process whether it may serve is the right question
	// there and the wrong one afterwards.
	if (servingEra_ == 0) { return masterconn_session_serving_allowed(); }
	return masterconn_era_is_current(servingEra_);
}

bool ChunkserverEntry::writeGrantSuperseded(uint64_t opChunkId) const {
	return chunk_generation_fence::isSuperseded(opChunkId, boundGrantGeneration_);
}

bool ChunkserverEntry::cancelJobsAfterCutoff() {
	if (servingEraIsCurrent()) {
		// Readmission re-arms it, so a session that recovers and lapses again is drained again.
		cutoffCancellationDone_ = false;
		return false;
	}
	// Once per lapse. It no longer changes the connection's state, so nothing else would stop
	// it running on every loop for as long as the cutoff lasts.
	if (cutoffCancellationDone_) { return false; }
	if (state == State::Close || state == State::CloseWait || state == State::Closed) {
		return false;
	}
	// What counts as work inside is the connection's own state, not whether a chunk handle is
	// open yet. A read whose first job is still queued has opened nothing and is nonetheless
	// exactly the case this exists for: admitted before the cutoff, unfinished after it.
	const bool readInProgress = state == State::Read;
	const bool working = readInProgress || state == State::GetBlock ||
	                     state == State::WriteLast || state == State::WriteForward ||
	                     state == State::WriteInit || state == State::Connecting ||
	                     state == State::IOFinish;
	if (!working && !isChunkOpen() && pendingWriteJobs == 0) { return false; }

	// Cancel what is queued, and only that. Routing this through the teardown path instead
	// would also take the connection down, and taking the connection down is how the client
	// frame gates stopped being reachable: a frame the client sends next can only be refused
	// by a connection that is still there to refuse it. So the work stops and the connection
	// stays, which is both halves of what the cutoff is supposed to mean.
	size_t cancelled = readHLO_->cancelQueuedJobs() + getBlocksHLO_->cancelQueuedJobs();
	// Write jobs were missed the first time, which left the half of the client plane that
	// actually changes data as the only one the cutoff did not stop.
	for (auto &writeHLO : writeHLOs_) { cancelled += writeHLO->cancelQueuedJobs(); }
	cutoffCancellationDone_ = true;

	// The record carries what was inside, because that is the thing a gate at the entrance
	// cannot show: refusing entry proves nothing about what was already through the door.
	if (test_event_stream::enabled()) {
		test_event_stream::emit("jobs_cancelled_after_cutoff",
		                        {{"read_in_progress", readInProgress ? 1U : 0U},
		                         {"read_open", readHLO_->isChunkOpen() ? 1U : 0U},
		                         {"cancelled_jobs", static_cast<uint32_t>(cancelled)},
		                         {"pending_write_jobs", pendingWriteJobs}});
	}
	return true;
}

void ChunkserverEntry::closeJobs() {
	TRACETHIS();

	if (readHLO_->prepareForDelayedClose()) {
		readHLO_->delayedClose();
		state = State::CloseWait;
	} else if (!writeHLOs_.empty()) {
		for (auto &writeHLO : writeHLOs_) {
			writeHLO->delayedClose();
		}

		if (pendingWriteJobs == 0) {
			checkAndApplyClosed();
			return;
		}

		// There are pending write jobs
		state = State::CloseWait;
	} else if (getBlocksHLO_->isRunning()) {
		getBlocksHLO_->delayedClose();
		state = State::CloseWait;
	} else {
		// Not necessary to close chunk - checkAndApplyClosed will do it
		checkAndApplyClosed();
	}
}

WriteHighLevelOp *ChunkserverEntry::getActiveWriteHLO(const char *callerName) {
	if (writeHLOs_.empty()) {
		safs::log_warn("({}) No active write high level operation", callerName);
		state = State::Close;
		return nullptr;
	}
	return writeHLOs_.back().get();
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
		case SAU_CLTOCS_WRITE_FLUSH:
			writeFlush(data, length);
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
		case SAU_CLTOCS_WRITE_FLUSH:
			writeFlush(data, length);
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
	} else if (state == State::IOFinish) {
		switch (type) {
		case SAU_CLTOCS_WRITE_DATA:
		case SAU_CLTOCS_WRITE_FLUSH:
		case SAU_CLTOCS_WRITE_END:
			return;
		default:
			safs::log_info("Got invalid message in IOFinish state (type:{})", type);
			state = State::Close;
		}
	} else {
		safs::log_info("Got invalid message (current state: {}) (type:{})", static_cast<int>(state),
		               type);
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
		auto *writeHLO = getActiveWriteHLO(__func__);
		if (!writeHLO) { return false; }

		writeHLO->prepareForNewWriteData(mustForward, headerBuffer);
		// No need to set up packet.startPtr here; writeHLO_'s input buffer will be used to receive
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

	if (mustForward && (type == SAU_CLTOCS_WRITE_DATA || type == SAU_CLTOCS_WRITE_END ||
	                    type == SAU_CLTOCS_WRITE_FLUSH)) {
		fwdOutputPacket.bytesLeft = PacketHeader::kSize;
		// Use the correct buffer for forwarding
		if (type == SAU_CLTOCS_WRITE_DATA) {
			assert(!writeHLOs_.empty());

			fwdOutputPacket.startPtr = writeHLOs_.back()->getLastOperationHeader();
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
		auto *writeHLO = getActiveWriteHLO(__func__);
		if (!writeHLO) { return false; }


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
		auto *writeHLO = getActiveWriteHLO(__func__);
		if (!writeHLO) { return false; }

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
		auto *writeHLO = getActiveWriteHLO(__func__);
		if (!writeHLO) { return; }

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
