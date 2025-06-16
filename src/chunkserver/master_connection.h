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

#include "common/network_address.h"
#include "common/output_packet.h"
#include "common/time_utils.h"
#include "protocol/input_packet.h"

static constexpr uint32_t kMaxPacketSize = 10000;
static constexpr uint32_t kMaxBackgroundJobsCount = 1000;

// Common variables from config
inline std::string gBindHostStr;
inline std::string gLabel;
inline uint32_t gTimeout_ms;

// Forward declaration
class JobPool;

/// @brief Enum representing the connection mode to the Metadata Server (MDS).
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

	int initConnect();

	void connectTest();

	void onConnected();

	// Polling

	void providePollDescriptors(std::vector<pollfd> &pdesc);

	void handlePollErrors(const std::vector<pollfd> &pdesc);

	void servePoll(const std::vector<pollfd> &pdesc);

	void readFromSocket();

	void writeToSocket();

	void gotPacket(PacketHeader header, const MessageBuffer &message);

	// Chunk operations

	void createChunk(const std::vector<uint8_t> &data);

	void deleteChunk(const std::vector<uint8_t> &data);

	void setChunkVersion(const std::vector<uint8_t> &data);

	void duplicateChunk(const std::vector<uint8_t> &data);

	void truncateChunk(const std::vector<uint8_t> &data);

	void duplicateTruncateChunk(const std::vector<uint8_t> &data);

	void replicateChunk(const std::vector<uint8_t> &data);

	// Callbacks

	static std::function<void(uint8_t status, void *packet)> sauJobFinished(MasterConn *masterConn);

	void sauJobFinished(uint8_t status, void *packet);

	// Termination

	void releaseResources();

	void resetPackets();

	// Inline getters and setters

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
	std::string masterHostStr_;         ///< Hostname of the master server.
	std::string masterPortStr_;         ///< Port of the master server.
	std::shared_ptr<JobPool> jobPool_;  ///< Shared reference to the JobPool.

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
