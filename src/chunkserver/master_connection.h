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

#include "common/chunk_part_type.h"
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

/// CHUNK_REGISTRATION_START_TIMEOUT (seconds): how long a pull-capable
/// chunkserver waits for SAU_MATOCS_REGISTER_CHUNKS_START before falling
/// back to the old push registration.
inline uint32_t gPullRegistrationStartTimeout_s;

/// CHUNK_REGISTRATION_FORCE_PUSH: skip the pull protocol entirely and always
/// push the full chunk list (testability / emergency escape hatch).
inline bool gForcePushRegistration;

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
	kAwaitingPullStart,      ///< Waiting for the master to start pull registration.
	kChunksRegistering,      ///< Master-driven (pull) chunk registration in progress.
	kChunksRegistered,       ///< Chunks have been registered with the MDS.
};

/// @brief Class representing a connection to a Metadata Server (MDS).
///
/// Currently, only one active MDS (known as Master) is supported.
class MasterConn {
public:
	explicit MasterConn(const std::string &masterHostStr, const std::string &masterPortStr,
	                    const std::string &clusterId, const std::shared_ptr<MasterJobPool> &jobPool,
	                    const std::shared_ptr<MasterJobPool> &replicationJobPool)
	    : masterHostStr_(masterHostStr),
	      masterPortStr_(masterPortStr),
	      clusterId_(clusterId),
	      jobPool_(jobPool),
	      replicationJobPool_(replicationJobPool) {}

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

	/// Enqueues a packet right after the (possibly partially written) head of
	/// the output queue, so it jumps any long backlog (e.g. registration
	/// bulks). Used for replies the master is actively waiting for.
	void createAttachedPriorityPacket(MessageBuffer serializedPacket);

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

	void onRegistered(const std::vector<uint8_t> &data);

	// Master-driven (pull) chunk registration

	/// Handles SAU_MATOCS_REGISTER_CHUNKS_START: begins the pull sweep.
	void onRegisterChunksStart(const std::vector<uint8_t> &data);

	/// Handles SAU_MATOCS_REGISTER_CHUNKS_CREDIT: releases more bulks.
	void onRegisterChunksCredit(const std::vector<uint8_t> &data);

	/// Falls back to the old push registration when the master never sent
	/// SAU_MATOCS_REGISTER_CHUNKS_START (e.g. a master which advertises a
	/// pull-capable version but does not speak the protocol).
	/// Called periodically from the event loop.
	void checkPullRegistrationStartTimeout();

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

	/// Handles SAU_MATOCS_QUERY_CHUNKS: replies immediately (from the event
	/// loop, in-memory lookup only) which of the queried chunks this
	/// chunkserver hosts. See cstoma::queryChunksResponse.
	void queryChunks(const std::vector<uint8_t> &data);

	// Callbacks

	static std::function<void(uint8_t status, void *packet)> sauJobFinished(MasterConn *masterConn);

	static std::function<void(uint8_t status, void *packet)> sauJobFinishedAndLock(
	    MasterConn *masterConn, uint64_t chunkId, ChunkPartType chunkType);

	void sauJobFinished(uint8_t status, void *packet);

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

private:
	std::string masterHostStr_;                     ///< Hostname of the master server.
	std::string masterPortStr_;                     ///< Port of the master server.
	uint32_t version_{saunafsVersion(0, 0, 0)};     ///< Version of the master server.
	std::string clusterId_;                         ///< Cluster ID for this connection.
	std::shared_ptr<MasterJobPool> jobPool_;        ///< Shared reference to the JobPool.
	/// Shared reference to the ReplicationJobPool.
	std::shared_ptr<MasterJobPool> replicationJobPool_;

	// For compatibility with old masters (version < 5.0)
	void handleRegistrationAttempt();
	static constexpr uint8_t kMaxRegistrationAttemptsToBeConsideredOldMaster = 3;
	uint32_t registrationAttempts_{0};  ///< Number of registration attempts.
	bool isVersionLessThan5_{false};    ///< Indicates if the master server is an old version.

	// Master-driven (pull) chunk registration

	/// Sends the space/label/config registration tail.
	void sendRegistrationTail();

	/// Runs the old push registration (full sweep in one go).
	void pushRegisterChunks();

	/// Sends sweep bulks while pull credits remain; sends
	/// SAU_CSTOMA_REGISTER_CHUNKS_END when the sweep completes.
	void pumpPullRegistration();

	uint32_t pullBulkSize_{0};    ///< Chunks per bulk, dictated by the master.
	uint32_t pullCredits_{0};     ///< Bulks the master is ready to accept.
	uint64_t pullChunksSent_{0};  ///< Chunks reported in this pull session.
	/// Expires while waiting for SAU_MATOCS_REGISTER_CHUNKS_START.
	Timeout pullStartTimeout_{std::chrono::seconds(0)};

	ConnectionMode mode_{ConnectionMode::FREE};  ///< Current mode of the connection to this master.
	/// Registration status to this MDS.
	RegistrationStatus registrationStatus_{RegistrationStatus::kUnregistered};
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

	std::unique_ptr<TlsSession> tlsSession_{nullptr};  ///< Context of the TLS channel used for communication with master.
	std::string tlsCertFile_;                          ///< Path to the TLS certificate file.
	std::string tlsKeyFile_;                           ///< Path to the TLS private key file.
	std::string tlsCaCertFile_;                        ///< Path to the TLS CA certificate file.
	int lastHandshakeError_{0};                        ///< Last error code from TLS handshake.
};
