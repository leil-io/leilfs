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
#include <set>

#include "chunkserver/bgjobs.h"
#include "chunkserver/hdd_readahead.h"
#include "chunkserver/hddspacemgr.h"
#include "chunkserver/network_stats.h"
#include "common/charts.h"
#include "protocol/cltocs.h"
#include "protocol/cstocl.h"
#include "protocol/cstocs.h"
#include "common/datapack.h"
#include "common/event_loop.h"
#include "common/saunafs_version.h"
#include "common/massert.h"
#include "protocol/SFSCommunication.h"
#include "common/legacy_vector.h"
#include "protocol/packet.h"
#include "slogger/slogger.h"
#include "common/sockets.h"
#include "devtools/request_log.h"
#include "devtools/TracePrinter.h"

constexpr uint32_t kMaxPacketSize = 100000 + SFSBLOCKSIZE;
constexpr uint8_t kConnectRetries = 10;

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

class LegacyMessageSerializer : public MessageSerializer {
public:
	void serializePrefixOfCstoclReadData(std::vector<uint8_t> &buffer,
	                                     uint64_t chunkId, uint32_t offset,
	                                     uint32_t size) override {
		// This prefix requires CRC (uint32_t) and data (size * uint8_t) to be
		// appended
		uint32_t extraSpace = sizeof(uint32_t) + size;
		serializeLegacyPacketPrefix(buffer, extraSpace, CSTOCL_READ_DATA,
		                            chunkId, offset, size);
	}

	void serializeCstoclReadStatus(std::vector<uint8_t> &buffer,
	                               uint64_t chunkId, uint8_t status) override {
		serializeLegacyPacket(buffer, CSTOCL_READ_STATUS, chunkId, status);
	}

	void serializeCstoclWriteStatus(std::vector<uint8_t> &buffer,
	                                uint64_t chunkId, uint32_t writeId,
	                                uint8_t status) override {
		serializeLegacyPacket(buffer, CSTOCL_WRITE_STATUS, chunkId, writeId,
		                      status);
	}
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
	sassert((type >= PacketHeader::kMinSauPacketType &&
	         type <= PacketHeader::kMaxSauPacketType) ||
	        type <= PacketHeader::kMaxOldPacketType);
	if (type <= PacketHeader::kMaxOldPacketType) {
		static LegacyMessageSerializer singleton;
		return &singleton;
	}

	static SaunaFsMessageSerializer singleton;
	return &singleton;
}

std::unique_ptr<PacketStruct>
ChunkserverEntry::createDetachedPacketWithOutputBuffer(
    const std::vector<uint8_t> &packetPrefix) {
	TRACETHIS();

	PacketHeader header;
	deserializePacketHeader(packetPrefix, header);

	uint32_t sizeOfWholePacket = PacketHeader::kSize + header.length;
	std::unique_ptr<PacketStruct> outPacket = std::make_unique<PacketStruct>();
	passert(outPacket);

	outPacket->outputBuffer = getReadOutputBufferPool().get(sizeOfWholePacket);

	if (outPacket->outputBuffer->copyIntoBuffer(packetPrefix) !=
	    static_cast<ssize_t>(packetPrefix.size())) {
		if (outPacket->outputBuffer) {
			getReadOutputBufferPool().put(std::move(outPacket->outputBuffer));
		}

		return kInvalidPacket;
	}

	return outPacket;
}

ChunkserverEntry::~ChunkserverEntry() {
	if (sock >= 0) { tcpclose(sock); }
	if (fwdSocket >= 0) { tcpclose(fwdSocket); }
}

void ChunkserverEntry::attachPacket(std::unique_ptr<PacketStruct> &&packet) {
	outputPackets.push_back(std::move(packet));
}

void ChunkserverEntry::preserveInputPacket() {
	TRACETHIS();

	if (inputPacket.useAlignedMemory) {
		writePacket->alignedBuffer = std::move(inputPacket.alignedBuffer);
	} else {
		writePacket->packet = std::move(inputPacket.packet);
	}
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

// initialize connection to another CS
int ChunkserverEntry::initConnection() {
	TRACETHIS();
	int status;
	// TODO(msulikowski) If we want to use a ConnectionPool, this is the right
	// place to get a connection from it
	fwdSocket = tcpsocket();
	if (fwdSocket < 0) {
		safs_pretty_errlog(LOG_WARNING, "create socket, error");
		return kInitConnectionFailed;
	}

	if (tcpnonblock(fwdSocket) < 0) {
		safs_pretty_errlog(LOG_WARNING, "set nonblock, error");
		tcpclose(fwdSocket);
		fwdSocket = kInvalidSocket;
		return kInitConnectionFailed;
	}

	status = tcpnumconnect(fwdSocket, fwdServer.ip, fwdServer.port);
	if (status < 0) {
		safs_pretty_errlog(LOG_WARNING, "connect failed, error");
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
void ChunkserverEntry::delayedCloseCallback(uint8_t status, void *entry) {
	TRACETHIS();
	auto *eptr = static_cast<ChunkserverEntry*>(entry);
	if (eptr->writeJobId > 0 && eptr->writeJobWriteId == 0 &&
	    status == SAUNAFS_STATUS_OK) {  // this was job_open
		eptr->isChunkOpen = 1;
	} else if (eptr->readJobId > 0 &&
	           status == SAUNAFS_STATUS_OK) {  // this could be job_open
		eptr->isChunkOpen = 1;
	}
	if (eptr->isChunkOpen) {
		job_close(*eptr->workerJobPool, nullptr, nullptr, eptr->chunkId, eptr->chunkType);
		eptr->isChunkOpen = 0;
	}
	eptr->state = State::Closed;
}

// bg reading

void ChunkserverEntry::readFinishedCallback(uint8_t status, void *entry) {
	TRACETHIS();
	auto *eptr = static_cast<ChunkserverEntry*>(entry);
	eptr->readJobId = 0;
	if (status == SAUNAFS_STATUS_OK) {
		eptr->todoReadCounter--;
		eptr->isChunkOpen = 1;
		if (eptr->todoReadCounter == 0) {
			eptr->readContinue();
		}
	} else {
		std::vector<uint8_t> buffer;
		eptr->messageSerializer->serializeCstoclReadStatus(
		    buffer, eptr->chunkId, status);
		eptr->createAttachedPacket(buffer);
		if (eptr->isChunkOpen) {
			job_close(*eptr->workerJobPool, nullptr, nullptr, eptr->chunkId, eptr->chunkType);
			eptr->isChunkOpen = 0;
		}
		// after sending status even if there was an error it's possible to
		eptr->state = State::Idle;
		// receive new requests on the same connection
		LOG_AVG_STOP(readOperationTimer);
	}
}

void ChunkserverEntry::sendFinished() {
	TRACETHIS();
	todoReadCounter--;
	if (todoReadCounter == 0) {
		readContinue();
	}
}

void ChunkserverEntry::readContinue() {
	TRACETHIS2(offset, size);

	if (readPacket) {
		attachPacket(std::move(readPacket));
		readPacket.reset();
		todoReadCounter++;
	}
	if (size == 0) {  // everything has been read
		std::vector<uint8_t> buffer;
		messageSerializer->serializeCstoclReadStatus(
		    buffer, chunkId, SAUNAFS_STATUS_OK);
		createAttachedPacket(buffer);
		sassert(isChunkOpen);

		job_close(*workerJobPool, nullptr, nullptr, chunkId, chunkType);
		isChunkOpen = 0;
		// no error - do not disconnect - go direct to the IDLE state, ready for
		// requests on the same connection
		state = State::Idle;
		LOG_AVG_STOP(readOperationTimer);
	} else {
		const uint32_t totalRequestSize = size;
		const uint32_t thisPartOffset = offset % SFSBLOCKSIZE;
		const uint32_t thisPartSize = std::min<uint32_t>(
				totalRequestSize, SFSBLOCKSIZE - thisPartOffset);
		const uint16_t totalRequestBlocks =
		    (totalRequestSize + thisPartOffset + SFSBLOCKSIZE - 1) /
		    SFSBLOCKSIZE;

		std::vector<uint8_t> readDataPrefix;
		messageSerializer->serializePrefixOfCstoclReadData(
		    readDataPrefix, chunkId, offset, thisPartSize);
		auto packet = createDetachedPacketWithOutputBuffer(readDataPrefix);
		if (packet == kInvalidPacket) {
			state = State::Close;
			return;
		}
		readPacket = std::move(packet);

		uint32_t readAheadBlocks = 0;
		uint32_t maxReadBehindBlocks = 0;

		if (!static_cast<bool>(isChunkOpen)) {
			if (gHDDReadAhead.blocksToBeReadAhead() > 0) {
				readAheadBlocks =
				    totalRequestBlocks + gHDDReadAhead.blocksToBeReadAhead();
			}
			// Try not to influence slow streams to much:
			maxReadBehindBlocks = std::min(totalRequestBlocks,
					gHDDReadAhead.maxBlocksToBeReadBehind());
		}
		readJobId = job_read(*workerJobPool, readFinishedCallback, this, chunkId, chunkVersion,
		                     chunkType, offset, thisPartSize, maxReadBehindBlocks, readAheadBlocks,
		                     readPacket->outputBuffer.get(), !isChunkOpen);
		if (readJobId == 0) {
			state = State::Close;
			return;
		}

		todoReadCounter++;
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
	sassert(type == SAU_CLTOCS_READ || type == CLTOCS_READ);
	try {
		if (type == SAU_CLTOCS_READ) {
			PacketVersion v;
			deserializePacketVersionNoHeader(data, length, v);
			if (v == cltocs::read::kECChunks) {
				cltocs::read::deserialize(data, length, chunkId, chunkVersion,
				                          chunkType, offset, size);
			} else {
				legacy::ChunkPartType legacy_type;
				cltocs::read::deserialize(data, length, chunkId, chunkVersion,
				                          legacy_type, offset, size);
				chunkType = legacy_type;
			}
		} else {
			deserializeAllLegacyPacketDataNoHeader(data, length, chunkId,
			                                       chunkVersion, offset, size);
			chunkType = slice_traits::standard::ChunkPartType();
		}
		messageSerializer = MessageSerializer::getSerializer(type);
	} catch (IncorrectDeserializationException&) {
		safs_pretty_syslog(
		    LOG_NOTICE,
		    "read_init: Cannot deserialize READ message (type:%" PRIX32
		    ", length:%" PRIu32 ")",
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
	todoReadCounter = 0;
	readJobId = 0;
	LOG_AVG_START0(readOperationTimer, "csserv_read");
	readContinue();
}

void ChunkserverEntry::prefetch(const uint8_t *data, PacketHeader::Type type,
                                PacketHeader::Length length) {
	sassert(type == SAU_CLTOCS_PREFETCH);
	PacketVersion v;
	try {
		deserializePacketVersionNoHeader(data, length, v);
		if (v == cltocs::prefetch::kECChunks) {
			cltocs::prefetch::deserialize(data, length, chunkId, chunkVersion,
			                              chunkType, offset, size);
		} else {
			legacy::ChunkPartType legacy_type;
			cltocs::prefetch::deserialize(data, length, chunkId, chunkVersion,
			                              legacy_type, offset, size);
			chunkType = legacy_type;
		}
	} catch (IncorrectDeserializationException &) {
		safs_pretty_syslog(
		    LOG_NOTICE,
		    "prefetch: Cannot deserialize PREFETCH message (type:%" PRIX32
		    ", length:%" PRIu32 ")",
		    type, length);
		state = State::Close;
		return;
	}
	// Start prefetching in background, don't wait for it to complete
	auto firstBlock = offset / SFSBLOCKSIZE;
	auto lastByte = offset + size - 1;
	auto lastBlock = lastByte / SFSBLOCKSIZE;
	auto nrOfBlocks = lastBlock - firstBlock + 1;
	job_prefetch(*workerJobPool, chunkId, chunkVersion, chunkType, firstBlock, nrOfBlocks);
}

// bg writing

void ChunkserverEntry::writeFinishedCallback(uint8_t status, void *entry) {
	TRACETHIS();
	auto *eptr = static_cast<ChunkserverEntry *>(entry);
	eptr->writeJobId = 0;
	sassert(eptr->messageSerializer != nullptr);
	if (status != SAUNAFS_STATUS_OK) {
		std::vector<uint8_t> buffer;
		eptr->messageSerializer->serializeCstoclWriteStatus(
		    buffer, eptr->chunkId, eptr->writeJobWriteId, status);
		eptr->createAttachedPacket(buffer);
		eptr->state = State::WriteFinish;
		return;
	}
	if (eptr->writeJobWriteId == 0) {
		eptr->isChunkOpen = 1;
	}
	if (eptr->state == State::WriteLast) {
		std::vector<uint8_t> buffer;
		eptr->messageSerializer->serializeCstoclWriteStatus(
		    buffer, eptr->chunkId, eptr->writeJobWriteId, status);
		eptr->createAttachedPacket(buffer);
	} else {
		if (eptr->partiallyCompletedWrites.count(eptr->writeJobWriteId) > 0) {
			// found - it means that it was added by status_receive, ie. next
			// chunkserver from a chain finished writing before our worker
			sassert(eptr->messageSerializer != nullptr);
			std::vector<uint8_t> buffer;
			eptr->messageSerializer->serializeCstoclWriteStatus(
			    buffer, eptr->chunkId, eptr->writeJobWriteId,
			    SAUNAFS_STATUS_OK);
			eptr->createAttachedPacket(buffer);
			eptr->partiallyCompletedWrites.erase(eptr->writeJobWriteId);
		} else {
			// not found - so add it
			eptr->partiallyCompletedWrites.insert(eptr->writeJobWriteId);
		}
	}
	eptr->checkNextPacket();
}

void serializeCltocsWriteInit(std::vector<uint8_t> &buffer, uint64_t chunkId,
                              uint32_t chunkVersion, ChunkPartType chunkType,
                              const std::vector<ChunkTypeWithAddress> &chain,
                              uint32_t target_version) {
	if (target_version >= kFirstECVersion) {
		cltocs::writeInit::serialize(buffer, chunkId, chunkVersion, chunkType,
		                             chain);
	} else if (target_version >= kFirstXorVersion) {
		assert((int)chunkType.getSliceType() < Goal::Slice::Type::kECFirst);
		std::vector<NetworkAddress> legacy_chain;
		legacy_chain.reserve(chain.size());
		for (const auto &entry : chain) {
			legacy_chain.push_back(entry.address);
		}
		cltocs::writeInit::serialize(buffer, chunkId, chunkVersion,
		                             (legacy::ChunkPartType)chunkType,
		                             legacy_chain);
	} else {
		assert(slice_traits::isStandard(chunkType));
		LegacyVector<NetworkAddress> legacy_chain;
		legacy_chain.reserve(chain.size());
		for (const auto &entry : chain) {
			legacy_chain.push_back(entry.address);
		}
		serializeLegacyPacket(buffer, CLTOCS_WRITE, chunkId, chunkVersion,
		                      legacy_chain);
	}
}

void ChunkserverEntry::writeInit(const uint8_t *data, PacketHeader::Type type,
                                 PacketHeader::Length length) {
	TRACETHIS();
	std::vector<ChunkTypeWithAddress> chain;

	sassert(type == SAU_CLTOCS_WRITE_INIT || type == CLTOCS_WRITE);
	try {
		if (type == SAU_CLTOCS_WRITE_INIT) {
			PacketVersion v;
			deserializePacketVersionNoHeader(data, length, v);
			if (v == cltocs::writeInit::kECChunks) {
				cltocs::writeInit::deserialize(data, length, chunkId,
				                               chunkVersion, chunkType, chain);
			} else {
				std::vector<NetworkAddress> legacy_chain;
				legacy::ChunkPartType legacy_type;
				cltocs::writeInit::deserialize(data, length, chunkId,
				                               chunkVersion, legacy_type,
				                               legacy_chain);
				chunkType = legacy_type;
				for (const auto &address : legacy_chain) {
					chain.emplace_back(address, chunkType, kFirstXorVersion);
				}
			}
		} else {
			LegacyVector<NetworkAddress> legacyChain;
			deserializeAllLegacyPacketDataNoHeader(data, length, chunkId,
			                                       chunkVersion, legacyChain);
			for (const auto &address : legacyChain) {
				chain.emplace_back(address,
				                   slice_traits::standard::ChunkPartType(),
				                   kStdVersion);
			}
			chunkType = slice_traits::standard::ChunkPartType();
		}
		messageSerializer = MessageSerializer::getSerializer(type);
	} catch (IncorrectDeserializationException &ex) {
		safs_pretty_syslog(
		    LOG_NOTICE,
		    "Received malformed WRITE_INIT message (length: %" PRIu32 ")",
		    length);
		state = State::Close;
		return;
	}

	if (!chain.empty()) {
		// Create a chain -- connect to the next chunkserver
		fwdServer = chain[0].address;
		uint32_t target_version = chain[0].chunkserver_version;
		chain.erase(chain.begin());
		serializeCltocsWriteInit(fwdInitPacket, chunkId, chunkVersion,
		                         chunkType, chain, target_version);
		fwdStartPtr = fwdInitPacket.data();
		fwdBytesLeft = fwdInitPacket.size();
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
	writeJobWriteId = 0;
	writeJobId = job_open(*workerJobPool, writeFinishedCallback, this, chunkId, chunkType);
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
	const uint8_t* dataToWrite;

	sassert(type == SAU_CLTOCS_WRITE_DATA || type == CLTOCS_WRITE_DATA);
	try {
		const auto *serializer = MessageSerializer::getSerializer(type);
		if (messageSerializer != serializer) {
			safs_pretty_syslog(
			    LOG_NOTICE,
			    "Received WRITE_DATA message incompatible with WRITE_INIT");
			state = State::Close;
			return;
		}
		if (type == SAU_CLTOCS_WRITE_DATA) {
			cltocs::writeData::deserializePrefix(data, length, opChunkId,
			                                     writeId, blocknum, opOffset,
			                                     opSize, crc);
			dataToWrite = data + cltocs::writeData::kPrefixSize;
		} else {
			uint16_t offset16;
			deserializeAllLegacyPacketDataNoHeader(data, length, opChunkId,
			                                       writeId, blocknum, offset16,
			                                       opSize, crc, dataToWrite);
			opOffset = offset16;
			sassert(chunkType == slice_traits::standard::ChunkPartType());
		}
	} catch (IncorrectDeserializationException &) {
		safs_pretty_syslog(
		    LOG_NOTICE,
		    "Received malformed WRITE_DATA message (length: %" PRIu32 ")",
		    length);
		state = State::Close;
		return;
	}

	uint8_t status = SAUNAFS_STATUS_OK;
	uint32_t dataSize = data + length - dataToWrite;
	if (dataSize != opSize) {
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

	preserveInputPacket();
	writeJobWriteId = writeId;
	writeJobId = job_write(*workerJobPool, writeFinishedCallback, this, opChunkId, chunkVersion,
	                       chunkType, blocknum, opOffset, opSize, crc, dataToWrite);
}

void ChunkserverEntry::writeStatus(const uint8_t *data, PacketHeader::Type type,
                                   PacketHeader::Length length) {
	TRACETHIS();
	uint64_t opChunkId;
	uint32_t writeId;
	uint8_t status;

	sassert(type == SAU_CSTOCL_WRITE_STATUS || type == CSTOCL_WRITE_STATUS);
	sassert(messageSerializer != nullptr);
	try {
		const auto *serializer = MessageSerializer::getSerializer(type);
		if (messageSerializer != serializer) {
			safs_pretty_syslog(
			    LOG_NOTICE,
			    "Received WRITE_DATA message incompatible with WRITE_INIT");
			state = State::Close;
			return;
		}
		if (type == SAU_CSTOCL_WRITE_STATUS) {
			std::vector<uint8_t> message(data, data + length);
			cstocl::writeStatus::deserialize(message, opChunkId, writeId,
			                                 status);
		} else {
			deserializeAllLegacyPacketDataNoHeader(data, length, opChunkId,
			                                       writeId, status);
			sassert(chunkType == slice_traits::standard::ChunkPartType());
		}
	} catch (IncorrectDeserializationException &) {
		safs_pretty_syslog(
		    LOG_NOTICE,
		    "Received malformed WRITE_STATUS message (length: %" PRIu32 ")",
		    length);
		state = State::Close;
		return;
	}

	if (chunkId != opChunkId) {
		status = SAUNAFS_ERROR_WRONGCHUNKID;
		writeId = 0;
	}

	if (status != SAUNAFS_STATUS_OK) {
		std::vector<uint8_t> buffer;
		messageSerializer->serializeCstoclWriteStatus(buffer, opChunkId,
		                                              writeId, status);
		createAttachedPacket(buffer);
		state = State::WriteFinish;
		return;
	}

	if (partiallyCompletedWrites.contains(writeId)) {
		// found - means it was added by write_finished
		std::vector<uint8_t> buffer;
		messageSerializer->serializeCstoclWriteStatus(
		    buffer, opChunkId, writeId, SAUNAFS_STATUS_OK);
		createAttachedPacket(buffer);
		partiallyCompletedWrites.erase(writeId);
	} else {
		// if not found then add record
		partiallyCompletedWrites.insert(writeId);
	}
}

void ChunkserverEntry::writeEnd(const uint8_t *data, uint32_t length) {
	TRACETHIS();
	uint64_t opChunkId;
	messageSerializer = nullptr;

	try {
		cltocs::writeEnd::deserialize(data, length, opChunkId);
	} catch (IncorrectDeserializationException&) {
		safs_pretty_syslog(
		    LOG_NOTICE,
		    "Received malformed WRITE_END message (length: %" PRIu32 ")",
		    length);
		state = State::WriteFinish;
		return;
	}
	if (opChunkId != chunkId) {
		safs_pretty_syslog(LOG_NOTICE,"Received malformed WRITE_END message "
				"(got chunkId=%016" PRIX64 ", expected %016" PRIX64 ")",
				opChunkId, chunkId);
		state = State::WriteFinish;
		return;
	}
	if (writeJobId > 0 || !partiallyCompletedWrites.empty() ||
	    !outputPackets.empty()) {
		/*
		 * WRITE_END received too early:
		 * eptr->wjobid > 0 -- hdd worker is working (writing some data)
		 * !eptr->partiallyCompletedWrites.empty() -- there are write tasks
		 * which have not been acked by our hdd worker EX-or next chunkserver
		 * from a chain eptr->outputhead != nullptr -- there is a status being
		 * send
		 */
		// TODO(msulikowski) temporary syslog message. May be useful until this
		// code is fully tested
		safs_pretty_syslog(LOG_NOTICE, "Received WRITE_END message too early");
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
	inputPacket.useAlignedMemory = false;
	state = State::Idle;
}

void ChunkserverEntry::sauGetChunkBlocksFinishedLegacyCallback(uint8_t status,
                                                               void *entry) {
	TRACETHIS();
	auto *eptr = static_cast<ChunkserverEntry*>(entry);
	eptr->getBlocksJobId = 0;
	std::vector<uint8_t> buffer;
	cstocs::getChunkBlocksStatus::serialize(
	    buffer, eptr->chunkId, eptr->chunkVersion,
	    (legacy::ChunkPartType)eptr->chunkType, eptr->getBlocksJobResult,
	    status);
	eptr->createAttachedPacket(buffer);
	eptr->state = State::Idle;
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

void ChunkserverEntry::getChunkBlocksFinishedCallback(uint8_t status,
                                                      void *entry) {
	TRACETHIS();
	auto *eptr = static_cast<ChunkserverEntry *>(entry);
	eptr->getBlocksJobId = 0;
	std::vector<uint8_t> buffer;
	serializeLegacyPacket(buffer, CSTOCS_GET_CHUNK_BLOCKS_STATUS, eptr->chunkId,
	                      eptr->chunkVersion, eptr->getBlocksJobResult, status);
	eptr->createAttachedPacket(buffer);
	eptr->state = State::Idle;
}

void ChunkserverEntry::sauGetChunkBlocks(const uint8_t *data, uint32_t length) {
	PacketVersion v;
	deserializePacketVersionNoHeader(data, length, v);
	if (v == cstocs::getChunkBlocks::kECChunks) {
		cstocs::getChunkBlocks::deserialize(data, length, chunkId, chunkVersion,
		                                    chunkType);

		getBlocksJobId = job_get_blocks(*workerJobPool, sauGetChunkBlocksFinishedCallback, this,
		                                chunkId, chunkVersion, chunkType, &getBlocksJobResult);

	} else {
		legacy::ChunkPartType legacy_type;
		cstocs::getChunkBlocks::deserialize(data, length, chunkId, chunkVersion,
		                                    legacy_type);
		chunkType = legacy_type;
		getBlocksJobId =
		    job_get_blocks(*workerJobPool, sauGetChunkBlocksFinishedLegacyCallback, this, chunkId,
		                   chunkVersion, chunkType, &getBlocksJobResult);
	}
	state = State::GetBlock;
}

void ChunkserverEntry::getChunkBlocks(const uint8_t *data, uint32_t length) {
	deserializeAllLegacyPacketDataNoHeader(data, length, chunkId,
	                                       chunkVersion);
	chunkType = slice_traits::standard::ChunkPartType();
	getBlocksJobId = job_get_blocks(*workerJobPool, getChunkBlocksFinishedCallback, this, chunkId,
	                                chunkVersion, chunkType, &(getBlocksJobResult));
	state = State::GetBlock;
}

/* IDLE operations */

void ChunkserverEntry::hddListV2([[maybe_unused]] const uint8_t *data,
                                 uint32_t length) {
	TRACETHIS();
	uint32_t opSize;
	uint8_t *ptr;

	if (length != 0) {  // This packet should not have any data
		safs_pretty_syslog(LOG_NOTICE,
		                   "CLTOCS_HDD_LIST_V2 - wrong size (%" PRIu32 "/0)",
		                   length);
		state = State::Close;
		return;
	}
	opSize = hddGetSerializedSizeOfAllDiskInfosV2(); // lock
	ptr = createAttachedPacket(CSTOCL_HDD_LIST_V2, opSize);
	hddSerializeAllDiskInfosV2(ptr); // unlock
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
	chartid = get32bit(&data);
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
	chartid = get32bit(&data);
	len = charts_datasize(chartid);
	ptr = createAttachedPacket(ANTOCL_CHART_DATA, len);
	if (len > 0) {
		charts_makedata(ptr, chartid);
	}
}

void ChunkserverEntry::testChunk(const uint8_t *data, uint32_t length) {
	try {
		PacketVersion vers;
		deserializePacketVersionNoHeader(data, length, vers);
		ChunkWithVersionAndType chunk;
		if (vers == cltocs::testChunk::kECChunks) {
			cltocs::testChunk::deserialize(data, length, chunk.id,
			                               chunk.version, chunk.type);
		} else {
			legacy::ChunkPartType legacy_type;
			cltocs::testChunk::deserialize(data, length, chunk.id,
			                               chunk.version, legacy_type);
			chunk.type = legacy_type;
		}
		hddAddChunkToTestQueue(chunk);
	} catch (IncorrectDeserializationException &e) {
		safs_pretty_syslog(
		    LOG_NOTICE,
		    "SAU_CLTOCS_TEST_CHUNK - bad packet: %s (length: %" PRIu32 ")",
		    e.what(), length);
		state = State::Close;
		return;
	}
}

void ChunkserverEntry::outputCheckReadFinished() {
	TRACETHIS();
	if (state == State::Read) {
		sendFinished();
	}
}

void ChunkserverEntry::closeJobs() {
	TRACETHIS();
	if (readJobId > 0) {
		workerJobPool->disableJob(readJobId);
		workerJobPool->changeCallback(readJobId, delayedCloseCallback, this);
		state = State::CloseWait;
	} else if (writeJobId > 0) {
		workerJobPool->disableJob(writeJobId);
		workerJobPool->changeCallback(writeJobId, delayedCloseCallback, this);
		state = State::CloseWait;
	} else if (getBlocksJobId > 0) {
		workerJobPool->disableJob(getBlocksJobId);
		workerJobPool->changeCallback(getBlocksJobId, delayedCloseCallback, this);
		state = State::CloseWait;
	} else {
		if (isChunkOpen) {
			job_close(*workerJobPool, nullptr, nullptr, chunkId, chunkType);
			isChunkOpen = 0;
		}
		state = State::Closed;
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
		case CLTOCS_READ:
		case SAU_CLTOCS_READ:
			readInit(data, type, length);
			break;
		case SAU_CLTOCS_PREFETCH:
			prefetch(data, type, length);
			break;
		case CLTOCS_WRITE:
		case SAU_CLTOCS_WRITE_INIT:
			writeInit(data, type, length);
			break;
		case CSTOCS_GET_CHUNK_BLOCKS:
			getChunkBlocks(data, length);
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
			safs_pretty_syslog(
			    LOG_NOTICE,
			    "Got invalid message in Idle state (type:%" PRIu32 ")", type);
			state = State::Close;
			break;
		}
	} else if (state == State::WriteLast) {
		switch (type) {
		case CLTOCS_WRITE_DATA:
		case SAU_CLTOCS_WRITE_DATA:
			writeData(data, type, length);
			break;
		case SAU_CLTOCS_WRITE_END:
			writeEnd(data, length);
			break;
		default:
			safs_pretty_syslog(
			    LOG_NOTICE,
			    "Got invalid message in WriteLast state (type:%" PRIu32 ")",
			    type);
			state = State::Close;
			break;
		}
	} else if (state == State::WriteForward) {
		switch (type) {
		case CLTOCS_WRITE_DATA:
		case SAU_CLTOCS_WRITE_DATA:
			writeData(data, type, length);
			break;
		case CSTOCL_WRITE_STATUS:
		case SAU_CSTOCL_WRITE_STATUS:
			writeStatus(data, type, length);
			break;
		case SAU_CLTOCS_WRITE_END:
			writeEnd(data, length);
			break;
		default:
			safs_pretty_syslog(
			    LOG_NOTICE,
			    "Got invalid message in WriteForward state (type:%" PRIu32 ")",
			    type);
			state = State::Close;
			break;
		}
	} else if (state == State::WriteFinish) {
		switch (type) {
		case CLTOCS_WRITE_DATA:
		case SAU_CLTOCS_WRITE_DATA:
		case SAU_CLTOCS_WRITE_END:
			return;
		default:
			safs_pretty_syslog(
			    LOG_NOTICE,
			    "Got invalid message in WriteFinish state (type:%" PRIu32 ")",
			    type);
			state = State::Close;
		}
	} else {
		safs_pretty_syslog(LOG_NOTICE, "Got invalid message (type:%" PRIu32 ")",
		                   type);
		state = State::Close;
	}
}

void ChunkserverEntry::checkNextPacket() {
	TRACETHIS();
	uint32_t type;
	uint32_t opSize;
	const uint8_t *ptr;

	if (state == State::WriteForward) {
		if (mode == Mode::Data && inputPacket.bytesLeft == 0 &&
		    fwdBytesLeft == 0) {
			ptr = headerBuffer;
			type = get32bit(&ptr);
			opSize = get32bit(&ptr);

			mode = Mode::Header;
			inputPacket.bytesLeft = PacketHeader::kSize;
			inputPacket.startPtr = headerBuffer;

			if (inputPacket.useAlignedMemory) {
				gotPacket(type,
				          inputPacket.alignedBuffer.data() + kIOAlignedOffset +
				              PacketHeader::kSize,
				          opSize);
			} else {
				gotPacket(type, inputPacket.packet.data() + PacketHeader::kSize,
				          opSize);
			}
		}
	} else {
		if (mode == Mode::Data && inputPacket.bytesLeft == 0) {
			ptr = headerBuffer;
			type = get32bit(&ptr);
			opSize = get32bit(&ptr);

			mode = Mode::Header;
			inputPacket.bytesLeft = PacketHeader::kSize;
			inputPacket.startPtr = headerBuffer;

			if (inputPacket.useAlignedMemory) {
				gotPacket(type,
				          inputPacket.alignedBuffer.data() + kIOAlignedOffset,
				          opSize);
			} else {
				gotPacket(type, inputPacket.packet.data(), opSize);
			}
		}
	}
}

void ChunkserverEntry::fwdConnected() {
	TRACETHIS();
	int status = tcpgetstatus(fwdSocket);
	if (status) {
		safs_silent_errlog(LOG_WARNING, "connection failed, error");
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
		bytesRead =
		    read(fwdSocket, fwdInputPacket.startPtr, fwdInputPacket.bytesLeft);
		if (bytesRead == 0) {
			fwdError();
			return;
		}
		if (bytesRead < 0) {
			if (errno != EAGAIN) {
				safs_silent_errlog(LOG_NOTICE, "(fwdread) read error");
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
		ptr = fwdHeaderBuffer + sizeof(PacketHeader::Type);  // skip type
		opSize = get32bit(&ptr);
		if (opSize > kMaxPacketSize) {
			safs_pretty_syslog(LOG_WARNING,
			                   "(fwdread) packet too long (%" PRIu32 "/%u)",
			                   opSize, kMaxPacketSize);
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
			bytesRead = read(fwdSocket, fwdInputPacket.startPtr,
			                 fwdInputPacket.bytesLeft);
			if (bytesRead == 0) {
				fwdError();
				return;
			}
			if (bytesRead < 0) {
				if (errno != EAGAIN) {
					safs_silent_errlog(LOG_NOTICE, "(fwdread) read error");
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
		type = get32bit(&ptr);
		opSize = get32bit(&ptr);

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
				safs_silent_errlog(LOG_NOTICE, "(fwdwrite) write error");
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
	int32_t bytesReadOrWritten;

	if (mode == Mode::Header) {
		bytesReadOrWritten =
		    ::read(sock, inputPacket.startPtr, inputPacket.bytesLeft);
		if (bytesReadOrWritten == 0) {
			state = State::Close;
			return;
		}
		if (bytesReadOrWritten < 0) {
			if (errno != EAGAIN) {
				safs_silent_errlog(LOG_NOTICE, "(forward) read error");
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
			deserializePacketHeader(headerBuffer,
			                        sizeof(headerBuffer), header);
		} catch (IncorrectDeserializationException&) {
			safs_pretty_syslog(LOG_WARNING,
			                   "(forward) Received malformed network packet");
			state = State::Close;
			return;
		}
		if (header.length > kMaxPacketSize) {
			safs_pretty_syslog(LOG_WARNING,
			                   "(forward) packet too long (%" PRIu32 "/%u)",
			                   header.length, kMaxPacketSize);
			state = State::Close;
			return;
		}
		uint32_t totalPacketLength = PacketHeader::kSize + header.length;
		inputPacket.packet.resize(totalPacketLength);
		passert(inputPacket.packet.data());
		std::copy(headerBuffer, headerBuffer + PacketHeader::kSize,
		          inputPacket.packet.begin());
		inputPacket.bytesLeft = header.length;
		inputPacket.startPtr = inputPacket.packet.data() + PacketHeader::kSize;
		if (header.type == CLTOCS_WRITE_DATA
				|| header.type == SAU_CLTOCS_WRITE_DATA
				|| header.type == SAU_CLTOCS_WRITE_END) {
			fwdBytesLeft = PacketHeader::kSize;
			fwdStartPtr = inputPacket.packet.data();
		}
		mode = Mode::Data;
	}

	if (inputPacket.bytesLeft > 0) {
		bytesReadOrWritten =
		    ::read(sock, inputPacket.startPtr, inputPacket.bytesLeft);
		if (bytesReadOrWritten == 0) {
			state = State::Close;
			return;
		}
		if (bytesReadOrWritten < 0) {
			if (errno != EAGAIN) {
				safs_silent_errlog(LOG_NOTICE, "(forward) read error");
				state = State::Close;
			}
			return;
		}
		stats_bytesin += bytesReadOrWritten;
		inputPacket.startPtr += bytesReadOrWritten;
		inputPacket.bytesLeft -= bytesReadOrWritten;
		if (fwdStartPtr != nullptr) {
			fwdBytesLeft += bytesReadOrWritten;
		}
	}

	if (fwdBytesLeft > 0) {
		sassert(fwdStartPtr != nullptr);
		bytesReadOrWritten = ::write(fwdSocket, fwdStartPtr, fwdBytesLeft);
		if (bytesReadOrWritten == 0) {
			fwdError();
			return;
		}
		if (bytesReadOrWritten < 0) {
			if (errno != EAGAIN) {
				safs_silent_errlog(LOG_NOTICE, "(forward) write error");
				fwdError();
			}
			return;
		}
		stats_bytesout += bytesReadOrWritten;
		fwdStartPtr += bytesReadOrWritten;
		fwdBytesLeft -= bytesReadOrWritten;
	}

	if (inputPacket.bytesLeft == 0 && fwdBytesLeft == 0 && writeJobId == 0) {
		PacketHeader header;
		try {
			deserializePacketHeader(headerBuffer, sizeof(headerBuffer), header);
		} catch (IncorrectDeserializationException &) {
			safs_pretty_syslog(LOG_WARNING,
			                   "(forward) Received malformed network packet");
			state = State::Close;
			return;
		}
		mode = Mode::Header;
		inputPacket.bytesLeft = PacketHeader::kSize;
		inputPacket.startPtr = headerBuffer;

		uint8_t *packetData = inputPacket.packet.data() + PacketHeader::kSize;
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
		sassert(inputPacket.startPtr + inputPacket.bytesLeft ==
		        headerBuffer + PacketHeader::kSize);
		bytesRead = ::read(sock, inputPacket.startPtr, inputPacket.bytesLeft);
		if (bytesRead == 0) {
			state = State::Close;
			return;
		}
		if (bytesRead < 0) {
			if (errno != EAGAIN) {
				safs_silent_errlog(LOG_NOTICE, "(read) read error");
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
		type = get32bit(&ptr);
		opSize = get32bit(&ptr);

		if (opSize > 0) {
			if (opSize > kMaxPacketSize) {
				safs_pretty_syslog(LOG_WARNING,
				                   "(read) packet too long (%" PRIu32 "/%u)",
				                   opSize, kMaxPacketSize);
				state = State::Close;
				return;
			}

			if (type == SAU_CLTOCS_WRITE_DATA || type == SAU_CLTOCS_WRITE_END) {
				// Allocate memory only if needed. Reuse it most of the time.
				if (inputPacket.alignedBuffer.size() < kIOAlignedPacketSize) {
					inputPacket.alignedBuffer.reserve(kIOAlignedPacketSize);
					passert(inputPacket.alignedBuffer.data());
				}
				inputPacket.startPtr =
				    inputPacket.alignedBuffer.data() + kIOAlignedOffset;
				inputPacket.useAlignedMemory = true;
			} else {
				inputPacket.packet.resize(opSize);
				passert(inputPacket.packet.data());
				inputPacket.startPtr = inputPacket.packet.data();
				inputPacket.useAlignedMemory = false;
			}
		}
		inputPacket.bytesLeft = opSize;
		mode = Mode::Data;
	}

	if (mode == Mode::Data) {
		if (inputPacket.bytesLeft > 0) {
			bytesRead =
			    ::read(sock, inputPacket.startPtr, inputPacket.bytesLeft);
			if (bytesRead == 0) {
				state = State::Close;
				return;
			}
			if (bytesRead < 0) {
				if (errno != EAGAIN) {
					safs_silent_errlog(LOG_NOTICE, "(read) read error");
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
		}
		if (writeJobId == 0) {
			ptr = headerBuffer;
			type = get32bit(&ptr);
			opSize = get32bit(&ptr);

			mode = Mode::Header;
			inputPacket.bytesLeft = PacketHeader::kSize;
			inputPacket.startPtr = headerBuffer;

			if (inputPacket.useAlignedMemory) {
				gotPacket(type,
				          inputPacket.alignedBuffer.data() + kIOAlignedOffset,
				          opSize);
			} else {
				gotPacket(type, inputPacket.packet.data(), opSize);
			}
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
			if (ret == OutputBuffer::WRITE_ERROR) {
				safs_silent_errlog(LOG_NOTICE, "(write) write error");
				state = State::Close;
				return;
			} else if (ret == OutputBuffer::WRITE_AGAIN) {
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
					safs_silent_errlog(LOG_NOTICE, "(write) write error");
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
