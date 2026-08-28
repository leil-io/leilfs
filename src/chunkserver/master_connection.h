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

#pragma once

#include "common/platform.h"

#include <sys/poll.h>
#include <cstdint>
#include <list>
#include <memory>
#include <string>

#include "common/chunk_command_identity.h"
#include "common/chunk_part_type.h"
#include "common/distributed_chunkserver_session.h"
#include "common/input_packet.h"
#include "common/network_address.h"
#include "common/output_packet.h"
#include "common/saunafs_version.h"
#include "common/time_utils.h"
#include "common/tls_session.h"

static constexpr uint32_t kMaxPacketSize = 10000;
static constexpr uint32_t kMaxBackgroundJobsCount = 1000;

// Common variables from config
inline std::string gBindHostStr;
inline std::string gLabel;
inline uint32_t gTimeout_ms;

// Forward declaration
class MasterJobPool;

/// @brief Enum representing the connection mode to the Metadata Server (MDS).
enum class ConnectionMode : std::uint8_t {
	FREE,        /// There is no socket for the connection yet.
	CONNECTING,  /// Connection is being established.
	CONNECTED,   /// Connection is active.
	KILL,        /// Connection has been dropped, a reconnection will be attempted.
	HANDSHAKE    /// TLS handshake is in progress.
};

/// @brief Enum representing the registration status of a connection to the Metadata Server (MDS).
enum class RegistrationStatus : std::uint8_t {
	kUnregistered,           ///< Initial state, not registered yet.
	kRegistrationRequested,  ///< Registration has been requested but not yet confirmed.
	kHostRegistered,         ///< Registration has been confirmed.
	kChunksRegistered,       ///< Chunks have been registered with the MDS.
};

/// @brief Class representing a connection to a Metadata Server (MDS).
///
/// Currently, only one active MDS (known as Master) is supported.
class MasterConn {
public:
	explicit MasterConn(
	    const std::string &masterHostStr, const std::string &masterPortStr,
	    const std::string &clusterId, const std::shared_ptr<MasterJobPool> &jobPool,
	    const std::shared_ptr<MasterJobPool> &replicationJobPool, bool distributedMode = false,
	    bool configuredSeed = true,
	    DistributedRegistrationRole distributedRole = DistributedRegistrationRole::kObserver)
	    : masterHostStr_(masterHostStr),
	      masterPortStr_(masterPortStr),
	      clusterId_(clusterId),
	      jobPool_(jobPool),
	      replicationJobPool_(replicationJobPool),
	      distributedMode_(distributedMode),
	      configuredSeed_(configuredSeed),
	      distributedRole_(distributedRole) {}

	// Disable unneeded copying and moving of the connection objects.
	MasterConn(const MasterConn &) = delete;
	MasterConn(MasterConn &&) = delete;
	MasterConn &operator=(const MasterConn &) = delete;
	MasterConn &operator=(const MasterConn &&) = delete;

	~MasterConn();

	// Packet handling

	static void deletePacket(void *packet);

	void attachPacket(void *packet);

	void createAttachedPacket(MessageBuffer serializedPacket);

	template <class... Data>
	void createAttachedNoVersionPacket(PacketHeader::Type type, const Data &...data) {
		std::vector<uint8_t> buffer;
		serializeLegacyPacket(buffer, type, data...);
		createAttachedPacket(std::move(buffer));
	}

	// Configuration

	void reloadConfig();

	// Connection management

	void sendRegisterLabel();

	void sendConfig();

	void sendRegister();

	/// Re-presents the distributed registration on the LIVE admitted connection with
	/// the CLAIM_RENEWER role, so a renewer nomination needs no reconnect and the MDS
	/// keeps serving this chunkserver throughout the FDB-arbitrated role move.
	void requestRenewerUpgrade();

	void onRegistered(const std::vector<uint8_t> &data);

	void onDistributedRegistered(const std::vector<uint8_t> &data);

	void finishRegistration(bool sendInventory);

	int initConnect();

	void connectTest();

	void tlsHandshake();

	void onConnected();

	// Polling

	void providePollDescriptors(std::vector<pollfd> &pdesc, bool doTerminate);

	void handlePollErrors(const std::vector<pollfd> &pdesc);

	void servePoll(const std::vector<pollfd> &pdesc);

	void readFromSocket();

	void writeToSocket();

	void gotPacket(PacketHeader header, const MessageBuffer &message);

	/// The dispatch the H4 fault seam wraps: gotPacket decides drop, duplicate or delay and
	/// this is what actually handles the packet.
	void dispatchPacket(PacketHeader header, const MessageBuffer &message);

	/// Releases every packet the delay fault is holding whose time has come.
	void releaseDeferredPackets();

	/// Packets held back by the H4 delay fault, each with the microsecond it is due at.
	struct DeferredPacket {
		PacketHeader header;
		MessageBuffer message;
		uint64_t dueMicroseconds = 0;
	};
	std::vector<DeferredPacket> deferredPackets_;

	void clusterMembers(const std::vector<uint8_t> &data);

	void sessionLease(const std::vector<uint8_t> &data);

	// Chunk operations

	void createChunk(const std::vector<uint8_t> &data);

	void createAndLockChunk(const std::vector<uint8_t> &data);

	void deleteChunk(const std::vector<uint8_t> &data);

	void setChunkVersion(const std::vector<uint8_t> &data);

	void setChunkVersionAndLock(const std::vector<uint8_t> &data);

	void lockChunk(const std::vector<uint8_t> &data);

	void unlockChunk(const std::vector<uint8_t> &data);

	void duplicateChunk(const std::vector<uint8_t> &data);

	void duplicateAndLockChunk(const std::vector<uint8_t> &data);

	void truncateChunk(const std::vector<uint8_t> &data);

	void duplicateTruncateChunk(const std::vector<uint8_t> &data);

	void replicateChunk(const std::vector<uint8_t> &data);

	// Fenced chunk operations

	void fencedCreateChunk(const std::vector<uint8_t> &data);

	void fencedDeleteChunk(const std::vector<uint8_t> &data);

	void fencedSetVersion(const std::vector<uint8_t> &data);

	/// Answers whether a part the published set expects here is still here, from this process's
	/// own record of it. A negative answer is a statement about this incarnation, which is what
	/// lets a metadata server treat it as evidence instead of a guess.
	void fencedVerifyPart(const std::vector<uint8_t> &data);

	void fencedLockChunk(const std::vector<uint8_t> &data);

	void fencedUnlockChunk(const std::vector<uint8_t> &data);

	void fencedDuplicateChunk(const std::vector<uint8_t> &data);

	void fencedTruncateChunk(const std::vector<uint8_t> &data);

	void fencedDuptruncChunk(const std::vector<uint8_t> &data);

	void fencedReplicateChunk(const std::vector<uint8_t> &data);

	/// A metadata server committed the outbox items up to a sequence; the outbox may forget them.
	void evidenceAck(const std::vector<uint8_t> &data);

	/// Decides whether a fenced command may run here, and builds the reply it will answer with.
	///
	/// A command names the process it was addressed to, so the first question is whether this is
	/// that process: a stable id can be reassigned, an incarnation names one process start, and
	/// the claim it was issued under says whether it predates this chunkserver's last cutoff.
	/// Checking that at execution time rather than at receipt is the point, since a command can
	/// sit in a queue across the very boundary it names.
	///
	/// @return the reply to hand to the job, already carrying the identity to echo, or nullptr
	/// when the command was refused and already answered.
	OutputPacket *acceptFencedCommand(const ChunkCommandIdentity &identity, uint64_t chunkId,
	                                  ChunkPartType chunkType, ChunkCommandFamily family,
	                                  uint32_t resultVersion);

	/// Answers a fenced command that will not run, echoing its identity so the metadata server
	/// can retire exactly the row it left rather than waiting out its deadline.
	void refuseFencedCommand(const ChunkCommandIdentity &identity, uint64_t chunkId,
	                         ChunkPartType chunkType, ChunkCommandFamily family, uint8_t status);

	// Callbacks

	static std::function<void(uint8_t status, void *packet)> sauJobFinished(MasterConn *masterConn);

	static std::function<void(uint8_t status, void *packet)> sauJobFinishedAndLock(
	    MasterConn *masterConn, uint64_t chunkId, ChunkPartType chunkType);

	/// Delivers one control-plane job result, or refuses to.
	///
	/// @p admittedEra is the serving era that was current when the command was accepted. The
	/// job may finish long after that era ended, and the physical work it did is real either
	/// way; what a retired era may not do is tell the cluster the work succeeded under an
	/// authority the cluster has already given to somebody else. Such a result is dropped and
	/// whatever it did becomes reconciliation evidence rather than an assertion.
	void sauJobFinished(uint8_t status, void *packet, uint64_t admittedEra);

	// Termination

	void releaseResources();

	void resetPackets();

	// Inline getters and setters

	ConnectionMode mode() const { return mode_; }

	RegistrationStatus registrationStatus() const { return registrationStatus_; }

	void setMode(ConnectionMode newMode) {
		mode_ = newMode;

		if (mode_ == ConnectionMode::KILL) {  // The socket will be closed soon.
			registrationStatus_ = RegistrationStatus::kUnregistered;
			// The first KILL deliberately schedules an observer to reconnect with
			// CLAIM_RENEWER. A later KILL during that attempt is a failed nomination,
			// allowing another still-admitted observer to be selected.
			if (sessionRenewerNominationAttempting_) { abandonSessionRenewerNomination(); }
		}
	}

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

	const std::string &clusterId() const { return clusterId_; }

	bool isTlsEnabled() const { return !tlsCertFile_.empty() && !tlsKeyFile_.empty(); }

	bool isOutputQueueEmpty() const { return outputPackets_.empty(); }

	/// Stable id of the MDS behind this connection; 0 until a cluster view binds it.
	uint32_t mdsId() const { return mdsId_; }

	void setMdsId(uint32_t mdsId) { mdsId_ = mdsId; }

	uint64_t mdsIncarnation() const { return mdsIncarnation_; }

	bool distributedMode() const { return distributedMode_; }

	DistributedRegistrationRole distributedRole() const { return distributedRole_; }

	void setDistributedRole(DistributedRegistrationRole role) {
		distributedRole_ = role;
		if (role != DistributedRegistrationRole::kClaimRenewer) {
			sessionRenewerNominationPending_ = false;
			sessionRenewerNominationAttempting_ = false;
			sessionRenewerConfirmed_ = false;
			sessionRenewerNominationDeadline_ = 0;
		}
	}

	/// True from deterministic observer selection until that same connection either
	/// completes CLAIM_RENEWER admission or its reconnect attempt fails.
	bool sessionRenewerNominationPending() const { return sessionRenewerNominationPending_; }

	/// True only once this MDS answered a CLAIM_RENEWER registration with an accepted
	/// admission tuple. The role this process set on itself is a request, not an answer:
	/// an election that reads its own request as a result never runs a second time.
	bool sessionRenewerConfirmed() const { return sessionRenewerConfirmed_; }

	/// The session authority instant this nomination attempt stops being worth waiting
	/// for; 0 when no bound could be established.
	uint64_t sessionRenewerNominationDeadline() const { return sessionRenewerNominationDeadline_; }

	/// Retires a nomination whose deadline has passed, returning the connection to
	/// observer so the next reconciliation pass may arbitrate again. Returns true when
	/// an attempt was actually retired.
	bool expireSessionRenewerNomination(uint64_t now);

	/// Changes an admitted observer into the sole in-flight renewer nominee. The
	/// distributed registration role is part of the handshake, so the caller must
	/// subsequently reconnect this connection or re-present the role in place.
	void nominateSessionRenewer(uint64_t attemptDeadline) {
		distributedRole_ = DistributedRegistrationRole::kClaimRenewer;
		sessionRenewerNominationPending_ = true;
		sessionRenewerNominationAttempting_ = false;
		sessionRenewerConfirmed_ = false;
		sessionRenewerNominationDeadline_ = attemptDeadline;
	}

	/// A retired connection lost its identity to another connection reaching the same
	/// MDS; it stays allocated (job callbacks may still reference it) but never redials.
	bool retired() const { return retired_; }

	void retire() {
		retired_ = true;
		setMode(ConnectionMode::KILL);
	}

private:
	std::string masterHostStr_;                  ///< Hostname of the master server.
	std::string masterPortStr_;                  ///< Port of the master server.
	uint32_t version_{saunafsVersion(0, 0, 0)};  ///< Version of the master server.
	std::string clusterId_;                      ///< Cluster ID for this connection.
	std::shared_ptr<MasterJobPool> jobPool_;     ///< Shared reference to the JobPool.
	/// Shared reference to the ReplicationJobPool.
	std::shared_ptr<MasterJobPool> replicationJobPool_;
	bool distributedMode_{false};
	bool configuredSeed_{true};
	DistributedRegistrationRole distributedRole_{DistributedRegistrationRole::kObserver};
	bool sessionRenewerNominationPending_{false};
	bool sessionRenewerNominationAttempting_{false};
	bool sessionRenewerConfirmed_{false};
	uint64_t sessionRenewerNominationDeadline_{0};
	uint64_t sessionClaimSequence_{0};
	uint64_t sessionLeaseDeadline_{0};

	void beginSessionRenewerNominationAttempt();
	void completeSessionRenewerNomination();
	void abandonSessionRenewerNomination();

	// For compatibility with old masters (version < 5.0)
	void handleRegistrationAttempt();
	static constexpr uint8_t kMaxRegistrationAttemptsToBeConsideredOldMaster = 3;
	uint32_t registrationAttempts_{0};  ///< Number of registration attempts.
	bool isVersionLessThan5_{false};    ///< Indicates if the master server is an old version.

	uint32_t mdsId_{0};           ///< Stable id of the MDS behind this connection; 0 = unknown.
	uint64_t mdsIncarnation_{0};  ///< Process incarnation returned by the distributed handshake.
	bool retired_{false};         ///< Never redial: another connection owns this MDS.

	ConnectionMode mode_{ConnectionMode::FREE};  ///< Current mode of the connection to this master.
	/// Registration status to this MDS.
	RegistrationStatus registrationStatus_{RegistrationStatus::kUnregistered};
	int socketFD_{-1};                         ///< Socket file descriptor for this connection.
	int32_t pDescPos_{-1};                     ///< Position in the pollfd array.
	Timer lastRead_;                           ///< Time since the last read operation.
	Timer lastWrite_;                          ///< Time since the last write operation.
	InputPacket inputPacket_{kMaxPacketSize};  ///< Input buffer for reading data from the socket.
	std::list<OutputPacket> outputPackets_;    ///< Output packets to be sent to the master.

	NetworkAddress address_;            ///< Address of this master server (IP and port).
	NetworkAddress bindHostAddress_;    ///< Address to bind the socket to (IP and port).
	bool isMasterAddressValid_{false};  ///< Tells if the master address is valid.

	// Statistics
	uint64_t bytesIn_ = 0;   ///< Number of bytes read from the master.
	uint64_t bytesOut_ = 0;  ///< Number of bytes sent to the master.

	std::unique_ptr<TlsSession> tlsSession_{
	    nullptr};                ///< Context of the TLS channel used for communication with master.
	std::string tlsCertFile_;    ///< Path to the TLS certificate file.
	std::string tlsKeyFile_;     ///< Path to the TLS private key file.
	std::string tlsCaCertFile_;  ///< Path to the TLS CA certificate file.
	int lastHandshakeError_{0};  ///< Last error code from TLS handshake.
};
