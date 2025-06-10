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
#include <cstdint>
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

// Common variables from config
static std::string gBindHostStr;
static bool gEnableLoadFactor;
static std::string gLabel;
static uint32_t gTimeout_ms;

enum class ConnectionMode : std::uint8_t {
	FREE,        /// There is no socket for the connection yet.
	CONNECTING,  /// Connection is being established.
	CONNECTED,   /// Connection is active.
	KILL         /// Connection has been dropped, a reconnection will be attempted.
};

/// @brief Class representing a connection to a Metadata Server (MDS).
///
/// Currently, only one active MDS (known as Master) is supported.
class MasterConn {
public:
	explicit MasterConn(const std::string &masterHostStr, const std::string &masterPortStr,
	                    const std::shared_ptr<JobPool> &jobPool)
	    : masterHostStr_(masterHostStr), masterPortStr_(masterPortStr), jobPool_(jobPool) {}

	// Disable unneeded copying and moving of the connection objects.
	MasterConn(const MasterConn &) = delete;
	MasterConn(MasterConn &&) = delete;
	MasterConn &operator=(const MasterConn &) = delete;
	MasterConn &operator=(const MasterConn &&) = delete;

	~MasterConn() {
		if (socketFD_ >= 0) { tcpclose(socketFD_); }
	}

	// Packet handling

	static void deletePacket(void *packet) {
		auto *outputPacket = static_cast<OutputPacket *>(packet);
		delete outputPacket;
	}

	void attachPacket(void *packet) {
		auto *outputPacket = static_cast<OutputPacket *>(packet);
		outputPackets_.emplace_back(std::move(*outputPacket));
		delete outputPacket;
	}

	void createAttachedPacket(MessageBuffer serializedPacket) {
		outputPackets_.emplace_back(std::move(serializedPacket));
	}

	template <class... Data>
	void createAttachedNoVersionPacket(PacketHeader::Type type, const Data &...data) {
		std::vector<uint8_t> buffer;
		serializeLegacyPacket(buffer, type, data...);
		createAttachedPacket(std::move(buffer));
	}

	// Configuration

	void reloadConfig() {
		auto newMasterHostStr_ = cfg_getstring("MASTER_HOST", "sfsmaster");
		auto newMasterPortStr_ = cfg_getstring("MASTER_PORT", "9420");

		if (newMasterHostStr_ == masterHostStr_ && newMasterPortStr_ == masterPortStr_) {
			return;  // no change
		}

		masterHostStr_ = newMasterHostStr_;
		masterPortStr_ = newMasterPortStr_;

		uint32_t mip{};
		uint16_t mport{};

		if (tcpresolve(masterHostStr_.c_str(), masterPortStr_.c_str(), &mip, &mport, 0) >= 0) {
			if (isLoopbackAddress(mip)) {
				safs::log_warn(
				    "Chunkserver loopback IP addresses are experimental; consider a non-loopback IP address to chunkserver (via /etc/hosts or some other way)");
			}

			if (address().ip != mip || address().port != mport) {
				setMasterAddress(mip, mport);
				setMode(ConnectionMode::KILL);
			}
		} else {
			safs::log_warn("master connection module: can't resolve master host/port ({}:{})",
			               masterHostStr_, masterPortStr_);
		}
	}

	// Connection management

	void sendRegisterLabel() {
		if (mode_ == ConnectionMode::CONNECTED) {
			createAttachedPacket(cstoma::registerLabel::build(gLabel));
		}
	}

	void sendConfig() {
		if (mode_ == ConnectionMode::CONNECTED) {
			createAttachedPacket(cstoma::registerConfig::build(cfg_yaml_string()));
		}
	}

	void sendRegister() {
		uint32_t myip;
		uint16_t myport;
		uint64_t usedspace, totalspace;
		uint64_t tdusedspace, tdtotalspace;
		uint32_t chunkcount, tdchunkcount;

		myip = mainNetworkThreadGetListenIp();
		myport = mainNetworkThreadGetListenPort();
		createAttachedPacket(
		    cstoma::registerHost::build(myip, myport, gTimeout_ms, SAUNAFS_VERSHEX));

		hddForeachChunkInBulks([this](const std::vector<ChunkWithVersionAndType> &chunksBulk) {
			createAttachedPacket(cstoma::registerChunks::build(chunksBulk));
		});

		hddGetTotalSpace(&usedspace, &totalspace, &chunkcount, &tdusedspace, &tdtotalspace,
		                 &tdchunkcount);
		auto registerSpace = cstoma::registerSpace::build(usedspace, totalspace, chunkcount,
		                                                  tdusedspace, tdtotalspace, tdchunkcount);
		createAttachedPacket(std::move(registerSpace));
		sendRegisterLabel();
		sendConfig();
	}

	int initConnect() {
		if (!isMasterAddressValid_) {
			uint32_t mip{};
			uint32_t bip{};
			uint16_t mport{};

			if (tcpresolve(gBindHostStr.c_str(), nullptr, &bip, nullptr, 1) < 0) { bip = 0; }

			bindHostAddress_.ip = bip;

			if (tcpresolve(masterHostStr_.c_str(), masterPortStr_.c_str(), &mip, &mport, 0) >= 0) {
				if (isLoopbackAddress(mip)) {
					safs::log_warn(
					    "Chunkserver loopback IP addresses are experimental; consider assigning an IP address to chunkserver (via /etc/hosts or some other way)");
				}
				address_.ip = mip;
				address_.port = mport;
				isMasterAddressValid_ = true;
			} else {
				safs::log_warn("master connection module: can't resolve master host/port ({}:{})",
				               masterHostStr_, masterPortStr_);
				return -1;
			}
		}

		socketFD_ = tcpsocket();

		if (socketFD_ < 0) {
			safs_pretty_errlog(LOG_WARNING, "master connection module: create socket error");
			return -1;
		}

		if (tcpnonblock(socketFD_) < 0) {
			safs_pretty_errlog(LOG_WARNING, "master connection module: set nonblock error");
			tcpclose(socketFD_);
			socketFD_ = -1;
			return -1;
		}

		if (bindHostAddress_.ip > 0) {
			if (tcpnumbind(socketFD_, bindHostAddress_.ip, 0) < 0) {
				safs_pretty_errlog(LOG_WARNING,
				                   "master connection module: can't bind socket to given ip");
				tcpclose(socketFD_);
				socketFD_ = -1;
				return -1;
			}
		}

		int status = tcpnumconnect(socketFD_, address_.ip, address_.port);

		if (status < 0) {
			safs_pretty_errlog(LOG_WARNING, "master connection module: connect failed");
			tcpclose(socketFD_);
			socketFD_ = -1;
			isMasterAddressValid_ = false;
			return -1;
		}

		if (status == 0) {
			safs_pretty_syslog(LOG_NOTICE, "connected to Master immediately");
			onConnected();
		} else {
			mode_ = ConnectionMode::CONNECTING;
			safs_pretty_syslog_attempt(LOG_NOTICE, "connecting to Master");
		}

		return 0;
	}

	void connectTest() {
		int status = tcpgetstatus(socketFD_);

		if (status) {
			safs_silent_errlog(LOG_WARNING, "connection failed, error");
			tcpclose(socketFD_);
			socketFD_ = -1;
			mode_ = ConnectionMode::FREE;
			isMasterAddressValid_ = 0;
		} else {
			safs::log_info("connected to Master");
			onConnected();
		}
	}

	void onConnected() {
		tcpnodelay(socketFD_);
		mode_ = ConnectionMode::CONNECTED;
		inputPacket_.reset();

		sendRegister();
		lastRead_.reset();
		lastWrite_.reset();
	}

	// Polling

	void providePollDescriptors(std::vector<pollfd> &pdesc) {
		pDescPos_ = -1;

		if (mode_ == ConnectionMode::FREE || socketFD_ < 0) { return; }

		if (mode_ == ConnectionMode::CONNECTED) {
			if (jobPool_->getJobCount() < (kMaxBackgroundJobsCount * 9) / 10) {
				pdesc.emplace_back(socketFD_, POLLIN, 0);
				pDescPos_ = static_cast<int32_t>(pdesc.size() - 1);
			}
		}

		if (((mode_ == ConnectionMode::CONNECTED) && !outputPackets_.empty()) ||
		    mode_ == ConnectionMode::CONNECTING) {
			if (pDescPos_ >= 0) {
				pdesc[pDescPos_].events |= POLLOUT;
			} else {
				pdesc.emplace_back(socketFD_, POLLOUT, 0);
				pDescPos_ = static_cast<int32_t>(pdesc.size() - 1);
			}
		}
	}

	void handlePollErrors(const std::vector<pollfd> &pdesc) {
		// Check if the socket has been closed or has an error.
		if (pDescPos_ >= 0 && (pdesc[pDescPos_].revents & (POLLHUP | POLLERR))) {
			if (mode_ == ConnectionMode::CONNECTING) {
				connectTest();
			} else {
				mode_ = ConnectionMode::KILL;
			}
		}
	}

	void servePoll(const std::vector<pollfd> &pdesc) {
		if (mode_ == ConnectionMode::CONNECTING) {
			// Check if the connection has been established.
			if (socketFD_ >= 0 && pDescPos_ >= 0 && (pdesc[pDescPos_].revents & POLLOUT)) {
				connectTest();
			}
		} else {
			if (pDescPos_ >= 0) {
				// Check if there is data to read from this connection
				if ((mode_ == ConnectionMode::CONNECTED) && (pdesc[pDescPos_].revents & POLLIN)) {
					lastRead_.reset();
					readFromSocket();
				}

				// Check if there is data to write to this connection
				if ((mode_ == ConnectionMode::CONNECTED) && (pdesc[pDescPos_].revents & POLLOUT)) {
					lastWrite_.reset();
					writeToSocket();
				}

				// Check if the connection has not been used for a while and should be closed
				if ((mode_ == ConnectionMode::CONNECTED) && lastRead_.elapsed_ms() > gTimeout_ms) {
					mode_ = ConnectionMode::KILL;
				}

				// Keep the connection alive by sending a NOP packet
				if ((mode_ == ConnectionMode::CONNECTED) &&
				    lastWrite_.elapsed_ms() > (gTimeout_ms / 3) && outputPackets_.empty()) {
					createAttachedNoVersionPacket(ANTOAN_NOP, 0);
				}
			}
		}
	}

	void readFromSocket() {
		ActiveLoopWatchdog watchdog(std::chrono::milliseconds(20));

		watchdog.start();

		while (mode_ != ConnectionMode::KILL) {
			// If the job pool is too busy, do not read more data.
			if (jobPool_->getJobCount() >= (kMaxBackgroundJobsCount * 9) / 10) { return; }

			uint32_t bytesToRead = inputPacket_.bytesToBeRead();
			ssize_t ret = ::read(socketFD_, inputPacket_.pointerToBeReadInto(), bytesToRead);

			if (ret == 0) {
				safs_silent_syslog(LOG_NOTICE, "connection reset by Master");
				mode_ = ConnectionMode::KILL;
				return;
			}

			if (ret < 0) {
				if (errno != EAGAIN) {
					safs_silent_errlog(LOG_NOTICE, "read from Master error");
					mode_ = ConnectionMode::KILL;
				}
				return;
			}

			bytesIn_ += ret;

			try {
				inputPacket_.increaseBytesRead(ret);
			} catch (InputPacketTooLongException &ex) {
				safs_silent_syslog(LOG_WARNING, "reading from master: %s", ex.what());
				mode_ = ConnectionMode::KILL;
				return;
			}

			if (ret == bytesToRead && !inputPacket_.hasData()) {
				continue;  // there might be more data to read in socket's buffer
			}

			if (!inputPacket_.hasData()) { return; }

			// We have a complete packet in the input buffer, let's process it.
			gotPacket(inputPacket_.getHeader(), inputPacket_.getData());

			inputPacket_.reset();

			if (watchdog.expired()) { break; }
		}
	}

	void writeToSocket() {
		ActiveLoopWatchdog watchdog(std::chrono::milliseconds(20));
		ssize_t bytesWritten{-1};

		watchdog.start();

		while (!outputPackets_.empty()) {
			OutputPacket &pack = outputPackets_.front();
			bytesWritten = ::write(socketFD_, pack.packet.data() + pack.bytesSent,
			                       pack.packet.size() - pack.bytesSent);

			if (bytesWritten < 0) {
				if (errno != EAGAIN) {
					safs_silent_errlog(LOG_NOTICE, "write to Master error");
					mode_ = ConnectionMode::KILL;
				}
				return;
			}

			bytesOut_ += bytesWritten;
			pack.bytesSent += bytesWritten;

			if (pack.packet.size() != pack.bytesSent) { return; }

			outputPackets_.pop_front();

			if (watchdog.expired()) { break; }
		}
	}

	void gotPacket(PacketHeader header, const MessageBuffer &message) try {
		switch (header.type) {
		case ANTOAN_NOP:
			break;
		case ANTOAN_UNKNOWN_COMMAND:  // for future use
			break;
		case ANTOAN_BAD_COMMAND_SIZE:  // for future use
			break;
		case SAU_MATOCS_CREATE_CHUNK:
			createChunk(message);
			break;
		case SAU_MATOCS_DELETE_CHUNK:
			deleteChunk(message);
			break;
		case SAU_MATOCS_SET_VERSION:
			setChunkVersion(message);
			break;
		case SAU_MATOCS_DUPLICATE_CHUNK:
			duplicateChunk(message);
			break;
		case SAU_MATOCS_REPLICATE_CHUNK:
			replicateChunk(message);
			break;
		case SAU_MATOCS_TRUNCATE:
			truncateChunk(message);
			break;
		case SAU_MATOCS_DUPTRUNC_CHUNK:
			duplicateTruncateChunk(message);
			break;
		default:
			safs::log_info("got unknown message (type: {})", header.type);
			mode_ = ConnectionMode::KILL;
		}
	} catch (IncorrectDeserializationException &e) {
		safs::log_info(
		    "chunkserver <-> master module: got inconsistent message "
		    "(type:{}, length:{}), {}",
		    header.type, uint32_t(message.size()), e.what());
		mode_ = ConnectionMode::KILL;
	}

	// Chunk operations

	void createChunk(const std::vector<uint8_t> &data) {
		uint64_t chunkId;
		ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
		uint32_t chunkVersion;

		matocs::createChunk::deserialize(data, chunkId, chunkType, chunkVersion);
		auto *outputPacket = new OutputPacket;
		cstoma::createChunk::serialize(outputPacket->packet, chunkId, chunkType, SAUNAFS_STATUS_OK);
		if (jobPool_) {
			job_create(*jobPool_, sauJobFinished(this), outputPacket, chunkId, chunkVersion,
			           chunkType);
		} else {
			safs::log_err("masterconn_create: jobPool is null.");
			delete outputPacket;
		}
	}

	void deleteChunk(const std::vector<uint8_t> &data) {
		uint64_t chunkId;
		uint32_t chunkVersion;
		ChunkPartType chunkType = slice_traits::standard::ChunkPartType();

		matocs::deleteChunk::deserialize(data, chunkId, chunkType, chunkVersion);
		auto *outputPacket = new OutputPacket;
		cstoma::deleteChunk::serialize(outputPacket->packet, chunkId, chunkType, 0);
		if (jobPool_) {
			job_delete(*jobPool_, sauJobFinished(this), outputPacket, chunkId, chunkVersion,
			           chunkType);
		} else {
			safs::log_err("masterconn_delete: jobPool is null.");
			delete outputPacket;
		}
	}

	void setChunkVersion(const std::vector<uint8_t> &data) {
		uint64_t chunkId;
		uint32_t chunkVersion;
		uint32_t newVersion;
		ChunkPartType chunkType = slice_traits::standard::ChunkPartType();

		matocs::setVersion::deserialize(data, chunkId, chunkType, chunkVersion, newVersion);
		auto *outputPacket = new OutputPacket;
		cstoma::setVersion::serialize(outputPacket->packet, chunkId, chunkType, 0);
		if (jobPool_) {
			job_version(*jobPool_, sauJobFinished(this), outputPacket, chunkId, chunkVersion,
			            chunkType, newVersion);
		} else {
			safs::log_err("masterconn_setversion: jobPool is null.");
			delete outputPacket;
		}
	}

	void duplicateChunk(const std::vector<uint8_t> &data) {
		uint64_t newChunkId, oldChunkId;
		uint32_t newChunkVersion, oldChunkVersion;
		ChunkPartType chunkType = slice_traits::standard::ChunkPartType();

		matocs::duplicateChunk::deserialize(data, newChunkId, newChunkVersion, chunkType,
		                                    oldChunkId, oldChunkVersion);
		auto *outputPacket = new OutputPacket;
		cstoma::duplicateChunk::serialize(outputPacket->packet, newChunkId, chunkType, 0);
		if (jobPool_) {
			job_duplicate(*jobPool_, sauJobFinished(this), outputPacket, oldChunkId,
			              oldChunkVersion, oldChunkVersion, chunkType, newChunkId, newChunkVersion);
		} else {
			safs::log_err("masterconn_duplicate: jobPool is null.");
			delete outputPacket;
		}
	}

	void truncateChunk(const std::vector<uint8_t> &data) {
		uint64_t chunkId;
		ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
		uint32_t version;
		uint32_t chunkLength;
		uint32_t newVersion;

		matocs::truncateChunk::deserialize(data, chunkId, chunkType, chunkLength, newVersion,
		                                   version);
		auto *outputPacket = new OutputPacket;
		cstoma::truncate::serialize(outputPacket->packet, chunkId, chunkType, 0);
		if (jobPool_) {
			job_truncate(*jobPool_, sauJobFinished(this), outputPacket, chunkId, chunkType, version,
			             newVersion, chunkLength);
		} else {
			safs::log_err("masterconn_truncate: jobPool is null.");
			delete outputPacket;
		}
	}

	void duplicateTruncateChunk(const std::vector<uint8_t> &data) {
		uint64_t chunkId, copyChunkId;
		uint32_t chunkVersion, copyChunkVersion;
		ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
		uint32_t newLength;

		matocs::duptruncChunk::deserialize(data, copyChunkId, copyChunkVersion, chunkType, chunkId,
		                                   chunkVersion, newLength);
		auto *outputPacket = new OutputPacket;
		cstoma::duptruncChunk::serialize(outputPacket->packet, copyChunkId, chunkType, 0);
		if (jobPool_) {
			job_duptrunc(*jobPool_, sauJobFinished(this), outputPacket, chunkId, chunkVersion,
			             chunkVersion, chunkType, copyChunkId, copyChunkVersion, newLength);
		} else {
			safs::log_err("masterconn_duptrunc: jobPool is null.");
			delete outputPacket;
		}
	}

	void replicateChunk(const std::vector<uint8_t> &data) {
		uint64_t chunkId;
		ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
		uint32_t chunkVersion;
		uint32_t sourcesBufferSize;
		const uint8_t *sourcesBuffer;

		matocs::replicateChunk::deserializePartial(data, chunkId, chunkVersion, chunkType,
		                                           sourcesBuffer);
		sourcesBufferSize = data.size() - (sourcesBuffer - data.data());

		auto *outputPacket = new OutputPacket;
		cstoma::replicateChunk::serialize(outputPacket->packet, chunkId, chunkType,
		                                  SAUNAFS_STATUS_OK, chunkVersion);
		safs_silent_syslog(LOG_DEBUG, "cs.matocs.replicate %" PRIu64, chunkId);

		if (hddScansInProgress()) {
			// Disk scan in progress - replication is not possible
			sauJobFinished(SAUNAFS_ERROR_WAITING, outputPacket);
		} else {
			if (jobPool_) {
				job_replicate(*jobPool_, sauJobFinished(this), outputPacket, chunkId, chunkVersion,
				              chunkType, sourcesBufferSize, sourcesBuffer);
			} else {
				safs::log_err("masterconn_replicate: jobPool is null.");
				delete outputPacket;
			}
		}
	}

	// Callbacks

	static std::function<void(uint8_t status, void *packet)> sauJobFinished(
	    MasterConn *masterConn) {
		return [masterConn](uint8_t status, void *packet) {
			masterConn->sauJobFinished(status, packet);
		};
	}

	void sauJobFinished(uint8_t status, void *packet) {
		auto *outputPacket = static_cast<OutputPacket *>(packet);

		if (mode_ == ConnectionMode::CONNECTED) {
			cstoma::overwriteStatusField(outputPacket->packet, status);
			attachPacket(packet);
		} else {
			deletePacket(packet);
		}
	}

	// Termination

	void releaseResources() {
		if (mode_ != ConnectionMode::FREE && mode_ != ConnectionMode::CONNECTING) {
			tcpclose(socketFD_);
			inputPacket_.reset();
		}
	}

	void resetPackets() {
		inputPacket_.reset();
		outputPackets_.clear();
	}

	// Getters and setters

	ConnectionMode mode() const { return mode_; }

	void setMode(ConnectionMode newMode) { mode_ = newMode; }

	const NetworkAddress &address() const { return address_; }

	void setMasterAddress(uint32_t ip_, uint16_t port_) {
		address_.ip = ip_;
		address_.port = port_;
	}

	const NetworkAddress &bindHostAddress() const { return bindHostAddress_; }

	void setBindHostAddress(uint32_t ip_, uint16_t port_) {
		bindHostAddress_.ip = ip_;
		bindHostAddress_.port = port_;
	}

	int socketFD() const { return socketFD_; }

	bool isMasterAddressValid() const { return isMasterAddressValid_; }

	void setMasterAddressValid(bool valid) { isMasterAddressValid_ = valid; }

	uint64_t bytesIn() const { return bytesIn_; }
	uint64_t bytesOut() const { return bytesOut_; }

	void resetStats() {
		bytesIn_ = 0;
		bytesOut_ = 0;
	}

private:
	std::string masterHostStr_;                  ///< Hostname of the master server.
	std::string masterPortStr_;                  ///< Port of the master server.
	std::shared_ptr<JobPool> jobPool_;           ///< Shared reference to the JobPool.

	ConnectionMode mode_{ConnectionMode::FREE};  ///< Current mode of the connection to this master.
	int socketFD_{-1};                           ///< Socket file descriptor for this connection.
	int32_t pDescPos_{-1};                       ///< Position in the pollfd array.
	Timer lastRead_;                             ///< Time since the last read operation.
	Timer lastWrite_;                            ///< Time since the last write operation.
	InputPacket inputPacket_{kMaxPacketSize};    ///< Input buffer for reading data from the socket.
	std::list<OutputPacket> outputPackets_;      ///< Output packets to be sent to the master.


	NetworkAddress address_;            ///< Address of this master server (IP and port).
	NetworkAddress bindHostAddress_;    ///< Address to bind the socket to (IP and port).
	bool isMasterAddressValid_{false};  ///< Tells if the master address is valid.

	// Statistics
	uint64_t bytesIn_ = 0;   ///< Number of bytes read from the master.
	uint64_t bytesOut_ = 0;  ///< Number of bytes sent to the master.
};

//  From config
static std::string gMasterHost;
static std::string gMasterPort;

static const uint64_t kSendStatusDelay = 5;

//  JobPool shared between all connections to MDSs
static std::shared_ptr<JobPool> gJobPool;

//  Singleton for the MasterConn instance (will become a list of connections in the future)
static std::unique_ptr<MasterConn> gMasterConnSingleton = nullptr;

static int gJobFD{-1};  ///< File descriptor for the job pool notifications
static int32_t gJobFDpDescPos{-1};  ///< Position in the pollfd array for the job pool notifications

constexpr uint32_t kDefaultNumberOfWorkers = 10;
constexpr uint32_t kMinNumberOfWorkers = 2;
static uint32_t gNumberOfWorkers = kDefaultNumberOfWorkers;

static void* gReconnectHook;

//  Stats
static uint32_t stats_maxjobscnt = 0;

void masterconn_stats(uint64_t *bin,uint64_t *bout,uint32_t *maxjobscnt) {
	//  For each connection, add the statistics
	auto totalBytesIn = gMasterConnSingleton->bytesIn();
	auto totalBytesOut = gMasterConnSingleton->bytesOut();

	*bin = totalBytesIn;
	*bout = totalBytesOut;

	gMasterConnSingleton->resetStats();

	// Get the stats non dependent on specific connections
	*maxjobscnt = stats_maxjobscnt;
	stats_maxjobscnt = 0;
}

void masterconn_check_hdd_reports() {
	MasterConn *eptr = gMasterConnSingleton.get();
	uint32_t errorcounter;
	if (eptr->mode() == ConnectionMode::CONNECTED) {
		if (hddGetAndResetSpaceChanged()) {
			uint64_t usedspace, totalspace, tdusedspace, tdtotalspace;
			uint32_t chunkcount, tdchunkcount;
			hddGetTotalSpace(&usedspace, &totalspace, &chunkcount, &tdusedspace, &tdtotalspace,
			                 &tdchunkcount);
			eptr->createAttachedNoVersionPacket(CSTOMA_SPACE, usedspace, totalspace, chunkcount,
			                                    tdusedspace, tdtotalspace, tdchunkcount);
		}
		errorcounter = hddGetAndResetErrorCounter();
		while (errorcounter) {
			eptr->createAttachedNoVersionPacket(CSTOMA_ERROR_OCCURRED);
			errorcounter--;
		}

		std::vector<ChunkWithType> chunks_with_type;
		hddGetDamagedChunks(chunks_with_type, 1000);
		if (!chunks_with_type.empty()) {
			eptr->createAttachedPacket(cstoma::chunkDamaged::build(chunks_with_type));
		}

		hddGetLostChunks(chunks_with_type, 1000);
		if (!chunks_with_type.empty()) {
			eptr->createAttachedPacket(cstoma::chunkLost::build(chunks_with_type));
		}

		std::vector<ChunkWithVersionAndType> chunks_with_version;
		hddGetNewChunks(chunks_with_version, 1000);
		if (!chunks_with_version.empty()) {
			eptr->createAttachedPacket(cstoma::chunkNew::build(chunks_with_version));
		}
	}
}

void masterconn_unwantedjobfinished(uint8_t status, void *packet) {
	(void)status;
	MasterConn::deletePacket(packet);
}

void masterconn_term(void) {
	//  For each connection (currently only one), release its resources.
	MasterConn *eptr = gMasterConnSingleton.get();
	eptr->releaseResources();
	gMasterConnSingleton.reset();

	//  Now reset the last reference to the job pool.
	gJobPool.reset();
}

void masterconn_desc(std::vector<pollfd> &pdesc) {
	LOG_AVG_TILL_END_OF_SCOPE0("master_desc");

	// For each connection to master (currently only one), add its socket to the pollfd array.
	MasterConn *eptr = gMasterConnSingleton.get();

	// Add the descriptor for listening for background jobs finishing.
	gJobFDpDescPos = -1;

	if (eptr->mode() == ConnectionMode::CONNECTED) {
		pdesc.emplace_back(gJobFD, POLLIN, 0);
		gJobFDpDescPos = static_cast<int32_t>(pdesc.size() - 1);
	}

	eptr->providePollDescriptors(pdesc);
}

void masterconn_send_status() {
	static uint8_t prev_factor = 0;
	MasterConn *eptr = gMasterConnSingleton.get();

	if (gEnableLoadFactor) {
		uint8_t load_factor = hddGetLoadFactor();
		if (eptr->mode() == ConnectionMode::CONNECTED && load_factor != prev_factor) {
			eptr->createAttachedPacket(cstoma::status::build(load_factor));
			prev_factor = load_factor;
		}
	}
}

void masterconn_serve(const std::vector<pollfd> &pdesc) {
	LOG_AVG_TILL_END_OF_SCOPE0("master_serve");
	
	MasterConn *eptr = gMasterConnSingleton.get();

	eptr->handlePollErrors(pdesc);
	
	// Check if there are any background jobs to process.
	if ((eptr->mode() == ConnectionMode::CONNECTED) && gJobFDpDescPos >= 0 &&
	    (pdesc[gJobFDpDescPos].revents & POLLIN)) {
		gJobPool->processCompletedJobs();
	}

	// For each connection to master (currently only one), process its socket.
	eptr->servePoll(pdesc);

	// Update general statistics
	if (eptr->mode() == ConnectionMode::CONNECTED) {
		uint32_t jobscnt = gJobPool->getJobCount();
		stats_maxjobscnt = std::max(jobscnt, stats_maxjobscnt);
	}

	// If the connection is in KILL mode, disable the job pool and close the socket.
	if (eptr->mode() == ConnectionMode::KILL) {
		gJobPool->disableAndChangeCallbackAll(masterconn_unwantedjobfinished);
		tcpclose(eptr->socketFD());
		eptr->resetPackets();
		eptr->setMode(ConnectionMode::FREE);
	}
}

void masterconn_reconnect(void) {
	MasterConn *eptr = gMasterConnSingleton.get();
	if (eptr->mode() == ConnectionMode::FREE) {
		eptr->initConnect();
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
	//  Read the common configuration from file.
	gBindHostStr = cfg_getstring("BIND_HOST", "*");
	gEnableLoadFactor = static_cast<bool>(cfg_getuint32("ENABLE_LOAD_FACTOR", 0));

	uint32_t bip = 0;

	if (tcpresolve(gBindHostStr.c_str(), nullptr, &bip, nullptr, 1) < 0) { bip = 0; }

	// For each connection, reload the configuration and reconnect if needed.
	MasterConn *eptr = gMasterConnSingleton.get();

	if (eptr->isMasterAddressValid() && eptr->mode() != ConnectionMode::FREE) {
		if (eptr->bindHostAddress().ip != bip) {
			eptr->setBindHostAddress(bip, eptr->bindHostAddress().port);
			eptr->setMode(ConnectionMode::KILL);
		}

		eptr->reloadConfig();
	} else {
		eptr->setMasterAddressValid(false);
	}

	gTimeout_ms = get_cfg_timeout();

	if (masterconn_load_label()) { eptr->sendRegisterLabel(); }

	eptr->sendConfig();

	uint32_t reconnectionDelay = cfg_getuint32("MASTER_RECONNECTION_DELAY", 5);
	eventloop_timechange(gReconnectHook, TIMEMODE_RUN_LATE, reconnectionDelay, 0);
}

int masterconn_init(void) {
	// Read the configuration
	uint32_t reconnectionDelay = cfg_getuint32("MASTER_RECONNECTION_DELAY", 5);
	gMasterHost = cfg_getstring("MASTER_HOST", "sfsmaster");
	gMasterPort = cfg_getstring("MASTER_PORT", "9420");
	gBindHostStr = cfg_getstring("BIND_HOST", "*");
	gTimeout_ms = get_cfg_timeout();
	gEnableLoadFactor = static_cast<bool>(cfg_getuint32("ENABLE_LOAD_FACTOR", 0));

	if (!masterconn_load_label()) { return -1; }

	// Create the connections (only one at this point)
	gMasterConnSingleton = std::make_unique<MasterConn>(gMasterHost, gMasterPort, gJobPool);
	MasterConn *eptr = gMasterConnSingleton.get();
	passert(eptr);

	// Init the connections
	if (eptr->initConnect() < 0) { return -1; }

	// Register the callbacks in the event loop
	eventloop_eachloopregister(masterconn_check_hdd_reports);
	eventloop_timeregister(TIMEMODE_RUN_LATE, kSendStatusDelay,
	                       rnd_ranged<uint32_t>(kSendStatusDelay), masterconn_send_status);
	gReconnectHook =
	    eventloop_timeregister(TIMEMODE_RUN_LATE, reconnectionDelay,
	                           rnd_ranged<uint32_t>(reconnectionDelay), masterconn_reconnect);
	eventloop_destructregister(masterconn_term);
	eventloop_pollregister(masterconn_desc, masterconn_serve);
	eventloop_reloadregister(masterconn_reload);

	return 0;
}

int masterconn_init_threads(void) {
	gNumberOfWorkers = cfg_get_minvalue<uint32_t>("MASTER_NR_OF_WORKERS", kDefaultNumberOfWorkers,
	                                              kMinNumberOfWorkers);

	try {
		gJobPool =
		    std::make_shared<JobPool>("ma", gNumberOfWorkers, kMaxBackgroundJobsCount, &gJobFD);
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
