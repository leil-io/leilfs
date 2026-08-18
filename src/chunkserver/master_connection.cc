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

#include "chunkserver/master_connection.h"

#include <netinet/in.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <syslog.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "chunkserver-common/global_shared_resources.h"
#include "chunkserver-common/hdd_utils.h"
#include "chunkserver/bgjobs.h"
#include "chunkserver/hddspacemgr.h"
#include "chunkserver/network_main_thread.h"
#include "common/chunk_version_with_todel_flag.h"
#include "common/input_packet.h"
#include "common/loop_watchdog.h"
#include "common/network_address.h"
#include "common/output_packet.h"
#include "common/saunafs_version.h"
#include "common/sockets.h"
#include "config/cfg.h"
#include "protocol/SFSCommunication.h"
#include "protocol/cstoma.h"
#include "protocol/matocs.h"
#include "protocol/packet.h"
#include "slogger/slogger.h"

static constexpr uint32_t kMaxBackgroundJobsThreshold = (kMaxBackgroundJobsCount * 9) / 10;

MasterConn::~MasterConn() {
	if (socketFD_ >= 0) { tcpclose(socketFD_); }
}

// Packet handling

void MasterConn::deletePacket(void *packet) {
	auto *outputPacket = static_cast<OutputPacket *>(packet);
	delete outputPacket;
}

void MasterConn::attachPacket(void *packet) {
	auto *outputPacket = static_cast<OutputPacket *>(packet);
	outputPackets_.emplace_back(std::move(*outputPacket));
	delete outputPacket;
}

void MasterConn::createAttachedPacket(MessageBuffer serializedPacket) {
	outputPackets_.emplace_back(std::move(serializedPacket));
}

void MasterConn::createAttachedPriorityPacket(MessageBuffer serializedPacket) {
	if (outputPackets_.empty()) {
		outputPackets_.emplace_back(std::move(serializedPacket));
		return;
	}

	// The head packet may already be partially written to the socket, so the
	// earliest safe position is right after it.
	outputPackets_.emplace(std::next(outputPackets_.begin()), std::move(serializedPacket));
}

// Configuration

void MasterConn::reloadConfig() {
	auto newMasterHostStr_ = cfg_getstring("MASTER_HOST", "sfsmaster");
	auto newMasterPortStr_ = cfg_getstring("MASTER_PORT", "9420");

	auto newClusterId = cfg_getstring("CLUSTER_ID", "default");

	if (newClusterId != clusterId_) {
		safs::log_warn(
		    "MasterConn: Non-reloadable CLUSTER_ID changed from {} to {} during reload. Using the original value until restart.",
		    clusterId_, newClusterId);
	}

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
		safs::log_warn("MasterConn: can't resolve master host/port ({}:{})",
		               masterHostStr_, masterPortStr_);
	}
}

// Connection management

void MasterConn::sendRegisterLabel() {
	if (mode_ == ConnectionMode::CONNECTED) {
		createAttachedPacket(cstoma::registerLabel::build(gLabel));
	}
}

void MasterConn::sendConfig() {
	if (mode_ == ConnectionMode::CONNECTED) {
		createAttachedPacket(cstoma::registerConfig::build(cfg_yaml_string()));
	}
}

void MasterConn::sendRegister() {
	assert(registrationStatus_ == RegistrationStatus::kUnregistered);

	uint32_t myip = mainNetworkThreadGetListenIp();
	uint16_t myport = mainNetworkThreadGetListenPort();

	if (isVersionLessThan5_) {  // Preserve compatibility with masters < 5.0
		createAttachedPacket(
		    cstoma::registerHost::build(myip, myport, gTimeout_ms, SAUNAFS_VERSHEX));

		safs::log_warn("MasterConn: using old master registration to {}", address_.toString());

		registrationStatus_ = RegistrationStatus::kHostRegistered;

		onRegistered({});
	} else {
		createAttachedPacket(
		    cstoma::registerHost::build(myip, myport, gTimeout_ms, SAUNAFS_VERSHEX, clusterId_));

		safs::log_info("MasterConn: registering to MDS: {}", address_.toString());

		registrationStatus_ = RegistrationStatus::kRegistrationRequested;
	}
}

void MasterConn::onRegistered(const std::vector<uint8_t> &data) {
	if (isVersionLessThan5_) {
		(void)data;  // Not used
		version_ = saunafsVersion(0, 0, 0);  // The version is unknown at this point
	} else {
		uint8_t status{};
		uint32_t version{};
		std::string clusterId;
		matocs::registerHost::deserialize(data, status, version, clusterId);

		if (status != SAUNAFS_STATUS_OK) {
			safs::log_err(
			    "MasterConn: registration to {} failed with status: {}, version: {}, clusterId: {}",
			    address_.toString(), saunafs_error_string(status), saunafsVersionToString(version),
			    clusterId);
			setMode(ConnectionMode::KILL);
			return;
		}

		version_ = version;

		safs::log_info("MasterConn: registered to MDS: {}, version: {}, clusterId: {}",
		               address_.toString(), saunafsVersionToString(version), clusterId);

		registrationStatus_ = RegistrationStatus::kHostRegistered;
	}

	// Reset registration parameters for future reconnections to use the new protocol first
	isVersionLessThan5_ = false;
	registrationAttempts_ = 0;

	if (!gForcePushRegistration && version_ >= kFirstVersionWithPullChunkRegistration) {
		// Master-driven (pull) registration: send the registration tail right
		// away (this chunkserver becomes placeable for new chunks) and wait
		// for the master to release the chunk stream with
		// SAU_MATOCS_REGISTER_CHUNKS_START.
		sendRegistrationTail();
		registrationStatus_ = RegistrationStatus::kAwaitingPullStart;
		pullStartTimeout_ = Timeout(std::chrono::seconds(gPullRegistrationStartTimeout_s));
		return;
	}

	pushRegisterChunks();
}

void MasterConn::pushRegisterChunks() {
	// No sweep runs on this path, but the epoch must still advance. It is read
	// on every drain of the new-chunk queue, whichever way this connection
	// registered, and marks left by a pull session earlier in this process
	// would otherwise read as "already reported to this master" and suppress
	// announcements it was never told about.
	hddRegistrationSweepBegin();

	hddForeachChunkInBulks(
	    [this](const std::vector<ChunkWithVersionAndType> &chunksBulk) {
		    createAttachedPacket(cstoma::registerChunks::build(chunksBulk));
	    },
	    gChunkBulkSize.load(std::memory_order_relaxed));

	sendRegistrationTail();

	registrationStatus_ = RegistrationStatus::kChunksRegistered;
}

void MasterConn::sendRegistrationTail() {
	uint64_t usedSpace;
	uint64_t totalSpace;
	uint64_t toDelUsedSpace;
	uint64_t toDelTotalSpace;
	uint32_t chunkCount;
	uint32_t toDelChunkCount;

	hddGetTotalSpace(&usedSpace, &totalSpace, &chunkCount, &toDelUsedSpace, &toDelTotalSpace,
	                 &toDelChunkCount);
	auto registerSpace = cstoma::registerSpace::build(
	    usedSpace, totalSpace, chunkCount, toDelUsedSpace, toDelTotalSpace, toDelChunkCount);
	createAttachedPacket(std::move(registerSpace));

	sendRegisterLabel();

	sendConfig();
}

void MasterConn::onRegisterChunksStart(const std::vector<uint8_t> &data) {
	uint32_t bulkSize{};
	uint32_t initialCredits{};
	matocs::registerChunksStart::deserialize(data, bulkSize, initialCredits);

	if (registrationStatus_ != RegistrationStatus::kAwaitingPullStart) {
		safs::log_warn(
		    "MasterConn: unexpected REGISTER_CHUNKS_START in registration status {}, ignoring",
		    static_cast<int>(registrationStatus_));
		return;
	}

	pullBulkSize_ = std::clamp(bulkSize, kMinChunkBulkSize, kMaxChunkBulkSize);
	pullCredits_ = initialCredits;
	pullChunksSent_ = 0;
	registrationStatus_ = RegistrationStatus::kChunksRegistering;

	safs::log_info(
	    "MasterConn: master-driven chunk registration started (bulk size {}, initial credits {})",
	    pullBulkSize_, pullCredits_);

	hddRegistrationSweepBegin();
	pumpPullRegistration();
}

void MasterConn::onRegisterChunksCredit(const std::vector<uint8_t> &data) {
	uint32_t credits{};
	matocs::registerChunksCredit::deserialize(data, credits);

	if (registrationStatus_ != RegistrationStatus::kChunksRegistering) {
		// Late credit, e.g. after the END packet was already sent
		return;
	}

	pullCredits_ += credits;
	pumpPullRegistration();
}

void MasterConn::pumpPullRegistration() {
	std::vector<ChunkWithVersionAndType> bulk;

	while (pullCredits_ > 0) {
		if (!hddRegistrationSweepNext(bulk, pullBulkSize_)) {
			createAttachedPacket(cstoma::registerChunksEnd::build(pullChunksSent_));
			registrationStatus_ = RegistrationStatus::kChunksRegistered;
			safs::log_info("MasterConn: master-driven chunk registration complete ({} chunks)",
			               pullChunksSent_);
			return;
		}

		pullChunksSent_ += bulk.size();
		--pullCredits_;
		createAttachedPacket(cstoma::registerChunks::build(bulk));
	}
}

void MasterConn::checkPullRegistrationStartTimeout() {
	if (registrationStatus_ != RegistrationStatus::kAwaitingPullStart ||
	    !pullStartTimeout_.expired()) {
		return;
	}

	safs::log_warn(
	    "MasterConn: master did not start chunk registration within {}s, "
	    "falling back to push registration",
	    gPullRegistrationStartTimeout_s);
	pushRegisterChunks();
}

void MasterConn::handleRegistrationAttempt() {
	if (registrationAttempts_ < kMaxRegistrationAttemptsToBeConsideredOldMaster) {
		if (registrationStatus_ == RegistrationStatus::kRegistrationRequested) {
			safs::log_warn(
			    "MasterConn: Master server did not answer the registration request; make sure Master is at least version {} (attempt {}/{})",
			    saunafsVersionToString(kFirstVersionWithClusterId), registrationAttempts_ + 1,
			    kMaxRegistrationAttemptsToBeConsideredOldMaster);
			++registrationAttempts_;
		}

		// Reconnection will be retried as usual
	} else {
		safs::log_warn("MasterConn: reached max registration attempts, considering old master");
		// Assume the master is not answering because it is an old version
		// The old protocol will be tried on the next reconnection
		isVersionLessThan5_ = true;
	}
}

int MasterConn::initConnect() {
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
			safs::log_warn("MasterConn: can't resolve master host/port ({}:{})",
			               masterHostStr_, masterPortStr_);
			return -1;
		}
	}

	socketFD_ = tcpsocket();

	if (socketFD_ < 0) {
		safs::log_error_code(errno, "MasterConn: create socket error");
		return -1;
	}

	if (tcpnonblock(socketFD_) < 0) {
		safs::log_error_code(errno, "MasterConn: set nonblock error");
		tcpclose(socketFD_);
		socketFD_ = -1;
		return -1;
	}

	if (bindHostAddress_.ip > 0) {
		if (tcpnumbind(socketFD_, bindHostAddress_.ip, 0) < 0) {
			safs::log_error_code(errno, "MasterConn: can't bind socket to given ip");
			tcpclose(socketFD_);
			socketFD_ = -1;
			return -1;
		}
	}

	int status = tcpnumconnect(socketFD_, address_.ip, address_.port);

	if (status < 0) {
		safs::log_error_code(errno, "MasterConn: connect failed");
		tcpclose(socketFD_);
		socketFD_ = -1;
		isMasterAddressValid_ = false;
		return -1;
	}

	if (status == 0) {
		safs::log_info("MasterConn: connected to Master immediately: {}", address_.toString());
		onConnected();
	} else {
		mode_ = ConnectionMode::CONNECTING;
		safs::log_info("MasterConn: connecting to Master: {}", address_.toString());
	}

	return 0;
}

void MasterConn::connectTest() {
	int status = tcpgetstatus(socketFD_);

	if (status) {
		safs::log_error_code(errno, "MasterConn: connection failed");
		tcpclose(socketFD_);
		socketFD_ = -1;
		mode_ = ConnectionMode::FREE;
		isMasterAddressValid_ = false;
	} else {
		safs::log_info("MasterConn: connected to Master: {}", address_.toString());
		onConnected();
	}
}

void MasterConn::tlsHandshake() {
	sassert(mode_ == ConnectionMode::HANDSHAKE);

	int ret = SSL_connect(tlsSession_->session());

	if (ret == 1) {
		safs::log_info("TLS handshake completed with master from {}:{}", ipToString(address_.ip),
		               address_.port);
		lastRead_.reset();
		setMode(ConnectionMode::CONNECTED);
		sendRegister();
		return;
	}

	int err = SSL_get_error(tlsSession_->session(), ret);
	lastHandshakeError_ = err;

	if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
		safs::log_info("TLS handshake in progress with master from {}:{}: {}",
		               ipToString(address_.ip), address_.port, opensslErrorString(err));
		return;  // retry later
	}

	setMode(ConnectionMode::KILL);
	safs::log_err("TLS handshake failed: {}", opensslErrorString(err));
}

void MasterConn::onConnected() {
	assert(mode_ == ConnectionMode::CONNECTING);
	tcpnodelay(socketFD_);
	inputPacket_.reset();
	lastRead_.reset();
	lastWrite_.reset();

	tlsKeyFile_ = cfg_getstring("TLS_KEY_FILE", std::string(TlsSession::kNoFile));
	tlsCertFile_ = cfg_getstring("TLS_CERT_FILE", std::string(TlsSession::kNoFile));
	tlsCaCertFile_ = cfg_getstring("TLS_CA_CERT_FILE", std::string(TlsSession::kNoFile));

	if (isTlsEnabled()) {
		try {
			// Initialize a TLS session for the peer.
			tlsSession_ = std::make_unique<TlsSession>(socketFD_, false, tlsKeyFile_, tlsCertFile_,
			                                           tlsCaCertFile_, masterHostStr_);
			safs::log_info("initiating TLS handshake with SFS master");

			auto startTlsRequest = cstoma::startTls::build();
			ssize_t ret = ::write(socketFD_, startTlsRequest.data(), startTlsRequest.size());
			if (ret < 0) {
				safs::log_error_code(errno, "cannot transmit startTls request to SFS master");
				setMode(ConnectionMode::KILL);
				return;
			} else if (ret != static_cast<int>(startTlsRequest.size())) {
				safs::log_err(
				    "cannot transmit startTls request to SFS master: send(len={} ) returned {}",
				    startTlsRequest.size(), ret);
				setMode(ConnectionMode::KILL);
				return;
			}

			// Proceed to the handshake.
			setMode(ConnectionMode::HANDSHAKE);
			tlsHandshake();
		} catch (const Exception &ex) {
			safs::log_err("MasterConn: TLS handshake setup failed: {}", ex.what());
			setMode(ConnectionMode::KILL);
			return;
		}
	} else {
		setMode(ConnectionMode::CONNECTED);
		sendRegister();
	}
}

// Polling

void MasterConn::providePollDescriptors(std::vector<pollfd> &pdesc, bool doTerminate) {
	pDescPos_ = -1;

	if (mode_ == ConnectionMode::FREE || socketFD_ < 0) { return; }

	if (mode_ == ConnectionMode::CONNECTED) {
		if (!doTerminate && (jobPool_->getJobCount() < kMaxBackgroundJobsThreshold ||
		    replicationJobPool_->getJobCount() < kMaxBackgroundJobsThreshold)) {
			pdesc.emplace_back(socketFD_, POLLIN, 0);
			pDescPos_ = static_cast<int32_t>(pdesc.size() - 1);
		}
	}

	if (mode_ == ConnectionMode::HANDSHAKE) {
		// Let's proceed with the handshake even if doTerminate is true, to avoid leaving a
		// half-open connection. The handshake will be attempted to be completed, but if it fails,
		// the connection will be closed and the thread will be able to terminate.
		short event = 0;
		switch (lastHandshakeError_) {
		case SSL_ERROR_WANT_READ:
			event = POLLIN;
			break;
		case SSL_ERROR_WANT_WRITE:
			event = POLLOUT;
			break;
		default:
			event = POLLIN | POLLOUT;
			break;
		}
		pdesc.emplace_back(socketFD_, event, 0);
		pDescPos_ = static_cast<int32_t>(pdesc.size() - 1);
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

void MasterConn::handlePollErrors(const std::vector<pollfd> &pdesc) {
	// Check if the socket has been closed or has an error.
	if (pDescPos_ >= 0 && (pdesc[pDescPos_].revents & (POLLHUP | POLLERR))) {
		if (mode_ == ConnectionMode::CONNECTING) {
			connectTest();
		} else {
			setMode(ConnectionMode::KILL);
		}
	}
}

void MasterConn::servePoll(const std::vector<pollfd> &pdesc) {
	if (mode_ == ConnectionMode::CONNECTING) {
		// Check if the connection has been established.
		if (socketFD_ >= 0 && pDescPos_ >= 0 && (pdesc[pDescPos_].revents & POLLOUT)) {
			connectTest();
		}
	} else {
		if (pDescPos_ >= 0) {
			// Check if there is a TLS handshake in progress
			if (mode_ == ConnectionMode::HANDSHAKE &&
			    (pdesc[pDescPos_].revents & (POLLIN | POLLOUT))) {
				lastRead_.reset();
				lastWrite_.reset();
				tlsHandshake();
			}

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
				setMode(ConnectionMode::KILL);
			}

			// Keep the connection alive by sending a NOP packet
			if ((mode_ == ConnectionMode::CONNECTED) &&
			    lastWrite_.elapsed_ms() > (gTimeout_ms / 3) && outputPackets_.empty()) {
				createAttachedNoVersionPacket(ANTOAN_NOP, 0);
			}
		}
	}
}

void MasterConn::readFromSocket() {
	ActiveLoopWatchdog watchdog(std::chrono::milliseconds(20));

	watchdog.start();

	while (mode_ != ConnectionMode::KILL) {
		// If any job pool is too busy, do not read more data.
		if (jobPool_->getJobCount() >= kMaxBackgroundJobsThreshold ||
		    replicationJobPool_->getJobCount() >= kMaxBackgroundJobsThreshold) {
			return;
		}

		uint32_t bytesToRead = inputPacket_.bytesToBeRead();

		ssize_t ret = -1;
		if (tlsSession_) {
			ret = ::SSL_read(tlsSession_->session(), inputPacket_.pointerToBeReadInto(),
			                 static_cast<int>(bytesToRead));

			if (ret <= 0) {
				int err = ::SSL_get_error(tlsSession_->session(), static_cast<int>(ret));
				if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
					// Handshake/record layer needs more I/O; poll will drive it.
					return;
				}

				if (ret == 0) {
					safs::log_info("MasterConn(TLS): connection reset by Master: {}",
					               address_.toString());
					handleRegistrationAttempt();
					setMode(ConnectionMode::KILL);
					return;
				}

				safs::log_err("MasterConn(TLS): read error from {}: {}", address_.toString(),
				              opensslErrorString(err));
				setMode(ConnectionMode::KILL);
				return;
			}
		} else {
			ret = ::read(socketFD_, inputPacket_.pointerToBeReadInto(), bytesToRead);

			if (ret == 0) {
				safs::log_info("MasterConn: connection reset by Master: {}", address_.toString());
				handleRegistrationAttempt();
				setMode(ConnectionMode::KILL);
				return;
			}

			if (ret < 0) {
				if (errno != EAGAIN) {
					safs::log_error_code(errno, "MasterConn: read error from {}",
					                     address_.toString());
					setMode(ConnectionMode::KILL);
				}
				return;
			}
		}

		bytesIn_ += ret;

		try {
			inputPacket_.increaseBytesRead(ret);
		} catch (InputPacketTooLongException &ex) {
			safs::log_warn("MasterConn: reading from master: {}", ex.what());
			setMode(ConnectionMode::KILL);
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

void MasterConn::writeToSocket() {
	ActiveLoopWatchdog watchdog(std::chrono::milliseconds(20));
	ssize_t bytesWritten{-1};

	watchdog.start();

	while (!outputPackets_.empty()) {
		OutputPacket &pack = outputPackets_.front();

		if (mode_ == ConnectionMode::CONNECTED && tlsSession_) {
			bytesWritten = ::SSL_write(tlsSession_->session(), pack.packet.data() + pack.bytesSent,
			                           static_cast<int>(pack.packet.size() - pack.bytesSent));
			if (bytesWritten <= 0) {
				int err = ::SSL_get_error(tlsSession_->session(), static_cast<int>(bytesWritten));
				if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
					// Need more I/O; return and let poll drive readiness.
					return;
				}
				safs::log_err("MasterConn(TLS): write error to {}: {}", address_.toString(),
				              opensslErrorString(err));
				setMode(ConnectionMode::KILL);
				return;
			}
		} else {
			bytesWritten = ::write(socketFD_, pack.packet.data() + pack.bytesSent,
			                       pack.packet.size() - pack.bytesSent);

			if (bytesWritten < 0) {
				if (errno != EAGAIN) {
					safs::log_error_code(errno, "MasterConn: write to Master error: {}",
					                     address_.toString());
					setMode(ConnectionMode::KILL);
				}
				return;
			}
		}

		bytesOut_ += bytesWritten;
		pack.bytesSent += bytesWritten;

		if (pack.packet.size() != pack.bytesSent) { return; }

		outputPackets_.pop_front();

		if (watchdog.expired()) { break; }
	}
}

void MasterConn::gotPacket(PacketHeader header, const MessageBuffer &message) try {
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
	case SAU_MATOCS_CREATE_AND_LOCK_CHUNK:
		createAndLockChunk(message);
		break;
	case SAU_MATOCS_DELETE_CHUNK:
		deleteChunk(message);
		break;
	case SAU_MATOCS_SET_VERSION:
		setChunkVersion(message);
		break;
	case SAU_MATOCS_SET_VERSION_AND_LOCK:
		setChunkVersionAndLock(message);
		break;
	case SAU_MATOCS_LOCK_CHUNK:
		lockChunk(message);
		break;
	case SAU_MATOCS_UNLOCK_CHUNK:
		unlockChunk(message);
		break;
	case SAU_MATOCS_DUPLICATE_CHUNK:
		duplicateChunk(message);
		break;
	case SAU_MATOCS_DUPLICATE_AND_LOCK_CHUNK:
		duplicateAndLockChunk(message);
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
	case SAU_MATOCS_REGISTER_HOST:
		onRegistered(message);
		break;
	case SAU_MATOCS_QUERY_CHUNKS:
		queryChunks(message);
		break;
	case SAU_MATOCS_REGISTER_CHUNKS_START:
		onRegisterChunksStart(message);
		break;
	case SAU_MATOCS_REGISTER_CHUNKS_CREDIT:
		onRegisterChunksCredit(message);
		break;
	default:
		safs::log_info("MasterConn: got unknown message (type: {}): {}", header.type,
		               address_.toString());
		setMode(ConnectionMode::KILL);
	}
} catch (IncorrectDeserializationException &e) {
	safs::log_info("MasterConn: got inconsistent message (type:{}, length:{}), {}, {}", header.type,
	               uint32_t(message.size()), e.what(), address_.toString());
	setMode(ConnectionMode::KILL);
}

// Chunk operations

void MasterConn::queryChunks(const std::vector<uint8_t> &data) {
	std::vector<uint64_t> chunkIds;
	matocs::queryChunks::deserialize(data, chunkIds);

	std::vector<ChunkWithVersionAndType> foundChunks;
	{
		std::lock_guard chunksMapLockGuard(gChunksMapMutex);
		for (const auto chunkId : chunkIds) {
			for (const auto &[chunkType, count] : gPresentChunkTypes) {
				auto chunkIter = gChunksMap.find(makeChunkKey(chunkId, chunkType));
				if (chunkIter == gChunksMap.end()) { continue; }

				const IChunk *chunk = chunkIter->second.get();
				if (chunk->state() == ChunkState::Deleted ||
				    chunk->state() == ChunkState::ToBeDeleted) {
					continue;
				}

				const bool markedForDeletion =
				    chunk->owner() != nullptr && chunk->owner()->isMarkedForDeletion();
				foundChunks.emplace_back(
				    chunkId,
				    common::combineVersionWithTodelFlag(chunk->version(), markedForDeletion),
				    chunkType);
			}
		}
	}

	// During a pull-registration sweep the answered chunks are already known
	// to the master, so the sweep does not need to repeat them.
	if (registrationStatus_ == RegistrationStatus::kChunksRegistering) {
		for (const auto &foundChunk : foundChunks) {
			hddRegistrationSweepMarkRegistered(foundChunk.id, foundChunk.type);
		}
	}

	// The master holds client operations back until this answer arrives, so
	// it must jump any registration backlog in the output queue.
	createAttachedPriorityPacket(cstoma::queryChunksResponse::build(chunkIds, foundChunks));
}

void MasterConn::createChunk(const std::vector<uint8_t> &data) {
	uint64_t chunkId;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	uint32_t chunkVersion;

	matocs::createChunk::deserialize(data, chunkId, chunkType, chunkVersion);
	auto *outputPacket = new OutputPacket;
	cstoma::createChunk::serialize(outputPacket->packet, chunkId, chunkType, SAUNAFS_STATUS_OK);
	if (jobPool_) {
		job_create(*jobPool_, sauJobFinished(this), outputPacket, chunkId, chunkVersion, chunkType);
	} else {
		safs::log_err("MasterConn::createChunk: jobPool is null.");
		delete outputPacket;
	}
}

void MasterConn::createAndLockChunk(const std::vector<uint8_t> &data) {
	uint64_t chunkId = 0;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	uint32_t chunkVersion = 0;

	matocs::createAndLockChunk::deserialize(data, chunkId, chunkType, chunkVersion);
	auto *outputPacket = new OutputPacket;
	cstoma::createChunk::serialize(outputPacket->packet, chunkId, chunkType, SAUNAFS_STATUS_OK);
	if (jobPool_) {
		job_create(*jobPool_, sauJobFinishedAndLock(this, chunkId, chunkType), outputPacket,
		           chunkId, chunkVersion, chunkType);
	} else {
		safs::log_err("MasterConn::{}: jobPool is null.", __func__);
		delete outputPacket;
	}
}

void MasterConn::deleteChunk(const std::vector<uint8_t> &data) {
	uint64_t chunkId;
	uint32_t chunkVersion;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();

	matocs::deleteChunk::deserialize(data, chunkId, chunkType, chunkVersion);
	auto *outputPacket = new OutputPacket;
	cstoma::deleteChunk::serialize(outputPacket->packet, chunkId, chunkType, 0);
	if (jobPool_) {
		job_delete(*jobPool_, sauJobFinished(this), outputPacket, chunkId, chunkVersion, chunkType);
	} else {
		safs::log_err("MasterConn::deleteChunk: jobPool is null.");
		delete outputPacket;
	}
}

void MasterConn::setChunkVersion(const std::vector<uint8_t> &data) {
	uint64_t chunkId;
	uint32_t chunkVersion;
	uint32_t newVersion;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();

	matocs::setVersion::deserialize(data, chunkId, chunkType, chunkVersion, newVersion);
	auto *outputPacket = new OutputPacket;
	cstoma::setVersion::serialize(outputPacket->packet, chunkId, chunkType, 0);
	if (jobPool_) {
		job_version(*jobPool_, sauJobFinished(this), outputPacket, chunkId, chunkVersion, chunkType,
		            newVersion);
	} else {
		safs::log_err("MasterConn::setChunkVersion: jobPool is null.");
		delete outputPacket;
	}
}

void MasterConn::setChunkVersionAndLock(const std::vector<uint8_t> &data) {
	uint64_t chunkId = 0;
	uint32_t chunkVersion = 0;
	uint32_t newVersion = 0;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();

	matocs::setVersionAndLock::deserialize(data, chunkId, chunkType, chunkVersion, newVersion);
	auto *outputPacket = new OutputPacket;
	cstoma::setVersion::serialize(outputPacket->packet, chunkId, chunkType, 0);
	if (jobPool_) {
		job_version(*jobPool_, sauJobFinishedAndLock(this, chunkId, chunkType), outputPacket,
		            chunkId, chunkVersion, chunkType, newVersion);
	} else {
		safs::log_err("MasterConn::{}: jobPool is null.", __func__);
		delete outputPacket;
	}
}

void MasterConn::lockChunk(const std::vector<uint8_t> &data) {
	uint64_t chunkId = 0;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();

	matocs::chunkLock::deserialize(data, chunkId, chunkType);

	auto *chunkLockOutputPacket = new OutputPacket;
	auto *writeEndStatusOutputPacket = new OutputPacket;
	cstoma::chunkLock::serialize(chunkLockOutputPacket->packet, chunkId, chunkType, 0);
	cstoma::writeEndStatus::serialize(writeEndStatusOutputPacket->packet, chunkId, chunkType, 0);
	if (jobPool_) {
		bool createdNewLockJob = jobPool_->startChunkLock(
		    sauJobFinished(this), writeEndStatusOutputPacket, chunkId, chunkType);
		if (!createdNewLockJob) {
			// A lock job for this chunk and type is already in progress, so we can free the output
			// packet recently allocated for the job callback, as it won't be used.
			delete writeEndStatusOutputPacket;
		}

		sauJobFinished(SAUNAFS_STATUS_OK, chunkLockOutputPacket);
	} else {
		safs::log_err("MasterConn::{}: jobPool is null.", __func__);
		sauJobFinished(SAUNAFS_ERROR_NOTDONE, chunkLockOutputPacket);
		delete writeEndStatusOutputPacket;
	}
}

void MasterConn::unlockChunk(const std::vector<uint8_t> &data) {
	uint64_t chunkId = 0;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();

	matocs::chunkUnlock::deserialize(data, chunkId, chunkType);
	if (jobPool_) {
		jobPool_->eraseChunkLock(chunkId, chunkType);
	} else {
		safs::log_err("MasterConn::{}: jobPool is null.", __func__);
	}
}

void MasterConn::duplicateChunk(const std::vector<uint8_t> &data) {
	uint64_t newChunkId, oldChunkId;
	uint32_t newChunkVersion, oldChunkVersion;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();

	matocs::duplicateChunk::deserialize(data, newChunkId, newChunkVersion, chunkType, oldChunkId,
	                                    oldChunkVersion);
	auto *outputPacket = new OutputPacket;
	cstoma::duplicateChunk::serialize(outputPacket->packet, newChunkId, chunkType, 0);
	if (jobPool_) {
		job_duplicate(*jobPool_, sauJobFinished(this), outputPacket, oldChunkId, oldChunkVersion,
		              oldChunkVersion, chunkType, newChunkId, newChunkVersion);
	} else {
		safs::log_err("MasterConn::duplicateChunk: jobPool is null.");
		delete outputPacket;
	}
}

void MasterConn::duplicateAndLockChunk(const std::vector<uint8_t> &data) {
	uint64_t newChunkId, oldChunkId;
	uint32_t newChunkVersion, oldChunkVersion;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();

	matocs::duplicateAndLockChunk::deserialize(data, newChunkId, newChunkVersion, chunkType,
	                                           oldChunkId, oldChunkVersion);
	auto *outputPacket = new OutputPacket;
	cstoma::duplicateChunk::serialize(outputPacket->packet, newChunkId, chunkType, 0);
	if (jobPool_) {
		job_duplicate(*jobPool_, sauJobFinishedAndLock(this, newChunkId, chunkType), outputPacket,
		              oldChunkId, oldChunkVersion, oldChunkVersion, chunkType, newChunkId,
		              newChunkVersion);
	} else {
		safs::log_err("MasterConn::{}: jobPool is null.", __func__);
		delete outputPacket;
	}
}

void MasterConn::truncateChunk(const std::vector<uint8_t> &data) {
	uint64_t chunkId;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	uint32_t version;
	uint32_t chunkLength;
	uint32_t newVersion;

	matocs::truncateChunk::deserialize(data, chunkId, chunkType, chunkLength, newVersion, version);
	auto *outputPacket = new OutputPacket;
	cstoma::truncate::serialize(outputPacket->packet, chunkId, chunkType, 0);
	if (jobPool_) {
		job_truncate(*jobPool_, sauJobFinished(this), outputPacket, chunkId, chunkType, version,
		             newVersion, chunkLength);
	} else {
		safs::log_err("MasterConn::truncateChunk: jobPool is null.");
		delete outputPacket;
	}
}

void MasterConn::duplicateTruncateChunk(const std::vector<uint8_t> &data) {
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
		safs::log_err("MasterConn::duplicateTruncateChunk: jobPool is null.");
		delete outputPacket;
	}
}

void MasterConn::replicateChunk(const std::vector<uint8_t> &data) {
	uint64_t chunkId;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	uint32_t chunkVersion;
	uint32_t sourcesBufferSize;
	const uint8_t *sourcesBuffer;

	matocs::replicateChunk::deserializePartial(data, chunkId, chunkVersion, chunkType,
	                                           sourcesBuffer);
	sourcesBufferSize = data.size() - (sourcesBuffer - data.data());

	auto *outputPacket = new OutputPacket;
	cstoma::replicateChunk::serialize(outputPacket->packet, chunkId, chunkType, SAUNAFS_STATUS_OK,
	                                  chunkVersion);
	safs::log_debug("cs.matocs.replicate {}", chunkId);

	if (hddScansInProgress()) {
		// Disk scan in progress - replication is not possible
		sauJobFinished(SAUNAFS_ERROR_WAITING, outputPacket);
	} else {
		if (replicationJobPool_) {
			// If replication job pool is available, use it to handle the replication job
			job_replicate(*replicationJobPool_, sauJobFinished(this), outputPacket, chunkId,
			              chunkVersion, chunkType, sourcesBufferSize, sourcesBuffer);
		} else {
			safs::log_err("MasterConn::replicateChunk: replicationJobPool is null.");
			delete outputPacket;
		}
	}
}

// Callbacks

std::function<void(uint8_t status, void *packet)> MasterConn::sauJobFinishedAndLock(
    MasterConn *masterConn, uint64_t chunkId, ChunkPartType chunkType) {
	return [masterConn, chunkId, chunkType](uint8_t status, void *packet) {
		// The original job's output packet is sent as the response to the master's request
		masterConn->sauJobFinished(status, packet);

		if (status != SAUNAFS_STATUS_OK) {
			return;  // If the original job failed, do not prepare the chunk lock
		}

		// After the original job is finished, we need to prepare the chunk lock
		auto *writeEndStatusOutputPacket = new OutputPacket;
		cstoma::writeEndStatus::serialize(writeEndStatusOutputPacket->packet, chunkId, chunkType,
		                                  status);
		bool createdNewLockJob = masterConn->jobPool_->startChunkLock(
		    masterConn->sauJobFinished(masterConn), writeEndStatusOutputPacket, chunkId, chunkType);
		if (!createdNewLockJob) {
			// A lock job for this chunk and type is already in progress, so we can free the output
			// packet recently allocated for the job callback, as it won't be used.
			delete writeEndStatusOutputPacket;
		}
	};
}

std::function<void(uint8_t status, void *packet)> MasterConn::sauJobFinished(
    MasterConn *masterConn) {
	return
	    [masterConn](uint8_t status, void *packet) { masterConn->sauJobFinished(status, packet); };
}

void MasterConn::sauJobFinished(uint8_t status, void *packet) {
	auto *outputPacket = static_cast<OutputPacket *>(packet);

	if (mode_ == ConnectionMode::CONNECTED) {
		cstoma::overwriteStatusField(outputPacket->packet, status);
		attachPacket(packet);
	} else {
		deletePacket(packet);
	}
}

// Termination

void MasterConn::releaseResources() {
	if (mode_ != ConnectionMode::FREE && mode_ != ConnectionMode::CONNECTING) {
		if (tlsSession_ != nullptr) {
			int ret = SSL_shutdown(tlsSession_->session());
			if (ret < 0) {
				safs::log_warn("TLS shutdown failed: {}",
				               opensslErrorString(SSL_get_error(tlsSession_->session(), ret)));
			}
			tlsSession_.reset();
			safs::log_info("TLS session closed.");
		}

		if (socketFD_ >= 0) {
			tcpclose(socketFD_);
			socketFD_ = -1;
			inputPacket_.reset();
		}
	}
}

void MasterConn::resetPackets() {
	inputPacket_.reset();
	outputPackets_.clear();
}
