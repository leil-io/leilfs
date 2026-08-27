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
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "chunkserver-common/hdd_utils.h"
#include "chunkserver/bgjobs.h"
#include "chunkserver/hddspacemgr.h"
#include "chunkserver/masterconn.h"
#include "chunkserver/network_main_thread.h"
#include "common/input_packet.h"
#include "common/loop_watchdog.h"
#include "common/network_address.h"
#include "common/output_packet.h"
#include "common/saunafs_version.h"
#include "common/sockets.h"
#include <algorithm>
#include <iterator>
#include "common/event_loop.h"
#include "common/test_event_stream.h"
#include "common/test_packet_faults.h"
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

// Configuration

void MasterConn::reloadConfig() {
	// Cluster-view connections own their advertised endpoint; applying the seed's
	// reloadable address to every connection would collapse the dynamic set onto one MDS.
	if (!configuredSeed_) { return; }
	auto newMasterHostStr_ = distributedMode_ ? cfg_getstring("MDS_SEED_HOST", "")
	                                          : cfg_getstring("MASTER_HOST", "sfsmaster");
	auto newMasterPortStr_ = distributedMode_ ? cfg_getstring("MDS_SEED_PORT", "")
	                                          : cfg_getstring("MASTER_PORT", "9420");

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
		safs::log_warn("MasterConn: can't resolve master host/port ({}:{})", masterHostStr_,
		               masterPortStr_);
	}
}

// Connection management

void MasterConn::beginSessionRenewerNominationAttempt() {
	if (sessionRenewerNominationPending_) { sessionRenewerNominationAttempting_ = true; }
}

void MasterConn::completeSessionRenewerNomination() {
	sessionRenewerNominationPending_ = false;
	sessionRenewerNominationAttempting_ = false;
	sessionRenewerNominationDeadline_ = 0;
	// The answer, not the request, is what makes this connection the renewer. An observer
	// reply confirms nothing about the role and must leave the election free to run.
	sessionRenewerConfirmed_ = distributedRole_ == DistributedRegistrationRole::kClaimRenewer;
}

void MasterConn::abandonSessionRenewerNomination() {
	if (!sessionRenewerNominationPending_) { return; }
	safs::log_warn("MasterConn: renewer nomination of MDS {} failed; returning it to observer",
	               mdsId_);
	if (test_event_stream::enabled()) {
		test_event_stream::emit("renewer_attempt_failed", {{"mds_id", mdsId_}});
	}
	distributedRole_ = DistributedRegistrationRole::kObserver;
	sessionRenewerNominationPending_ = false;
	sessionRenewerNominationAttempting_ = false;
	sessionRenewerConfirmed_ = false;
	sessionRenewerNominationDeadline_ = 0;
}

bool MasterConn::expireSessionRenewerNomination(uint64_t now) {
	if (!sessionRenewerNominationPending_ || sessionRenewerNominationDeadline_ == 0 ||
	    now < sessionRenewerNominationDeadline_) {
		return false;
	}
	safs::log_warn("MasterConn: renewer nomination of MDS {} expired unanswered at {}", mdsId_,
	               sessionRenewerNominationDeadline_);
	if (test_event_stream::enabled()) {
		const uint64_t deadline = sessionRenewerNominationDeadline_;
		test_event_stream::emit("renewer_nomination_expired",
		                        {{"mds_id", mdsId_}, {"deadline", deadline}});
	}
	abandonSessionRenewerNomination();
	return true;
}

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
	if (distributedMode_) {
		const uint32_t stableId = masterconn_stable_id();
		const auto role = stableId == 0 ? DistributedRegistrationRole::kMintOnly : distributedRole_;
		const bool ready = !hddScansInProgress();
		const uint64_t scanEpoch = ready ? 1 : 0;
		createAttachedPacket(cstoma::registerDistributed::build(
		    myip, myport, gTimeout_ms, SAUNAFS_VERSHEX, clusterId_, stableId,
		    masterconn_incarnation(), static_cast<uint8_t>(ready), scanEpoch,
		    static_cast<uint8_t>(role)));
		safs::log_info("MasterConn: requesting distributed registration from {} as role {}",
		               address_.toString(), static_cast<uint8_t>(role));
		registrationStatus_ = RegistrationStatus::kRegistrationRequested;
		return;
	}

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

void MasterConn::requestRenewerUpgrade() {
	if (mode_ != ConnectionMode::CONNECTED ||
	    registrationStatus_ != RegistrationStatus::kChunksRegistered) {
		// No live admitted connection to upgrade in place; fall back to the
		// reconnect-based nomination path.
		setMode(ConnectionMode::KILL);
		return;
	}
	sessionRenewerNominationAttempting_ = true;
	const uint32_t myip = mainNetworkThreadGetListenIp();
	const uint16_t myport = mainNetworkThreadGetListenPort();
	const bool ready = !hddScansInProgress();
	const uint64_t scanEpoch = ready ? 1 : 0;
	createAttachedPacket(cstoma::registerDistributed::build(
	    myip, myport, gTimeout_ms, SAUNAFS_VERSHEX, clusterId_, masterconn_stable_id(),
	    masterconn_incarnation(), static_cast<uint8_t>(ready), scanEpoch,
	    static_cast<uint8_t>(DistributedRegistrationRole::kClaimRenewer)));
	safs::log_info("MasterConn: requesting in-place renewer upgrade from MDS {}", mdsId_);
}

void MasterConn::onRegistered(const std::vector<uint8_t> &data) {
	if (isVersionLessThan5_) {
		(void)data;                          // Not used
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
	finishRegistration(true);
}

void MasterConn::onDistributedRegistered(const std::vector<uint8_t> &data) {
	uint8_t status = SAUNAFS_ERROR_UNKNOWN;
	uint32_t stableId = 0;
	uint32_t mdsId = 0;
	uint64_t mdsIncarnation = 0;
	uint32_t version = 0;
	std::string clusterId;
	uint64_t claimSequence = 0;
	uint64_t leaseDeadline = 0;
	uint64_t cutoffReserveSeconds = 0;
	matocs::registerDistributed::deserialize(data, status, stableId, mdsId, mdsIncarnation, version,
	                                         clusterId, claimSequence, leaseDeadline,
	                                         cutoffReserveSeconds);

	if (status != SAUNAFS_STATUS_OK) {
		safs::log_err("MasterConn: distributed registration to {} failed with status {}",
		              address_.toString(), saunafs_error_string(status));
		setMode(ConnectionMode::KILL);
		return;
	}
	if (clusterId != clusterId_ || stableId == 0 || mdsId == 0 || mdsIncarnation == 0) {
		safs::log_err("MasterConn: non-canonical distributed registration reply from {}",
		              address_.toString());
		setMode(ConnectionMode::KILL);
		return;
	}

	const uint32_t ownedStableId = masterconn_stable_id();
	if (ownedStableId == 0) {
		if (claimSequence != 0 || leaseDeadline != 0 || !masterconn_adopt_stable_id(stableId)) {
			safs::log_err("MasterConn: invalid or unpersistable mint-only reply from {}",
			              address_.toString());
			setMode(ConnectionMode::KILL);
			return;
		}
		// Minting never admits this connection. Reconnect and re-present the durably
		// stamped id before any inventory, report, client work or secondary connection.
		safs::log_info("MasterConn: persisted minted stable id {}; re-presenting it", stableId);
		setMode(ConnectionMode::KILL);
		return;
	}

	if (stableId != ownedStableId || claimSequence == 0 || leaseDeadline == 0) {
		safs::log_err("MasterConn: distributed admission tuple does not match this process");
		setMode(ConnectionMode::KILL);
		return;
	}
	if (distributedRole_ == DistributedRegistrationRole::kClaimRenewer) {
		// The renewer's reply is a lease-bearing tuple; only the acceptance model may
		// install it. An observer's reply stays a hint and never extends authority.
		const ChunkserverSessionLease incoming{stableId,      masterconn_incarnation(),
		                                       mdsId,         mdsIncarnation,
		                                       claimSequence, leaseDeadline};
		const auto verdict = masterconn_accept_session_lease(
		    incoming, cutoffReserveSeconds, mdsId, mdsIncarnation, "registration");
		if (!leaseTupleAccepted(verdict)) {
			safs::log_err("MasterConn: renewer admission tuple from {} rejected",
			              address_.toString());
			setMode(ConnectionMode::KILL);
			return;
		}
	}
	version_ = version;
	mdsId_ = mdsId;
	mdsIncarnation_ = mdsIncarnation;
	sessionClaimSequence_ = claimSequence;
	sessionLeaseDeadline_ = leaseDeadline;
	completeSessionRenewerNomination();
	safs::log_info(
	    "MasterConn: admitted by MDS {} incarnation {} under session sequence {} deadline {}",
	    mdsId_, mdsIncarnation_, sessionClaimSequence_, sessionLeaseDeadline_);
	if (registrationStatus_ == RegistrationStatus::kChunksRegistered) {
		return;  // In-place role upgrade on an admitted connection; nothing to resend.
	}
	registrationStatus_ = RegistrationStatus::kHostRegistered;
	finishRegistration(false);
}

void MasterConn::sessionLease(const std::vector<uint8_t> &data) {
	uint32_t stableId = 0;
	uint64_t chunkserverIncarnation = 0;
	uint32_t renewerMdsId = 0;
	uint64_t renewerMdsIncarnation = 0;
	uint64_t claimSequence = 0;
	uint64_t leaseDeadline = 0;
	uint64_t cutoffReserveSeconds = 0;
	matocs::chunkserverSessionLease::deserialize(data, stableId, chunkserverIncarnation,
	                                             renewerMdsId, renewerMdsIncarnation, claimSequence,
	                                             leaseDeadline, cutoffReserveSeconds);
	if (test_event_stream::enabled()) {
		test_event_stream::emit("lease_packet_received",
		                        {{"claim_sequence", claimSequence},
		                         {"claim_deadline", leaseDeadline},
		                         {"sender_mds_id", mdsId_}});
	}
	if (mdsId_ == 0 || mdsIncarnation_ == 0) {
		safs::log_warn("MasterConn: lease packet on a connection with no bound MDS identity");
		return;
	}
	const ChunkserverSessionLease incoming{stableId,      chunkserverIncarnation, renewerMdsId,
	                                       renewerMdsIncarnation, claimSequence, leaseDeadline};
	const auto verdict = masterconn_accept_session_lease(incoming, cutoffReserveSeconds, mdsId_,
	                                                     mdsIncarnation_, "lease");
	if (verdict == LeaseTupleAcceptance::kRejectWrongHolder ||
	    verdict == LeaseTupleAcceptance::kRejectMalformed) {
		// This MDS believes in a different process incarnation; re-arbitrate through
		// a fresh registration instead of serving under a claim that is not ours.
		setMode(ConnectionMode::KILL);
	}
}

void MasterConn::finishRegistration(bool sendInventory) {
	// Reset registration parameters for future reconnections to use the new protocol first
	isVersionLessThan5_ = false;
	registrationAttempts_ = 0;

	if (sendInventory) {
		hddForeachChunkInBulks(
		    [this](const std::vector<ChunkWithVersionAndType> &chunksBulk) {
			    createAttachedPacket(cstoma::registerChunks::build(chunksBulk));
		    },
		    gChunkBulkSize.load(std::memory_order_relaxed));
	}

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

	registrationStatus_ = RegistrationStatus::kChunksRegistered;

	sendRegisterLabel();

	sendConfig();
}

void MasterConn::handleRegistrationAttempt() {
	if (distributedMode_) {
		safs::log_warn(
		    "MasterConn: distributed registration to {} received no reply; legacy fallback is disabled",
		    address_.toString());
		return;
	}
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
	beginSessionRenewerNominationAttempt();
	auto connectFailed = [this]() {
		abandonSessionRenewerNomination();
		return -1;
	};

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
			safs::log_warn("MasterConn: can't resolve master host/port ({}:{})", masterHostStr_,
			               masterPortStr_);
			return connectFailed();
		}
	}

	socketFD_ = tcpsocket();

	if (socketFD_ < 0) {
		safs::log_error_code(errno, "MasterConn: create socket error");
		return connectFailed();
	}

	if (tcpnonblock(socketFD_) < 0) {
		safs::log_error_code(errno, "MasterConn: set nonblock error");
		tcpclose(socketFD_);
		socketFD_ = -1;
		return connectFailed();
	}

	if (bindHostAddress_.ip > 0) {
		if (tcpnumbind(socketFD_, bindHostAddress_.ip, 0) < 0) {
			safs::log_error_code(errno, "MasterConn: can't bind socket to given ip");
			tcpclose(socketFD_);
			socketFD_ = -1;
			return connectFailed();
		}
	}

	int status = tcpnumconnect(socketFD_, address_.ip, address_.port);

	if (status < 0) {
		safs::log_error_code(errno, "MasterConn: connect failed");
		tcpclose(socketFD_);
		socketFD_ = -1;
		isMasterAddressValid_ = false;
		return connectFailed();
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
		abandonSessionRenewerNomination();
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
	releaseDeferredPackets();

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

void MasterConn::gotPacket(PacketHeader header, const MessageBuffer &message) {
	if (test_packet_faults::enabled()) {
		uint32_t delayMilliseconds = 0;
		switch (test_packet_faults::decide("masterconn_recv", header.type, &delayMilliseconds)) {
		case test_packet_faults::Verdict::kDrop:
			return;
		case test_packet_faults::Verdict::kDelay:
			deferredPackets_.push_back(
			    {header, message, eventloop_utime() + delayMilliseconds * 1000ULL});
			return;
		case test_packet_faults::Verdict::kDuplicate:
			dispatchPacket(header, message);
			break;
		case test_packet_faults::Verdict::kPass:
			break;
		}
	}
	dispatchPacket(header, message);
}

void MasterConn::releaseDeferredPackets() {
	if (deferredPackets_.empty()) { return; }

	const uint64_t now = eventloop_utime();
	std::vector<DeferredPacket> due;
	auto stillHeld = std::stable_partition(
	    deferredPackets_.begin(), deferredPackets_.end(),
	    [&](const DeferredPacket &held) { return held.dueMicroseconds > now; });
	due.assign(std::make_move_iterator(stillHeld), std::make_move_iterator(deferredPackets_.end()));
	deferredPackets_.erase(stillHeld, deferredPackets_.end());

	for (const auto &held : due) { dispatchPacket(held.header, held.message); }
}

void MasterConn::dispatchPacket(PacketHeader header, const MessageBuffer &message) try {
	if (distributedMode_ && registrationStatus_ != RegistrationStatus::kChunksRegistered &&
	    header.type != SAU_MATOCS_REGISTER_DISTRIBUTED && header.type != ANTOAN_NOP &&
	    header.type != ANTOAN_UNKNOWN_COMMAND && header.type != ANTOAN_BAD_COMMAND_SIZE) {
		safs::log_err("MasterConn: distributed command arrived before session admission");
		setMode(ConnectionMode::KILL);
		return;
	}
	if (distributedMode_ && !masterconn_session_serving_allowed() &&
	    header.type != SAU_MATOCS_REGISTER_DISTRIBUTED &&
	    header.type != SAU_MATOCS_CS_SESSION_LEASE && header.type != SAU_MATOCS_CLUSTER_MEMBERS &&
	    header.type != ANTOAN_NOP && header.type != ANTOAN_UNKNOWN_COMMAND &&
	    header.type != ANTOAN_BAD_COMMAND_SIZE) {
		// Deny-by-default after the local session cutoff: a chunk command here means
		// the sender overran its own gate window. Drop the connection; readmission
		// re-arbitrates through a fresh registration or a newer accepted lease.
		safs::log_err("MasterConn: distributed command refused after session cutoff");
		setMode(ConnectionMode::KILL);
		return;
	}
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
	case SAU_MATOCS_FENCED_CREATE_CHUNK:
		fencedCreateChunk(message);
		break;
	case SAU_MATOCS_FENCED_DELETE_CHUNK:
		fencedDeleteChunk(message);
		break;
	case SAU_MATOCS_FENCED_SET_VERSION:
		fencedSetVersion(message);
		break;
	case SAU_MATOCS_FENCED_VERIFY_PART:
		fencedVerifyPart(message);
		break;
	case SAU_MATOCS_FENCED_LOCK_CHUNK:
		fencedLockChunk(message);
		break;
	case SAU_MATOCS_FENCED_UNLOCK_CHUNK:
		fencedUnlockChunk(message);
		break;
	case SAU_MATOCS_FENCED_DUPLICATE_CHUNK:
		fencedDuplicateChunk(message);
		break;
	case SAU_MATOCS_FENCED_TRUNCATE:
		fencedTruncateChunk(message);
		break;
	case SAU_MATOCS_FENCED_DUPTRUNC_CHUNK:
		fencedDuptruncChunk(message);
		break;
	case SAU_MATOCS_FENCED_REPLICATE_CHUNK:
		fencedReplicateChunk(message);
		break;
	case SAU_MATOCS_REGISTER_HOST:
		if (distributedMode_) {
			setMode(ConnectionMode::KILL);
		} else {
			onRegistered(message);
		}
		break;
	case SAU_MATOCS_REGISTER_DISTRIBUTED:
		if (!distributedMode_) {
			setMode(ConnectionMode::KILL);
		} else {
			onDistributedRegistered(message);
		}
		break;
	case SAU_MATOCS_CLUSTER_MEMBERS:
		if (!distributedMode_) {
			setMode(ConnectionMode::KILL);
		} else {
			clusterMembers(message);
		}
		break;
	case SAU_MATOCS_CS_SESSION_LEASE:
		if (!distributedMode_) {
			setMode(ConnectionMode::KILL);
		} else {
			sessionLease(message);
		}
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

void MasterConn::clusterMembers(const std::vector<uint8_t> &data) {
	uint32_t senderMdsId = 0;
	std::vector<MetadataserverClusterEntry> members;

	matocs::clusterMembers::deserialize(data, senderMdsId, members);
	masterconn_apply_cluster_view(this, senderMdsId, members);
}

// Chunk operations

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

		sauJobFinished(SAUNAFS_STATUS_OK, chunkLockOutputPacket, masterconn_serving_era());
	} else {
		safs::log_err("MasterConn::{}: jobPool is null.", __func__);
		sauJobFinished(SAUNAFS_ERROR_NOTDONE, chunkLockOutputPacket, masterconn_serving_era());
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
		sauJobFinished(SAUNAFS_ERROR_WAITING, outputPacket, masterconn_serving_era());
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

// Fenced chunk operations

void MasterConn::refuseFencedCommand(const ChunkCommandIdentity &identity, uint64_t chunkId,
                                     ChunkPartType chunkType, ChunkCommandFamily family,
                                     uint8_t status) {
	// A family that answers nothing has no row to retire, so answering it would invent a reply
	// the metadata server never asked for.
	if (!chunkCommandFamilyExpectsReply(family)) { return; }
	auto *outputPacket = new OutputPacket;
	cstoma::fencedStatus::serialize(outputPacket->packet, chunkId, chunkType, status, identity,
	                                static_cast<uint8_t>(family), 0);
	if (mode_ == ConnectionMode::CONNECTED) {
		attachPacket(outputPacket);
	} else {
		deletePacket(outputPacket);
	}
}

OutputPacket *MasterConn::acceptFencedCommand(const ChunkCommandIdentity &identity,
                                              uint64_t chunkId, ChunkPartType chunkType,
                                              ChunkCommandFamily family, uint32_t resultVersion) {
	const char *refusal = nullptr;
	if (identity.targetStableId != masterconn_stable_id()) {
		refusal = "wrong stable id";
	} else if (identity.targetIncarnation != masterconn_incarnation()) {
		// A stable id outlives the process that held it. Without this, a command addressed to the
		// chunkserver that died holding this id would be executed by the one that replaced it.
		refusal = "wrong incarnation";
	} else if (!masterconn_claim_admits_work(identity.targetServingEra)) {
		// Issued under a claim older than the one that readmitted this process, so it was issued
		// before the cutoff. An ordinary renewal raises the claim without ending the era, which is
		// why this compares against the era's starting claim and not against the current one.
		refusal = "stale claim";
	}
	if (refusal == nullptr) {
		auto *outputPacket = new OutputPacket;
		cstoma::fencedStatus::serialize(outputPacket->packet, chunkId, chunkType,
		                                SAUNAFS_STATUS_OK, identity,
		                                static_cast<uint8_t>(family), resultVersion);
		return outputPacket;
	}

	if (test_event_stream::enabled()) {
		test_event_stream::emit("fenced_command_refused",
		                        {{"family", chunkCommandFamilyName(family)},
		                         {"chunk", chunkId},
		                         {"sequence", identity.sequence},
		                         {"reason", refusal}});
	}
	// Answered rather than dropped. Refusing to execute and refusing to say so are separate
	// choices, and only the first is required here: an answer that names the command lets the
	// metadata server retire exactly that row now instead of waiting out its deadline.
	refuseFencedCommand(identity, chunkId, chunkType, family, SAUNAFS_ERROR_TEMP_NOTPOSSIBLE);
	return nullptr;
}

void MasterConn::fencedCreateChunk(const std::vector<uint8_t> &data) {
	ChunkCommandIdentity identity;
	uint64_t chunkId = 0;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	uint32_t chunkVersion = 0;
	bool needsLock = false;

	matocs::fencedCreateChunk::deserialize(data, identity, chunkId, chunkType, chunkVersion,
	                                       needsLock);
	auto *outputPacket =
	    acceptFencedCommand(identity, chunkId, chunkType, ChunkCommandFamily::kCreate, chunkVersion);
	if (outputPacket == nullptr) { return; }
	if (jobPool_ == nullptr) {
		safs::log_err("MasterConn::{}: jobPool is null.", __func__);
		sauJobFinished(SAUNAFS_ERROR_NOTDONE, outputPacket, masterconn_serving_era());
		return;
	}
	job_create(*jobPool_,
	           needsLock ? sauJobFinishedAndLock(this, chunkId, chunkType) : sauJobFinished(this),
	           outputPacket, chunkId, chunkVersion, chunkType);
}

void MasterConn::fencedDeleteChunk(const std::vector<uint8_t> &data) {
	ChunkCommandIdentity identity;
	uint64_t chunkId = 0;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	uint32_t chunkVersion = 0;

	matocs::fencedDeleteChunk::deserialize(data, identity, chunkId, chunkType, chunkVersion);
	auto *outputPacket =
	    acceptFencedCommand(identity, chunkId, chunkType, ChunkCommandFamily::kDelete, chunkVersion);
	if (outputPacket == nullptr) { return; }
	if (jobPool_ == nullptr) {
		safs::log_err("MasterConn::{}: jobPool is null.", __func__);
		sauJobFinished(SAUNAFS_ERROR_NOTDONE, outputPacket, masterconn_serving_era());
		return;
	}
	job_delete(*jobPool_, sauJobFinished(this), outputPacket, chunkId, chunkVersion, chunkType);
}

void MasterConn::fencedSetVersion(const std::vector<uint8_t> &data) {
	ChunkCommandIdentity identity;
	uint64_t chunkId = 0;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	uint32_t chunkVersion = 0;
	uint32_t newVersion = 0;
	bool needsLock = false;

	matocs::fencedSetVersion::deserialize(data, identity, chunkId, chunkType, chunkVersion,
	                                      newVersion, needsLock);
	auto *outputPacket = acceptFencedCommand(identity, chunkId, chunkType,
	                                         ChunkCommandFamily::kSetVersion, newVersion);
	if (outputPacket == nullptr) { return; }
	if (jobPool_ == nullptr) {
		safs::log_err("MasterConn::{}: jobPool is null.", __func__);
		sauJobFinished(SAUNAFS_ERROR_NOTDONE, outputPacket, masterconn_serving_era());
		return;
	}
	job_version(*jobPool_,
	            needsLock ? sauJobFinishedAndLock(this, chunkId, chunkType) : sauJobFinished(this),
	            outputPacket, chunkId, chunkVersion, chunkType, newVersion);
}

void MasterConn::fencedVerifyPart(const std::vector<uint8_t> &data) {
	ChunkCommandIdentity identity;
	uint64_t chunkId = 0;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();

	matocs::fencedVerifyPart::deserialize(data, identity, chunkId, chunkType);
	auto *outputPacket =
	    acceptFencedCommand(identity, chunkId, chunkType, ChunkCommandFamily::kVerifyPart, 0);
	if (outputPacket == nullptr) { return; }
	if (hddScansInProgress()) {
		// A verification asked while the disks are still being scanned cannot answer that a part
		// is gone: one this server holds may simply not be discovered yet. Refusing with a non
		// NOCHUNK status the master reads as no answer keeps a mid scan false absence from
		// removing a live member; the question is asked again after the scan.
		sauJobFinished(SAUNAFS_ERROR_WAITING, outputPacket, masterconn_serving_era());
		return;
	}
	if (jobPool_ == nullptr) {
		safs::log_err("MasterConn::{}: jobPool is null.", __func__);
		sauJobFinished(SAUNAFS_ERROR_NOTDONE, outputPacket, masterconn_serving_era());
		return;
	}
	job_verify_part(*jobPool_, sauJobFinished(this), outputPacket, chunkId, chunkType);
}

void MasterConn::fencedLockChunk(const std::vector<uint8_t> &data) {
	ChunkCommandIdentity identity;
	uint64_t chunkId = 0;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();

	matocs::fencedLockChunk::deserialize(data, identity, chunkId, chunkType);
	auto *outputPacket =
	    acceptFencedCommand(identity, chunkId, chunkType, ChunkCommandFamily::kChunkLock, 0);
	if (outputPacket == nullptr) { return; }
	if (jobPool_ == nullptr) {
		safs::log_err("MasterConn::{}: jobPool is null.", __func__);
		sauJobFinished(SAUNAFS_ERROR_NOTDONE, outputPacket, masterconn_serving_era());
		return;
	}

	// The write end status this lock will eventually answer with belongs to the client write
	// grant rather than to this command, so it keeps its own packet type.
	auto *writeEndStatusOutputPacket = new OutputPacket;
	cstoma::writeEndStatus::serialize(writeEndStatusOutputPacket->packet, chunkId, chunkType, 0);
	if (!jobPool_->startChunkLock(sauJobFinished(this), writeEndStatusOutputPacket, chunkId,
	                              chunkType)) {
		delete writeEndStatusOutputPacket;
	}
	sauJobFinished(SAUNAFS_STATUS_OK, outputPacket, masterconn_serving_era());
}

void MasterConn::fencedUnlockChunk(const std::vector<uint8_t> &data) {
	ChunkCommandIdentity identity;
	uint64_t chunkId = 0;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();

	matocs::fencedUnlockChunk::deserialize(data, identity, chunkId, chunkType);
	// Nothing answers an unlock, so a refusal is silent too: acceptFencedCommand returns nullptr
	// without sending, and the only thing that changes is that the lock stays.
	auto *outputPacket =
	    acceptFencedCommand(identity, chunkId, chunkType, ChunkCommandFamily::kChunkUnlock, 0);
	if (outputPacket == nullptr) { return; }
	delete outputPacket;
	if (jobPool_ == nullptr) {
		safs::log_err("MasterConn::{}: jobPool is null.", __func__);
		return;
	}
	jobPool_->eraseChunkLock(chunkId, chunkType);
}

void MasterConn::fencedDuplicateChunk(const std::vector<uint8_t> &data) {
	ChunkCommandIdentity identity;
	uint64_t newChunkId = 0;
	uint64_t oldChunkId = 0;
	uint32_t newChunkVersion = 0;
	uint32_t oldChunkVersion = 0;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	bool needsLock = false;

	matocs::fencedDuplicateChunk::deserialize(data, identity, newChunkId, newChunkVersion, chunkType,
	                                          oldChunkId, oldChunkVersion, needsLock);
	auto *outputPacket = acceptFencedCommand(identity, newChunkId, chunkType,
	                                         ChunkCommandFamily::kDuplicate, newChunkVersion);
	if (outputPacket == nullptr) { return; }
	if (jobPool_ == nullptr) {
		safs::log_err("MasterConn::{}: jobPool is null.", __func__);
		sauJobFinished(SAUNAFS_ERROR_NOTDONE, outputPacket, masterconn_serving_era());
		return;
	}
	job_duplicate(
	    *jobPool_,
	    needsLock ? sauJobFinishedAndLock(this, newChunkId, chunkType) : sauJobFinished(this),
	    outputPacket, oldChunkId, oldChunkVersion, oldChunkVersion, chunkType, newChunkId,
	    newChunkVersion);
}

void MasterConn::fencedTruncateChunk(const std::vector<uint8_t> &data) {
	ChunkCommandIdentity identity;
	uint64_t chunkId = 0;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	uint32_t chunkLength = 0;
	uint32_t newVersion = 0;
	uint32_t version = 0;

	matocs::fencedTruncateChunk::deserialize(data, identity, chunkId, chunkType, chunkLength,
	                                         newVersion, version);
	auto *outputPacket = acceptFencedCommand(identity, chunkId, chunkType,
	                                         ChunkCommandFamily::kTruncate, newVersion);
	if (outputPacket == nullptr) { return; }
	if (jobPool_ == nullptr) {
		safs::log_err("MasterConn::{}: jobPool is null.", __func__);
		sauJobFinished(SAUNAFS_ERROR_NOTDONE, outputPacket, masterconn_serving_era());
		return;
	}
	job_truncate(*jobPool_, sauJobFinished(this), outputPacket, chunkId, chunkType, version,
	             newVersion, chunkLength);
}

void MasterConn::fencedDuptruncChunk(const std::vector<uint8_t> &data) {
	ChunkCommandIdentity identity;
	uint64_t copyChunkId = 0;
	uint64_t chunkId = 0;
	uint32_t copyChunkVersion = 0;
	uint32_t chunkVersion = 0;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	uint32_t newLength = 0;

	matocs::fencedDuptruncChunk::deserialize(data, identity, copyChunkId, copyChunkVersion,
	                                         chunkType, chunkId, chunkVersion, newLength);
	auto *outputPacket = acceptFencedCommand(identity, copyChunkId, chunkType,
	                                         ChunkCommandFamily::kDuptrunc, copyChunkVersion);
	if (outputPacket == nullptr) { return; }
	if (jobPool_ == nullptr) {
		safs::log_err("MasterConn::{}: jobPool is null.", __func__);
		sauJobFinished(SAUNAFS_ERROR_NOTDONE, outputPacket, masterconn_serving_era());
		return;
	}
	job_duptrunc(*jobPool_, sauJobFinished(this), outputPacket, chunkId, chunkVersion, chunkVersion,
	             chunkType, copyChunkId, copyChunkVersion, newLength);
}

void MasterConn::fencedReplicateChunk(const std::vector<uint8_t> &data) {
	ChunkCommandIdentity identity;
	uint64_t chunkId = 0;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	uint32_t chunkVersion = 0;
	const uint8_t *sourcesBuffer = nullptr;

	matocs::fencedReplicateChunk::deserializePartial(data, identity, chunkId, chunkVersion,
	                                                 chunkType, sourcesBuffer);
	const uint32_t sourcesBufferSize = data.size() - (sourcesBuffer - data.data());

	auto *outputPacket = acceptFencedCommand(identity, chunkId, chunkType,
	                                         ChunkCommandFamily::kReplicate, chunkVersion);
	if (outputPacket == nullptr) { return; }
	safs::log_debug("cs.matocs.fenced_replicate {}", chunkId);

	if (hddScansInProgress()) {
		// Disk scan in progress, so replication is not possible yet.
		sauJobFinished(SAUNAFS_ERROR_WAITING, outputPacket, masterconn_serving_era());
		return;
	}
	if (replicationJobPool_ == nullptr) {
		safs::log_err("MasterConn::{}: replicationJobPool is null.", __func__);
		sauJobFinished(SAUNAFS_ERROR_NOTDONE, outputPacket, masterconn_serving_era());
		return;
	}
	job_replicate(*replicationJobPool_, sauJobFinished(this), outputPacket, chunkId, chunkVersion,
	              chunkType, sourcesBufferSize, sourcesBuffer);
}

// Callbacks

std::function<void(uint8_t status, void *packet)> MasterConn::sauJobFinishedAndLock(
    MasterConn *masterConn, uint64_t chunkId, ChunkPartType chunkType) {
	const uint64_t admittedEra = masterconn_serving_era();
	return [masterConn, chunkId, chunkType, admittedEra](uint8_t status, void *packet) {
		// The original job's output packet is sent as the response to the master's request
		masterConn->sauJobFinished(status, packet, admittedEra);

		if (status != SAUNAFS_STATUS_OK) {
			return;  // If the original job failed, do not prepare the chunk lock
		}
		// Locking is new work, and new work needs an authority that still holds. Starting it
		// under a retired era would take a lock nothing is entitled to hold.
		if (masterConn->distributedMode_ && !masterconn_era_is_current(admittedEra)) { return; }

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
	// Bound here, where the command is accepted, and never rewritten afterwards. By the time
	// the callback runs the process may be serving again under a different authority, and
	// asking then would answer a question about that one instead of about this work.
	const uint64_t admittedEra = masterconn_serving_era();
	return [masterConn, admittedEra](uint8_t status, void *packet) {
		masterConn->sauJobFinished(status, packet, admittedEra);
	};
}

void MasterConn::sauJobFinished(uint8_t status, void *packet, uint64_t admittedEra) {
	auto *outputPacket = static_cast<OutputPacket *>(packet);

	if (distributedMode_ && !masterconn_era_is_current(admittedEra)) {
		if (test_event_stream::enabled()) {
			test_event_stream::emit("job_result_quarantined",
			                        {{"admitted_era", admittedEra},
			                         {"current_era", masterconn_serving_era()},
			                         {"status", status}});
		}
		deletePacket(packet);
		return;
	}

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
