/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include <sys/poll.h>
#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "common/chunk_part_type.h"
#include "common/input_packet.h"
#include "common/metadata_cluster_member.h"
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

/// One active connection to a configured or discovered metadata server. The configured one is
/// listener 0 with server id 0 and keeps the registration sequence and the reports; discovered
/// ones carry the id the snapshot named. A server that asks for an identity gets no inventory,
/// whichever connection it is.
class MasterConn {
public:
	explicit MasterConn(const std::string &masterHostStr, const std::string &masterPortStr,
	                    const std::string &clusterId, const std::shared_ptr<MasterJobPool> &jobPool,
	                    const std::shared_ptr<MasterJobPool> &replicationJobPool,
	                    uint32_t listenerId = 0, uint32_t serverId = 0)
	    : masterHostStr_(masterHostStr),
	      masterPortStr_(masterPortStr),
	      clusterId_(clusterId),
	      jobPool_(jobPool),
	      replicationJobPool_(replicationJobPool),
	      listenerId_(listenerId),
	      serverId_(serverId) {}

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

	/// Answers the metadata server's identity request during registration.
	void sendChunkserverId(const std::vector<uint8_t> &data);

	void onRegistered(const std::vector<uint8_t> &data);

	/// Stores the discovery snapshot the configured connection receives after registering.
	/// @throws IncorrectDeserializationException when the packet is unexpected, names another
	/// server than this connection's, or repeats the configured endpoint.
	void receiveClusterSnapshot(const std::vector<uint8_t> &data);

	/// Hands the latest snapshot to the connection manager once; later calls return nothing
	/// until a new snapshot arrives.
	std::optional<MetadataClusterSnapshot> takeClusterSnapshot() {
		return std::exchange(clusterSnapshot_, std::nullopt);
	}

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

	/// Answers a metadata server asking which version of a chunk part this server holds, the
	/// replacement for the inventory that discovered connections never send.
	void probeChunk(const std::vector<uint8_t> &data);

	void setChunkVersion(const std::vector<uint8_t> &data);

	void setChunkVersionAndLock(const std::vector<uint8_t> &data);

	void lockChunk(const std::vector<uint8_t> &data);

	void unlockChunk(const std::vector<uint8_t> &data);

	void duplicateChunk(const std::vector<uint8_t> &data);

	void duplicateAndLockChunk(const std::vector<uint8_t> &data);

	void truncateChunk(const std::vector<uint8_t> &data);

	void duplicateTruncateChunk(const std::vector<uint8_t> &data);

	void replicateChunk(const std::vector<uint8_t> &data);

	// Callbacks

	static std::function<void(uint8_t status, void *packet)> sauJobFinished(MasterConn *masterConn);

	static std::function<void(uint8_t status, void *packet)> sauJobFinishedAndLock(
	    MasterConn *masterConn, uint64_t chunkId, ChunkPartType chunkType);

	void sauJobFinished(uint8_t status, void *packet);

	// Termination

	void releaseResources();

	/// Closes the socket the way the released code closed it on an ordinary disconnect: the
	/// descriptor goes, and no TLS shutdown alert is sent, which only termination did.
	void closeSocketQuietly();

	void resetPackets();

	/// Returns lost and damaged reports still queued for this socket to the disk report queues,
	/// so the next registered connection delivers them without an inventory.
	void requeueUnsentReports();

	// Inline getters and setters

	ConnectionMode mode() const { return mode_; }

	RegistrationStatus registrationStatus() const { return registrationStatus_; }

	/// False once the metadata server asked for the identity: that server records copies by
	/// identity and needs no inventory.
	bool sendsInventory() const { return sendInventory_; }

	/// Remains true after a metadata server selects identity registration, including reconnects.
	bool identityProtocolSelected() const { return identityProtocolSelected_; }

	/// The configured connection is the discovery seed and the only one that registers chunks.
	bool isConfigured() const { return serverId_ == 0; }

	/// Metadata server id the discovery snapshot named; 0 for the configured connection.
	uint32_t serverId() const { return serverId_; }

	bool isRegistered() const {
		return mode_ == ConnectionMode::CONNECTED &&
		       registrationStatus_ == RegistrationStatus::kChunksRegistered;
	}

	void setMode(ConnectionMode newMode) {
		if (newMode == ConnectionMode::CONNECTED && mode_ != newMode) {
			callbackActive_ = std::make_shared<bool>(true);
			// Retain the last peer's report policy while offline; negotiate again on a new socket.
			sendInventory_ = true;
		}
		mode_ = newMode;

		if (mode_ == ConnectionMode::KILL) {  // The socket will be closed soon.
			*callbackActive_ = false;
			registrationStatus_ = RegistrationStatus::kUnregistered;
			clusterSnapshot_.reset();
		}
	}

	void setSendInventory(bool sendInventory) {
		sendInventory_ = sendInventory;
		if (!sendInventory) { identityProtocolSelected_ = true; }
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
	std::string masterHostStr_;                  ///< Hostname of the master server.
	std::string masterPortStr_;                  ///< Port of the master server.
	uint32_t version_{saunafsVersion(0, 0, 0)};  ///< Version of the master server.
	std::string clusterId_;                      ///< Cluster ID for this connection.
	std::shared_ptr<MasterJobPool> jobPool_;     ///< Shared reference to the JobPool.
	/// Shared reference to the ReplicationJobPool.
	std::shared_ptr<MasterJobPool> replicationJobPool_;
	const uint32_t listenerId_;  ///< Job-pool listener owned by this connection.
	const uint32_t serverId_;    ///< Metadata server id from discovery; 0 when configured.
	/// Cleared on close so job callbacks of this socket never answer over a replacement socket.
	std::shared_ptr<bool> callbackActive_{std::make_shared<bool>(true)};
	/// Discovery snapshot waiting for the connection manager; only the configured one fills it.
	std::optional<MetadataClusterSnapshot> clusterSnapshot_;

	// For compatibility with old masters (version < 5.0)
	void handleRegistrationAttempt();
	static constexpr uint8_t kMaxRegistrationAttemptsToBeConsideredOldMaster = 3;
	uint32_t registrationAttempts_{0};  ///< Number of registration attempts.
	bool isVersionLessThan5_{false};    ///< Indicates if the master server is an old version.

	ConnectionMode mode_{ConnectionMode::FREE};  ///< Current mode of the connection to this master.
	/// Registration status to this MDS.
	RegistrationStatus registrationStatus_{RegistrationStatus::kUnregistered};
	bool sendInventory_{true};  ///< Whether registration sends the chunk inventory.
	/// Durable completion policy for connections which selected identity registration.
	bool identityProtocolSelected_{false};
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
