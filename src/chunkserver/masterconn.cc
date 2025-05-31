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

#include "chunkserver/masterconn.h"

#include <netinet/in.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <syslog.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <list>
#include <stdexcept>

#include "chunkserver-common/hdd_utils.h"
#include "chunkserver/bgjobs.h"
#include "chunkserver/hddspacemgr.h"
#include "chunkserver/network_main_thread.h"
#include "common/event_loop.h"
#include "common/loop_watchdog.h"
#include "common/massert.h"
#include "common/network_address.h"
#include "common/output_packet.h"
#include "common/random.h"
#include "common/sockets.h"
#include "common/time_utils.h"
#include "config/cfg.h"
#include "devtools/request_log.h"
#include "protocol/SFSCommunication.h"
#include "protocol/cstoma.h"
#include "protocol/input_packet.h"
#include "protocol/matocs.h"
#include "protocol/packet.h"
#include "slogger/slogger.h"

static constexpr uint32_t kMaxPacketSize = 10000;
static constexpr uint32_t kMaxBackgroundJobsCount = 1000;

enum class ConnectionMode : std::uint8_t {
	FREE,        /// There is no socket for the connection yet.
	CONNECTING,  /// Connection is being established.
	CONNECTED,   /// Connection is active.
	KILL         /// Connection has been dropped, a reconnection will be attempted.
};

struct MasterConn {
	MasterConn() : inputPacket(kMaxPacketSize) {}

	// Disable unneeded copying and moving of the connection objects.
	MasterConn(const MasterConn &) = delete;
	MasterConn(MasterConn &&) = delete;
	MasterConn &operator=(const MasterConn &) = delete;
	MasterConn &operator=(const MasterConn &&) = delete;

	~MasterConn() {
		if (sock >= 0) { tcpclose(sock); }
	}

	ConnectionMode mode{ConnectionMode::FREE};
	int sock{-1};
	int32_t pDescPos{-1};  ///< Position in the pollfd array.
	Timer lastRead;
	Timer lastWrite;
	InputPacket inputPacket;
	std::list<OutputPacket> outputPackets;
	NetworkAddress address;
	bool isMasterAddressValid{false};
};

static const uint64_t kSendStatusDelay = 5;

static std::unique_ptr<MasterConn> gMasterConnSingleton = nullptr;
static std::unique_ptr<JobPool> gJobPool;

static int gJobFD{-1};  ///< File descriptor for the job pool notifications
static int32_t gJobFDpDescPos{-1};  ///< Position in the pollfd array for the job pool notifications

// from config
static std::string gMasterHost;
static std::string gMasterPort;
static std::string gBindHostStr;
static NetworkAddress gBindHost;
static uint32_t gTimeout_ms;
static std::string gLabel;
static bool gEnableLoadFactor;

constexpr uint32_t kDefaultNumberOfWorkers = 10;
constexpr uint32_t kMinNumberOfWorkers = 2;
static uint32_t gNumberOfWorkers = kDefaultNumberOfWorkers;

static uint64_t stats_bytesout = 0;
static uint64_t stats_bytesin = 0;
static uint32_t stats_maxjobscnt = 0;

static void* reconnect_hook;

void masterconn_stats(uint64_t *bin,uint64_t *bout,uint32_t *maxjobscnt) {
	*bin = stats_bytesin;
	*bout = stats_bytesout;
	*maxjobscnt = stats_maxjobscnt;
	stats_bytesin = 0;
	stats_bytesout = 0;
	stats_maxjobscnt = 0;
}

void* masterconn_create_detached_packet(uint32_t type,uint32_t size) {
	return new OutputPacket(PacketHeader(type, size));
}

uint8_t* masterconn_get_packet_data(void *packet) {
	OutputPacket* outpacket = (OutputPacket*)packet;
	return outpacket->packet.data() + PacketHeader::kSize;
}

void masterconn_delete_packet(void *packet) {
	OutputPacket* outputPacket = (OutputPacket*)packet;
	delete outputPacket;
}

void masterconn_attach_packet(MasterConn *eptr, void* packet) {
	OutputPacket* outputPacket = (OutputPacket*) packet;
	eptr->outputPackets.emplace_back(std::move(*outputPacket));
	delete outputPacket;
}

void masterconn_create_attached_packet(MasterConn *eptr, MessageBuffer serializedPacket) {
	eptr->outputPackets.emplace_back(std::move(serializedPacket));
}

template<class... Data>
void masterconn_create_attached_no_version_packet(MasterConn *eptr,
		PacketHeader::Type type, const Data&... data) {
	std::vector<uint8_t> buffer;
	serializeLegacyPacket(buffer, type, data...);
	masterconn_create_attached_packet(eptr, std::move(buffer));
}

void masterconn_sendregisterlabel(MasterConn *eptr) {
	if (eptr->mode == ConnectionMode::CONNECTED) {
		masterconn_create_attached_packet(eptr, cstoma::registerLabel::build(gLabel));
	}
}

void masterconn_send_metalogger_config(MasterConn *eptr) {
	if (eptr->mode == ConnectionMode::CONNECTED) {
		masterconn_create_attached_packet(
		    eptr, cstoma::registerConfig::build(cfg_yaml_string()));
	}
}

void masterconn_sendregister(MasterConn *eptr) {
	uint32_t myip;
	uint16_t myport;
	uint64_t usedspace,totalspace;
	uint64_t tdusedspace,tdtotalspace;
	uint32_t chunkcount,tdchunkcount;

	myip = mainNetworkThreadGetListenIp();
	myport = mainNetworkThreadGetListenPort();
	masterconn_create_attached_packet(
	    eptr, cstoma::registerHost::build(myip, myport, gTimeout_ms,
	                                      SAUNAFS_VERSHEX));

	hddForeachChunkInBulks(
	    [eptr](const std::vector<ChunkWithVersionAndType> &chunksBulk) {
		    masterconn_create_attached_packet(
		        eptr, cstoma::registerChunks::build(chunksBulk));
	    });

	hddGetTotalSpace(&usedspace, &totalspace, &chunkcount, &tdusedspace,
	                 &tdtotalspace, &tdchunkcount);
	auto registerSpace =
	    cstoma::registerSpace::build(usedspace, totalspace, chunkcount,
	                                 tdusedspace, tdtotalspace, tdchunkcount);
	masterconn_create_attached_packet(eptr, std::move(registerSpace));
	masterconn_sendregisterlabel(eptr);
	masterconn_send_metalogger_config(eptr);
}

void masterconn_check_hdd_reports() {
	MasterConn *eptr = gMasterConnSingleton.get();
	uint32_t errorcounter;
	if (eptr->mode == ConnectionMode::CONNECTED) {
		if (hddGetAndResetSpaceChanged()) {
			uint64_t usedspace,totalspace,tdusedspace,tdtotalspace;
			uint32_t chunkcount,tdchunkcount;
			hddGetTotalSpace(&usedspace, &totalspace, &chunkcount, &tdusedspace, &tdtotalspace,
					&tdchunkcount);
			masterconn_create_attached_no_version_packet(
					eptr, CSTOMA_SPACE,
					usedspace, totalspace, chunkcount, tdusedspace, tdtotalspace, tdchunkcount);
		}
		errorcounter = hddGetAndResetErrorCounter();
		while (errorcounter) {
			masterconn_create_attached_no_version_packet(eptr, CSTOMA_ERROR_OCCURRED);
			errorcounter--;
		}

		std::vector<ChunkWithType> chunks_with_type;
		hddGetDamagedChunks(chunks_with_type, 1000);
		if (!chunks_with_type.empty()) {
			masterconn_create_attached_packet(eptr, cstoma::chunkDamaged::build(chunks_with_type));
		}

		hddGetLostChunks(chunks_with_type, 1000);
		if (!chunks_with_type.empty()) {
			masterconn_create_attached_packet(eptr, cstoma::chunkLost::build(chunks_with_type));
		}

		std::vector<ChunkWithVersionAndType> chunks_with_version;
		hddGetNewChunks(chunks_with_version, 1000);
		if (!chunks_with_version.empty()) {
			masterconn_create_attached_packet(eptr, cstoma::chunkNew::build(chunks_with_version));
		}
	}
}

void masterconn_jobfinished(uint8_t status, void *packet) {
	uint8_t *ptr;
	MasterConn *eptr = gMasterConnSingleton.get();
	if (eptr->mode == ConnectionMode::CONNECTED) {
		ptr = masterconn_get_packet_data(packet);
		ptr[8]=status;
		masterconn_attach_packet(eptr,packet);
	} else {
		masterconn_delete_packet(packet);
	}
}

void masterconn_saujobfinished(uint8_t status, void *packet) {
	OutputPacket* outputPacket = static_cast<OutputPacket*>(packet);
	MasterConn *eptr = gMasterConnSingleton.get();
	if (eptr->mode == ConnectionMode::CONNECTED) {
		cstoma::overwriteStatusField(outputPacket->packet, status);
		masterconn_attach_packet(eptr, packet);
	} else {
		masterconn_delete_packet(packet);
	}
}

void masterconn_chunkopfinished(uint8_t status,void *packet) {
	uint8_t *ptr;
	MasterConn *eptr = gMasterConnSingleton.get();
	if (eptr->mode == ConnectionMode::CONNECTED) {
		ptr = masterconn_get_packet_data(packet);
		ptr[32]=status;
		masterconn_attach_packet(eptr,packet);
	} else {
		masterconn_delete_packet(packet);
	}
}

void masterconn_replicationfinished(uint8_t status,void *packet) {
	uint8_t *ptr;
	MasterConn *eptr = gMasterConnSingleton.get();
//      syslog(LOG_NOTICE,"job replication status: %" PRIu8,status);
	if (eptr->mode == ConnectionMode::CONNECTED) {
		ptr = masterconn_get_packet_data(packet);
		ptr[12]=status;
		masterconn_attach_packet(eptr,packet);
	} else {
		masterconn_delete_packet(packet);
	}
}

void masterconn_unwantedjobfinished(uint8_t status,void *packet) {
	(void)status;
	masterconn_delete_packet(packet);
}

void masterconn_create([[maybe_unused]] MasterConn *eptr, const std::vector<uint8_t> &data) {
	uint64_t chunkId;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	uint32_t chunkVersion;

	matocs::createChunk::deserialize(data, chunkId, chunkType, chunkVersion);
	OutputPacket *outputPacket = new OutputPacket;
	cstoma::createChunk::serialize(outputPacket->packet, chunkId, chunkType, SAUNAFS_STATUS_OK);
	if (gJobPool) {
		job_create(*gJobPool, masterconn_saujobfinished, outputPacket, chunkId, chunkVersion,
		           chunkType);
	}
	else {
		safs::log_err("masterconn_create: jobPool is null.");
		delete outputPacket;
	}
}

void masterconn_delete([[maybe_unused]] MasterConn *eptr, const std::vector<uint8_t> &data) {
	uint64_t chunkId;
	uint32_t chunkVersion;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();

	matocs::deleteChunk::deserialize(data, chunkId, chunkType, chunkVersion);
	OutputPacket* outputPacket = new OutputPacket;
	cstoma::deleteChunk::serialize(outputPacket->packet, chunkId, chunkType, 0);
	if (gJobPool) {
		job_delete(*gJobPool, masterconn_saujobfinished, outputPacket, chunkId, chunkVersion,
		           chunkType);
	} else {
		safs::log_err("masterconn_delete: jobPool is null.");
		delete outputPacket;
	}
}

void masterconn_setversion([[maybe_unused]] MasterConn *eptr, const std::vector<uint8_t> &data) {
	uint64_t chunkId;
	uint32_t chunkVersion;
	uint32_t newVersion;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();

	matocs::setVersion::deserialize(data, chunkId, chunkType, chunkVersion, newVersion);
	OutputPacket* outputPacket = new OutputPacket;
	cstoma::setVersion::serialize(outputPacket->packet, chunkId, chunkType, 0);
	if (gJobPool) {
		job_version(*gJobPool, masterconn_saujobfinished, outputPacket, chunkId, chunkVersion,
		            chunkType, newVersion);
	} else {
		safs::log_err("masterconn_setversion: jobPool is null.");
		delete outputPacket;
	}
}

void masterconn_duplicate([[maybe_unused]] MasterConn *eptr, const std::vector<uint8_t> &data) {
	uint64_t newChunkId, oldChunkId;
	uint32_t newChunkVersion, oldChunkVersion;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();

	matocs::duplicateChunk::deserialize(data, newChunkId, newChunkVersion, chunkType,
			oldChunkId, oldChunkVersion);
	OutputPacket* outputPacket = new OutputPacket;
	cstoma::duplicateChunk::serialize(outputPacket->packet, newChunkId, chunkType, 0);
	if (gJobPool) {
		job_duplicate(*gJobPool, masterconn_saujobfinished, outputPacket, oldChunkId,
		              oldChunkVersion, oldChunkVersion, chunkType, newChunkId, newChunkVersion);
	} else {
		safs::log_err("masterconn_duplicate: jobPool is null.");
		delete outputPacket;
	}
}

void masterconn_truncate([[maybe_unused]] MasterConn *eptr, const std::vector<uint8_t> &data) {
	uint64_t chunkId;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	uint32_t version;
	uint32_t chunkLength;
	uint32_t newVersion;

	matocs::truncateChunk::deserialize(data, chunkId, chunkType, chunkLength, newVersion, version);
	OutputPacket* outputPacket = new OutputPacket;
	cstoma::truncate::serialize(outputPacket->packet, chunkId, chunkType, 0);
	if (gJobPool) {
		job_truncate(*gJobPool, masterconn_saujobfinished, outputPacket, chunkId, chunkType, version,
		             newVersion, chunkLength);
	} else {
		safs::log_err("masterconn_truncate: jobPool is null.");
		delete outputPacket;
	}
}

void masterconn_duptrunc([[maybe_unused]] MasterConn *eptr, const std::vector<uint8_t> &data) {
	uint64_t chunkId, copyChunkId;
	uint32_t chunkVersion, copyChunkVersion;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	uint32_t newLength;

	matocs::duptruncChunk::deserialize(data, copyChunkId, copyChunkVersion,
			chunkType, chunkId, chunkVersion, newLength);
	OutputPacket* outputPacket = new OutputPacket;
	cstoma::duptruncChunk::serialize(outputPacket->packet, copyChunkId, chunkType, 0);
	if (gJobPool) {
		job_duptrunc(*gJobPool, masterconn_saujobfinished, outputPacket, chunkId, chunkVersion,
		             chunkVersion, chunkType, copyChunkId, copyChunkVersion, newLength);
	} else {
		safs::log_err("masterconn_duptrunc: jobPool is null.");
		delete outputPacket;
	}
}

void masterconn_replicate(const std::vector<uint8_t>& data) {
	uint64_t chunkId;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	uint32_t chunkVersion;
	uint32_t sourcesBufferSize;
	const uint8_t* sourcesBuffer;

	matocs::replicateChunk::deserializePartial(data,
			chunkId, chunkVersion, chunkType, sourcesBuffer);
	sourcesBufferSize = data.size() - (sourcesBuffer - data.data());

	OutputPacket* outputPacket = new OutputPacket;
	cstoma::replicateChunk::serialize(outputPacket->packet,
			chunkId, chunkType, SAUNAFS_STATUS_OK, chunkVersion);
	safs_silent_syslog(LOG_DEBUG, "cs.matocs.replicate %" PRIu64, chunkId);
	if (hddScansInProgress()) {
		// Disk scan in progress - replication is not possible
		masterconn_saujobfinished(SAUNAFS_ERROR_WAITING, outputPacket);
	} else {
		if (gJobPool) {
			job_replicate(*gJobPool, masterconn_saujobfinished, outputPacket, chunkId, chunkVersion,
			              chunkType, sourcesBufferSize, sourcesBuffer);
		} else {
			safs::log_err("masterconn_replicate: jobPool is null.");
			delete outputPacket;
		}
	}

}

void masterconn_gotpacket(MasterConn *eptr, PacketHeader header, const MessageBuffer& message) try {
	switch (header.type) {
		case ANTOAN_NOP:
			break;
		case ANTOAN_UNKNOWN_COMMAND: // for future use
			break;
		case ANTOAN_BAD_COMMAND_SIZE: // for future use
			break;
		case SAU_MATOCS_CREATE_CHUNK:
			masterconn_create(eptr, message);
			break;
		case SAU_MATOCS_DELETE_CHUNK:
			masterconn_delete(eptr, message);
			break;
		case SAU_MATOCS_SET_VERSION:
			masterconn_setversion(eptr, message);
			break;
		case SAU_MATOCS_DUPLICATE_CHUNK:
			masterconn_duplicate(eptr, message);
			break;
		case SAU_MATOCS_REPLICATE_CHUNK:
			masterconn_replicate(message);
			break;
		case SAU_MATOCS_TRUNCATE:
			masterconn_truncate(eptr, message);
			break;
		case SAU_MATOCS_DUPTRUNC_CHUNK:
			masterconn_duptrunc(eptr, message);
			break;
		default:
			safs_pretty_syslog(LOG_NOTICE,"got unknown message (type:%" PRIu32 ")", header.type);
			eptr->mode = ConnectionMode::KILL;
	}
} catch (IncorrectDeserializationException& e) {
	safs_pretty_syslog(LOG_NOTICE,
			"chunkserver <-> master module: got inconsistent message "
			"(type:%" PRIu32 ", length:%" PRIu32"), %s",
			header.type, uint32_t(message.size()), e.what());
	eptr->mode = ConnectionMode::KILL;
}


void masterconn_term(void) {
	MasterConn *eptr = gMasterConnSingleton.get();

	gJobPool.reset();

	if (eptr->mode != ConnectionMode::FREE && eptr->mode != ConnectionMode::CONNECTING) {
		tcpclose(eptr->sock);
		eptr->inputPacket.reset();
	}

	gMasterConnSingleton.reset();
}

void masterconn_connected(MasterConn *eptr) {
	tcpnodelay(eptr->sock);
	eptr->mode = ConnectionMode::CONNECTED;
	eptr->inputPacket.reset();

	masterconn_sendregister(eptr);
	eptr->lastRead.reset();
	eptr->lastWrite.reset();
}

int masterconn_initconnect(MasterConn *eptr) {
	int status;

	if (!eptr->isMasterAddressValid) {
		uint32_t mip;
		uint32_t bip;
		uint16_t mport;

		if (tcpresolve(gBindHostStr.c_str(), nullptr, &bip, nullptr, 1) < 0) { bip = 0; }

		gBindHost.ip = bip;

		if (tcpresolve(gMasterHost.c_str(), gMasterPort.c_str(), &mip, &mport, 0) >= 0) {
			if (isLoopbackAddress(mip)) {
				safs::log_warn(
				    "Chunkserver loopback IP addresses are experimental; consider assigning an IP address to chunkserver (via /etc/hosts or some other way)");
			}
			eptr->address.ip = mip;
			eptr->address.port = mport;
			eptr->isMasterAddressValid = true;
		} else {
			safs::log_warn("master connection module: can't resolve master host/port ({}:{})",
			               gMasterHost, gMasterPort);
			return -1;
		}
	}

	eptr->sock = tcpsocket();

	if (eptr->sock<0) {
		safs_pretty_errlog(LOG_WARNING,"master connection module: create socket error");
		return -1;
	}

	if (tcpnonblock(eptr->sock)<0) {
		safs_pretty_errlog(LOG_WARNING,"master connection module: set nonblock error");
		tcpclose(eptr->sock);
		eptr->sock = -1;
		return -1;
	}

	if (gBindHost.ip > 0) {
		if (tcpnumbind(eptr->sock, gBindHost.ip, 0) < 0) {
			safs_pretty_errlog(LOG_WARNING,
			                   "master connection module: can't bind socket to given ip");
			tcpclose(eptr->sock);
			eptr->sock = -1;
			return -1;
		}
	}

	status = tcpnumconnect(eptr->sock, eptr->address.ip, eptr->address.port);

	if (status<0) {
		safs_pretty_errlog(LOG_WARNING,"master connection module: connect failed");
		tcpclose(eptr->sock);
		eptr->sock = -1;
		eptr->isMasterAddressValid = false;
		return -1;
	}

	if (status==0) {
		safs_pretty_syslog(LOG_NOTICE,"connected to Master immediately");
		masterconn_connected(eptr);
	} else {
		eptr->mode = ConnectionMode::CONNECTING;
		safs_pretty_syslog_attempt(LOG_NOTICE,"connecting to Master");
	}

	return 0;
}

void masterconn_connecttest(MasterConn *eptr) {
	int status;

	status = tcpgetstatus(eptr->sock);
	if (status) {
		safs_silent_errlog(LOG_WARNING,"connection failed, error");
		tcpclose(eptr->sock);
		eptr->sock = -1;
		eptr->mode = ConnectionMode::FREE;
		eptr->isMasterAddressValid = 0;
	} else {
		safs_pretty_syslog(LOG_NOTICE,"connected to Master");
		masterconn_connected(eptr);
	}
}

void masterconn_read(MasterConn *eptr) {
	ActiveLoopWatchdog watchdog(std::chrono::milliseconds(20));

	watchdog.start();
	while (eptr->mode != ConnectionMode::KILL) {
		// If the job pool is too busy, do not read more data.
		if (gJobPool->getJobCount() >= (kMaxBackgroundJobsCount * 9) / 10) { return; }

		uint32_t bytesToRead = eptr->inputPacket.bytesToBeRead();
		ssize_t ret = ::read(eptr->sock, eptr->inputPacket.pointerToBeReadInto(), bytesToRead);

		if (ret == 0) {
			safs_silent_syslog(LOG_NOTICE, "connection reset by Master");
			eptr->mode = ConnectionMode::KILL;
			return;
		}

		if (ret < 0) {
			if (errno != EAGAIN) {
				safs_silent_errlog(LOG_NOTICE, "read from Master error");
				eptr->mode = ConnectionMode::KILL;
			}
			return;
		}

		stats_bytesin += ret;

		try {
			eptr->inputPacket.increaseBytesRead(ret);
		} catch (InputPacketTooLongException &ex) {
			safs_silent_syslog(LOG_WARNING, "reading from master: %s", ex.what());
			eptr->mode = ConnectionMode::KILL;
			return;
		}

		if (ret == bytesToRead && !eptr->inputPacket.hasData()) {
			continue;  // there might be more data to read in socket's buffer
		}

		if (!eptr->inputPacket.hasData()) { return; }

		// We have a complete packet in the input buffer, let's process it.
		masterconn_gotpacket(eptr, eptr->inputPacket.getHeader(), eptr->inputPacket.getData());

		eptr->inputPacket.reset();

		if (watchdog.expired()) { break; }
	}
}

void masterconn_write(MasterConn *eptr) {
	ActiveLoopWatchdog watchdog(std::chrono::milliseconds(20));
	int32_t bytesWritten{-1};

	watchdog.start();
	while (!eptr->outputPackets.empty()) {
		OutputPacket &pack = eptr->outputPackets.front();
		bytesWritten = ::write(eptr->sock, pack.packet.data() + pack.bytesSent,
		                       pack.packet.size() - pack.bytesSent);

		if (bytesWritten < 0) {
			if (errno != EAGAIN) {
				safs_silent_errlog(LOG_NOTICE, "write to Master error");
				eptr->mode = ConnectionMode::KILL;
			}
			return;
		}

		stats_bytesout += bytesWritten;
		pack.bytesSent += bytesWritten;

		if (pack.packet.size() != pack.bytesSent) { return; }

		eptr->outputPackets.pop_front();

		if (watchdog.expired()) { break; }
	}
}

void masterconn_desc(std::vector<pollfd> &pdesc) {
	LOG_AVG_TILL_END_OF_SCOPE0("master_desc");
	MasterConn *eptr = gMasterConnSingleton.get();

	eptr->pDescPos = -1;
	gJobFDpDescPos = -1;

	if (eptr->mode == ConnectionMode::FREE || eptr->sock < 0) { return; }

	if (eptr->mode == ConnectionMode::CONNECTED) {
		pdesc.emplace_back(gJobFD, POLLIN, 0);
		gJobFDpDescPos = static_cast<int32_t>(pdesc.size() - 1);

		if (gJobPool->getJobCount() < (kMaxBackgroundJobsCount * 9) / 10) {
			pdesc.emplace_back(eptr->sock, POLLIN, 0);
			eptr->pDescPos = static_cast<int32_t>(pdesc.size() - 1);
		}
	}

	if (((eptr->mode == ConnectionMode::CONNECTED) && !eptr->outputPackets.empty()) ||
	    eptr->mode == ConnectionMode::CONNECTING) {
		if (eptr->pDescPos >= 0) {
			pdesc[eptr->pDescPos].events |= POLLOUT;
		} else {
			pdesc.emplace_back(eptr->sock, POLLOUT, 0);
			eptr->pDescPos = static_cast<int32_t>(pdesc.size() - 1);
		}
	}
}

void masterconn_send_status() {
	static uint8_t prev_factor = 0;
	MasterConn *eptr = gMasterConnSingleton.get();

	if (gEnableLoadFactor) {
		uint8_t load_factor = hddGetLoadFactor();
		if (eptr->mode == ConnectionMode::CONNECTED && load_factor != prev_factor) {
			masterconn_create_attached_packet(eptr,
				cstoma::status::build(load_factor));
			prev_factor = load_factor;
		}
	}
}

void masterconn_serve(const std::vector<pollfd> &pdesc) {
	LOG_AVG_TILL_END_OF_SCOPE0("master_serve");
	MasterConn *eptr = gMasterConnSingleton.get();

	// Check if the socket has been closed or has an error.
	if (eptr->pDescPos >= 0 && (pdesc[eptr->pDescPos].revents & (POLLHUP | POLLERR))) {
		if (eptr->mode == ConnectionMode::CONNECTING) {
			masterconn_connecttest(eptr);
		} else {
			eptr->mode = ConnectionMode::KILL;
		}
	}

	if (eptr->mode == ConnectionMode::CONNECTING) {
		// Check if the connection has been established.
		if (eptr->sock >= 0 && eptr->pDescPos >= 0 && (pdesc[eptr->pDescPos].revents & POLLOUT)) {
			masterconn_connecttest(eptr);
		}
	} else {
		// Check if there are any background jobs to process.
		if ((eptr->mode == ConnectionMode::CONNECTED) && gJobFDpDescPos >= 0 &&
		    (pdesc[gJobFDpDescPos].revents & POLLIN)) {
			gJobPool->checkJobs();
		}

		if (eptr->pDescPos >= 0) {
			// Check if there is data to read from this connection
			if ((eptr->mode == ConnectionMode::CONNECTED) &&
			    (pdesc[eptr->pDescPos].revents & POLLIN)) {
				eptr->lastRead.reset();
				masterconn_read(eptr);
			}

			// Check if there is data to write to this connection
			if ((eptr->mode == ConnectionMode::CONNECTED) &&
			    (pdesc[eptr->pDescPos].revents & POLLOUT)) {
				eptr->lastWrite.reset();
				masterconn_write(eptr);
			}

			// Check if the connection has not been used for a while and should be closed
			if ((eptr->mode == ConnectionMode::CONNECTED) &&
			    eptr->lastRead.elapsed_ms() > gTimeout_ms) {
				eptr->mode = ConnectionMode::KILL;
			}

			// Keep the connection alive by sending a NOP packet
			if ((eptr->mode == ConnectionMode::CONNECTED) &&
			    eptr->lastWrite.elapsed_ms() > (gTimeout_ms / 3) && eptr->outputPackets.empty()) {
				masterconn_create_attached_no_version_packet(eptr, ANTOAN_NOP, 0);
			}
		}
	}

	if (eptr->mode == ConnectionMode::CONNECTED) {
		uint32_t jobscnt = gJobPool->getJobCount();
		stats_maxjobscnt = std::max(jobscnt, stats_maxjobscnt);
	}

	if (eptr->mode == ConnectionMode::KILL) {
		gJobPool->disableAndChangeCallbackAll(masterconn_unwantedjobfinished);
		tcpclose(eptr->sock);
		eptr->inputPacket.reset();
		eptr->outputPackets.clear();
		eptr->mode = ConnectionMode::FREE;
	}
}

void masterconn_reconnect(void) {
	MasterConn *eptr = gMasterConnSingleton.get();
	if (eptr->mode == ConnectionMode::FREE) {
		masterconn_initconnect(eptr);
	}
}

static uint32_t get_cfg_timeout() {
	return 1000 * cfg_get_minmaxvalue<double>("MASTER_TIMEOUT", 60, 0.01, 1000 * 1000);
}

/// Read the label from configuration file and return true if it's changed to a valid one
bool masterconn_load_label() {
	std::string oldLabel = gLabel;
	gLabel = cfg_getstring("LABEL", MediaLabelManager::kWildcard);
	if (!MediaLabelManager::isLabelValid(gLabel)) {
		safs_pretty_syslog(LOG_WARNING,"invalid label '%s'", gLabel.c_str());
		return false;
	}
	return gLabel != oldLabel;
}

void masterconn_reload(void) {
	MasterConn *eptr = gMasterConnSingleton.get();

	gMasterHost = cfg_getstring("MASTER_HOST", "sfsmaster");
	gMasterPort = cfg_getstring("MASTER_PORT", "9420");
	gBindHostStr = cfg_getstring("BIND_HOST", "*");

	gEnableLoadFactor = static_cast<bool>(cfg_getuint32("ENABLE_LOAD_FACTOR", 0));

	if (eptr->isMasterAddressValid && eptr->mode != ConnectionMode::FREE) {
		uint32_t mip{};
		uint32_t bip{};
		uint16_t mport{};

		if (tcpresolve(gBindHostStr.c_str(), nullptr, &bip, nullptr, 1) < 0) { bip = 0; }

		if (gBindHost.ip != bip) {
			gBindHost.ip = bip;
			eptr->mode = ConnectionMode::KILL;
		}

		if (tcpresolve(gMasterHost.c_str(), gMasterPort.c_str(), &mip, &mport, 0) >= 0) {
			if (isLoopbackAddress(mip)) {
				safs::log_warn(
				    "Chunkserver loopback IP addresses are experimental; consider a non-loopback IP address to chunkserver (via /etc/hosts or some other way)");
			}

			if (eptr->address.ip != mip || eptr->address.port != mport) {
				eptr->address.ip = mip;
				eptr->address.port = mport;
				eptr->mode = ConnectionMode::KILL;
			}
		} else {
			safs::log_warn("master connection module: can't resolve master host/port ({}:{})",
			               gMasterHost, gMasterPort);
		}
	} else {
		eptr->isMasterAddressValid = false;
	}

	gTimeout_ms = get_cfg_timeout();

	uint32_t reconnectionDelay = cfg_getuint32("MASTER_RECONNECTION_DELAY", 5);

	if (masterconn_load_label()) { masterconn_sendregisterlabel(eptr); }

	masterconn_send_metalogger_config(eptr);

	eventloop_timechange(reconnect_hook, TIMEMODE_RUN_LATE, reconnectionDelay, 0);
}

int masterconn_init(void) {
	uint32_t ReconnectionDelay;
	MasterConn *eptr;

	ReconnectionDelay = cfg_getuint32("MASTER_RECONNECTION_DELAY", 5);
	gMasterHost = cfg_getstring("MASTER_HOST", "sfsmaster");
	gMasterPort = cfg_getstring("MASTER_PORT", "9420");
	gBindHostStr = cfg_getstring("BIND_HOST", "*");
	gTimeout_ms = get_cfg_timeout();
	gEnableLoadFactor = static_cast<bool>(cfg_getuint32("ENABLE_LOAD_FACTOR", 0));

	gNumberOfWorkers = cfg_get_minvalue<uint32_t>("MASTER_NR_OF_WORKERS", kDefaultNumberOfWorkers,
	                                              kMinNumberOfWorkers);

	if (!masterconn_load_label()) { return -1; }

	gMasterConnSingleton = std::make_unique<MasterConn>();
	eptr = gMasterConnSingleton.get();
	passert(eptr);

	eptr->isMasterAddressValid = false;
	eptr->mode = ConnectionMode::FREE;
	eptr->pDescPos = -1;

	if (masterconn_initconnect(eptr) < 0) { return -1; }

	eventloop_eachloopregister(masterconn_check_hdd_reports);
	eventloop_timeregister(TIMEMODE_RUN_LATE, kSendStatusDelay,
	                       rnd_ranged<uint32_t>(kSendStatusDelay), masterconn_send_status);
	reconnect_hook =
	    eventloop_timeregister(TIMEMODE_RUN_LATE, ReconnectionDelay,
	                           rnd_ranged<uint32_t>(ReconnectionDelay), masterconn_reconnect);
	eventloop_destructregister(masterconn_term);
	eventloop_pollregister(masterconn_desc, masterconn_serve);
	eventloop_reloadregister(masterconn_reload);

	return 0;
}

int masterconn_init_threads(void) {
	try {
		gJobPool =
		    std::make_unique<JobPool>("ma", gNumberOfWorkers, kMaxBackgroundJobsCount, &gJobFD);
	} catch (const std::runtime_error &e) {
		safs::log_err("masterconn_init_threads: Failed to create JobPool instance: {}", e.what());
		return -1;
	} catch (const std::exception &e) {
		safs::log_err("masterconn_init_threads: Failed to create JobPool instance: {}", e.what());
		return -1;
	}

	if (gJobPool == nullptr) {
		safs::log_err("masterconn_init_threads: jobPool is null. Unable to create worker threads.");
		return -1;
	}

	safs::log_info("master connection: {} background workers created", gNumberOfWorkers);

	return 0;
}
