/*
   Copyright 2005-2017 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2017 Skytechnology sp. z o.o.
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

#include "master/matoclserv.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <cstdint>
#include <fstream>
#include <memory>

#include "common/charts.h"
#include "common/chunk_type_with_address.h"
#include "common/chunk_with_address_and_label.h"
#include "common/chunks_availability_state.h"
#include "common/cwrap.h"
#include "common/datapack.h"
#include "common/event_loop.h"
#include "common/generic_lru_cache.h"
#include "common/goal.h"
#include "common/human_readable_format.h"
#include "common/io_limits_config_loader.h"
#include "common/io_limits_database.h"
#include "common/legacy_vector.h"
#include "common/loop_watchdog.h"
#include "common/massert.h"
#include "common/md5.h"
#include "common/network_address.h"
#include "common/random.h"
#include "common/saunafs_statistics.h"
#include "common/saunafs_version.h"
#include "common/serialized_goal.h"
#include "common/sessions_file.h"
#include "common/sockets.h"
#include "common/type_defs.h"
#include "common/user_groups.h"
#include "config/cfg.h"
#include "master/changelog.h"
#include "master/chartsdata.h"
#include "master/chunks.h"
#include "master/chunkserver_db.h"
#include "master/datacachemgr.h"
#include "master/exports.h"
#include "master/filesystem.h"
#include "master/filesystem_node.h"
#include "master/filesystem_node_types.h"
#include "master/filesystem_operations.h"
#include "master/filesystem_periodic.h"
#include "master/filesystem_snapshot.h"
#include "master/masterconn.h"
#include "master/matocsserv.h"
#include "master/matomlserv.h"
#include "master/metadata_backend_common.h"
#include "master/metadata_backend_interface.h"
#include "master/personality.h"
#include "master/settrashtime_task.h"
#include "metrics/metrics.h"
#include "protocol/SFSCommunication.h"
#include "protocol/cltoma.h"
#include "protocol/matocl.h"
#include "slogger/slogger.h"

#define MaxPacketSize 1000000

// matoclserventry.mode
enum class ClientConnectionMode : std::uint8_t {
	KILL,			/// Connection is terminated,
	HEADER,			/// Read header
	DATA			/// Read data packet
};

// chunkDelayedOperation types
enum DelayedChunkOperationType : std::uint32_t {
	FUSE_WRITE,          /// Reply to FUSE_WRITE_CHUNK is delayed
	FUSE_TRUNCATE,       /// Reply to FUSE_TRUNCATE which does not require writing is delayed
	FUSE_TRUNCATE_BEGIN, /// Reply to FUSE_TRUNCATE which does require writing is delayed
	FUSE_TRUNCATE_END    /// Reply to FUSE_TRUNCATE_END is delayed
};

#define SESSION_STATS 16

const uint32_t kMaxNumberOfChunkCopies = 100U;
constexpr uint8_t kClientInactivityTimeout = 10;

struct matoclserventry;

// locked chunks
class PacketSerializer;

struct DelayedChunkOperation {
	uint64_t chunkId;       ///< Chunk ID
	uint64_t fileLength;    ///< File length
	uint32_t lockId;        ///< Lock ID
	uint32_t messageId;     ///< Message ID for reply
	inode_t inode;          ///< Inode
	uint32_t uid;           ///< Remapped uid of the user which will run the operation
	uint32_t gid;           ///< Remapped gid of the user which will run the operation
	uint32_t auid;          ///< Real uid (not remapped) of the user who will run the operation
	uint32_t agid;          ///< Real gid (not remapped) of the user who will run the operation
	uint8_t type;           ///< Delayed operation type: FUSE_WRITE, FUSE_TRUNCATE,
	                        ///< FUSE_TRUNCATE_BEGIN or FUSE_TRUNCATE_END
	const PacketSerializer* serializer;  ///< Packet serializer for the operation
};

struct Session {
	using GroupCache = GenericLruCache<uint32_t, FsContext::GroupsContainer, 1024>;
	using OpenFilesSet = std::set<inode_t>;

	uint32_t sessionId;      ///< Session ID
	std::string info;        ///< Mount point path
	std::string mountInfo;   ///< Mount information shown in the CGI, e.g., arguments, mount options
	std::string config;      ///< Session configuration
	uint32_t peerIpAddress;  ///< Peer IP address
	uint16_t peerPort{};     ///< Peer port
	uint8_t newSession;      ///< Indicates if this is a new session (1) or a reconnect (0)
	uint8_t flags;           ///< Session flags. See more details in SFSCommunication.h
	uint8_t minGoal;         ///< Minimum goal allowed for this session
	uint8_t maxGoal;         ///< Maximum goal allowed for this session
	uint32_t minTrashTime;   ///< Minimum time in seconds to keep files in trash
	uint32_t maxTrashTime;   ///< Maximum time in seconds to keep files in trash
	uint32_t rootUid;        ///< Remapped UID of the user who created the session
	uint32_t rootGid;        ///< Remapped GID of the user who created the session
	uint32_t mapAllUid;    ///< UID to map all non-root users to when the session has SESFLAG_MAPALL
	                       ///< flag set)
	uint32_t mapAllGid;    ///< GID to map all non-root users to when the session has SESFLAG_MAPALL
	                       ///< flag set)
	inode_t rootInode;     ///< Special Root Inode with value = 1
	uint32_t disconnectedTimestamp;  ///< Last connected timestamp for this session
	                                 ///< A value = 0 means a client is connected
	                                 ///< A value > 0 means the last disconnection timestamp
	uint32_t connections;  ///< Number of active connections. A value of 0 means no connections
	std::array<uint32_t, SESSION_STATS> currHourOperationsStats; ///< Current hour operations stats
	std::array<uint32_t, SESSION_STATS> prevHourOperationsStats; ///< Previous hour operations stats
	GroupCache groupsCache;     ///< Cache for groups ID for this session
	OpenFilesSet openFilesSet;  ///< Set of open files for this session

	Session()
	    : sessionId(),
	      info(),
	      peerIpAddress(),
	      newSession(),
	      flags(),
	      minGoal(GoalId::kMin),
	      maxGoal(GoalId::kMax),
	      minTrashTime(),
	      maxTrashTime(std::numeric_limits<uint32_t>::max()),
	      rootUid(),
	      rootGid(),
	      mapAllUid(),
	      mapAllGid(),
	      rootInode(SPECIAL_INODE_ROOT),
	      disconnectedTimestamp(),
	      connections(),
	      currHourOperationsStats(),
	      prevHourOperationsStats(),
	      groupsCache(),
	      openFilesSet() {
	}
};

struct packetstruct {
	struct packetstruct *next;
	uint8_t *startPtr;
	uint32_t bytesLeft;
	uint8_t *packet;
};

///< This looks to be the client type.
///< This is set in matoclserv_serve and matoclserv_fuse_register, and there are 4 possible values:
enum class ClientState {
	kUnregistered = 0,  ///< New client (default, just after TCP accept).
	                    ///  This is referred to as "unregistered clients".
	kRegistered = 1,    ///< FUSE_REGISTER_BLOB_NOACL or (FUSE_REGISTER_BLOB_ACL and
	                  ///<  (REGISTER_NEWSESSION or REGISTER_NEWMETASESSION or REGISTER_RECONNECT))
	           ///< This is referred to as "mounts and new tools" or "standard, registered clients".
	kOldTools = 100, ///< FUSE_REGISTER_BLOB_TOOLS_NOACL or (FUSE_REGISTER_BLOB_ACL and REGISTER_TOOLS)
	           ///< This is referred to as "old sfstools" or "old tools clients".
	kAdmin = 665 ///< saunafs-admin after successful authentication
};

/// Values for matoclserventry.adminTask
/// Lists of tasks that admins can request.
enum class AdminTask {
	kNone,
	kTerminate,  ///< Admin successfully requested termination of the server
	kReload,  ///< Admin successfully requested reloading the configuration
	kSaveMetadata,  ///< Admin successfully requested saving metadata
	kRecalculateChecksums   ///< Admin successfully requested recalculation of metadata checksum
};

///< Client entry in the server.
struct matoclserventry {
	ClientState registered;           ///< Client state, see ClientState enum
	ClientConnectionMode mode;        ///< Connection mode, see ClientConnectionMode enum
	bool ioLimitsEnabled;             ///< Whether I/O limits are enabled for this client
	int socket;                       ///< Socket number
	int32_t pDescPos;                 ///< Position in the poll descriptors array
	uint32_t lastReadTimestamp;       ///< Timestamp of last read operation
	uint32_t lastWriteTimestamp;      ///< Timestamp of last write operation
	uint32_t version;                 ///< SaunaFS version of the client, see saunafsVersion()
	uint32_t peerIpAddress;           ///< Peer IP address of the client
	uint16_t peerPort;                ///< Peer port of the client
	uint8_t headerBuffer[8];          ///< Buffer for packet header
	packetstruct inputPacket;         ///< Input packet structure for reading data from the client
	packetstruct *outputPacketHead;   ///< Pointer to the head of the output packet list
	packetstruct **outputPacketTail;  ///< Pointer to the tail of the output packet list

	static constexpr uint8_t kPasswordSize = 32;
	uint8_t randomPassword[kPasswordSize];  ///< Random password for authentication
	Session *sessionData;                   ///< Pointer to the session data for this client
	///< Challenge data for admin authentication
	std::unique_ptr<SauMatoclAdminRegisterChallengeData> adminChallenge;
	AdminTask adminTask;  ///< admin task requested by this client
	///< Delayed chunk operations for this client
	std::vector<std::unique_ptr<DelayedChunkOperation>> delayedChunkOperations;
};

static std::vector<std::unique_ptr<Session>> sessionVector;
static std::list<std::unique_ptr<matoclserventry>> matoclservList;

static int masterSocket;             ///< Master socket for accepting new connections
static int32_t masterSocketDescPos;  ///< Position in the poll descriptors array for masterSocket
static int exiting;   ///< Flag indicating whether the server is exiting (1) or running (0)
static int starting;  ///< Flag indicating whether the server is starting (1) or not (0)

// from config
static std::string ListenHost;
static std::string ListenPort;
static uint32_t RejectOld;
static uint32_t SessionSustainTime;

static uint32_t gIoLimitsAccumulate_ms;
static double gIoLimitsRefreshTime;
static uint32_t gIoLimitsConfigId;
static std::string gIoLimitsSubsystem;
static IoLimitsDatabase gIoLimitsDatabase;

static uint32_t statsPacketsReceived = 0;
static uint32_t statsPacketsSent = 0;
static uint64_t statsBytesReceived = 0;
static uint64_t statsBytesSent = 0;

static void getStandardChunkCopies(const std::vector<ChunkTypeWithAddress>& allCopies,
		std::vector<NetworkAddress>& standardCopies);

class PacketSerializer {
public:
	static const PacketSerializer* getSerializer(PacketHeader::Type type, uint32_t version);
	virtual ~PacketSerializer() {}

	virtual bool isSaunaFsPacketSerializer() const = 0;

	virtual void serializeFuseReadChunk(std::vector<uint8_t>& packetBuffer,
			uint32_t messageId, uint8_t status) const = 0;
	virtual void serializeFuseReadChunk(std::vector<uint8_t>& packetBuffer,
			uint32_t messageId, uint64_t fileLength, uint64_t chunkId, uint32_t chunkVersion,
			const std::vector<ChunkTypeWithAddress>& chunkCopies) const = 0;
	virtual void deserializeFuseReadChunk(const std::vector<uint8_t>& packetBuffer,
			uint32_t& messageId, inode_t& inode, uint32_t& chunkIndex) const = 0;

	virtual void serializeFuseWriteChunk(std::vector<uint8_t>& packetBuffer,
			uint32_t messageId, uint8_t status) const = 0;
	virtual void serializeFuseWriteChunk(std::vector<uint8_t>& packetBuffer,
			uint32_t messageId, uint64_t fileLength,
			uint64_t chunkId, uint32_t chunkVersion, uint32_t lockId,
			const std::vector<ChunkTypeWithAddress>& chunkCopies) const = 0;
	virtual void deserializeFuseWriteChunk(const std::vector<uint8_t>& packetBuffer,
			uint32_t& messageId, inode_t& inode, uint32_t& chunkIndex, uint32_t& lockId) const = 0;

	virtual void serializeFuseWriteChunkEnd(std::vector<uint8_t>& packetBuffer,
			uint32_t messageId, uint8_t status) const = 0;
	virtual void deserializeFuseWriteChunkEnd(const std::vector<uint8_t>& packetBuffer,
			uint32_t& messageId, uint64_t& chunkId, uint32_t& lockId,
			inode_t& inode, uint64_t& fileLength) const = 0;

	virtual void serializeFuseTruncate(std::vector<uint8_t>& packetBuffer,
			uint32_t type /* FUSE_TRUNCATE | FUSE_TRUNCATE_END*/,
			uint32_t messageId, uint8_t status) const = 0;
	virtual void serializeFuseTruncate(std::vector<uint8_t>& packetBuffer,
			uint32_t type /* FUSE_TRUNCATE | FUSE_TRUNCATE_END*/,
			uint32_t messageId, const Attributes& attributes) const = 0;
	virtual void deserializeFuseTruncate(std::vector<uint8_t>& packetBuffer,
			uint32_t& messageId, inode_t& inode, bool& isOpened,
			uint32_t& uid, uint32_t& gid, uint64_t& length) const = 0;
};

class LegacyPacketSerializer : public PacketSerializer {
public:
	virtual bool isSaunaFsPacketSerializer() const {
		return false;
	}

	virtual void serializeFuseReadChunk(std::vector<uint8_t>& packetBuffer,
			uint32_t messageId, uint8_t status) const {
		serializeLegacyPacket(packetBuffer, MATOCL_FUSE_READ_CHUNK, messageId, status);
	}

	virtual void serializeFuseReadChunk(std::vector<uint8_t>& packetBuffer,
			uint32_t messageId, uint64_t fileLength, uint64_t chunkId, uint32_t chunkVersion,
			const std::vector<ChunkTypeWithAddress>& chunkCopies) const {
		LegacyVector<NetworkAddress> standardChunkCopies;
		getStandardChunkCopies(chunkCopies, standardChunkCopies);
		serializeLegacyPacket(packetBuffer, MATOCL_FUSE_READ_CHUNK, messageId, fileLength,
				chunkId, chunkVersion, standardChunkCopies);
	}

	virtual void deserializeFuseReadChunk(const std::vector<uint8_t>& packetBuffer,
			uint32_t& messageId, inode_t& inode, uint32_t& chunkIndex) const {
		deserializeAllLegacyPacketDataNoHeader(packetBuffer, messageId, inode, chunkIndex);
	}

	virtual void serializeFuseWriteChunk(std::vector<uint8_t>& packetBuffer,
			uint32_t messageId, uint8_t status) const {
		serializeLegacyPacket(packetBuffer, MATOCL_FUSE_WRITE_CHUNK, messageId, status);
	}

	virtual void serializeFuseWriteChunk(std::vector<uint8_t>& packetBuffer,
			uint32_t messageId, uint64_t fileLength,
			uint64_t chunkId, uint32_t chunkVersion, uint32_t lockId,
			const std::vector<ChunkTypeWithAddress>& chunkCopies) const {
		sassert(lockId == 1);
		LegacyVector<NetworkAddress> standardChunkCopies;
		getStandardChunkCopies(chunkCopies, standardChunkCopies);
		serializeLegacyPacket(packetBuffer, MATOCL_FUSE_WRITE_CHUNK, messageId, fileLength,
						chunkId, chunkVersion, standardChunkCopies);
	}

	virtual void deserializeFuseWriteChunk(const std::vector<uint8_t>& packetBuffer,
			uint32_t& messageId, inode_t& inode, uint32_t& chunkIndex, uint32_t& lockId) const {
		deserializeAllLegacyPacketDataNoHeader(packetBuffer, messageId, inode, chunkIndex);
		lockId = 1;
	}

	virtual void serializeFuseWriteChunkEnd(std::vector<uint8_t>& packetBuffer,
			uint32_t messageId, uint8_t status) const {
		serializeLegacyPacket(packetBuffer, MATOCL_FUSE_WRITE_CHUNK_END, messageId, status);
	}

	virtual void deserializeFuseWriteChunkEnd(const std::vector<uint8_t>& packetBuffer,
			uint32_t& messageId, uint64_t& chunkId, uint32_t& lockId,
			inode_t& inode, uint64_t& fileLength) const {
		deserializeAllLegacyPacketDataNoHeader(packetBuffer,
				messageId, chunkId, inode, fileLength);
		lockId = 1;
	}

	virtual void serializeFuseTruncate(std::vector<uint8_t>& packetBuffer,
			uint32_t type, uint32_t messageId, uint8_t status) const {
		sassert(type == FUSE_TRUNCATE || type == FUSE_TRUNCATE_END);
		if (type == FUSE_TRUNCATE) {
			serializeLegacyPacket(packetBuffer, MATOCL_FUSE_TRUNCATE, messageId, status);
		} else {
			// this should never happen, so do anything
			serializeLegacyPacket(packetBuffer, MATOCL_FUSE_TRUNCATE,
					messageId, uint8_t(SAUNAFS_ERROR_ENOTSUP));
		}
	}

	virtual void serializeFuseTruncate(std::vector<uint8_t>& packetBuffer,
			uint32_t type, uint32_t messageId, const Attributes& attributes) const {
		sassert(type == FUSE_TRUNCATE || type == FUSE_TRUNCATE_END);
		if (type == FUSE_TRUNCATE) {
			serializeLegacyPacket(packetBuffer, MATOCL_FUSE_TRUNCATE, messageId, attributes);
		} else {
			// this should never happen, so do anything
			serializeLegacyPacket(packetBuffer, MATOCL_FUSE_TRUNCATE,
					messageId, uint8_t(SAUNAFS_ERROR_ENOTSUP));
		}

	}

	virtual void deserializeFuseTruncate(std::vector<uint8_t>& packetBuffer,
			uint32_t& messageId, inode_t& inode, bool& isOpened,
			uint32_t& uid, uint32_t& gid, uint64_t& length) const {
		deserializeAllLegacyPacketDataNoHeader(packetBuffer,
				messageId, inode, isOpened, uid, gid, length);

	}
};

class SaunaFsPacketSerializer : public PacketSerializer {
public:
	virtual bool isSaunaFsPacketSerializer() const {
		return true;
	}

	virtual void serializeFuseReadChunk(std::vector<uint8_t>& packetBuffer,
			uint32_t messageId, uint8_t status) const {
		matocl::fuseReadChunk::serialize(packetBuffer, messageId, status);
	}

	virtual void serializeFuseReadChunk(std::vector<uint8_t>& packetBuffer,
			uint32_t messageId, uint64_t fileLength, uint64_t chunkId, uint32_t chunkVersion,
			const std::vector<ChunkTypeWithAddress>& chunkCopies) const {
		matocl::fuseReadChunk::serialize(packetBuffer, messageId, fileLength, chunkId, chunkVersion,
				chunkCopies);
	}

	virtual void deserializeFuseReadChunk(const std::vector<uint8_t>& packetBuffer,
			uint32_t& messageId, inode_t& inode, uint32_t& chunkIndex) const {
		cltoma::fuseReadChunk::deserialize(packetBuffer, messageId, inode, chunkIndex);
	}

	virtual void serializeFuseWriteChunk(std::vector<uint8_t>& packetBuffer,
			uint32_t messageId, uint8_t status) const {
		matocl::fuseWriteChunk::serialize(packetBuffer, messageId, status);
	}

	virtual void serializeFuseWriteChunk(std::vector<uint8_t>& packetBuffer,
			uint32_t messageId, uint64_t fileLength,
			uint64_t chunkId, uint32_t chunkVersion, uint32_t lockId,
			const std::vector<ChunkTypeWithAddress>& chunkCopies) const {
		matocl::fuseWriteChunk::serialize(packetBuffer, messageId,
				fileLength, chunkId, chunkVersion, lockId, chunkCopies);
	}

	virtual void deserializeFuseWriteChunk(const std::vector<uint8_t>& packetBuffer,
			uint32_t& messageId, inode_t& inode, uint32_t& chunkIndex, uint32_t& lockId) const {
		cltoma::fuseWriteChunk::deserialize(packetBuffer, messageId, inode, chunkIndex, lockId);
	}

	virtual void serializeFuseWriteChunkEnd(std::vector<uint8_t>& packetBuffer,
			uint32_t messageId, uint8_t status) const {
		matocl::fuseWriteChunkEnd::serialize(packetBuffer, messageId, status);
	}

	virtual void deserializeFuseWriteChunkEnd(const std::vector<uint8_t>& packetBuffer,
			uint32_t& messageId, uint64_t& chunkId, uint32_t& lockId,
			inode_t& inode, uint64_t& fileLength) const {
		cltoma::fuseWriteChunkEnd::deserialize(packetBuffer,
				messageId, chunkId, lockId, inode, fileLength);
	}

	virtual void serializeFuseTruncate(std::vector<uint8_t>& packetBuffer,
			uint32_t type, uint32_t messageId, uint8_t status) const {
		sassert(type == FUSE_TRUNCATE || type == FUSE_TRUNCATE_END);
		if (type == FUSE_TRUNCATE) {
			matocl::fuseTruncate::serialize(packetBuffer, messageId, status);
		} else {
			matocl::fuseTruncateEnd::serialize(packetBuffer, messageId, status);
		}
	}

	virtual void serializeFuseTruncate(std::vector<uint8_t>& packetBuffer,
			uint32_t type, uint32_t messageId, const Attributes& attributes) const {
		sassert(type == FUSE_TRUNCATE || type == FUSE_TRUNCATE_END);
		if (type == FUSE_TRUNCATE) {
			matocl::fuseTruncate::serialize(packetBuffer, messageId, attributes);
		} else {
			matocl::fuseTruncateEnd::serialize(packetBuffer, messageId, attributes);
		}

	}

	virtual void deserializeFuseTruncate(std::vector<uint8_t>& packetBuffer,
			uint32_t& messageId, inode_t& inode, bool& isOpened,
			uint32_t& uid, uint32_t& gid, uint64_t& length) const {
		cltoma::fuseTruncate::deserialize(packetBuffer,
				messageId, inode, isOpened, uid, gid, length);
	}
};

class SaunaFsStdXorPacketSerializer : public SaunaFsPacketSerializer {
public:
	virtual void serializeFuseReadChunk(std::vector<uint8_t>& packetBuffer,
			uint32_t messageId, uint64_t fileLength, uint64_t chunkId, uint32_t chunkVersion,
			const std::vector<ChunkTypeWithAddress>& chunkCopies) const {
		std::vector<legacy::ChunkTypeWithAddress> chunk_copies;
		for (const auto &part : chunkCopies) {
			if ((int)part.chunk_type.getSliceType() >= Goal::Slice::Type::kECFirst) {
				continue;
			}
			chunk_copies.push_back(legacy::ChunkTypeWithAddress(part.address, (legacy::ChunkPartType)part.chunk_type));
		}
		matocl::fuseReadChunk::serialize(packetBuffer, messageId, fileLength, chunkId, chunkVersion,
				chunk_copies);
	}

	virtual void serializeFuseWriteChunk(std::vector<uint8_t>& packetBuffer,
			uint32_t messageId, uint64_t fileLength,
			uint64_t chunkId, uint32_t chunkVersion, uint32_t lockId,
			const std::vector<ChunkTypeWithAddress>& chunkCopies) const {
		std::vector<legacy::ChunkTypeWithAddress> chunk_copies;
		for (const auto &part : chunkCopies) {
			if ((int)part.chunk_type.getSliceType() >= Goal::Slice::Type::kECFirst) {
				continue;
			}
			chunk_copies.push_back(legacy::ChunkTypeWithAddress(part.address, (legacy::ChunkPartType)part.chunk_type));
		}
		matocl::fuseWriteChunk::serialize(packetBuffer, messageId,
				fileLength, chunkId, chunkVersion, lockId, chunk_copies);
	}
};


const PacketSerializer* PacketSerializer::getSerializer(PacketHeader::Type type, uint32_t version) {
	sassert((type >= PacketHeader::kMinSauPacketType && type <= PacketHeader::kMaxSauPacketType)
			|| type <= PacketHeader::kMaxOldPacketType);
	if (type <= PacketHeader::kMaxOldPacketType) {
		static LegacyPacketSerializer singleton;
		return &singleton;
	}

	static SaunaFsPacketSerializer singleton;
	if (version < kFirstECVersion) {
		static SaunaFsStdXorPacketSerializer singletonStdXor;
		return &singletonStdXor;
	}

	return &singleton;
}

void matoclserv_stats(uint64_t stats[5]) {
	stats[0] = statsPacketsReceived;
	stats[1] = statsPacketsSent;
	stats[2] = statsBytesReceived;
	stats[3] = statsBytesSent;

	statsPacketsReceived = 0;
	statsPacketsSent = 0;
	statsBytesReceived = 0;
	statsBytesSent = 0;
}

/// Returns the connection for a given session.
/// @param sessionId The session ID to search for
/// @return Pointer to the matoclserventry if found, nullptr otherwise
matoclserventry *matoclserv_find_connection(uint32_t sessionId) {
	for (const auto& eptr : matoclservList) {
		if (eptr->sessionData && eptr->sessionData->sessionId == sessionId) {
			return eptr.get();
		}
	}
	return nullptr;
}

/// Creates a new session.
/// @param newSession Indicates if this is a new session
/// @param noNewId Indicates if a new session ID should be generated
/// @return Pointer to the newly created session
Session *matoclserv_new_session(uint8_t newSession, uint8_t noNewId) {
	auto sessionPtr = std::make_unique<Session>();
	passert(sessionPtr.get());

	auto newSessionIdNotNeeded = (newSession == 0 && noNewId);
	sessionPtr->sessionId = (newSessionIdNotNeeded) ? 0 : fs_newsessionid();
	sessionPtr->newSession = newSession;
	sessionPtr->connections = 1;
	sessionVector.push_back(std::move(sessionPtr));
	return sessionVector.back().get();
}

/// Returns the session for a given ID.
/// @param sessionId The session ID to search for
/// @return Pointer to the session if found, nullptr otherwise
Session* matoclserv_find_session(uint32_t sessionId) {
	if (sessionId == 0) { return nullptr; }

	for (const auto& sessionPtr : sessionVector) {
		if (sessionPtr->sessionId == sessionId) {
			if (sessionPtr->newSession >= 2) {
				sessionPtr->newSession -= 2;
			}
			sessionPtr->connections++;
			sessionPtr->disconnectedTimestamp = 0;
			return sessionPtr.get();
		}
	}
	return nullptr;
}

/// Closes a session by its ID.
/// @param sessionId The session ID to close
void matoclserv_close_session(uint32_t sessionId) {
	if (sessionId == 0) { return; }

	for (const auto& sessionPtr : sessionVector) {
		if (sessionPtr->sessionId == sessionId) {
			if (sessionPtr->connections == 1 && sessionPtr->newSession < 2) {
				sessionPtr->newSession += 2;
			}
		}
	}
}

/// Stores all sessions to a file.
void matoclserv_store_sessions() {
	uint32_t sessionInfoLength;
	constexpr uint32_t kSessionSerializedSize =
	    sizeof(Session::sessionId) + sizeof(sessionInfoLength) + sizeof(Session::peerIpAddress) +
	    sizeof(Session::rootInode) + sizeof(Session::flags) + sizeof(Session::minGoal) +
	    sizeof(Session::maxGoal) + sizeof(Session::minTrashTime) + sizeof(Session::maxTrashTime) +
	    sizeof(Session::rootUid) + sizeof(Session::rootGid) + sizeof(Session::mapAllUid) +
	    sizeof(Session::mapAllGid);
	constexpr uint32_t kBufferSize = kSessionSerializedSize + (SESSION_STATS * 8);
	std::vector<uint8_t> fsesrecord(kBufferSize); // 4+4+4+4+1+1+1+4+4+4+4+4+4+SESSION_STATS*4+SESSION_STATS*4

	FILE *fd = fopen(kSessionsTmpFilename, "w");
	if (fd == nullptr) {
		safs_silent_errlog(LOG_WARNING,"can't store sessions, open error");
		return;
	}

	memcpy(fsesrecord.data(), SFSSIGNATURE "S \001\006\004", 8);
	uint8_t *ptr = fsesrecord.data() + 8;
	put16bit(&ptr,SESSION_STATS);

	if (fwrite(fsesrecord.data(), 10, 1, fd) != 1) {
		safs_pretty_syslog(LOG_WARNING,"can't store sessions, fwrite error");
		fclose(fd);
		return;
	}

	for (const auto& sessionPtr : sessionVector) {
		if (sessionPtr->newSession == 1) {
			ptr = fsesrecord.data();
			sessionInfoLength = sessionPtr->info.size();

			put32bit(&ptr, sessionPtr->sessionId);
			put32bit(&ptr, sessionInfoLength);
			put32bit(&ptr, sessionPtr->peerIpAddress);
			putINode(&ptr, sessionPtr->rootInode);
			put8bit(&ptr, sessionPtr->flags);
			put8bit(&ptr, sessionPtr->minGoal);
			put8bit(&ptr, sessionPtr->maxGoal);
			put32bit(&ptr, sessionPtr->minTrashTime);
			put32bit(&ptr, sessionPtr->maxTrashTime);
			put32bit(&ptr, sessionPtr->rootUid);
			put32bit(&ptr, sessionPtr->rootGid);
			put32bit(&ptr, sessionPtr->mapAllUid);
			put32bit(&ptr, sessionPtr->mapAllGid);

			for (auto i = 0; i < SESSION_STATS; i++) {
				put32bit(&ptr, sessionPtr->currHourOperationsStats[i]);
			}

			for (auto i = 0; i < SESSION_STATS; i++) {
				put32bit(&ptr, sessionPtr->prevHourOperationsStats[i]);
			}

			if (fwrite(fsesrecord.data(), kBufferSize, 1, fd) != 1) {
				safs::log_warn("can't store sessions, fwrite error");
				fclose(fd);
				return;
			}

			if (sessionInfoLength > 0) {
				if (fwrite(sessionPtr->info.data(), sessionInfoLength, 1, fd) != 1) {
					safs::log_warn("can't store sessions, fwrite error");
					fclose(fd);
					return;
				}
			}
		}
	}

	if (fclose(fd) != 0) {
		safs_silent_errlog(LOG_WARNING,"can't store sessions, fclose error");
		return;
	}

	if (rename(kSessionsTmpFilename, kSessionsFilename) < 0) {
		safs_silent_errlog(LOG_WARNING, "can't store sessions, rename error");
	}
}

#define MFSSIGNATURE "MFS"

/// Loads all sessions from a file.
/// @return 0 on success, -1 on error
int matoclserv_load_sessions() {
	uint32_t sessionInfoLength;
	uint8_t headerBuffer[8];  // for signature and version. e.g. "SFS" " S 1.5"
	std::vector<uint8_t> sessionBuffer;
	const uint8_t *ptr;
	uint8_t mapAllData;
	uint8_t goalTrashData;
	uint32_t statsInFile;
	int bytesRead;

	FILE *fd = fopen(kSessionsFilename, "r");

	if (fd == nullptr) {
		safs_silent_errlog(LOG_WARNING, "can't load sessions, fopen error");
		if (errno == ENOENT) {  // it's ok if file does not exist
			return 0;
		}

		return -1;
	}

	const size_t kSessionsHeaderSize = strlen(SFSSIGNATURE) + 5;

	if (fread(headerBuffer, kSessionsHeaderSize, 1, fd) != 1) {
		safs::log_warn("can't load sessions, fread error");
		fclose(fd);
		return -1;
	}

	// Guillex: Only "S \001\006\004" (last option) is expected
	if (memcmp(headerBuffer, SFSSIGNATURE "S 1.5", kSessionsHeaderSize) == 0 ||
	    memcmp(headerBuffer, MFSSIGNATURE "S 1.5", kSessionsHeaderSize) == 0) {
		mapAllData = 0;
		goalTrashData = 0;
		statsInFile = 16;
	} else if (memcmp(headerBuffer, SFSSIGNATURE "S \001\006\001", kSessionsHeaderSize) == 0 ||
	           memcmp(headerBuffer, MFSSIGNATURE "S \001\006\001", kSessionsHeaderSize) == 0) {
		mapAllData = 1;
		goalTrashData = 0;
		statsInFile = 16;
	} else if (memcmp(headerBuffer, SFSSIGNATURE "S \001\006\002", kSessionsHeaderSize) == 0 ||
	           memcmp(headerBuffer, MFSSIGNATURE "S \001\006\002", kSessionsHeaderSize) == 0) {
		mapAllData = 1;
		goalTrashData = 0;
		statsInFile = 21;
	} else if (memcmp(headerBuffer, SFSSIGNATURE "S \001\006\003", kSessionsHeaderSize) == 0 ||
	           memcmp(headerBuffer, MFSSIGNATURE "S \001\006\003", kSessionsHeaderSize) == 0) {
		mapAllData = 1;
		goalTrashData = 0;
		if (fread(headerBuffer, 2, 1, fd) != 1) {
			safs::log_warn("can't load sessions, fread error");
			fclose(fd);
			return -1;
		}
		ptr = headerBuffer;
		statsInFile = get16bit(&ptr);
	} else if (memcmp(headerBuffer, SFSSIGNATURE "S \001\006\004", kSessionsHeaderSize) == 0 ||
	           memcmp(headerBuffer, MFSSIGNATURE "S \001\006\004", kSessionsHeaderSize) == 0) {
		mapAllData = 1;
		goalTrashData = 1;
		if (fread(headerBuffer, sizeof(uint16_t), 1, fd) != 1) {
			safs::log_warn("can't load sessions, fread error");
			fclose(fd);
			return -1;
		}
		ptr = headerBuffer;
		statsInFile = get16bit(&ptr);
	} else {
		safs::log_warn("can't load sessions, bad header");
		fclose(fd);
		return -1;
	}

	// Compile time constants
	constexpr uint8_t kStatEntrySize =
	    sizeof(std::remove_extent<decltype(Session::currHourOperationsStats)>::type::value_type) +
	    sizeof(std::remove_extent<decltype(Session::prevHourOperationsStats)>::type::value_type);

	constexpr uint32_t kCommonSize = sizeof(Session::sessionId) + sizeof(sessionInfoLength) +
	                                 sizeof(Session::peerIpAddress) + sizeof(Session::rootInode) +
	                                 sizeof(Session::flags) + sizeof(Session::rootUid) +
	                                 sizeof(Session::rootGid);
	constexpr uint32_t kExtraSizeWithMapAll =
	    sizeof(Session::mapAllUid) + sizeof(Session::mapAllGid);
	constexpr uint32_t kExtraSizeWithGoalTrash =
	    sizeof(Session::minGoal) + sizeof(Session::maxGoal) + sizeof(Session::minTrashTime) +
	    sizeof(Session::maxTrashTime);

	// statsInFile is unknown at compile time, we need to use a runtime constant
	const uint32_t kStatsSize = statsInFile * kStatEntrySize;

	if (mapAllData == 0) {
		sessionBuffer.resize(kCommonSize + kStatsSize);
	} else if (goalTrashData == 0) {
		sessionBuffer.resize(kCommonSize + kExtraSizeWithMapAll + kStatsSize);
	} else {
		sessionBuffer.resize(kCommonSize + kExtraSizeWithMapAll + kExtraSizeWithGoalTrash +
		                  kStatsSize);
	}

	while (!feof(fd)) {
		bytesRead = fread(sessionBuffer.data(), sessionBuffer.size(), 1, fd);

		if (bytesRead == 1) {
			ptr = sessionBuffer.data();
			auto sessionPtr = std::make_unique<Session>();
			passert(sessionPtr);
			get32bit(&ptr, sessionPtr->sessionId);
			get32bit(&ptr, sessionInfoLength);
			get32bit(&ptr, sessionPtr->peerIpAddress);
			getINode(&ptr, sessionPtr->rootInode);
			sessionPtr->flags = get8bit(&ptr);
			if (goalTrashData) {
				sessionPtr->minGoal = get8bit(&ptr);
				sessionPtr->maxGoal = get8bit(&ptr);
				get32bit(&ptr, sessionPtr->minTrashTime);
				get32bit(&ptr, sessionPtr->maxTrashTime);
			}
			get32bit(&ptr, sessionPtr->rootUid);
			get32bit(&ptr, sessionPtr->rootGid);
			if (mapAllData) {
				get32bit(&ptr, sessionPtr->mapAllUid);
				get32bit(&ptr, sessionPtr->mapAllGid);
			}
			sessionPtr->newSession = 1;
			sessionPtr->disconnectedTimestamp = eventloop_time();
			for (uint32_t i = 0; i < SESSION_STATS; i++) {
				if (i < statsInFile) {
					get32bit(&ptr, sessionPtr->currHourOperationsStats[i]);
				} else {
					sessionPtr->currHourOperationsStats[i] = 0;
				}
			}

			if (statsInFile > SESSION_STATS) {
				ptr += 4 * (statsInFile - SESSION_STATS);
			}

			for (uint32_t i = 0; i < SESSION_STATS; i++) {
				if (i < statsInFile) {
					get32bit(&ptr, sessionPtr->prevHourOperationsStats[i]);
				} else {
					sessionPtr->prevHourOperationsStats[i] = 0;
				}
			}

			if (sessionInfoLength > 0) {
				sessionPtr->info.resize(sessionInfoLength);
				if (fread(sessionPtr->info.data(), sessionInfoLength, 1, fd) != 1) {
					sessionPtr.reset();
					safs::log_warn("can't load sessions, fread error");
					fclose(fd);
					return -1;
				}
			}

			sessionVector.push_back(std::move(sessionPtr));
		}

		if (ferror(fd)) {
			safs::log_warn("can't load sessions, fread error");
			fclose(fd);
			return -1;
		}
	}

	safs::log_info("sessions have been loaded");
	fclose(fd);
	return 1;
}
#undef MFSSIGNATURE

/// Inserts an open file to the list of open files for a given session.
/// @param currentSession Pointer to the session
/// @param inode The inode of the open file
/// @return SAUNAFS_STATUS_OK if the file was successfully acquired, or an error code otherwise
int matoclserv_insert_open_file(Session *currentSession, inode_t inode) {
	if (currentSession->openFilesSet.contains(inode)) {
		return SAUNAFS_STATUS_OK;  // file already acquired - nothing to do
	}

	int status = fs_acquire(FsContext::getForMaster(eventloop_time()), inode,
	                        currentSession->sessionId);

	if (status == SAUNAFS_STATUS_OK) { currentSession->openFilesSet.insert(inode); }

	return status;
}

/// Adds an open file to the list of open files for a given session.
/// @param sessionId The ID of the session
/// @param inode The inode of the open file
/// If the session exists, the open file will be added to the list of open files of the session.
/// Otherwise, a new session will be created and the open file will be added to the new session.
void matoclserv_add_open_file(uint32_t sessionId, inode_t inode) {
	for (const auto& sessionPtr : sessionVector) {
		if (sessionPtr->sessionId == sessionId) {
			if (!sessionPtr->openFilesSet.contains(inode)) {
				sessionPtr->openFilesSet.insert(inode);
			}
			return;
		}
	}

	// If session does not exist, create a new one
	auto sessionPtr = std::make_unique<Session>();
	passert(sessionPtr.get());
	sessionPtr->sessionId = sessionId;
	/* session created by filesystem - only for old clients (pre 1.5.13) */
	sessionPtr->disconnectedTimestamp = eventloop_time();
	sessionPtr->openFilesSet.insert(inode);
	sessionVector.push_back(std::move(sessionPtr));
}

/// Removes an open file from a given session.
/// @param sessionId The IF of the session
/// @param inode The inode of the open file
void matoclserv_remove_open_file(uint32_t sessionId, inode_t inode) {
	for (const auto& sessionPtr : sessionVector) {
		if (sessionPtr->sessionId == sessionId) {
			if (sessionPtr->openFilesSet.contains(inode)) {
				sessionPtr->openFilesSet.erase(inode);
			}
			return;
		}
	}

	safs::log_err("sessions file is corrupted");
}

/// Resets the session timeouts for all sessions.
void matoclserv_reset_session_timeouts() {
	uint32_t now = eventloop_time();

	for (auto& sessionPtr : sessionVector) {
		sessionPtr->disconnectedTimestamp = now;
	}
}

/// Creates a new output packet for a given session entry.
/// @param eptr Pointer to the client connection in the master
/// @param type The type of the packet
/// @param size The size of the packet data
/// @return Pointer to the start of the packet data
uint8_t *matoclserv_createpacket(matoclserventry *eptr, uint32_t type, uint32_t size) {
	packetstruct *outpacket;

	outpacket = (packetstruct *)malloc(sizeof(packetstruct));
	passert(outpacket);

	uint32_t packetSize = sizeof(type) + sizeof(size) + size;
	outpacket->packet = (uint8_t *)malloc(packetSize);
	passert(outpacket->packet);
	outpacket->bytesLeft = packetSize;

	uint8_t *ptr = outpacket->packet;
	put32bit(&ptr, type);
	put32bit(&ptr, size);
	outpacket->startPtr = (uint8_t *)(outpacket->packet);
	outpacket->next = nullptr;
	*(eptr->outputPacketTail) = outpacket;
	eptr->outputPacketTail = &(outpacket->next);
	return ptr;
}

/// Creates a new output packet for a given session entry.
/// @param eptr Pointer to the client connection in the master
/// @param buffer The message buffer containing the packet data
void matoclserv_createpacket(matoclserventry *eptr, const MessageBuffer &buffer) {
	packetstruct *outpacket = (packetstruct *)malloc(sizeof(packetstruct));
	passert(outpacket);
	outpacket->packet = (uint8_t *)malloc(buffer.size());
	passert(outpacket->packet);

	outpacket->bytesLeft = buffer.size();
	// TODO unificate output packets and remove suboptimal memory copying
	memcpy(outpacket->packet, buffer.data(), buffer.size());
	outpacket->startPtr = outpacket->packet;
	outpacket->next = nullptr;
	*(eptr->outputPacketTail) = outpacket;
	eptr->outputPacketTail = &(outpacket->next);
}

/// Checks if user/group ID remapping is required for a given client connection.
/// @param eptr Pointer to the client connection in the master
/// @param uid The user ID to check
/// @return true if remapping is required, false otherwise
static inline bool matoclserv_ugid_remap_required(matoclserventry *eptr, uint32_t uid) {
	return uid == 0 || eptr->sessionData->flags & SESFLAG_MAPALL;
}

/// Remaps user and group IDs for a given client connection.
/// @param eptr Pointer to the client connection in the master
/// @param auid Pointer to the user ID to remap
/// @param agid Pointer to the group ID to remap
static inline void matoclserv_ugid_remap(matoclserventry *eptr, uint32_t *auid, uint32_t *agid) {
	if (*auid == 0) {
		*auid = eptr->sessionData->rootUid;
		if (agid) {
			*agid = eptr->sessionData->rootGid;
		}
	} else if (eptr->sessionData->flags & SESFLAG_MAPALL) {
		*auid = eptr->sessionData->mapAllUid;
		if (agid) {
			*agid = eptr->sessionData->mapAllGid;
		}
	}
}

/// Checks whether a given group ID is registered in the session cache.
/// @param eptr Pointer to the client connection in the master
/// @param gid The group ID to check
/// @return SAUNAFS_STATUS_OK if the group is registered, SAUNAFS_ERROR_GROUPNOTREGISTERED otherwise
static inline uint8_t matoclserv_check_group_cache(matoclserventry *eptr, uint32_t gid) {
	if (!user_groups::isGroupCacheId(gid)) {
		return SAUNAFS_STATUS_OK;
	}

	assert(eptr && eptr->sessionData);
	auto it = eptr->sessionData->groupsCache.find(user_groups::decodeGroupCacheId(gid));
	return (it == eptr->sessionData->groupsCache.end()) ? SAUNAFS_ERROR_GROUPNOTREGISTERED
	                                                    : SAUNAFS_STATUS_OK;
}

/// Returns FsContext without information about sessions like user ID or group ID
/// @param eptr Pointer to the client connection in the master
/// @return FsContext with session data
static inline FsContext matoclserv_get_context(matoclserventry *eptr) {
	assert(eptr && eptr->sessionData);
	return FsContext::getForMaster(eventloop_time(), eptr->sessionData->rootInode,
	                               eptr->sessionData->flags);
}

/// Returns FsContext with information about sessions like user ID and group ID
/// @param eptr Pointer to the client connection in the master
/// @param uid The user ID to use in the context
/// @param gid The group ID to use in the context
static inline FsContext matoclserv_get_context(matoclserventry *eptr, uint32_t uid, uint32_t gid) {
	assert(eptr && eptr->sessionData);

	if (user_groups::isGroupCacheId(gid)) {
		auto it = eptr->sessionData->groupsCache.find(user_groups::decodeGroupCacheId(gid));
		if (it == eptr->sessionData->groupsCache.end()) {
			throw std::runtime_error("Missing group data in session cache");
		}

		assert(!it->second.empty());

		if (!matoclserv_ugid_remap_required(eptr, uid)) {
			return FsContext::getForMasterWithSession(
			    eventloop_time(), eptr->sessionData->rootInode, eptr->sessionData->flags, uid,
			    it->second, uid, it->second[0]);
		}

		FsContext::GroupsContainer gids;
		gids.reserve(it->second.size());

		for(const auto &orig_gid : it->second) {
			uint32_t tmp_uid = uid;
			uint32_t tmp_gid = orig_gid;
			matoclserv_ugid_remap(eptr, &tmp_uid, &tmp_gid);
			gids.push_back(tmp_gid);
		}

		uint32_t auid = uid;
		matoclserv_ugid_remap(eptr, &uid, nullptr);

		return FsContext::getForMasterWithSession(eventloop_time(), eptr->sessionData->rootInode,
		                                          eptr->sessionData->flags, uid, std::move(gids),
		                                          auid, it->second[0]);
	}

	uint32_t auid = uid;
	uint32_t agid = gid;
	matoclserv_ugid_remap(eptr, &uid, &gid);
	return FsContext::getForMasterWithSession(eventloop_time(), eptr->sessionData->rootInode,
	                                          eptr->sessionData->flags, uid, gid, auid, agid);
}

/// Extracts network addresses of standard chunk copies from a list of all chunk copies.
/// @param allCopies List of all chunk copies with their addresses
/// @param standardCopies Output vector to store the addresses of standard chunk copies
static void getStandardChunkCopies(const std::vector<ChunkTypeWithAddress>& allCopies,
		std::vector<NetworkAddress>& standardCopies) {
	sassert(standardCopies.empty());
	for (auto& chunkCopy : allCopies) {
		if (slice_traits::isStandard(chunkCopy.chunk_type)) {
			standardCopies.push_back(chunkCopy.address);
		}
	}
}

/// Removes unsupported EC parts from a given list of chunk parts.
/// @param version The client version
/// @param chunksList The list of chunk parts to filter
static void remove_unsupported_ec_parts(uint32_t version,
                                        std::vector<ChunkTypeWithAddress> &chunksList) {
	auto it = std::remove_if(chunksList.begin(), chunksList.end(),
	     [version](const ChunkTypeWithAddress &entry) {
		return slice_traits::isEC(entry.chunk_type) &&
		       slice_traits::ec::isEC2Part(entry.chunk_type) &&
		       (version < kEC2Version || entry.chunkserver_version < kEC2Version);
	});
	chunksList.erase(it, chunksList.end());
}

/// Responds to a FUSE write chunk request.
/// @param eptr Pointer to the client connection in the master
/// @param serializer The packet serializer to use
/// @param chunkId The ID of the chunk being written
/// @param messageId The ID of the message
/// @param fileLength The length of the file being written
/// @param lockId The lock ID for the chunk
/// @return SAUNAFS_STATUS_OK on success, or an error code otherwise
uint8_t matoclserv_fuse_write_chunk_respond(matoclserventry *eptr,
                                            const PacketSerializer *serializer, uint64_t chunkId,
                                            uint32_t messageId, uint64_t fileLength,
                                            uint32_t lockId) {
	uint32_t chunkVersion;
	std::vector<ChunkTypeWithAddress> allChunkCopies;
	uint8_t status = chunk_getversionandlocations(chunkId, eptr->peerIpAddress, chunkVersion,
			kMaxNumberOfChunkCopies, allChunkCopies);

	remove_unsupported_ec_parts(eptr->version, allChunkCopies);

	// don't allow old clients to modify standard copy of a xor chunk
	if (status == SAUNAFS_STATUS_OK && !serializer->isSaunaFsPacketSerializer()) {
		for (const ChunkTypeWithAddress& chunkCopy : allChunkCopies) {
			if (!slice_traits::isStandard(chunkCopy.chunk_type)) {
				safs::log_err(
				    "matoclserv_fuse_write_chunk_respond: client tried to modify "
				    "standard copy of a xor chunk, chunkID {}",
				    chunkId);
				status = SAUNAFS_ERROR_NOCHUNK;
				break;
			}
		}
	}

	std::vector<uint8_t> outMessage;
	if (status == SAUNAFS_STATUS_OK) {
		serializer->serializeFuseWriteChunk(outMessage, messageId, fileLength,
				chunkId, chunkVersion, lockId, allChunkCopies);
	} else {
		serializer->serializeFuseWriteChunk(outMessage, messageId, status);
	}

	matoclserv_createpacket(eptr, outMessage);
	return status;
}

void matoclserv_chunk_status(uint64_t chunkId, uint8_t status) {
	DelayedChunkOperation *operation;
	const PacketSerializer *serializer;

	matoclserventry *eptr = nullptr;
	uint32_t lockId = 0;
	uint32_t messageId = 0;
	uint64_t fileLength = 0;
	uint8_t operationType = 0;
	inode_t inode = 0;
	uint32_t uid = 0;
	uint32_t gid = 0;
	uint32_t auid = 0;
	uint32_t agid = 0;
	serializer = nullptr;

	auto eptrIterator = matoclservList.begin();
	for (; eptrIterator != matoclservList.end() && eptr == nullptr; eptrIterator++) {
		matoclserventry* eaptr = eptrIterator->get();
		if (eaptr->mode != ClientConnectionMode::KILL) {
			auto chunkOpsIterator = eaptr->delayedChunkOperations.begin();
			while (chunkOpsIterator != eaptr->delayedChunkOperations.end() && eptr == nullptr) {
				operation = chunkOpsIterator->get();
				if (operation->chunkId == chunkId) {
					eptr = eaptr;
					messageId = operation->messageId;
					fileLength = operation->fileLength;
					lockId = operation->lockId;
					operationType = operation->type;
					inode = operation->inode;
					uid = operation->uid;
					gid = operation->gid;
					auid = operation->auid;
					agid = operation->agid;
					serializer = operation->serializer;

					chunkOpsIterator = eaptr->delayedChunkOperations.erase(chunkOpsIterator);
				} else {
					++chunkOpsIterator;
				}
			}
		}
	}

	if (!eptr) {
		safs_pretty_syslog(LOG_WARNING,"got chunk status, but don't want it");
		return;
	}
	if (status == SAUNAFS_STATUS_OK) { dcm_modify(inode, eptr->sessionData->sessionId); }

	std::vector<uint8_t> reply;
	FsContext context =
	    FsContext::getForMasterWithSession(eventloop_time(), eptr->sessionData->rootInode,
	                                       eptr->sessionData->flags, uid, gid, auid, agid);

	switch (operationType) {
	case FUSE_WRITE:
		if (status != SAUNAFS_STATUS_OK) {
			serializer->serializeFuseWriteChunk(reply, messageId, status);
			matoclserv_createpacket(eptr, std::move(reply));
		} else {
			status = matoclserv_fuse_write_chunk_respond(eptr, serializer,
					chunkId, messageId, fileLength, lockId);
		}
		if (status != SAUNAFS_STATUS_OK) {
			fs_writeend(0, 0, chunkId, 0); // ignore status - just do it.
		}
		return;
	case FUSE_TRUNCATE_BEGIN:
		if (status != SAUNAFS_STATUS_OK) {
			matocl::fuseTruncate::serialize(reply, messageId, status);
		} else {
			matocl::fuseTruncate::serialize(reply, messageId, fileLength, lockId);
		}
		matoclserv_createpacket(eptr, std::move(reply));
		return;
	case FUSE_TRUNCATE:
	case FUSE_TRUNCATE_END:
		fs_end_setlength(chunkId);
		if (status != SAUNAFS_STATUS_OK) {
			serializer->serializeFuseTruncate(reply, operationType, messageId, status);
		} else {
			Attributes attr;
			fs_do_setlength(context, inode, fileLength, attr);
			serializer->serializeFuseTruncate(reply, operationType, messageId, attr);
		}
		matoclserv_createpacket(eptr, std::move(reply));
		return;
	default:
		safs_pretty_syslog(LOG_WARNING,"got chunk status, but operation type is unknown");
	}
}

/// Handles the CLTOMA_CSERV_LIST command, which lists all chunkservers.
/// @param eptr Pointer to the client connection in the master
/// @param data Pointer to the data received from the client
/// @param length The length of the data received
void matoclserv_cserv_list(matoclserventry *eptr, const uint8_t */*data*/, uint32_t length) {
	if (length != 0) {
		safs::log_info("CLTOMA_CSERV_LIST - wrong size ({}/0)",length);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	constexpr uint32_t kCSSerializedSize =
	    sizeof(ChunkserverListEntry::version) + sizeof(ChunkserverListEntry::servip) +
	    sizeof(ChunkserverListEntry::servport) + sizeof(ChunkserverListEntry::usedspace) +
	    sizeof(ChunkserverListEntry::totalspace) + sizeof(ChunkserverListEntry::chunkscount) +
	    sizeof(ChunkserverListEntry::todelusedspace) +
	    sizeof(ChunkserverListEntry::todeltotalspace) +
	    sizeof(ChunkserverListEntry::todelchunkscount) + sizeof(ChunkserverListEntry::errorcounter);

	auto listOfChunkservers = csdb_chunkserver_list();
	uint8_t *ptr = matoclserv_createpacket(eptr, MATOCL_CSERV_LIST,
	                                       kCSSerializedSize * listOfChunkservers.size());

	for (const auto &server : listOfChunkservers) {
		put32bit(&ptr, server.version);
		put32bit(&ptr, server.servip);
		put16bit(&ptr, server.servport);
		put64bit(&ptr, server.usedspace);
		put64bit(&ptr, server.totalspace);
		put32bit(&ptr, server.chunkscount);
		put64bit(&ptr, server.todelusedspace);
		put64bit(&ptr, server.todeltotalspace);
		put32bit(&ptr, server.todelchunkscount);
		put32bit(&ptr, server.errorcounter);
	}
}

/// Handles the SAU_CLTOMA_CSERV_LIST command.
/// @param eptr Pointer to the client connection in the master
/// @param data Pointer to the data received from the client
/// @param length The length of the data received
/// This function serializes the list of chunkservers and sends it back to the client.
void matoclserv_sau_cserv_list(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	MessageBuffer buffer;
	PacketVersion version;
	bool dummy;

	deserializePacketVersionNoHeader(data, length, version);
	if (version == cltoma::cservList::kStandard) {
		matocl::cservList::serialize(buffer, csdb_chunkserver_list());
	} else if (version == cltoma::cservList::kWithMessageId) {
		uint32_t messageId = 0;
		cltoma::cservList::deserialize(data, length, messageId, dummy);
		matocl::cservList::serialize(buffer, messageId, csdb_chunkserver_list());
	} else {
		safs::log_info("SAU_CSERV_LIST - wrong packet version {}", version);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	matoclserv_createpacket(eptr, std::move(buffer));
}

/// Handles the CLTOMA_CSSERV_REMOVESERV command, which removes a chunkserver.
/// @param eptr Pointer to the client connection in the master
/// @param data Pointer to the data received from the client
/// @param length The length of the data received
/// This function extracts the IP address and port of the chunkserver that will be removed
/// from the database.
void matoclserv_cserv_removeserv(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t ip;
	uint16_t port;

	constexpr uint32_t kExpectedSize = sizeof(ip) + sizeof(port);

	if (length != kExpectedSize) {
		safs::log_info("CLTOMA_CSSERV_REMOVESERV - wrong size ({}/{})", length, kExpectedSize);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, ip);
	port = get16bit(&data);

	csdb_remove_server(ip, port);

	matoclserv_createpacket(eptr, MATOCL_CSSERV_REMOVESERV, 0);
}

/// Handles the SAU_CLTOMA_IOLIMITS_STATUS command, which retrieves the I/O limits status.
/// @param eptr Pointer to the client connection in the master
/// @param data Pointer to the data received from the client
/// @param length The length of the data received
/// This function retrieves the I/O limits configuration and serializes it into a response packet.
void matoclserv_iolimits_status(matoclserventry* eptr, const uint8_t* data, uint32_t length) {
	uint32_t messageId;
	cltoma::iolimitsStatus::deserialize(data, length, messageId);

	MessageBuffer buffer;
	matocl::iolimitsStatus::serialize(buffer,
			messageId,
			gIoLimitsConfigId,
			gIoLimitsRefreshTime * 1000 * 1000,
			gIoLimitsAccumulate_ms,
			gIoLimitsSubsystem,
			gIoLimitsDatabase.getGroupsAndLimits());

	matoclserv_createpacket(eptr, std::move(buffer));
}

/// Handles the SAU_CLTOMA_METADATASERVER_STATUS command, which retrieves the metadata server status.
/// @param eptr Pointer to the client connection in the master
/// @param data Pointer to the data received from the client
/// @param length The length of the data received
void matoclserv_metadataserver_status(matoclserventry* eptr, const uint8_t* data, uint32_t length) {
	uint32_t messageId = 0;
	cltoma::metadataserverStatus::deserialize(data, length, messageId);

	uint64_t metadataVersion = 0;
	try {
		metadataVersion = fs_getversion();
	} catch (NoMetadataException &) {}

	uint8_t status =
	    metadataserver::isMaster()
	        ? SAU_METADATASERVER_STATUS_MASTER
	        : (masterconn_is_connected() ? SAU_METADATASERVER_STATUS_SHADOW_CONNECTED
	                                     : SAU_METADATASERVER_STATUS_SHADOW_DISCONNECTED);

	MessageBuffer buffer;
	matocl::metadataserverStatus::serialize(buffer, messageId, status, metadataVersion);
	matoclserv_createpacket(eptr, std::move(buffer));
}

/// Handles the SAU_CLTOMA_LIST_GOALS command, which lists all goals defined in the system.
/// @param eptr Pointer to the client connection in the master
///
/// This function retrieves the goal definitions from the filesystem and serializes them into a
/// response packet.
void matoclserv_list_goals(matoclserventry* eptr) {
	std::vector<SerializedGoal> serializedGoals;
	const std::map<int, Goal>& goalsMap = fs_get_goal_definitions();

	for (const auto& goal : goalsMap) {
		serializedGoals.emplace_back(goal.first, goal.second.getName(), to_string(goal.second));
	}

	matoclserv_createpacket(eptr, matocl::listGoals::build(serializedGoals));
}

/// Handles the SAU_CLTOMA_CHUNKS_HEALTH command, which retrieves the health status of chunks.
/// @param eptr Pointer to the client connection in the master
/// @param data Pointer to the data received from the client
/// @param length The length of the data received
///
/// This function deserializes the request data to determine if only regular chunks should be
/// considered for health checks, and then builds a response message containing the health status
/// of the chunks, including their availability and replication states.
void matoclserv_chunks_health(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	bool regularChunksOnly;
	cltoma::chunksHealth::deserialize(data, length, regularChunksOnly);
	auto message = matocl::chunksHealth::build(regularChunksOnly,
			chunk_get_availability_state(),
			chunk_get_replication_state());

	matoclserv_createpacket(eptr, std::move(message));
}

/// Handles the CLTOMA_SESSION_LIST command, which lists all active sessions.
/// @param eptr Pointer to the client connection in the master
/// @param data Pointer to the data received from the client
/// @param length The length of the data received
///
/// This function retrieves the session information for all active sessions and serializes it into
/// a response packet. The response includes session statistics, user and group IDs, and other
/// session-related data.
void matoclserv_session_list(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t sessionInfoLength;
	uint32_t pathLength;
	uint8_t vmode;

	if (length != 0 && length != sizeof(vmode)) {
		safs::log_info("CLTOMA_SESSION_LIST - wrong size ({}/(0|{}))", length, sizeof(vmode));
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	if (length == 0) {
		(void)data;
		vmode = 0;
	} else {
		vmode = get8bit(&data);
	}

	uint32_t size = sizeof(uint16_t);  // 2 bytes for SESSION_STATS

	constexpr uint32_t kExtraVModeSize = sizeof(Session::minGoal) + sizeof(Session::maxGoal) +
	                                     sizeof(Session::minTrashTime) +
	                                     sizeof(Session::maxTrashTime);

	constexpr uint32_t kCommonSessionSize =
	    sizeof(Session::sessionId) + sizeof(matoclserventry::peerIpAddress) +
	    sizeof(matoclserventry::version) + sizeof(sessionInfoLength) + sizeof(Session::flags) +
	    sizeof(Session::rootUid) + sizeof(Session::rootGid) + sizeof(Session::mapAllUid) +
	    sizeof(Session::mapAllGid);

	constexpr uint32_t kCurrentPlusLastHourEntrySize = sizeof(uint32_t) + sizeof(uint32_t);

	for (const auto &eaptr : matoclservList) {
		if (eaptr->mode != ClientConnectionMode::KILL && eaptr->sessionData &&
		    eaptr->registered == ClientState::kRegistered) {
			size += kCommonSessionSize + (SESSION_STATS * kCurrentPlusLastHourEntrySize) +
			        (vmode ? kExtraVModeSize : 0);

			if (!eaptr->sessionData->info.empty()) {
				size += eaptr->sessionData->info.size();
			}

			if (eaptr->sessionData->rootInode == 0) {
				size += sizeof(Session::rootInode);
				size += 1;  // for '.'
			} else {
				size += sizeof(pathLength);
				size += fs_getdirpath_size(eaptr->sessionData->rootInode);
			}
		}
	}

	uint8_t *ptr = matoclserv_createpacket(eptr, MATOCL_SESSION_LIST, size);

	put16bit(&ptr, SESSION_STATS);

	for (const auto &eaptr : matoclservList) {
		if (eaptr->mode != ClientConnectionMode::KILL && eaptr->sessionData &&
		    eaptr->registered == ClientState::kRegistered) {
			put32bit(&ptr, eaptr->sessionData->sessionId);
			put32bit(&ptr, eaptr->peerIpAddress);
			put32bit(&ptr, eaptr->version);

			if (!eaptr->sessionData->info.empty()) {
				sessionInfoLength = eaptr->sessionData->info.size();
				put32bit(&ptr, sessionInfoLength);
				memcpy(ptr, eaptr->sessionData->info.data(), sessionInfoLength);
				ptr += sessionInfoLength;
			} else {
				put32bit(&ptr, 0);
			}

			if (eaptr->sessionData->rootInode == 0) {
				putINode(&ptr, static_cast<inode_t>(1));
				put8bit(&ptr, '.');
			} else {
				pathLength = fs_getdirpath_size(eaptr->sessionData->rootInode);
				put32bit(&ptr, pathLength);
				if (pathLength > 0) {
					fs_getdirpath_data(eaptr->sessionData->rootInode, ptr, pathLength);
					ptr += pathLength;
				}
			}

			put8bit(&ptr, eaptr->sessionData->flags);
			put32bit(&ptr, eaptr->sessionData->rootUid);
			put32bit(&ptr, eaptr->sessionData->rootGid);
			put32bit(&ptr, eaptr->sessionData->mapAllUid);
			put32bit(&ptr, eaptr->sessionData->mapAllGid);

			if (vmode) {
				put8bit(&ptr, eaptr->sessionData->minGoal);
				put8bit(&ptr, eaptr->sessionData->maxGoal);
				put32bit(&ptr, eaptr->sessionData->minTrashTime);
				put32bit(&ptr, eaptr->sessionData->maxTrashTime);
			}

			if (eaptr->sessionData) {
				for (auto i = 0; i < SESSION_STATS; i++) {
					put32bit(&ptr, eaptr->sessionData->currHourOperationsStats[i]);
				}
				for (auto i = 0; i < SESSION_STATS; i++) {
					put32bit(&ptr, eaptr->sessionData->prevHourOperationsStats[i]);
				}
			} else {
				memset(ptr, 0xFF,
				       kCurrentPlusLastHourEntrySize * static_cast<size_t>(SESSION_STATS));
				ptr += kCurrentPlusLastHourEntrySize * SESSION_STATS;
			}
		}
	}
}

/// Handles the SAU_CLTOMA_MOUNT_INFO_LIST command, which retrieves the list of mount information
/// for all active sessions.
/// @param eptr Pointer to the client connection in the master
/// @param data Pointer to the data received from the client
/// @param length The length of the data received
///
/// This function serializes the mount information for each active session and sends it back to the
/// client. Each entry in the list contains the session ID and the mount information.
void matoclserv_mount_info_list(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	std::vector<MountInfoEntry> mountInfoList;

	cltoma::mountInfoList::deserialize(data, length);

	for (const auto &eaptr : matoclservList) {
		if (eaptr->mode != ClientConnectionMode::KILL && eaptr->sessionData != nullptr &&
		    eaptr->registered == ClientState::kRegistered) {
			MountInfoEntry entry;
			entry.sessionId = eaptr->sessionData->sessionId;
			entry.mountInfo = eaptr->sessionData->mountInfo;
			mountInfoList.push_back(entry);
		}
	}

	matoclserv_createpacket(eptr, matocl::mountInfoList::build(mountInfoList));
}

/// Handles the CLTOAN_CHART command, which generates a chart based on the provided chart ID.
/// @param eptr Pointer to the client connection in the master
/// @param data Pointer to the data received from the client
/// @param length The length of the data received
///
/// This function retrieves the chart data based on the chart ID and sends it back to the client
/// as a PNG or CSV file, depending on the chart ID.
void matoclserv_chart(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t chartId = 0;
	uint8_t *ptr;
	uint32_t chartLength;

	if (length != sizeof(chartId)) {
		safs::log_info("CLTOAN_CHART - wrong size ({}/{})", length, sizeof(chartId));
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, chartId);

	if (chartId <= CHARTS_CSV_CHARTID_BASE) {
		chartLength = charts_make_png(chartId);
		ptr = matoclserv_createpacket(eptr, ANTOCL_CHART, chartLength);
		if (chartLength > 0) { charts_get_png(ptr); }
	} else {
		chartLength = charts_make_csv(chartId % CHARTS_CSV_CHARTID_BASE);
		ptr = matoclserv_createpacket(eptr, ANTOCL_CHART, chartLength);
		if (chartLength > 0) { charts_get_csv(ptr); }
	}
}

/// Handles the CLTOAN_CHART_DATA command, which retrieves data for a specific chart.
/// @param eptr Pointer to the client connection in the master
/// @param data Pointer to the data received from the client
/// @param length The length of the data received
///
/// This function retrieves the data for the specified chart ID and sends it back to the client.
void matoclserv_chart_data(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t chartId = 0;
	uint8_t *ptr;
	uint32_t chartsDataLength;

	if (length != sizeof(chartId)) {
		safs::log_info("CLTOAN_CHART_DATA - wrong size ({}/{})", length, sizeof(chartId));
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, chartId);

	chartsDataLength = charts_datasize(chartId);

	ptr = matoclserv_createpacket(eptr, ANTOCL_CHART_DATA, chartsDataLength);

	if (chartsDataLength > 0) { charts_makedata(ptr, chartId); }
}

/// Handles the CLTOMA_INFO command, which retrieves information about the SaunaFS system.
/// @param eptr Pointer to the client connection in the master
/// @param data Pointer to the data received from the client
/// @param length The length of the data received
///
/// This function gathers various statistics about the SaunaFS system, including total space,
/// available space, trash space, reserved space, and memory usage. It then serializes this
/// information into a response packet and sends it back to the client.
void matoclserv_info(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	SaunaFsStatistics statistics;
	(void)data;

	if (length != 0) {
		safs::log_info("CLTOMA_INFO - wrong size ({}/0)", length);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	statistics.version = saunafsVersion(SAUNAFS_PACKAGE_VERSION_MAJOR,
			SAUNAFS_PACKAGE_VERSION_MINOR, SAUNAFS_PACKAGE_VERSION_MICRO);

	fs_info(&statistics.totalSpace, &statistics.availableSpace,
	        &statistics.trashSpace, &statistics.trashNodes,
	        &statistics.reservedSpace, &statistics.reservedNodes,
	        &statistics.allNodes, &statistics.dirNodes, &statistics.fileNodes,
	        &statistics.symlinkNodes);

	chunk_info(&statistics.chunks, &statistics.chunkCopies, &statistics.regularCopies);

	statistics.memoryUsage = chartsdata_memusage();

	std::vector<uint8_t> response;
	serializeLegacyPacket(response, MATOCL_INFO, statistics);
	matoclserv_createpacket(eptr, response);
}

/// Handles the CLTOMA_FSTEST_INFO command, which retrieves filesystem test information.
/// @param eptr Pointer to the client connection in the master
/// @param data Pointer to the data received from the client
/// @param length The length of the data received
///
/// This function gathers various statistics about the filesystem, including loop start and end,
/// number of files, chunks, and user/group files. It then serializes this information into a
/// response packet and sends it back to the client.
void matoclserv_fstest_info(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t loopStart;
	uint32_t loopEnd;
	uint32_t chunks;
	uint32_t underGoalChunks;
	uint32_t missingChunks;
	inode_t files;
	inode_t underGoalFiles;
	inode_t missingFiles;
	uint8_t *ptr;
	(void)data;

	if (length != 0) {
		safs::log_info("CLTOMA_FSTEST_INFO - wrong size ({}/0)", length);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	std::string report;
	fs_test_getdata(loopStart, loopEnd, files, underGoalFiles, missingFiles, chunks,
	                underGoalChunks, missingChunks, report);

	constexpr uint32_t kPacketExtraSize = sizeof(loopStart) + sizeof(loopEnd) + sizeof(files) +
	                                      sizeof(underGoalFiles) + sizeof(missingFiles) +
	                                      sizeof(chunks) + sizeof(underGoalChunks) +
	                                      sizeof(missingChunks) + sizeof(uint32_t);
	ptr = matoclserv_createpacket(eptr, MATOCL_FSTEST_INFO, report.size() + kPacketExtraSize);

	put32bit(&ptr, loopStart);
	put32bit(&ptr, loopEnd);
	putINode(&ptr, files);
	putINode(&ptr, underGoalFiles);
	putINode(&ptr, missingFiles);
	put32bit(&ptr, chunks);
	put32bit(&ptr, underGoalChunks);
	put32bit(&ptr, missingChunks);
	put32bit(&ptr, (uint32_t)report.size());

	if (!report.empty()) { memcpy(ptr, report.c_str(), report.size()); }
}

/// Handles the CLTOMA_CHUNKSTEST_INFO command, which retrieves the serialized size of the
/// information related to the process of testing chunks.
/// @param eptr Pointer to the client connection in the master
/// @param data Pointer to the data received from the client
/// @param length The length of the data received
void matoclserv_chunkstest_info(matoclserventry *eptr,const uint8_t *data,uint32_t length) {
	uint8_t *ptr;
	(void)data;

	if (length != 0) {
		safs::log_info("CLTOMA_CHUNKSTEST_INFO - wrong size ({}/0)", length);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	auto chunksInfoSize = get_chunk_info_serialized_size();

	ptr = matoclserv_createpacket(eptr, MATOCL_CHUNKSTEST_INFO, chunksInfoSize);
	chunk_store_info(ptr);
}

void matoclserv_chunks_matrix(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint8_t *ptr;
	uint8_t matrixId;
	(void)data;

	if (length > sizeof(matrixId)) {
		safs::log_info("CLTOMA_CHUNKS_MATRIX - wrong size ({}/0|{})", length, sizeof(matrixId));
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	if (length == 1) {
		matrixId = get8bit(&data);
	} else {
		matrixId = 0;
	}

	ptr = matoclserv_createpacket(eptr, MATOCL_CHUNKS_MATRIX,
	                              CHUNK_MATRIX_SIZE * CHUNK_MATRIX_SIZE * sizeof(uint32_t));

	chunk_store_chunkcounters(ptr, matrixId);
}

void matoclserv_exports_info(matoclserventry *eptr,const uint8_t *data,uint32_t length) {
	uint8_t *ptr;
	uint8_t vmode;

	if (length != 0 && length != sizeof(vmode)) {
		safs::log_info("CLTOMA_EXPORTS_INFO - wrong size ({}/0|{})", length, sizeof(vmode));
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	if (length == 0) {
		vmode = 0;
	} else {
		vmode = get8bit(&data);
	}

	ptr = matoclserv_createpacket(eptr, MATOCL_EXPORTS_INFO, exports_info_size(vmode));
	exports_info_data(vmode, ptr);
}

void matoclserv_mlog_list(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint8_t *ptr;
	(void)data;

	if (length != 0) {
		safs::log_info("CLTOMA_MLOG_LIST - wrong size ({}/0)", length);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	ptr = matoclserv_createpacket(eptr, MATOCL_MLOG_LIST, matomlserv_mloglist_size());
	matomlserv_mloglist_data(ptr);
}

void matoclserv_metadataservers_list(matoclserventry* eptr, const uint8_t* data, uint32_t length) {
	cltoma::metadataserversList::deserialize(data, length);
	matoclserv_createpacket(eptr, matocl::metadataserversList::build(SAUNAFS_VERSHEX,
			matomlserv_shadows()));
}

static void matoclserv_send_iolimits_cfg(matoclserventry *eptr) {
	MessageBuffer buffer;
	matocl::iolimitsConfig::serialize(buffer, gIoLimitsConfigId,
			gIoLimitsRefreshTime * 1000 * 1000, gIoLimitsSubsystem,
			gIoLimitsDatabase.getGroups());
	matoclserv_createpacket(eptr, buffer);
}

static void matoclserv_broadcast_iolimits_cfg() {
	for (const auto &eptr : matoclservList) {
		if (eptr->ioLimitsEnabled) {
			matoclserv_send_iolimits_cfg(eptr.get());
		}
	}
}

void matoclserv_ping(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t size;
	deserializeAllLegacyPacketDataNoHeader(data, length, size);
	matoclserv_createpacket(eptr, ANTOAN_PING_REPLY, size);
}

void matoclserv_fuse_register(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	const uint8_t *rptr;
	uint8_t *wptr;
	uint32_t sessionId;
	uint8_t status;

	constexpr uint32_t kBlobSize = REGISTER_BLOB_SIZE;
	constexpr uint32_t kBlobSizeWithVersion = kBlobSize + sizeof(matoclserventry::version);
	constexpr uint32_t kBlobSizeWithSessionIdAndVersion =
	    kBlobSizeWithVersion + sizeof(Session::sessionId);

	if (starting) {
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	if (length < kBlobSize) {
		safs::log_info("CLTOMA_FUSE_REGISTER - wrong size ({}/<{})", length, kBlobSize);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	uint8_t toolsNoACL = (memcmp(data, FUSE_REGISTER_BLOB_TOOLS_NOACL, kBlobSize) == 0) ? 1 : 0;
	uint8_t clientsNoACL = (memcmp(data, FUSE_REGISTER_BLOB_NOACL, kBlobSize) == 0) ? 1 : 0;
	uint8_t clientsWithACL = (memcmp(data, FUSE_REGISTER_BLOB_ACL, kBlobSize) == 0) ? 1 : 0;

	// Unregistered no ACL clients and tools
	if (eptr->registered == ClientState::kUnregistered && (clientsNoACL || toolsNoACL)) {
		if (RejectOld) {
			safs::log_info("CLTOMA_FUSE_REGISTER/NOACL - rejected (option REJECT_OLD_CLIENTS is set)");
			eptr->mode = ClientConnectionMode::KILL;
			return;
		}
		if (toolsNoACL) {
			if (length != kBlobSize && length != kBlobSizeWithVersion) {
				safs::log_info("CLTOMA_FUSE_REGISTER/NOACL-TOOLS - wrong size ({}/{}|{})", length,
				               kBlobSize, kBlobSizeWithVersion);
				eptr->mode = ClientConnectionMode::KILL;
				return;
			}
		} else {  // clientsNoACL
			if (length != kBlobSizeWithVersion && length != kBlobSizeWithSessionIdAndVersion) {
				safs::log_info("CLTOMA_FUSE_REGISTER/NOACL-MOUNT - wrong size ({}/{}|{})", length,
				               kBlobSizeWithVersion, kBlobSizeWithSessionIdAndVersion);
				eptr->mode = ClientConnectionMode::KILL;
				return;
			}
		}

		rptr = data + kBlobSize;

		if (toolsNoACL) {
			sessionId = 0;
			if (length == kBlobSizeWithVersion) { get32bit(&rptr, eptr->version); }
		} else {
			get32bit(&rptr, sessionId);
			if (length == kBlobSizeWithSessionIdAndVersion) { get32bit(&rptr, eptr->version); }
		}

		if (eptr->version < 0x010500 && !toolsNoACL) {
			safs::log_info("got register packet from mount older than 1.5 - rejecting");
			eptr->mode = ClientConnectionMode::KILL;
			return;
		}

		if (sessionId == 0) {           // new session
			status = SAUNAFS_STATUS_OK; // exports_check(eptr->peerip,(const uint8_t*)"",NULL,NULL,&sesflags);      // check privileges for '/' w/o password
			eptr->sessionData = matoclserv_new_session(0, toolsNoACL);

			if (eptr->sessionData == nullptr) {
				safs::log_info("can't allocate session record");
				eptr->mode = ClientConnectionMode::KILL;
				return;
			}

			eptr->sessionData->rootInode = SPECIAL_INODE_ROOT;
			eptr->sessionData->flags = 0;
			eptr->sessionData->peerIpAddress = eptr->peerIpAddress;
			eptr->sessionData->peerPort = eptr->peerPort;
		} else {  // reconnect or tools
			eptr->sessionData = matoclserv_find_session(sessionId);
			if (eptr->sessionData == nullptr) {      // in old model if session doesn't exist then create it
				eptr->sessionData = matoclserv_new_session(0, 0);

				if (eptr->sessionData == nullptr) {
					safs::log_info("can't allocate session record");
					eptr->mode = ClientConnectionMode::KILL;
					return;
				}

				eptr->sessionData->rootInode = SPECIAL_INODE_ROOT;
				eptr->sessionData->flags = 0;
				eptr->sessionData->peerIpAddress = eptr->peerIpAddress;
				eptr->sessionData->peerPort = eptr->peerPort;
				status = SAUNAFS_STATUS_OK;
			} else if (eptr->sessionData->peerIpAddress == 0) {  // created by "filesystem"
				eptr->sessionData->peerIpAddress = eptr->peerIpAddress;
				eptr->sessionData->peerPort = eptr->peerPort;
				status = SAUNAFS_STATUS_OK;
			} else if (eptr->sessionData->peerIpAddress == eptr->peerIpAddress) {
				status = SAUNAFS_STATUS_OK;
			} else {
				status = SAUNAFS_ERROR_EACCES;
			}
		}

		// answer

		if (toolsNoACL) {
			wptr = matoclserv_createpacket(eptr, MATOCL_FUSE_REGISTER, sizeof(status));
		} else {
			wptr = matoclserv_createpacket(
			    eptr, MATOCL_FUSE_REGISTER,
			    (status != SAUNAFS_STATUS_OK) ? sizeof(status) : sizeof(sessionId));
		}

		if (status != SAUNAFS_STATUS_OK) {
			put8bit(&wptr, status);
			return;
		}

		if (toolsNoACL) {
			put8bit(&wptr, status);
		} else {
			sessionId = eptr->sessionData->sessionId;
			put32bit(&wptr, sessionId);
		}

		eptr->registered = (toolsNoACL) ? ClientState::kOldTools : ClientState::kRegistered;
		return;
	}

	// clients with ACL support and new tools
	if (clientsWithACL) {
		inode_t rootInode;
		uint8_t sessionFlags;
		uint8_t minGoal, maxGoal;
		uint32_t minTrashTime, maxTrashTime;
		uint32_t rootUid, rootGid;
		uint32_t mapAllUid, mapAllGid;
		uint32_t infoLength, pathLength;
		uint8_t rcode;
		const uint8_t *path;
		const char *info;

		constexpr uint32_t kBlobSizeWithRCode = kBlobSize + sizeof(rcode);
		constexpr uint32_t kRegisterNewMetaSessionMinSize =
		    kBlobSizeWithRCode + sizeof(eptr->version) + sizeof(infoLength);
		constexpr uint32_t kRegisterNewSessionMinSize =
		    kRegisterNewMetaSessionMinSize + sizeof(pathLength);
		constexpr uint32_t kRegisterWithSessionIdAndVersion =
		    kBlobSizeWithRCode + sizeof(sessionId) + sizeof(eptr->version);

		if (length < kBlobSizeWithRCode) {
			safs::log_info("CLTOMA_FUSE_REGISTER/ACL - wrong size ({}/<{})", length,
			               kBlobSizeWithRCode);
			eptr->mode = ClientConnectionMode::KILL;
			return;
		}

		rptr = data + kBlobSize;
		rcode = get8bit(&rptr);

		if ((eptr->registered == ClientState::kUnregistered && rcode == REGISTER_CLOSESESSION) ||
		    (eptr->registered != ClientState::kUnregistered && rcode != REGISTER_CLOSESESSION)) {
			safs::log_info("CLTOMA_FUSE_REGISTER/ACL - wrong rcode ({}) for registered status ({})",
			               rcode, (int)eptr->registered);
			eptr->mode = ClientConnectionMode::KILL;
			return;
		}

		switch (rcode) {
		case REGISTER_GETRANDOM:
			if (length != kBlobSizeWithRCode) {
				safs::log_info("CLTOMA_FUSE_REGISTER/ACL.1 - wrong size ({}/{})", length,
				               kBlobSizeWithRCode);
				eptr->mode = ClientConnectionMode::KILL;
				return;
			}
			wptr = matoclserv_createpacket(eptr,MATOCL_FUSE_REGISTER, matoclserventry::kPasswordSize);
			for (auto index = 0; index < matoclserventry::kPasswordSize; index++) {
				eptr->randomPassword[index] = rnd<uint8_t>();
			}
			memcpy(wptr, eptr->randomPassword, matoclserventry::kPasswordSize);

			return;
		case REGISTER_NEWSESSION:
			if (length < kRegisterNewSessionMinSize) {
				safs::log_info("CLTOMA_FUSE_REGISTER/ACL.2 - wrong size ({}/>={})", length,
				               kRegisterNewSessionMinSize);
				eptr->mode = ClientConnectionMode::KILL;
				return;
			}
			get32bit(&rptr, eptr->version);
			get32bit(&rptr, infoLength);
			if (length < kRegisterNewSessionMinSize + infoLength) {
				safs::log_info("CLTOMA_FUSE_REGISTER/ACL.2 - wrong size ({}/>={} + infoLength({}))",
				               length, kRegisterNewSessionMinSize, infoLength);
				eptr->mode = ClientConnectionMode::KILL;
				return;
			}
			info = (const char*)rptr;
			rptr += infoLength;
			get32bit(&rptr, pathLength);
			if (length != kRegisterNewSessionMinSize + infoLength + pathLength &&
			    length != kRegisterNewSessionMinSize + 16 + infoLength + pathLength) {
				safs::log_info(
				    "CLTOMA_FUSE_REGISTER/ACL.2 - wrong size "
				    "({}/{} + infoLength({}) + pathLength({}) + 16)",
				    length, kRegisterNewSessionMinSize, infoLength, pathLength);
				eptr->mode = ClientConnectionMode::KILL;
				return;
			}
			path = rptr;
			rptr += pathLength;
			if (pathLength > 0 && rptr[-1] != 0) {
				safs::log_info("CLTOMA_FUSE_REGISTER/ACL.2 - received path without ending zero");
				eptr->mode = ClientConnectionMode::KILL;
				return;
			}
			if (pathLength == 0) {
				path = (const uint8_t*)"";
			}
			if (length == kRegisterNewSessionMinSize + 16 + infoLength + pathLength) {
				status =
				    exports_check(eptr->peerIpAddress, eptr->version, 0, path, eptr->randomPassword,
				                  rptr, &sessionFlags, &rootUid, &rootGid, &mapAllUid, &mapAllGid,
				                  &minGoal, &maxGoal, &minTrashTime, &maxTrashTime);
			} else {
				status =
				    exports_check(eptr->peerIpAddress, eptr->version, 0, path, nullptr, nullptr,
				                  &sessionFlags, &rootUid, &rootGid, &mapAllUid, &mapAllGid,
				                  &minGoal, &maxGoal, &minTrashTime, &maxTrashTime);
			}

			if (status == SAUNAFS_STATUS_OK) {
				status = fs_getrootinode(&rootInode, path);
			}

			if (status == SAUNAFS_STATUS_OK) {
				eptr->sessionData = matoclserv_new_session(1, 0);
				if (eptr->sessionData == nullptr) {
					safs::log_info("can't allocate session record");
					eptr->mode = ClientConnectionMode::KILL;
					return;
				}

				eptr->sessionData->rootInode = rootInode;
				eptr->sessionData->flags = sessionFlags;
				eptr->sessionData->rootUid = rootUid;
				eptr->sessionData->rootGid = rootGid;
				eptr->sessionData->mapAllUid = mapAllUid;
				eptr->sessionData->mapAllGid = mapAllGid;
				eptr->sessionData->minGoal = minGoal;
				eptr->sessionData->maxGoal = maxGoal;
				eptr->sessionData->minTrashTime = minTrashTime;
				eptr->sessionData->maxTrashTime = maxTrashTime;
				eptr->sessionData->peerIpAddress = eptr->peerIpAddress;
				eptr->sessionData->peerPort = eptr->peerPort;

				if (infoLength > 0) {
					if (info[infoLength - 1] == 0) {
						eptr->sessionData->info = std::string(info);
					} else {
						eptr->sessionData->info = std::string(info, infoLength);
					}
				}

				safs::log_info(
				    "Session {} created for mount: {} exported path: {} with ip: {} and port: {}",
				    eptr->sessionData->sessionId,
				    !eptr->sessionData->info.empty() ? eptr->sessionData->info : "unknown info",
				    (path != nullptr) ? reinterpret_cast<const char *>(path) : "unknown path",
				    ipToString(eptr->peerIpAddress), eptr->peerPort);

				matoclserv_store_sessions();
			}

			// answer

			wptr = matoclserv_createpacket(eptr, MATOCL_FUSE_REGISTER,
			                               (status == SAUNAFS_STATUS_OK)
			                                   ? ((eptr->version >= saunafsVersion(1, 6, 26))   ? 35
			                                      : (eptr->version >= saunafsVersion(1, 6, 21)) ? 25
			                                      : (eptr->version >= saunafsVersion(1, 6, 1))  ? 21
			                                                                                   : 13)
			                                   : sizeof(status));

			if (status != SAUNAFS_STATUS_OK) {
				put8bit(&wptr, status);
				return;
			}
			sessionId = eptr->sessionData->sessionId;
			if (eptr->version == saunafsVersion(1, 6, 21)) {
				put32bit(&wptr, 0);
			} else if (eptr->version >= saunafsVersion(1, 6, 22)) {
				put16bit(&wptr, SAUNAFS_PACKAGE_VERSION_MAJOR);
				put8bit(&wptr, SAUNAFS_PACKAGE_VERSION_MINOR);
				put8bit(&wptr, SAUNAFS_PACKAGE_VERSION_MICRO);
			}
			put32bit(&wptr, sessionId);
			put8bit(&wptr, sessionFlags);
			put32bit(&wptr, rootUid);
			put32bit(&wptr, rootGid);
			if (eptr->version>=saunafsVersion(1, 6, 1)) {
				put32bit(&wptr, mapAllUid);
				put32bit(&wptr, mapAllGid);
			}
			if (eptr->version >= saunafsVersion(1, 6, 26)) {
				put8bit(&wptr, minGoal);
				put8bit(&wptr, maxGoal);
				put32bit(&wptr, minTrashTime);
				put32bit(&wptr, maxTrashTime);
			}
			if (eptr->version >= saunafsVersion(1, 6, 30)) {
				eptr->ioLimitsEnabled = true;
				matoclserv_send_iolimits_cfg(eptr);
			}
			eptr->registered = ClientState::kRegistered;
			return;
		case REGISTER_NEWMETASESSION:
			if (length < kRegisterNewMetaSessionMinSize) {
				safs::log_info("CLTOMA_FUSE_REGISTER/ACL.5 - wrong size ({}/>={})", length,
				               kRegisterNewMetaSessionMinSize);
				eptr->mode = ClientConnectionMode::KILL;
				return;
			}

			get32bit(&rptr, eptr->version);
			get32bit(&rptr, infoLength);

			if (length != kRegisterNewMetaSessionMinSize + infoLength &&
			    length != kRegisterNewMetaSessionMinSize + 16 + infoLength) {
				safs::log_info("CLTOMA_FUSE_REGISTER/ACL.5 - wrong size ({}/{} + ileng({}) + 16)",
				               length, kRegisterNewMetaSessionMinSize, infoLength);
				eptr->mode = ClientConnectionMode::KILL;
				return;
			}

			info = (const char*)rptr;
			rptr += infoLength;

			if (length == kRegisterNewMetaSessionMinSize + 16 + infoLength) {
				status = exports_check(eptr->peerIpAddress, eptr->version, 1, nullptr,
				                       eptr->randomPassword, rptr, &sessionFlags, &rootUid,
				                       &rootGid, &mapAllUid, &mapAllGid, &minGoal, &maxGoal,
				                       &minTrashTime, &maxTrashTime);
			} else {
				status =
				    exports_check(eptr->peerIpAddress, eptr->version, 1, nullptr, nullptr, nullptr,
				                  &sessionFlags, &rootUid, &rootGid, &mapAllUid, &mapAllGid,
				                  &minGoal, &maxGoal, &minTrashTime, &maxTrashTime);
			}

			if (status == SAUNAFS_STATUS_OK) {
				eptr->sessionData = matoclserv_new_session(1, 0);
				if (eptr->sessionData == nullptr) {
					safs::log_info("can't allocate session record");
					eptr->mode = ClientConnectionMode::KILL;
					return;
				}

				eptr->sessionData->rootInode = 0;
				eptr->sessionData->flags = sessionFlags;
				eptr->sessionData->rootUid = 0;
				eptr->sessionData->rootGid = 0;
				eptr->sessionData->mapAllUid = 0;
				eptr->sessionData->mapAllGid = 0;
				eptr->sessionData->minGoal = minGoal;
				eptr->sessionData->maxGoal = maxGoal;
				eptr->sessionData->minTrashTime = minTrashTime;
				eptr->sessionData->maxTrashTime = maxTrashTime;
				eptr->sessionData->peerIpAddress = eptr->peerIpAddress;
				eptr->sessionData->peerPort = eptr->peerPort;
				if (infoLength > 0) {
					if (info[infoLength - 1] == 0) {
						eptr->sessionData->info = std::string(info);
					} else {
						eptr->sessionData->info = std::string(info, infoLength);
					}
				}

				safs::log_info(
				    "Meta session {} created for mount: {} with ip: {} and port: {}",
				    eptr->sessionData->sessionId,
				    !eptr->sessionData->info.empty() ? eptr->sessionData->info : "unknown info",
				    ipToString(eptr->peerIpAddress), eptr->peerPort);

				matoclserv_store_sessions();
			}

			// answer

			wptr = matoclserv_createpacket(eptr, MATOCL_FUSE_REGISTER,
			                               (status == SAUNAFS_STATUS_OK)
			                                   ? ((eptr->version >= saunafsVersion(1, 6, 26))   ? 19
			                                      : (eptr->version >= saunafsVersion(1, 6, 21)) ? 9
			                                                                                    : 5)
			                                   : sizeof(status));
			if (status!=SAUNAFS_STATUS_OK) {
				put8bit(&wptr,status);
				return;
			}
			sessionId = eptr->sessionData->sessionId;
			if (eptr->version >= saunafsVersion(1, 6, 21)) {
				put16bit(&wptr,SAUNAFS_PACKAGE_VERSION_MAJOR);
				put8bit(&wptr,SAUNAFS_PACKAGE_VERSION_MINOR);
				put8bit(&wptr,SAUNAFS_PACKAGE_VERSION_MICRO);
			}
			put32bit(&wptr, sessionId);
			put8bit(&wptr, sessionFlags);
			if (eptr->version >= saunafsVersion(1, 6, 26)) {
				put8bit(&wptr, minGoal);
				put8bit(&wptr, maxGoal);
				put32bit(&wptr, minTrashTime);
				put32bit(&wptr, maxTrashTime);
			}
			eptr->registered = ClientState::kRegistered;
			return;
		case REGISTER_RECONNECT:
		case REGISTER_TOOLS:
			if (length < kRegisterWithSessionIdAndVersion) {
				safs::log_info("CLTOMA_FUSE_REGISTER/ACL.{} - wrong size ({}/>={})", rcode, length,
				               kRegisterWithSessionIdAndVersion);
				eptr->mode = ClientConnectionMode::KILL;
				return;
			}
			get32bit(&rptr, sessionId);
			get32bit(&rptr, eptr->version);
			eptr->sessionData = matoclserv_find_session(sessionId);
			if (eptr->sessionData == nullptr || eptr->sessionData->peerIpAddress == 0) {
				status = SAUNAFS_ERROR_BADSESSIONID;
			} else {
				if ((eptr->sessionData->flags & SESFLAG_DYNAMICIP) == 0 &&
				    eptr->peerIpAddress != eptr->sessionData->peerIpAddress) {
					status = SAUNAFS_ERROR_EACCES;
				} else {
					status = SAUNAFS_STATUS_OK;
					safs::log_info(
					    "Session {} reconnected for mount: {} with ip: {} and port: {}",
					    eptr->sessionData->sessionId,
					    !eptr->sessionData->info.empty() ? eptr->sessionData->info : "unknown info",
					    ipToString(eptr->peerIpAddress), eptr->peerPort);
				}
			}
			wptr = matoclserv_createpacket(eptr,MATOCL_FUSE_REGISTER, sizeof(status));
			put8bit(&wptr, status);
			if (status != SAUNAFS_STATUS_OK) { return; }

			if (rcode == REGISTER_RECONNECT) {
				if (eptr->version >= saunafsVersion(1, 6, 30) &&
				    eptr->sessionData->rootInode != 0) {
					eptr->ioLimitsEnabled = true;
					matoclserv_send_iolimits_cfg(eptr);
				}
				eptr->registered = ClientState::kRegistered;
			} else {
				eptr->registered = ClientState::kOldTools;
			}
			return;
		case REGISTER_CLOSESESSION:
			if (length < kBlobSizeWithRCode + sizeof(sessionId)) {
				safs::log_info("CLTOMA_FUSE_REGISTER/ACL.6 - wrong size ({}/>={})", length,
				               kBlobSizeWithRCode + sizeof(sessionId));
				eptr->mode = ClientConnectionMode::KILL;
				return;
			}
			get32bit(&rptr, sessionId);
			matoclserv_close_session(sessionId);
			safs::log_info(
			    "Session {} for mount {} was closed", sessionId,
			    !eptr->sessionData->info.empty() ? eptr->sessionData->info : "unknown info");
			eptr->mode = ClientConnectionMode::KILL;
			return;
		}
		safs::log_info("CLTOMA_FUSE_REGISTER/ACL - wrong rcode ({})", rcode);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	safs::log_info("CLTOMA_FUSE_REGISTER - wrong register blob");
	eptr->mode = ClientConnectionMode::KILL;
}

void matoclserv_register_config(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	cltoma::registerConfig::deserialize(data, length, eptr->sessionData->config);
}

void matoclserv_update_mount_info(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	std::string mount_info;
	cltoma::updateMountInfo::deserialize(data, length, mount_info);
	if (eptr->sessionData && eptr->sessionData->mountInfo != mount_info) {
		eptr->sessionData->mountInfo = mount_info;
	}
}

void matoclserv_fuse_reserved_inodes(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	const uint8_t *ptr;

	if (length % kinode_t_size != 0) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "CLTOMA_FUSE_RESERVED_INODES - wrong size (%" PRIu32 "/N*%zu)", length,
		                   kinode_t_size);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	ptr = data;
	length /= kinode_t_size;
	inode_t inode;
	std::set<inode_t> inodes_to_reserve;
	// read in advance all the files to reserve
	while (length) {
		length--;
		getINode(&ptr, inode);
		inodes_to_reserve.insert(inode);
	}

	FsContext context = FsContext::getForMaster(eventloop_time());
	changelog_disable_flush();
	auto it = eptr->sessionData->openFilesSet.begin();
	while (it != eptr->sessionData->openFilesSet.end()) {
		inode_t openFileIno = *it;
		if (!inodes_to_reserve.contains(openFileIno)) {
			// erase files not belonging to the reserve inodes list provided
			fs_release(context, openFileIno, eptr->sessionData->sessionId);
			it = eptr->sessionData->openFilesSet.erase(it);
		} else {
			// skip files already in session
			it++;
			// no need to remind this file as reserved, as it is already open
			inodes_to_reserve.erase(openFileIno);
		}
	}

	for (const auto &inode_to_reserve : inodes_to_reserve) {
		if (fs_acquire(context, inode_to_reserve, eptr->sessionData->sessionId) ==
		    SAUNAFS_STATUS_OK) {
			// Insert reserved inodes into the opened files set
			eptr->sessionData->openFilesSet.insert(inode_to_reserve);
		}
	}
	changelog_enable_flush();
}

void matoclserv_fuse_statfs(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint64_t totalspace, availspace, trashspace, reservedspace;
	uint32_t msgid;
	inode_t inodes;
	uint8_t *ptr;

	constexpr uint32_t kExpectedSize = sizeof(msgid);

	if (length != kExpectedSize) {
		safs_pretty_syslog(LOG_NOTICE, "CLTOMA_FUSE_STATFS - wrong size (%" PRIu32 "/%" PRIu32 ")",
		                   length, kExpectedSize);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);
	FsContext context = matoclserv_get_context(eptr);
	fs_statfs(context, &totalspace, &availspace, &trashspace, &reservedspace, &inodes);

	constexpr uint32_t kPacketSize = sizeof(msgid) + sizeof(totalspace) + sizeof(availspace) +
	                                 sizeof(trashspace) + sizeof(reservedspace) + sizeof(inodes);

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_STATFS, kPacketSize);
	put32bit(&ptr, msgid);
	put64bit(&ptr, totalspace);
	put64bit(&ptr, availspace);
	put64bit(&ptr, trashspace);
	put64bit(&ptr, reservedspace);
	putINode(&ptr, inodes);

	if (eptr->sessionData) {
		eptr->sessionData->currHourOperationsStats[0]++;
	}
}

void matoclserv_fuse_access(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	uint32_t uid,gid;
	uint8_t modemask;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	constexpr uint32_t kPacketSize =
	    sizeof(msgid) + sizeof(inode) + sizeof(uid) + sizeof(gid) + sizeof(modemask);
	if (length != kPacketSize) {
		safs_pretty_syslog(LOG_NOTICE, "CLTOMA_FUSE_ACCESS - wrong size (%" PRIu32 "/%" PRIu32 ")",
		                   length, kPacketSize);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}
	get32bit(&data, msgid);
	getINode(&data, inode);
	get32bit(&data, uid);
	get32bit(&data, gid);
	modemask = get8bit(&data);
	status = matoclserv_check_group_cache(eptr, gid);
	if (status == SAUNAFS_STATUS_OK && inode != SPECIAL_INODE_PATH_BY_INODE &&
	    inode != SPECIAL_INODE_FILE_BY_INODE) {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		status = fs_access(context, inode, modemask);
	}

	constexpr uint8_t kAnswerSize = sizeof(msgid) + sizeof(status);
	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_ACCESS, kAnswerSize);
	put32bit(&ptr, msgid);
	put8bit(&ptr, status);
}

void matoclserv_sau_whole_path_lookup(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t msgid;
	inode_t inode;
	inode_t found_inode;
	std::string path;
	uint32_t uid, gid;
	Attributes attr;
	uint8_t status = SAUNAFS_STATUS_OK;

	cltoma::wholePathLookup::deserialize(data, length, msgid, inode, path, uid, gid);

	status = matoclserv_check_group_cache(eptr, gid);
	if (status == SAUNAFS_STATUS_OK) {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		status = fs_whole_path_lookup(context, inode, path, &found_inode, attr);
	}

	if (status != SAUNAFS_STATUS_OK) {
		matoclserv_createpacket(eptr, matocl::wholePathLookup::build(msgid, status));
	} else {
		matoclserv_createpacket(eptr, matocl::wholePathLookup::build(msgid, found_inode, attr));
	}
	eptr->sessionData->currHourOperationsStats[3]++;
}

void matoclserv_sau_full_path_by_inode(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t msgid;
	inode_t inode;
	std::string fullPath;
	uint32_t uid, gid;
	uint8_t status = SAUNAFS_STATUS_OK;

	cltoma::fullPathByInode::deserialize(data, length, msgid, inode, uid, gid);

	status = matoclserv_check_group_cache(eptr, gid);
	if (status == SAUNAFS_STATUS_OK) {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		status = fs_full_path_by_inode(context, inode, fullPath);
	}

	if (status != SAUNAFS_STATUS_OK) {
		matoclserv_createpacket(eptr, matocl::fullPathByInode::build(msgid, status));
	} else {
		matoclserv_createpacket(eptr, matocl::fullPathByInode::build(msgid, fullPath));
	}
	eptr->sessionData->currHourOperationsStats[3]++;
}

void matoclserv_sau_get_self_quota(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t version, messageId, uid, gid;
	inode_t inode;
	std::vector<QuotaEntry> results;
	std::vector<std::string> info;
	uint8_t status;
	deserializePacketVersionNoHeader(data, length, version);

	auto foundContextRootInodeResult = [&](inode_t rootInode) {
		for (const auto &result : results) {
			if (result.entryKey.owner.ownerType == QuotaOwnerType::kInode &&
				result.entryKey.owner.ownerId == rootInode) {
				return true;
			}
		}
		return false;
	};

	std::vector<QuotaOwner> owners;
	if (version == cltoma::fuseGetSelfQuota::kGetSelfQuotaWithInode) {
		cltoma::fuseGetSelfQuota::deserialize(data, length, messageId, uid, gid, inode);
	} else {
		cltoma::fuseGetSelfQuota::deserialize(data, length, messageId, uid, gid);
		inode = SPECIAL_INODE_ROOT;
	}
	status = matoclserv_check_group_cache(eptr, gid);
	if (status == SAUNAFS_STATUS_OK) {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		if (inode == SPECIAL_INODE_ROOT) {
			inode = context.rootinode();
		}
		owners.emplace_back(QuotaOwnerType::kUser, uid);
		owners.emplace_back(QuotaOwnerType::kGroup, gid);
		owners.emplace_back(QuotaOwnerType::kInode, inode);
		status = fs_quota_get(context, owners, results);

		if (inode == context.rootinode() && !foundContextRootInodeResult(inode)) {
			auto ino = fsnodes_id_to_node(inode);
			statsrecord rootInodeStatRec;
			fsnodes_get_stats(ino, &rootInodeStatRec);
			results.emplace_back(QuotaEntry{QuotaEntryKey{QuotaOwner{QuotaOwnerType::kInode, inode},
			                                              QuotaRigor::kUsed, QuotaResource::kSize},
			                                rootInodeStatRec.size});
		}
	}

	MessageBuffer reply;
	if (status == SAUNAFS_STATUS_OK) {
		status = fs_quota_get_info(matoclserv_get_context(eptr), results, info);
	}
	if (status == SAUNAFS_STATUS_OK) {
		matocl::fuseGetSelfQuota::serialize(reply, messageId, results);
	} else {
		matocl::fuseGetSelfQuota::serialize(reply, messageId, status);
	}
	matoclserv_createpacket(eptr, std::move(reply));
}

void matoclserv_fuse_lookup(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	uint32_t uid,gid;
	uint8_t nleng;
	const uint8_t *name;
	inode_t newinode;
	Attributes attr;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	constexpr uint32_t kExpectedPacketSize =
	    sizeof(msgid) + sizeof(inode) + sizeof(uid) + sizeof(gid) + sizeof(nleng);
	if (length < kExpectedPacketSize) {
		safs::log_info("CLTOMA_FUSE_LOOKUP - wrong size ({})", length);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}
	get32bit(&data, msgid);
	getINode(&data, inode);
	nleng = get8bit(&data);
	if (length != kExpectedPacketSize + nleng) {
		safs::log_info("CLTOMA_FUSE_LOOKUP - wrong size ({}:nleng={})", length, nleng);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}
	name = data;
	data += nleng;
	get32bit(&data, uid);
	get32bit(&data, gid);
	status = matoclserv_check_group_cache(eptr, gid);
	if (status == SAUNAFS_STATUS_OK) {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		status = fs_lookup(context,inode,HString((char*)name, nleng),&newinode,attr);
	}

	constexpr uint32_t kFailedAnswerSize = sizeof(msgid) + sizeof(status);
	constexpr uint32_t kSuccessAnswerSize = sizeof(msgid) + sizeof(newinode) + attr.size();
	uint8_t answerSize = (status != SAUNAFS_STATUS_OK) ? kFailedAnswerSize : kSuccessAnswerSize;

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_LOOKUP, answerSize);
	put32bit(&ptr,msgid);
	if (status!=SAUNAFS_STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		putINode(&ptr,newinode);
		memcpy(ptr, attr.data(), attr.size());
	}
	eptr->sessionData->currHourOperationsStats[3]++;
}

void matoclserv_fuse_getattr(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	uint32_t uid,gid;
	Attributes attr;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	constexpr uint32_t kExpectedPacketSize =
	    sizeof(msgid) + sizeof(inode) + sizeof(uid) + sizeof(gid);
	if (length != kExpectedPacketSize) {
		safs_pretty_syslog(LOG_NOTICE, "CLTOMA_FUSE_GETATTR - wrong size (%" PRIu32 "/%" PRIu32 ")",
		                   length, kExpectedPacketSize);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}
	get32bit(&data, msgid);
	getINode(&data, inode);
	get32bit(&data, uid);
	get32bit(&data, gid);
	status = matoclserv_check_group_cache(eptr, gid);
	if (status == SAUNAFS_STATUS_OK) {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		status = fs_getattr(context,inode,attr);
	}

	constexpr uint32_t kFailedAnswerSize = sizeof(msgid) + sizeof(status);
	constexpr uint32_t kSuccessAnswerSize = sizeof(msgid) + attr.size();
	uint8_t answerSize = (status != SAUNAFS_STATUS_OK) ? kFailedAnswerSize : kSuccessAnswerSize;

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_GETATTR, answerSize);
	put32bit(&ptr,msgid);
	if (status!=SAUNAFS_STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		memcpy(ptr, attr.data(), attr.size());
	}
	if (eptr->sessionData) {
		eptr->sessionData->currHourOperationsStats[1]++;
	}
}

void matoclserv_fuse_setattr(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	uint32_t uid,gid;
	uint8_t setmask;
	Attributes attr;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	SugidClearMode sugidclearmode;
	uint16_t attrmode;
	uint32_t attruid,attrgid,attratime,attrmtime;

	constexpr uint32_t kExpectedPacketSize = sizeof(msgid) + sizeof(inode) + sizeof(uid) +
	                                         sizeof(gid) + sizeof(setmask) + sizeof(attrmode) +
	                                         sizeof(attruid) + sizeof(attrgid) + sizeof(attratime) +
	                                         sizeof(attrmtime);
	constexpr uint32_t kExpectedPacketSizeWithSugid = kExpectedPacketSize + sizeof(sugidclearmode);

	if (length != kExpectedPacketSize && length != kExpectedPacketSizeWithSugid) {
		safs_pretty_syslog(
		    LOG_NOTICE, "CLTOMA_FUSE_SETATTR - wrong size (%" PRIu32 "/%" PRIu32 " | %" PRIu32 ")",
		    length, kExpectedPacketSize, kExpectedPacketSizeWithSugid);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);
	getINode(&data, inode);
	get32bit(&data, uid);
	get32bit(&data, gid);
	setmask = get8bit(&data);
	attrmode = get16bit(&data);
	get32bit(&data, attruid);
	get32bit(&data, attrgid);
	get32bit(&data, attratime);
	get32bit(&data, attrmtime);

	if (length == kExpectedPacketSizeWithSugid) {
		sugidclearmode = static_cast<SugidClearMode>(get8bit(&data));
	} else {
		sugidclearmode = SugidClearMode::kAlways; // this is safest option
	}

	status = matoclserv_check_group_cache(eptr, gid);

	if (status == SAUNAFS_STATUS_OK) {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		status = fs_setattr(context, inode, setmask, attrmode, attruid, attrgid,
							attratime, attrmtime, sugidclearmode, attr);
	}

	constexpr uint32_t kFailedAnswerSize = sizeof(msgid) + sizeof(status);
	constexpr uint32_t kSuccessAnswerSize = sizeof(msgid) + attr.size();
	uint8_t answerSize = (status != SAUNAFS_STATUS_OK) ? kFailedAnswerSize : kSuccessAnswerSize;

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_SETATTR, answerSize);

	put32bit(&ptr,msgid);

	if (status != SAUNAFS_STATUS_OK) {
		put8bit(&ptr, status);
	} else {
		memcpy(ptr, attr.data(), attr.size());
	}

	if (eptr->sessionData) {
		eptr->sessionData->currHourOperationsStats[2]++;
	}
}

void matoclserv_fuse_truncate(matoclserventry *eptr, PacketHeader header, const uint8_t *data) {
	sassert(header.type == CLTOMA_FUSE_TRUNCATE
			|| header.type == SAU_CLTOMA_FUSE_TRUNCATE
			|| header.type == SAU_CLTOMA_FUSE_TRUNCATE_END);

	// Deserialize the request
	std::vector<uint8_t> request(data, data + header.length);
	uint8_t status = SAUNAFS_STATUS_OK;
	uint32_t messageId, uid, gid, type;
	inode_t inode;
	uint32_t lockId = 0;
	bool opened;
	uint64_t chunkId, length;
	FsContext context = matoclserv_get_context(eptr);

	const PacketSerializer *serializer = PacketSerializer::getSerializer(header.type, eptr->version);
	if (header.type == SAU_CLTOMA_FUSE_TRUNCATE_END) {
		cltoma::fuseTruncateEnd::deserialize(request,
				messageId, inode, uid, gid, length, lockId);
		type = FUSE_TRUNCATE_END;
		status = matoclserv_check_group_cache(eptr, gid);
		if (status == SAUNAFS_STATUS_OK) {
			opened = true; // permissions have already been checked on SAU_CLTOMA_TRUNCATE
			context = matoclserv_get_context(eptr, uid, gid);
			// We have to verify lockid in this request
			if (lockId == 0) { // unlocking with lockid == 0 means "force unlock", this is not allowed
				status = SAUNAFS_ERROR_WRONGLOCKID;
			} else {
				// let's check if chunk is still locked by us
				status = fs_get_chunkid(context, inode, length / SFSCHUNKSIZE, &chunkId);
				if (status == SAUNAFS_STATUS_OK) {
					status = chunk_can_unlock(chunkId, lockId);
				}
				fs_end_setlength(chunkId);
			}
		}
	} else {
		serializer->deserializeFuseTruncate(request, messageId, inode, opened, uid, gid, length);
		type = FUSE_TRUNCATE;
		status = matoclserv_check_group_cache(eptr, gid);
		if (status == SAUNAFS_STATUS_OK) {
			context = matoclserv_get_context(eptr, uid, gid);
		}
	}

	// Try to do the truncate
	Attributes attr;
	if (status == SAUNAFS_STATUS_OK) {
		status = fs_try_setlength(context, inode, opened, length,
								  (type != FUSE_TRUNCATE_END), lockId, attr, &chunkId);
	}

	// In case of SAUNAFS_ERROR_NOTPOSSIBLE we have to tell the client to write the chunk before truncating
	if (status == SAUNAFS_ERROR_NOTPOSSIBLE && header.type == CLTOMA_FUSE_TRUNCATE) {
		// Old client requested to truncate xor chunk. We can't do this!
		status = SAUNAFS_ERROR_ENOTSUP;
	} else if (status == SAUNAFS_ERROR_NOTPOSSIBLE && header.type == SAU_CLTOMA_FUSE_TRUNCATE) {
		// New client requested to truncate xor chunk. He has to do it himself.
		uint64_t fileLength;
		uint8_t opflag;
		fs_writechunk(context, inode, length / SFSCHUNKSIZE, false,
				&lockId, &chunkId, &opflag, &fileLength);
		if (opflag) {
			// But first we have to duplicate chunk :)
			type = FUSE_TRUNCATE_BEGIN;
			length = fileLength;
			status = SAUNAFS_ERROR_DELAYED;
		} else {
			// No duplication is needed
			std::vector<uint8_t> reply;
			matocl::fuseTruncate::serialize(reply, messageId, fileLength, lockId);
			matoclserv_createpacket(eptr, std::move(reply));
			if (eptr->sessionData) {
				eptr->sessionData->currHourOperationsStats[2]++;
			}
			return;
		}
	}

	if (status == SAUNAFS_ERROR_DELAYED) {
		// Duplicate or truncate request has been sent to chunkservers, delay the reply
		auto chunkOperationPtr = std::make_unique<DelayedChunkOperation>();
		passert(chunkOperationPtr.get());
		chunkOperationPtr->chunkId = chunkId;
		chunkOperationPtr->messageId = messageId;
		chunkOperationPtr->inode = inode;
		chunkOperationPtr->uid = context.uid();
		chunkOperationPtr->gid = context.gid();
		chunkOperationPtr->auid = context.auid();
		chunkOperationPtr->agid = context.agid();
		chunkOperationPtr->fileLength = length;
		chunkOperationPtr->lockId = lockId;
		chunkOperationPtr->type = type;
		chunkOperationPtr->serializer = serializer;
		eptr->delayedChunkOperations.push_back(std::move(chunkOperationPtr));
		if (eptr->sessionData) {
			eptr->sessionData->currHourOperationsStats[2]++;
		}
		return;
	}
	if (status == SAUNAFS_STATUS_OK) {
		status = fs_do_setlength(context, inode, length, attr);
	}
	if (status == SAUNAFS_STATUS_OK) {
		dcm_modify(inode, eptr->sessionData->sessionId);
	}

	std::vector<uint8_t> reply;
	if (status == SAUNAFS_STATUS_OK) {
		serializer->serializeFuseTruncate(reply, type, messageId, attr);
	} else {
		safs::log_debug("matoclserv_fuse_truncate: Failed to truncate: {} (code {})", saunafs_error_string(status));
		serializer->serializeFuseTruncate(reply, type, messageId, status);
	}
	matoclserv_createpacket(eptr, std::move(reply));
	if (eptr->sessionData) {
		eptr->sessionData->currHourOperationsStats[2]++;
	}
}

void matoclserv_fuse_readlink(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	std::string path;

	constexpr uint32_t kExpectedPacketSize = sizeof(msgid) + sizeof(inode);

	if (length != kExpectedPacketSize) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "CLTOMA_FUSE_READLINK - wrong size (%" PRIu32 "/%" PRIu32 ")", length,
		                   kExpectedPacketSize);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);
	getINode(&data, inode);

	FsContext context = matoclserv_get_context(eptr);
	status = fs_readlink(context, inode, path);

	constexpr uint32_t kFailedAnswerSize = sizeof(msgid) + sizeof(status);
	constexpr uint32_t kSuccessAnswerSize = sizeof(msgid) + sizeof(uint32_t);
	uint32_t answerSize =
	    (status != SAUNAFS_STATUS_OK) ? kFailedAnswerSize : kSuccessAnswerSize + path.length() + 1;

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_READLINK, answerSize);

	put32bit(&ptr, msgid);

	if (status != SAUNAFS_STATUS_OK) {
		put8bit(&ptr, status);
	} else {
		// Safe cast, the length should always fit
		put32bit(&ptr, static_cast<uint32_t>(path.length() + 1));
		if (path.length() > 0) {
			memcpy(ptr, path.c_str(), path.length());
		}
		ptr[path.length()] = 0;
	}

	if (eptr->sessionData) {
		eptr->sessionData->currHourOperationsStats[7]++;
	}
}

void matoclserv_fuse_symlink(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	uint8_t nleng;
	const uint8_t *name, *path;
	uint32_t uid, gid;
	uint32_t pleng;
	inode_t newinode;
	Attributes attr;
	uint32_t msgid;
	uint8_t status;
	uint8_t *ptr;

	constexpr uint32_t kMinExpectedPacketSize =
	    sizeof(msgid) + sizeof(inode) + sizeof(nleng) + sizeof(pleng) + sizeof(uid) + sizeof(gid);

	if (length < kMinExpectedPacketSize) {
		safs_pretty_syslog(LOG_NOTICE, "CLTOMA_FUSE_SYMLINK - wrong size (%" PRIu32 "/%" PRIu32 ")",
		                   length, kMinExpectedPacketSize);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);
	getINode(&data, inode);
	nleng = get8bit(&data);

	if (length < kMinExpectedPacketSize + nleng) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "CLTOMA_FUSE_SYMLINK - wrong size (%" PRIu32 ":nleng=%" PRIu8 ")",
		                   length, nleng);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	name = data;
	data += nleng;
	get32bit(&data, pleng);
	if (length != kMinExpectedPacketSize + nleng + pleng) {
		safs_pretty_syslog(LOG_NOTICE,
		       "CLTOMA_FUSE_SYMLINK - wrong size (%" PRIu32 ":nleng=%" PRIu8 ":pleng=%" PRIu32 ")",
		       length, nleng, pleng);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	path = data;
	data += pleng;
	get32bit(&data, uid);
	get32bit(&data, gid);
	while (pleng > 0 && path[pleng - 1] == 0) {
		pleng--;
	}
	newinode = 0;  // request to acquire new inode id
	status = matoclserv_check_group_cache(eptr, gid);
	if (status == SAUNAFS_STATUS_OK) {
		auto context = matoclserv_get_context(eptr, uid, gid);
		status = fs_symlink(context, inode, HString((char *)name, nleng),
	                    std::string((char *)path, pleng), &newinode, &attr);
	}

	constexpr uint32_t kFailedAnswerSize = sizeof(msgid) + sizeof(status);
	constexpr uint32_t kSuccessAnswerSize = sizeof(msgid) + sizeof(newinode) + attr.size();
	uint32_t answerSize = (status != SAUNAFS_STATUS_OK) ? kFailedAnswerSize : kSuccessAnswerSize;

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_SYMLINK, answerSize);

	put32bit(&ptr, msgid);

	if (status != SAUNAFS_STATUS_OK) {
		put8bit(&ptr, status);
	} else {
		putINode(&ptr, newinode);
		memcpy(ptr, attr.data(), attr.size());
	}

	if (eptr->sessionData) {
		eptr->sessionData->currHourOperationsStats[6]++;
	}
}

void matoclserv_fuse_mknod(matoclserventry *eptr, PacketHeader header, const uint8_t *data) {
	uint32_t messageId, uid, gid, rdev;
	inode_t inode;
	LegacyString<uint8_t> name;
	uint8_t type;
	uint16_t mode, umask;

	if (header.type == CLTOMA_FUSE_MKNOD) {
		deserializeAllLegacyPacketDataNoHeader(data, header.length,
				messageId, inode, name, type, mode, uid, gid, rdev);
		umask = 0;
	} else if (header.type == SAU_CLTOMA_FUSE_MKNOD) {
		cltoma::fuseMknod::deserialize(data, header.length,
				messageId, inode, name, type, mode, umask, uid, gid, rdev);
	} else {
		throw IncorrectDeserializationException(
				"Unknown packet type for matoclserv_fuse_mknod: " + std::to_string(header.type));
	}

	inode_t newinode;
	Attributes attr;
	uint8_t status = matoclserv_check_group_cache(eptr, gid);
	if (status == SAUNAFS_STATUS_OK) {
		FsContext context = matoclserv_get_context(eptr, uid, gid);

		status = fs_mknod(context, inode, HString(std::move(name)), static_cast<FSNodeType>(type),
		                  mode, umask, rdev, &newinode, attr);
	}

	MessageBuffer reply;
	if (status == SAUNAFS_STATUS_OK && header.type == CLTOMA_FUSE_MKNOD) {
		serializeLegacyPacket(reply, MATOCL_FUSE_MKNOD, messageId, newinode, attr);
	} else if (status == SAUNAFS_STATUS_OK && header.type == SAU_CLTOMA_FUSE_MKNOD) {
		matocl::fuseMknod::serialize(reply, messageId, newinode, attr);
	} else if (header.type == SAU_CLTOMA_FUSE_MKNOD) {
		matocl::fuseMknod::serialize(reply, messageId, status);
	} else {
		serializeLegacyPacket(reply, MATOCL_FUSE_MKNOD, messageId, status);
	}
	matoclserv_createpacket(eptr, std::move(reply));
	if (eptr->sessionData) {
		eptr->sessionData->currHourOperationsStats[8]++;
	}
}

void matoclserv_fuse_mkdir(matoclserventry *eptr, PacketHeader header, const uint8_t *data) {
	uint32_t messageId, uid, gid;
	inode_t inode;
	LegacyString<uint8_t> name;
	bool copysgid;
	uint16_t mode, umask;

	if (header.type == CLTOMA_FUSE_MKDIR) {
		if (eptr->version >= saunafsVersion(1, 6, 25)) {
			deserializeAllLegacyPacketDataNoHeader(data, header.length,
					messageId, inode, name, mode, uid, gid, copysgid);
		} else {
			deserializeAllLegacyPacketDataNoHeader(data, header.length,
					messageId, inode, name, mode, uid, gid);
			copysgid = false;
		}
		umask = 0;
	} else if (header.type == SAU_CLTOMA_FUSE_MKDIR) {
		cltoma::fuseMkdir::deserialize(data, header.length, messageId,
				inode, name, mode, umask, uid, gid, copysgid);
	} else {
		throw IncorrectDeserializationException(
				"Unknown packet type for matoclserv_fuse_mkdir: " + std::to_string(header.type));
	}

	inode_t newinode;
	Attributes attr;
	uint8_t status = matoclserv_check_group_cache(eptr, gid);
	if (status == SAUNAFS_STATUS_OK) {
		FsContext context = matoclserv_get_context(eptr, uid, gid);

		status = fs_mkdir(context, inode, HString(std::move(name)), mode, umask,
						copysgid, &newinode, attr);
	}

	MessageBuffer reply;
	if (status == SAUNAFS_STATUS_OK && header.type == CLTOMA_FUSE_MKDIR) {
		serializeLegacyPacket(reply, MATOCL_FUSE_MKDIR, messageId, newinode, attr);
	} else if (status == SAUNAFS_STATUS_OK && header.type == SAU_CLTOMA_FUSE_MKDIR) {
		matocl::fuseMkdir::serialize(reply, messageId, newinode, attr);
	} else if (header.type == SAU_CLTOMA_FUSE_MKDIR) {
		matocl::fuseMkdir::serialize(reply, messageId, status);
	} else {
		serializeLegacyPacket(reply, MATOCL_FUSE_MKDIR, messageId, status);
	}
	matoclserv_createpacket(eptr, std::move(reply));
	if (eptr->sessionData) {
		eptr->sessionData->currHourOperationsStats[4]++;
	}
}

void matoclserv_fuse_unlink(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	uint32_t uid,gid;
	uint8_t nleng;
	const uint8_t *name;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;

	constexpr uint32_t kMinExpectedPacketSize =
	    sizeof(msgid) + sizeof(inode) + sizeof(nleng) + sizeof(uid) + sizeof(gid);

	if (length < kMinExpectedPacketSize) {
		safs_pretty_syslog(LOG_NOTICE,"CLTOMA_FUSE_UNLINK - wrong size (%" PRIu32 ")",length);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);
	getINode(&data, inode);
	nleng = get8bit(&data);

	if (length != kMinExpectedPacketSize + nleng) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "CLTOMA_FUSE_UNLINK - wrong size (%" PRIu32 ":nleng=%" PRIu8 ")", length,
		                   nleng);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	name = data;
	data += nleng;
	get32bit(&data, uid);
	get32bit(&data, gid);

	status = matoclserv_check_group_cache(eptr, gid);

	if (status == SAUNAFS_STATUS_OK) {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		status = fs_unlink(context,inode, HString((char*)name, nleng));
	}

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_UNLINK, sizeof(msgid) + sizeof(status));

	put32bit(&ptr, msgid);
	put8bit(&ptr, status);

	if (eptr->sessionData) {
		eptr->sessionData->currHourOperationsStats[9]++;
	}
}

void matoclserv_fuse_recursive_remove_wake_up(uint32_t session_id, uint32_t msgid, int status) {
	matoclserventry *eptr = matoclserv_find_connection(session_id);
	if (!eptr) { return; }
	matoclserv_createpacket(eptr, matocl::recursiveRemove::build(msgid, status));
}

void matoclserv_fuse_recursive_remove(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t parent_inode;
	uint32_t uid, gid;
	uint32_t msgid;
	uint8_t status;

	std::string name;
	uint32_t job_id;
	cltoma::recursiveRemove::deserialize(data, length, msgid, job_id, parent_inode, name, uid, gid);

	status = matoclserv_check_group_cache(eptr, gid);
	if (status == SAUNAFS_STATUS_OK) {
		FsContext context = matoclserv_get_context(eptr, uid, gid);

		status = fs_recursive_remove(context, parent_inode, HString(name),
					    std::bind(matoclserv_fuse_recursive_remove_wake_up,
				      eptr->sessionData->sessionId, msgid, std::placeholders::_1), job_id);
	}
	if (status != SAUNAFS_ERROR_WAITING) {
		matoclserv_createpacket(eptr, matocl::recursiveRemove::build(msgid, status));
	}
}

void matoclserv_fuse_rmdir(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	uint32_t uid,gid;
	uint8_t nleng;
	const uint8_t *name;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;

	constexpr uint32_t kMinExpectedPacketSize =
	    sizeof(msgid) + sizeof(inode) + sizeof(nleng) + sizeof(uid) + sizeof(gid);

	if (length < kMinExpectedPacketSize) {
		safs_pretty_syslog(LOG_NOTICE, "CLTOMA_FUSE_RMDIR - wrong size (%" PRIu32 ")", length);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);
	getINode(&data, inode);
	nleng = get8bit(&data);

	if (length != kMinExpectedPacketSize + nleng) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "CLTOMA_FUSE_RMDIR - wrong size (%" PRIu32 ":nleng=%" PRIu8 ")", length,
		                   nleng);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	name = data;
	data += nleng;
	get32bit(&data, uid);
	get32bit(&data, gid);

	status = matoclserv_check_group_cache(eptr, gid);

	if (status == SAUNAFS_STATUS_OK) {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		status = fs_rmdir(context,inode,HString((char*)name, nleng));
	}

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_RMDIR, sizeof(msgid) + sizeof(status));

	put32bit(&ptr, msgid);
	put8bit(&ptr, status);

	if (eptr->sessionData) {
		eptr->sessionData->currHourOperationsStats[5]++;
	}
}

void matoclserv_fuse_rename(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	inode_t inode_src;
	inode_t inode_dst;
	uint8_t nleng_src,nleng_dst;
	const uint8_t *name_src,*name_dst;
	uint32_t uid,gid;
	Attributes attr;
	uint32_t msgid;
	uint8_t status;
	uint8_t *ptr;

	constexpr uint32_t kMinExpectedPacketSize =
	    sizeof(msgid) + sizeof(inode_src) + sizeof(nleng_src) + sizeof(inode_dst) +
	    sizeof(nleng_dst) + sizeof(uid) + sizeof(gid);

	if (length < kMinExpectedPacketSize) {
		safs_pretty_syslog(LOG_NOTICE, "CLTOMA_FUSE_RENAME - wrong size (%" PRIu32 ")", length);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);
	getINode(&data, inode_src);
	nleng_src = get8bit(&data);

	if (length < kMinExpectedPacketSize + nleng_src) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "CLTOMA_FUSE_RENAME - wrong size (%" PRIu32 ":nleng_src=%" PRIu8 ")",
		                   length, nleng_src);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	name_src = data;
	data += nleng_src;
	getINode(&data, inode_dst);
	nleng_dst = get8bit(&data);

	if (length != kMinExpectedPacketSize + nleng_src + nleng_dst) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "CLTOMA_FUSE_RENAME - wrong size (%" PRIu32 ":nleng_src=%" PRIu8
		                   ":nleng_dst=%" PRIu8 ")",
		                   length, nleng_src, nleng_dst);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	name_dst = data;
	data += nleng_dst;
	get32bit(&data, uid);
	get32bit(&data, gid);

	status = matoclserv_check_group_cache(eptr, gid);

	if (status == SAUNAFS_STATUS_OK) {
		auto context = matoclserv_get_context(eptr, uid, gid);
		status = fs_rename(context, inode_src, HString((char*)name_src, nleng_src),
		                   inode_dst, HString((char*)name_dst, nleng_dst), &inode, &attr);
	}

	if (eptr->version >= 0x010615 && status == SAUNAFS_STATUS_OK) {
		constexpr uint32_t kSuccessAnswerSize = sizeof(msgid) + sizeof(inode) + attr.size();
		ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_RENAME, kSuccessAnswerSize);
	} else {
		ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_RENAME, sizeof(msgid) + sizeof(status));
	}

	put32bit(&ptr, msgid);

	if (eptr->version >= 0x010615 && status == SAUNAFS_STATUS_OK) {
		putINode(&ptr, inode);
		memcpy(ptr, attr.data(), attr.size());
	} else {
		put8bit(&ptr,status);
	}

	if (eptr->sessionData) {
		eptr->sessionData->currHourOperationsStats[10]++;
	}
}

void matoclserv_fuse_link(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	inode_t inode_dst;
	uint8_t nleng_dst;
	const uint8_t *name_dst;
	uint32_t uid,gid;
	inode_t newinode;
	Attributes attr;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;

	constexpr uint32_t kMinExpectedPacketSize =
	    sizeof(msgid) + sizeof(inode) + sizeof(nleng_dst) + sizeof(inode_dst) +
	    sizeof(uid) + sizeof(gid);

	if (length < kMinExpectedPacketSize) {
		safs_pretty_syslog(LOG_NOTICE, "CLTOMA_FUSE_LINK - wrong size (%" PRIu32 ")", length);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);
	getINode(&data, inode);
	getINode(&data, inode_dst);
	nleng_dst = get8bit(&data);

	if (length != kMinExpectedPacketSize + nleng_dst) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "CLTOMA_FUSE_LINK - wrong size (%" PRIu32 ":nleng_dst=%" PRIu8 ")",
		                   length, nleng_dst);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	name_dst = data;
	data += nleng_dst;
	get32bit(&data, uid);
	get32bit(&data, gid);

	status = matoclserv_check_group_cache(eptr, gid);

	if (status == SAUNAFS_STATUS_OK) {
		auto context = matoclserv_get_context(eptr, uid, gid);
		status = fs_link(context, inode, inode_dst, HString((char*)name_dst, nleng_dst), &newinode, &attr);
	}

	constexpr uint32_t kFailedAnswerSize = sizeof(msgid) + sizeof(status);
	constexpr uint32_t kSuccessAnswerSize = sizeof(msgid) + sizeof(newinode) + attr.size();
	uint32_t answerSize = (status != SAUNAFS_STATUS_OK) ? kFailedAnswerSize : kSuccessAnswerSize;

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_LINK, answerSize);

	put32bit(&ptr,msgid);

	if (status != SAUNAFS_STATUS_OK) {
		put8bit(&ptr, status);
	} else {
		putINode(&ptr, newinode);
		memcpy(ptr, attr.data(), attr.size());
	}

	if (eptr->sessionData) {
		eptr->sessionData->currHourOperationsStats[11]++;
	}
}

void matoclserv_fuse_getdir(matoclserventry *eptr,const PacketHeader &header, const uint8_t *data) {
	uint32_t message_id, uid, gid;
	inode_t inode;
	uint64_t first_entry, number_of_entries;
	MessageBuffer buffer;

	PacketVersion packet_version;
	deserializePacketVersionNoHeader(data, header.length, packet_version);

	if (packet_version == cltoma::fuseGetDir::kClientAbleToProcessDirentIndex) {
		cltoma::fuseGetDir::deserialize(data, header.length, message_id, inode, uid, gid, first_entry, number_of_entries);
	} else if (packet_version == cltoma::fuseGetDirLegacy::kLegacyClient) {
		cltoma::fuseGetDirLegacy::deserialize(data, header.length, message_id, inode, uid, gid, first_entry, number_of_entries);
	} else {
		throw IncorrectDeserializationException(
				"Unknown SAU_CLTOMA_FUSE_GETDIR version: " + std::to_string(packet_version));
	}

	number_of_entries = std::min(number_of_entries, matocl::fuseGetDir::kMaxNumberOfDirectoryEntries);
	uint8_t status = matoclserv_check_group_cache(eptr, gid);

	if (status == SAUNAFS_STATUS_OK) {
		FsContext context = matoclserv_get_context(eptr, uid, gid);

		if (packet_version == cltoma::fuseGetDir::kClientAbleToProcessDirentIndex) {
			std::vector<DirectoryEntry> dir_entries;
			status = fs_readdir(context, inode, first_entry, number_of_entries, dir_entries); //<DirectoryEntry>

			if (status != SAUNAFS_STATUS_OK) {
				matocl::fuseGetDir::serialize(buffer, message_id, status);
			} else {
				matocl::fuseGetDir::serialize(buffer, message_id, first_entry, dir_entries);
			}
		} else if (packet_version == cltoma::fuseGetDirLegacy::kLegacyClient) {
			std::vector<legacy::DirectoryEntry> dir_entries;
			status = fs_readdir(context, inode, first_entry, number_of_entries, dir_entries); //<legacy::DirectoryEntry>

			if (status != SAUNAFS_STATUS_OK) {
				matocl::fuseGetDir::serialize(buffer, message_id, status);
			} else {
				matocl::fuseGetDirLegacy::serialize(buffer, message_id, first_entry, dir_entries);
			}
		} else {
			throw IncorrectDeserializationException(
					"Unknown SAU_CLTOMA_FUSE_GETDIR version: " + std::to_string(packet_version));
		}
	} else {
		matocl::fuseGetDir::serialize(buffer, message_id, status);
	}

	eptr->sessionData->currHourOperationsStats[12]++;
	matoclserv_createpacket(eptr, std::move(buffer));
}

void matoclserv_fuse_getdir(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	uint32_t uid,gid;
	uint8_t flags;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	uint32_t dleng;
	void *custom;

	constexpr uint32_t kExpectedSize = sizeof(msgid) + sizeof(inode) + sizeof(uid) + sizeof(gid);
	constexpr uint32_t kExpectedSizeWithFlags = kExpectedSize + sizeof(flags);

	if (length != kExpectedSize && length != kExpectedSizeWithFlags) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "CLTOMA_FUSE_GETDIR - wrong size (%" PRIu32 "/%" PRIu32 "|%" PRIu32 ")",
		                   length, kExpectedSize, kExpectedSizeWithFlags);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);
	getINode(&data, inode);
	get32bit(&data, uid);
	get32bit(&data, gid);
	if (length == kExpectedSizeWithFlags) {
		flags = get8bit(&data);
	} else {
		flags = 0;
	}

	status = matoclserv_check_group_cache(eptr, gid);

	if (status != SAUNAFS_STATUS_OK) {
		ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_GETDIR, sizeof(msgid) + sizeof(status));
		put32bit(&ptr, msgid);
		put8bit(&ptr, status);
		eptr->sessionData->currHourOperationsStats[12]++;
		return;
	}

	FsContext context = matoclserv_get_context(eptr, uid, gid);
	status = fs_readdir_size(context, inode, flags, &custom, &dleng);

	constexpr uint32_t kFailedAnswerSize = sizeof(msgid) + sizeof(status);
	const uint32_t kSuccessAnswerSize = sizeof(msgid) + dleng;  // Can't be constexpr
	uint32_t answerSize = (status != SAUNAFS_STATUS_OK) ? kFailedAnswerSize : kSuccessAnswerSize;

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_GETDIR, answerSize);

	put32bit(&ptr, msgid);

	if (status != SAUNAFS_STATUS_OK) {
		put8bit(&ptr, status);
	} else {
		fs_readdir_data(context, flags, custom, ptr);
	}

	eptr->sessionData->currHourOperationsStats[12]++;
}

void matoclserv_fuse_open(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	uint32_t uid,gid;
	uint8_t flags;
	Attributes attr;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	int allowcache;

	constexpr uint32_t kExpectedSize =
	    sizeof(msgid) + sizeof(inode) + sizeof(uid) + sizeof(gid) + sizeof(flags);

	if (length != kExpectedSize) {
		safs_pretty_syslog(LOG_NOTICE, "CLTOMA_FUSE_OPEN - wrong size (%" PRIu32 "/%" PRIu32 ")",
		                   length, kExpectedSize);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);
	getINode(&data, inode);
	get32bit(&data, uid);
	get32bit(&data, gid);
	flags = get8bit(&data);

	status = matoclserv_check_group_cache(eptr, gid);

	if (status == SAUNAFS_STATUS_OK) {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		status = matoclserv_insert_open_file(eptr->sessionData, inode);
		if (status == SAUNAFS_STATUS_OK) { status = fs_opencheck(context, inode, flags, attr); }
	}

	if (eptr->version >= 0x010609 && status == SAUNAFS_STATUS_OK) {
		allowcache = dcm_open(inode, eptr->sessionData->sessionId);
		if (allowcache == 0) {
			attr[1] &= (0xFF ^ (MATTR_ALLOWDATACACHE << 4));
		}
		ptr = matoclserv_createpacket(eptr,MATOCL_FUSE_OPEN, sizeof(msgid) + attr.size());
	} else {
		ptr = matoclserv_createpacket(eptr,MATOCL_FUSE_OPEN, sizeof(msgid) + sizeof(status));
	}

	put32bit(&ptr, msgid);

	if (eptr->version >= 0x010609 && status == SAUNAFS_STATUS_OK) {
		memcpy(ptr, attr.data(), attr.size());
	} else {
		put8bit(&ptr, status);
	}

	if (eptr->sessionData) {
		eptr->sessionData->currHourOperationsStats[13]++;
	}
}

void matoclserv_fuse_read_chunk(matoclserventry *eptr, PacketHeader header, const uint8_t *data) {
	sassert(header.type == CLTOMA_FUSE_READ_CHUNK || header.type == SAU_CLTOMA_FUSE_READ_CHUNK);
	uint8_t status;
	uint64_t chunkid;
	uint64_t fleng;
	uint32_t version;
	uint32_t messageId;
	inode_t inode;
	uint32_t index;
	std::vector<uint8_t> outMessage;
	const PacketSerializer* serializer = PacketSerializer::getSerializer(header.type, eptr->version);

	std::vector<uint8_t> receivedData(data, data + header.length);
	serializer->deserializeFuseReadChunk(receivedData, messageId, inode, index);

	status = fs_readchunk(inode, index, &chunkid, &fleng);
	std::vector<ChunkTypeWithAddress> allChunkCopies;
	if (status == SAUNAFS_STATUS_OK) {
		if (chunkid > 0) {
			status = chunk_getversionandlocations(chunkid, eptr->peerIpAddress, version,
					kMaxNumberOfChunkCopies, allChunkCopies);
			remove_unsupported_ec_parts(eptr->version, allChunkCopies);
		} else {
			version = 0;
		}
	}

	if (status != SAUNAFS_STATUS_OK) {
		serializer->serializeFuseReadChunk(outMessage, messageId, status);
		matoclserv_createpacket(eptr, outMessage);
		return;
	}

	dcm_access(inode, eptr->sessionData->sessionId);
	serializer->serializeFuseReadChunk(outMessage, messageId, fleng, chunkid, version,
			allChunkCopies);
	matoclserv_createpacket(eptr, outMessage);

	if (eptr->sessionData) {
		eptr->sessionData->currHourOperationsStats[14]++;
	}
}

void matoclserv_chunks_info(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t message_id{0}, chunk_index, chunk_count, uid, gid;
	inode_t inode;
	PacketVersion version;
	uint8_t status;
	std::vector<ChunkWithAddressAndLabel> chunks;

	deserializePacketVersionNoHeader(data, length, version);
	if (version != cltoma::chunksInfo::kMultiChunk) {
		matoclserv_createpacket(eptr, matocl::chunksInfo::build(message_id, (uint8_t)SAUNAFS_ERROR_EINVAL));
		return;
	}

	cltoma::chunksInfo::deserialize(data, length, message_id, uid, gid, inode, chunk_index, chunk_count);

	chunk_count = std::max<uint32_t>(chunk_count, 1);
	chunk_count = std::min(chunk_count, matocl::chunksInfo::kMaxNumberOfResultEntries);

	status = matoclserv_check_group_cache(eptr, gid);
	if (status == SAUNAFS_STATUS_OK) {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		status =
		    fs_getchunksinfo(context, eptr->peerIpAddress, inode, chunk_index, chunk_count, chunks);
	}

	if (status != SAUNAFS_STATUS_OK) {
		matoclserv_createpacket(eptr, matocl::chunksInfo::build(message_id, status));
		return;
	}

	matoclserv_createpacket(eptr, matocl::chunksInfo::build(message_id, chunks));
}

void matoclserv_fuse_write_chunk(matoclserventry *eptr, PacketHeader header, const uint8_t *data) {
	sassert(header.type == CLTOMA_FUSE_WRITE_CHUNK || header.type == SAU_CLTOMA_FUSE_WRITE_CHUNK);
	uint8_t status;
	inode_t inode;
	uint32_t chunkIndex;
	uint64_t fileLength;
	uint64_t chunkId;
	uint32_t lockId;
	uint32_t messageId;
	uint8_t opflag;

	std::vector<uint8_t> outMessage;
	const PacketSerializer* serializer = PacketSerializer::getSerializer(header.type, eptr->version);

	std::vector<uint8_t> receivedData(data, data + header.length);
	serializer->deserializeFuseWriteChunk(receivedData, messageId, inode, chunkIndex, lockId);

	uint32_t min_server_version = header.type == SAU_CLTOMA_FUSE_WRITE_CHUNK ? kFirstXorVersion : 0;

	// Original Legacy (1.6.27) does not use lock ID's
	bool useDummyLockId = (header.type == CLTOMA_FUSE_WRITE_CHUNK);
	status = fs_writechunk(matoclserv_get_context(eptr), inode, chunkIndex, useDummyLockId,
			&lockId, &chunkId, &opflag, &fileLength, min_server_version);

	if (status != SAUNAFS_STATUS_OK) {
		serializer->serializeFuseWriteChunk(outMessage, messageId, status);
		matoclserv_createpacket(eptr, outMessage);
		return;
	}

	if (opflag) {   // wait for operation end
		auto operation = std::make_unique<DelayedChunkOperation>();
		passert(operation.get());
		memset(operation.get(), 0, sizeof(DelayedChunkOperation));
		operation->inode = inode;
		operation->chunkId = chunkId;
		operation->messageId = messageId;
		operation->fileLength = fileLength;
		operation->lockId = lockId;
		operation->type = FUSE_WRITE;
		operation->serializer = serializer;
		eptr->delayedChunkOperations.push_back(std::move(operation));
	} else {        // return status immediately
		dcm_modify(inode,eptr->sessionData->sessionId);
		status = matoclserv_fuse_write_chunk_respond(eptr, serializer,
				chunkId, messageId, fileLength, lockId);
		if (status != SAUNAFS_STATUS_OK) {
			fs_writeend(0, 0, chunkId, 0);  // ignore status - just do it.
		}
	}

	if (eptr->sessionData) {
		eptr->sessionData->currHourOperationsStats[15]++;
	}
}

void matoclserv_fuse_write_chunk_end(matoclserventry *eptr, PacketHeader header,
                                     const uint8_t *data) {
	sassert(header.type == CLTOMA_FUSE_WRITE_CHUNK_END
			|| header.type == SAU_CLTOMA_FUSE_WRITE_CHUNK_END);
	uint32_t messageId;
	uint64_t chunkId;
	uint32_t lockId;
	inode_t inode;
	uint64_t fileLength;
	uint8_t status;
	std::vector<uint8_t> outMessage;

	std::vector<uint8_t> request(data, data + header.length);
	const PacketSerializer* serializer = PacketSerializer::getSerializer(header.type, eptr->version);
	serializer->deserializeFuseWriteChunkEnd(request, messageId, chunkId, lockId, inode, fileLength);

	if (lockId == 0) {
		// this lock id passed to chunk_unlock would force chunk unlock
		status = SAUNAFS_ERROR_WRONGLOCKID;
	} else if (eptr->sessionData->flags & SESFLAG_READONLY) {
		status = SAUNAFS_ERROR_EROFS;
	} else {
		status = fs_writeend(inode, fileLength, chunkId, lockId);
	}

	dcm_modify(inode,eptr->sessionData->sessionId);
	serializer->serializeFuseWriteChunkEnd(outMessage, messageId, status);
	matoclserv_createpacket(eptr, outMessage);
}

void matoclserv_fuse_repair(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	uint32_t uid,gid;
	uint32_t msgid;
	uint32_t chunksnotchanged, chunkserased, chunksrepaired;
	uint8_t *ptr;
	uint8_t status;
	uint8_t correct_only = 0;

	constexpr uint32_t kMinExpectedPacketSize =
	    sizeof(msgid) + sizeof(inode) + sizeof(uid) + sizeof(gid);
	constexpr uint32_t kMaxExpectedPacketSize = kMinExpectedPacketSize + sizeof(correct_only);

	if (length == kMinExpectedPacketSize || length == kMaxExpectedPacketSize) {
		get32bit(&data, msgid);
		getINode(&data, inode);
		get32bit(&data, uid);
		get32bit(&data, gid);
		if (length == kMaxExpectedPacketSize) { correct_only = get8bit(&data); }
	} else {
		safs_pretty_syslog(
		    LOG_NOTICE, "CLTOMA_FUSE_REPAIR - wrong size (%" PRIu32 "/(%" PRIu32 "|%" PRIu32 "))",
		    length, kMinExpectedPacketSize, kMaxExpectedPacketSize);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	status = matoclserv_check_group_cache(eptr, gid);

	if (status == SAUNAFS_STATUS_OK) {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		status = fs_repair(context, inode, correct_only, &chunksnotchanged, &chunkserased,
		                   &chunksrepaired);
	}

	constexpr uint32_t kFailedSize = sizeof(msgid) + sizeof(status);
	constexpr uint32_t kSuccessSize =
	    sizeof(msgid) + sizeof(chunksnotchanged) + sizeof(chunkserased) + sizeof(chunksrepaired);

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_REPAIR,
	                              (status != SAUNAFS_STATUS_OK) ? kFailedSize : kSuccessSize);

	put32bit(&ptr,msgid);

	if (status != SAUNAFS_STATUS_OK) {
		put8bit(&ptr, status);
	} else {
		put32bit(&ptr, chunksnotchanged);
		put32bit(&ptr, chunkserased);
		put32bit(&ptr, chunksrepaired);
	}
}

void matoclserv_fuse_check(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	uint32_t chunkcount[CHUNK_MATRIX_SIZE];
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;

	constexpr uint32_t kExpectedSize = sizeof(msgid) + sizeof(inode);

	if (length != kExpectedSize) {
		safs_pretty_syslog(LOG_NOTICE, "CLTOMA_FUSE_CHECK - wrong size (%" PRIu32 "/%" PRIu32 ")",
		                   length, kExpectedSize);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);
	getINode(&data, inode);

	status = fs_checkfile(matoclserv_get_context(eptr), inode, chunkcount);

	if (status != SAUNAFS_STATUS_OK) {
		ptr = matoclserv_createpacket(eptr,MATOCL_FUSE_CHECK, sizeof(msgid) + sizeof(status));
		put32bit(&ptr,msgid);
		put8bit(&ptr,status);
	} else {
		if (eptr->version >= 0x010617) {
			ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_CHECK,
			                              sizeof(msgid) + CHUNK_MATRIX_SIZE * sizeof(uint32_t));
			put32bit(&ptr, msgid);
			for (uint32_t i = 0; i < CHUNK_MATRIX_SIZE; i++) { put32bit(&ptr, chunkcount[i]); }
		} else {
			uint8_t j = 0;
			for (uint32_t i = 0; i < CHUNK_MATRIX_SIZE; i++) {
				if (chunkcount[i] > 0) { j++; }
			}

			ptr =
			    matoclserv_createpacket(eptr, MATOCL_FUSE_CHECK,
			                            sizeof(msgid) + ((sizeof(uint8_t) + sizeof(uint16_t)) * j));

			put32bit(&ptr, msgid);

			for (uint32_t i = 0; i < CHUNK_MATRIX_SIZE; i++) {
				if (chunkcount[i] > 0) {
					put8bit(&ptr, i);
					if (chunkcount[i] <= 65535) {
						put16bit(&ptr, chunkcount[i]);
					} else {
						put16bit(&ptr, 65535);
					}
				}
			}
		}
	}
}

void matoclserv_fuse_request_task_id(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t msgid, taskid;
	cltoma::requestTaskId::deserialize(data, length, msgid);
	taskid = fs_reserve_job_id();
	MessageBuffer reply;
	matocl::requestTaskId::serialize(reply, msgid, taskid);
	matoclserv_createpacket(eptr, reply);
}

void matoclserv_fuse_gettrashtime(matoclserventry *eptr,const uint8_t *data,uint32_t length) {
	inode_t inode;
	uint8_t gmode;
	TrashtimeMap fileTrashtimes, dirTrashtimes;
	uint32_t fileTrashtimesSize, dirTrashtimesSize;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;

	constexpr uint32_t kExpectedSize = sizeof(msgid) + sizeof(inode) + sizeof(gmode);

	if (length != kExpectedSize) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "CLTOMA_FUSE_GETTRASHTIME - wrong size (%" PRIu32 "/%" PRIu32 ")",
		                   length, kExpectedSize);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);
	getINode(&data, inode);
	gmode = get8bit(&data);

	status = fs_gettrashtime_prepare(matoclserv_get_context(eptr), inode, gmode, fileTrashtimes,
	                                 dirTrashtimes);
	fileTrashtimesSize = fileTrashtimes.size();
	dirTrashtimesSize = dirTrashtimes.size();

	constexpr uint32_t kFailedSize = sizeof(msgid) + sizeof(status);
	constexpr uint32_t kSuccesBaseSize =
	    sizeof(msgid) + sizeof(fileTrashtimesSize) + sizeof(dirTrashtimesSize);
	const uint32_t kSuccessSize =
	    kSuccesBaseSize + ((fileTrashtimesSize + dirTrashtimesSize) *
	                       (sizeof(TrashtimeMap::key_type) + sizeof(TrashtimeMap::mapped_type)));

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_GETTRASHTIME,
	                              (status != SAUNAFS_STATUS_OK) ? kFailedSize : kSuccessSize);

	put32bit(&ptr,msgid);

	if (status!=SAUNAFS_STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		put32bit(&ptr, fileTrashtimesSize);
		put32bit(&ptr, dirTrashtimesSize);
		fs_gettrashtime_store(fileTrashtimes, dirTrashtimes, ptr);
	}
}

void matoclserv_fuse_settrashtime_wake_up(uint32_t session_id, uint32_t msgid,
					  std::shared_ptr<SetTrashtimeTask::StatsArray> settrashtime_stats,
					  uint8_t status) {
	matoclserventry *eptr = matoclserv_find_connection(session_id);
	if (!eptr) {
		return;
	}

	MessageBuffer reply;
	if (status != SAUNAFS_STATUS_OK) {
		serializeLegacyPacket(reply, MATOCL_FUSE_SETTRASHTIME, msgid, status);
	} else {
		inode_t changed, notchanged, notpermitted;
		changed = (*settrashtime_stats)[SetTrashtimeTask::kChanged];
		notchanged = (*settrashtime_stats)[SetTrashtimeTask::kNotChanged];
		notpermitted = (*settrashtime_stats)[SetTrashtimeTask::kNotPermitted];
		serializeLegacyPacket(reply, MATOCL_FUSE_SETTRASHTIME, msgid, changed,
				       notchanged, notpermitted);
	}
	matoclserv_createpacket(eptr, std::move(reply));
}

void matoclserv_fuse_settrashtime(matoclserventry *eptr, PacketHeader header, const uint8_t *data) {
	inode_t inode;
	uint32_t uid, trashtime, msgid;
	uint8_t smode, status;

	deserializeAllLegacyPacketDataNoHeader(data, header.length, msgid, inode,
							uid, trashtime, smode);
// limits check
	status = SAUNAFS_STATUS_OK;
	switch (smode & SMODE_TMASK) {
	case SMODE_SET:
		if (trashtime < eptr->sessionData->minTrashTime ||
		    trashtime > eptr->sessionData->maxTrashTime) {
			status = SAUNAFS_ERROR_EPERM;
		}
		break;
	case SMODE_INCREASE:
		if (trashtime > eptr->sessionData->maxTrashTime) {
			status = SAUNAFS_ERROR_EPERM;
		}
		break;
	case SMODE_DECREASE:
		if (trashtime < eptr->sessionData->minTrashTime) {
			status = SAUNAFS_ERROR_EPERM;
		}
		break;
	}

	// array for settrashtime operation statistics
	auto settrashtime_stats = std::make_shared<SetTrashtimeTask::StatsArray>();

	if (status == SAUNAFS_STATUS_OK) {
		status = fs_settrashtime(matoclserv_get_context(eptr, uid, 0), inode, trashtime,
					 smode, settrashtime_stats,
			   std::bind(matoclserv_fuse_settrashtime_wake_up, eptr->sessionData->sessionId,
				     msgid, settrashtime_stats, std::placeholders::_1));
	}

	if (status != SAUNAFS_ERROR_WAITING) {
		matoclserv_fuse_settrashtime_wake_up(eptr->sessionData->sessionId, msgid,
		                                     settrashtime_stats, status);
	}
}

void matoclserv_fuse_getgoal(matoclserventry *eptr, PacketHeader header, const uint8_t *data) {
	inode_t inode;
	uint32_t msgid;
	uint8_t gmode;

	if (header.type == CLTOMA_FUSE_GETGOAL) {
		deserializeAllLegacyPacketDataNoHeader(data, header.length, msgid, inode, gmode);
	} else if (header.type == SAU_CLTOMA_FUSE_GETGOAL) {
		cltoma::fuseGetGoal::deserialize(data, header.length, msgid, inode, gmode);
	} else {
		throw IncorrectDeserializationException(
				"Unknown packet type for matoclserv_fuse_getgoal: " + std::to_string(header.type));
	}

	GoalStatistics fgtab{{0}}, dgtab{{0}}; // explicit value initialization to clear variables
	uint8_t status = fs_getgoal(matoclserv_get_context(eptr), inode, gmode, fgtab, dgtab);

	MessageBuffer reply;
	if (status == SAUNAFS_STATUS_OK) {
		const std::map<int, Goal>& goalDefinitions = fs_get_goal_definitions();
		std::vector<FuseGetGoalStats> sauReply;
		LegacyVector<std::pair<uint8_t, inode_t>> legacyReplyFiles, legacyReplyDirectories;
		for (const auto &goal : goalDefinitions) {
			if (fgtab[goal.first] || dgtab[goal.first]) {
				sauReply.emplace_back(goal.second.getName(), fgtab[goal.first], dgtab[goal.first]);
			}
			if (fgtab[goal.first] > 0) {
				legacyReplyFiles.emplace_back(goal.first, fgtab[goal.first]);
			}
			if (dgtab[goal.first] > 0) {
				legacyReplyDirectories.emplace_back(goal.first, dgtab[goal.first]);
			}
		}
		if (header.type == SAU_CLTOMA_FUSE_GETGOAL) {
			matocl::fuseGetGoal::serialize(reply, msgid, sauReply);
		} else {
			serializeLegacyPacket(reply, MATOCL_FUSE_GETGOAL,
					msgid,
					uint8_t(legacyReplyFiles.size()),
					uint8_t(legacyReplyDirectories.size()),
					legacyReplyFiles,
					legacyReplyDirectories);
		}
	} else {
		if (header.type == SAU_CLTOMA_FUSE_GETGOAL) {
			matocl::fuseGetGoal::serialize(reply, msgid, status);
		} else {
			serializeLegacyPacket(reply, MATOCL_FUSE_GETGOAL, msgid, status);
		}
	}
	matoclserv_createpacket(eptr, std::move(reply));
}

void matoclserv_fuse_setgoal_wake_up(uint32_t session_id, uint32_t msgid, uint32_t type,
				     std::shared_ptr<SetGoalTask::StatsArray> setgoal_stats,
				     uint32_t status) {
	matoclserventry *eptr = matoclserv_find_connection(session_id);
	if (!eptr) {
		return;
	}

	MessageBuffer reply;
	if (status == SAUNAFS_STATUS_OK) {
		inode_t changed, notchanged, notpermitted;
		changed = (*setgoal_stats)[SetGoalTask::kChanged];
		notchanged = (*setgoal_stats)[SetGoalTask::kNotChanged];
		notpermitted = (*setgoal_stats)[SetGoalTask::kNotPermitted];

		if (type == SAU_CLTOMA_FUSE_SETGOAL) {
			matocl::fuseSetGoal::serialize(reply, msgid, changed, notchanged, notpermitted);
		} else {
			serializeLegacyPacket(reply, MATOCL_FUSE_SETGOAL,
					msgid, changed, notchanged, notpermitted);
		}
	} else {
		if (type == SAU_CLTOMA_FUSE_SETGOAL) {
			matocl::fuseSetGoal::serialize(reply, msgid, status);
		} else {
			serializeLegacyPacket(reply, MATOCL_FUSE_SETGOAL, msgid, status);
		}
	}
	matoclserv_createpacket(eptr, std::move(reply));
}

void matoclserv_fuse_setgoal(matoclserventry *eptr, PacketHeader header, const uint8_t *data) {
	inode_t inode;
	uint32_t uid;
	uint32_t msgid;
	uint8_t goalId = 0, smode;
	uint8_t status = SAUNAFS_STATUS_OK;

	if (header.type == CLTOMA_FUSE_SETGOAL) {
		deserializeAllLegacyPacketDataNoHeader(data, header.length,
				msgid, inode, uid, goalId, smode);
	} else if (header.type == SAU_CLTOMA_FUSE_SETGOAL) {
		std::string goalName;
		cltoma::fuseSetGoal::deserialize(data, header.length,
				msgid, inode, uid, goalName, smode);
		// find a proper goalId,
		const std::map<int, Goal> &goalDefinitions = fs_get_goal_definitions();
		bool goalFound = false;
		for (const auto &goal : goalDefinitions) {
			if (goal.second.getName() == goalName) {
				goalId = goal.first;
				goalFound = true;
				break;
			}
		}
		if (!goalFound) {
			status = SAUNAFS_ERROR_EINVAL;
		}
	} else {
		throw IncorrectDeserializationException(
				"Unknown packet type for matoclserv_fuse_getgoal: " +
				std::to_string(header.type));
	}

	if (status == SAUNAFS_STATUS_OK && !GoalId::isValid(goalId)) {
		status = SAUNAFS_ERROR_EINVAL;
	}
	if (status == SAUNAFS_STATUS_OK) {
		if (status == SAUNAFS_STATUS_OK && goalId < eptr->sessionData->minGoal) {
			status = SAUNAFS_ERROR_EPERM;
		}
		if (status == SAUNAFS_STATUS_OK && goalId > eptr->sessionData->maxGoal) {
			status = SAUNAFS_ERROR_EPERM;
		}
	}

	// array for setgoal operation statistics
	auto setgoal_stats = std::make_shared<SetGoalTask::StatsArray>();

	if (status == SAUNAFS_STATUS_OK) {
		FsContext context = matoclserv_get_context(eptr, uid, 0);
		status = fs_setgoal(context, inode, goalId, smode, setgoal_stats,
			   std::bind(matoclserv_fuse_setgoal_wake_up, eptr->sessionData->sessionId,
				     msgid, header.type, setgoal_stats, std::placeholders::_1));
	}

	if (status != SAUNAFS_ERROR_WAITING) {
		matoclserv_fuse_setgoal_wake_up(eptr->sessionData->sessionId, msgid, header.type,
						setgoal_stats, status);
	}
}

void matoclserv_fuse_geteattr(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	uint32_t msgid;
	constexpr uint8_t kMaxEattr = 16;
	uint32_t feattrtab[kMaxEattr];
	uint32_t deattrtab[kMaxEattr];
	uint8_t i, fn, dn, gmode;
	uint8_t *ptr;
	uint8_t status;

	constexpr uint32_t kExpectedSize = sizeof(msgid) + sizeof(inode) + sizeof(gmode);

	if (length != kExpectedSize) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "CLTOMA_FUSE_GETEATTR - wrong size (%" PRIu32 "/%" PRIu32 ")", length,
		                   kExpectedSize);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);
	getINode(&data, inode);
	gmode = get8bit(&data);

	status = fs_geteattr(matoclserv_get_context(eptr), inode, gmode, feattrtab, deattrtab);
	fn = 0;
	dn = 0;

	if (status == SAUNAFS_STATUS_OK) {
		for (i = 0; i < kMaxEattr; i++) {
			if (feattrtab[i]) { fn++; }
			if (deattrtab[i]) { dn++; }
		}
	}

	constexpr uint32_t kFailedSize = sizeof(msgid) + sizeof(status);
	const uint32_t kSuccessSize =
	    sizeof(msgid) + sizeof(fn) + sizeof(dn) + (sizeof(uint8_t) + sizeof(uint32_t)) * (fn + dn);

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_GETEATTR,
	                              (status != SAUNAFS_STATUS_OK) ? kFailedSize : kSuccessSize);

	put32bit(&ptr, msgid);

	if (status != SAUNAFS_STATUS_OK) {
		put8bit(&ptr, status);
	} else {
		put8bit(&ptr, fn);
		put8bit(&ptr, dn);
		for (i = 0; i < kMaxEattr; i++) {
			if (feattrtab[i]) {
				put8bit(&ptr, i);
				put32bit(&ptr, feattrtab[i]);
			}
		}
		for (i = 0; i < kMaxEattr; i++) {
			if (deattrtab[i]) {
				put8bit(&ptr, i);
				put32bit(&ptr, deattrtab[i]);
			}
		}
	}
}

void matoclserv_fuse_seteattr(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	uint32_t uid;
	uint32_t msgid;
	uint8_t eattr,smode;
	inode_t changed,notchanged,notpermitted;
	uint8_t *ptr;
	uint8_t status;

	constexpr uint32_t kExpectedSize =
	    sizeof(msgid) + sizeof(inode) + sizeof(uid) + sizeof(eattr) + sizeof(smode);

	if (length != kExpectedSize) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "CLTOMA_FUSE_SETEATTR - wrong size (%" PRIu32 "/%" PRIu32 ")", length,
		                   kExpectedSize);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);
	getINode(&data, inode);
	get32bit(&data, uid);
	eattr = get8bit(&data);
	smode = get8bit(&data);

	status = fs_seteattr(matoclserv_get_context(eptr, uid, 0), inode, eattr, smode, &changed,
	                     &notchanged, &notpermitted);

	constexpr uint32_t kFailedSize = sizeof(msgid) + sizeof(status);
	constexpr uint32_t kSuccessSize =
	    sizeof(msgid) + sizeof(changed) + sizeof(notchanged) + sizeof(notpermitted);

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_SETEATTR,
	                              (status != SAUNAFS_STATUS_OK) ? kFailedSize : kSuccessSize);

	put32bit(&ptr, msgid);

	if (status != SAUNAFS_STATUS_OK) {
		put8bit(&ptr, status);
	} else {
		putINode(&ptr, changed);
		putINode(&ptr, notchanged);
		putINode(&ptr, notpermitted);
	}
}

void matoclserv_fuse_getxattr(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	uint32_t uid, gid;
	uint32_t msgid;
	uint8_t opened;
	uint8_t mode;
	uint8_t *ptr;
	uint8_t status;
	uint8_t anleng;
	const uint8_t *attrname;

	constexpr uint32_t kExpectedMinSize = sizeof(msgid) + sizeof(inode) + sizeof(opened) +
	                                      sizeof(uid) + sizeof(gid) + sizeof(anleng) + sizeof(mode);

	if (length < kExpectedMinSize) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "CLTOMA_FUSE_GETXATTR - wrong min size (%" PRIu32 "/%" PRIu32 ")",
		                   length, kExpectedMinSize);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);
	getINode(&data, inode);
	opened = get8bit(&data);
	get32bit(&data, uid);
	get32bit(&data, gid);
	anleng = get8bit(&data);
	attrname = data;
	data += anleng;

	if (length != kExpectedMinSize + anleng) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "CLTOMA_FUSE_GETXATTR - wrong size (%" PRIu32 ":anleng=%" PRIu8 ")",
		                   length, anleng);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	status = matoclserv_check_group_cache(eptr, gid);

	constexpr uint32_t kFailedSize = sizeof(msgid) + sizeof(status);

	if (status != SAUNAFS_STATUS_OK) {
		ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_GETXATTR, kFailedSize);
		put32bit(&ptr,msgid);
		put8bit(&ptr,status);
		return;
	}

	FsContext context = matoclserv_get_context(eptr, uid, gid);

	mode = get8bit(&data);

	if (mode != XATTR_GMODE_GET_DATA && mode != XATTR_GMODE_LENGTH_ONLY) {
		ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_GETXATTR, kFailedSize);
		put32bit(&ptr, msgid);
		put8bit(&ptr, SAUNAFS_ERROR_EINVAL);
	} else if (anleng == 0) {
		void *xanode;
		uint32_t xasize;
		status = fs_listxattr_leng(context, inode, opened, &xanode, &xasize);
		const uint32_t kSuccessSize =
		    sizeof(msgid) + sizeof(xasize) + ((mode == XATTR_GMODE_GET_DATA) ? xasize : 0);
		ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_GETXATTR,
		                              (status != SAUNAFS_STATUS_OK) ? kFailedSize : kSuccessSize);

		put32bit(&ptr,msgid);

		if (status != SAUNAFS_STATUS_OK) {
			put8bit(&ptr, status);
		} else {
			put32bit(&ptr, xasize);
			if (mode == XATTR_GMODE_GET_DATA && xasize > 0) { fs_listxattr_data(xanode, ptr); }
		}
	} else {
		uint8_t *attrvalue;
		uint32_t avleng;
		status = fs_getxattr(context, inode, opened, anleng, attrname, &avleng, &attrvalue);
		const uint32_t kSuccessSize =
		    sizeof(msgid) + sizeof(avleng) + ((mode == XATTR_GMODE_GET_DATA) ? avleng : 0);
		ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_GETXATTR,
		                              (status != SAUNAFS_STATUS_OK) ? kFailedSize : kSuccessSize);

		put32bit(&ptr, msgid);

		if (status != SAUNAFS_STATUS_OK) {
			put8bit(&ptr, status);
		} else {
			put32bit(&ptr, avleng);
			if (mode == XATTR_GMODE_GET_DATA && avleng > 0) { memcpy(ptr, attrvalue, avleng); }
		}
	}
}

void matoclserv_fuse_setxattr(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	uint32_t uid, gid;
	uint32_t msgid;
	const uint8_t *attrname,*attrvalue;
	uint8_t opened;
	uint8_t anleng;
	uint32_t avleng;
	uint8_t mode;
	uint8_t *ptr;
	uint8_t status;

	constexpr uint32_t kExpectedMinSize = sizeof(msgid) + sizeof(inode) + sizeof(opened) +
	                                      sizeof(uid) + sizeof(gid) + sizeof(anleng) +
	                                      sizeof(avleng) + sizeof(mode);

	if (length < kExpectedMinSize) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "CLTOMA_FUSE_SETXATTR - wrong min size (%" PRIu32 "/%" PRIu32 ")",
		                   length, kExpectedMinSize);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);
	getINode(&data, inode);
	opened = get8bit(&data);
	get32bit(&data, uid);
	get32bit(&data, gid);
	anleng = get8bit(&data);

	if (length < kExpectedMinSize + anleng) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "CLTOMA_FUSE_SETXATTR - wrong size (%" PRIu32 ":anleng=%" PRIu8 ")",
		                   length, anleng);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	attrname = data;
	data += anleng;
	get32bit(&data, avleng);

	if (length != kExpectedMinSize + anleng + avleng) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "CLTOMA_FUSE_SETXATTR - wrong size (%" PRIu32 ":anleng=%" PRIu8
		                   ":avleng=%" PRIu32 ")",
		                   length, anleng, avleng);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	attrvalue = data;
	data += avleng;
	mode = get8bit(&data);

	status = matoclserv_check_group_cache(eptr, gid);

	if (status == SAUNAFS_STATUS_OK) {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		status = fs_setxattr(context, inode, opened, anleng, attrname, avleng, attrvalue, mode);
	}

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_SETXATTR, sizeof(msgid) + sizeof(status));
	put32bit(&ptr, msgid);
	put8bit(&ptr, status);
}

void matoclserv_fuse_append(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	inode_t inode_src;
	uint32_t uid;
	uint32_t gid;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;

	constexpr uint32_t kExpectedSize =
	    sizeof(msgid) + sizeof(inode) + sizeof(inode_src) + sizeof(uid) + sizeof(gid);

	if (length != kExpectedSize) {
		safs_pretty_syslog(LOG_NOTICE, "CLTOMA_FUSE_APPEND - wrong size (%" PRIu32 "/%" PRIu32 ")",
		                   length, kExpectedSize);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);
	getINode(&data, inode);
	getINode(&data, inode_src);
	get32bit(&data, uid);
	get32bit(&data, gid);

	status = matoclserv_check_group_cache(eptr, gid);

	if (status == SAUNAFS_STATUS_OK) {
		auto context = matoclserv_get_context(eptr, uid, gid);
		status = fs_append(context, inode, inode_src);
	}

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_APPEND, sizeof(msgid) + sizeof(status));
	put32bit(&ptr, msgid);
	put8bit(&ptr, status);
}

void matoclserv_fuse_snapshot_wake_up(uint32_t type, uint32_t session_id, uint32_t msgid, int status) {
	matoclserventry *eptr = matoclserv_find_connection(session_id);
	if (!eptr) {
		return;
	}

	MessageBuffer buffer;
	if (type == SAU_CLTOMA_FUSE_SNAPSHOT) {
		matocl::snapshot::serialize(buffer, msgid, status);
	} else {
		serializeLegacyPacket(buffer, MATOCL_FUSE_SNAPSHOT, msgid, status);
	}
	matoclserv_createpacket(eptr, std::move(buffer));
}

void matoclserv_fuse_snapshot(matoclserventry *eptr, PacketHeader header, const uint8_t *data) {
	inode_t inode;
	inode_t inode_dst;
	uint32_t uid, gid;
	uint8_t canoverwrite;
	uint32_t msgid;
	uint8_t status;
	uint32_t job_id;
	uint8_t ignore_missing_src = 0;
	uint32_t initial_batch_size = 0;
	LegacyString<uint8_t> name_dst;

	if (header.type == CLTOMA_FUSE_SNAPSHOT) {
		deserializeAllLegacyPacketDataNoHeader(data, header.length,
				msgid, inode, inode_dst, name_dst, uid, gid, canoverwrite);
		job_id = fs_reserve_job_id();
	} else if (header.type == SAU_CLTOMA_FUSE_SNAPSHOT) {
		cltoma::snapshot::deserialize(data, header.length, msgid, job_id, inode,
		                              inode_dst, name_dst, uid, gid, canoverwrite,
		                              ignore_missing_src, initial_batch_size);
	} else {
		throw IncorrectDeserializationException(
				"Unknown packet type for matoclserv_fuse_snapshot: " +
				std::to_string(header.type));
	}
	status = matoclserv_check_group_cache(eptr, gid);
	if (status == SAUNAFS_STATUS_OK) {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		status = fs_snapshot(context, inode, inode_dst, HString(std::move(name_dst)),
		                     canoverwrite, ignore_missing_src, initial_batch_size,
		                     std::bind(matoclserv_fuse_snapshot_wake_up, header.type,
		                     eptr->sessionData->sessionId, msgid, std::placeholders::_1), job_id);
	}
	if (status != SAUNAFS_ERROR_WAITING) {
		matoclserv_fuse_snapshot_wake_up(header.type, eptr->sessionData->sessionId, msgid, status);
	}
}

void matoclserv_fuse_getdirstats_old(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode = 0, inodes = 0, files = 0, dirs = 0, links = 0;
	uint32_t chunks = 0;
	uint64_t leng = 0, size = 0, rsize = 0;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;

	constexpr uint32_t kExpectedLength = sizeof(msgid) + sizeof(inode);

	if (length != kExpectedLength) {
		safs_pretty_syslog(LOG_NOTICE,
			"CLTOMA_FUSE_GETDIRSTATS - wrong size (%" PRIu32 "/%" PRIu32 ")", length,
			kExpectedLength);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);
	getINode(&data, inode);

	status = fs_get_dir_stats(matoclserv_get_context(eptr), inode, &inodes, &dirs, &files, &links,
	                          &chunks, &leng, &size, &rsize);

	constexpr uint8_t kDirStatsLegacyFullPayload =
	    sizeof(msgid) + sizeof(inodes) + sizeof(dirs) + sizeof(files) + sizeof(links) +
	    (2 * sizeof(uint32_t)) + sizeof(chunks) + (2 * sizeof(uint32_t)) + sizeof(leng) +
	    sizeof(size) + sizeof(rsize);

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_GETDIRSTATS,
	                              (status != SAUNAFS_STATUS_OK) ? sizeof(msgid) + sizeof(status)
	                                                            : kDirStatsLegacyFullPayload);

	put32bit(&ptr, msgid);

	if (status != SAUNAFS_STATUS_OK) {
		put8bit(&ptr, status);
	} else {
		putINode(&ptr, inodes);
		putINode(&ptr, dirs);
		putINode(&ptr, files);
		putINode(&ptr, links);
		put32bit(&ptr, 0);
		put32bit(&ptr, 0);
		put32bit(&ptr, chunks);
		put32bit(&ptr, 0);
		put32bit(&ptr, 0);
		put64bit(&ptr, leng);
		put64bit(&ptr, size);
		put64bit(&ptr, rsize);
	}
}

void matoclserv_fuse_getdirstats(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode = 0, inodes = 0, files = 0, dirs = 0, links = 0;
	uint32_t chunks = 0;
	uint64_t leng = 0, size = 0, rsize = 0;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;

	constexpr uint32_t kExpectedLength = sizeof(msgid) + sizeof(inode);

	if (length != kExpectedLength) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "CLTOMA_FUSE_GETDIRSTATS - wrong size (%" PRIu32 "/%" PRIu32 ")", length,
		                   kExpectedLength);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);
	getINode(&data, inode);

	status = fs_get_dir_stats(matoclserv_get_context(eptr), inode, &inodes, &dirs, &files, &links,
	                          &chunks, &leng, &size, &rsize);

	constexpr uint8_t kFailedSize = sizeof(msgid) + sizeof(status);
	constexpr uint8_t kSuccessSize = sizeof(msgid) + sizeof(inodes) + sizeof(dirs) + sizeof(files) +
	                                 sizeof(links) + sizeof(chunks) + sizeof(leng) + sizeof(size) +
	                                 sizeof(rsize);

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_GETDIRSTATS,
	                              (status != SAUNAFS_STATUS_OK) ? kFailedSize : kSuccessSize);

	put32bit(&ptr, msgid);

	if (status != SAUNAFS_STATUS_OK) {
		put8bit(&ptr, status);
	} else {
		putINode(&ptr, inodes);
		putINode(&ptr, dirs);
		putINode(&ptr, files);
		putINode(&ptr, links);
		put32bit(&ptr, chunks);  // TODO(Guillex): check possible overflow
		put64bit(&ptr, leng);
		put64bit(&ptr, size);
		put64bit(&ptr, rsize);
	}
}

void matoclserv_fuse_gettrash(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	uint32_t dleng;

	if (length != sizeof(msgid)) {
		safs_pretty_syslog(LOG_NOTICE, "CLTOMA_FUSE_GETTRASH - wrong size (%" PRIu32 "/%zu)",
		                   length, sizeof(msgid));
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);

	status = fs_readtrash_size(eptr->sessionData->rootInode,eptr->sessionData->flags,&dleng);

	ptr = matoclserv_createpacket(
	    eptr, MATOCL_FUSE_GETTRASH,
	    (status != SAUNAFS_STATUS_OK) ? sizeof(msgid) + sizeof(status) : (sizeof(msgid) + dleng));

	put32bit(&ptr, msgid);

	if (status != SAUNAFS_STATUS_OK) {
		put8bit(&ptr, status);
	} else {
		fs_readtrash_data(eptr->sessionData->rootInode, eptr->sessionData->flags, ptr);
	}
}

void matoclserv_fuse_gettrash(matoclserventry *eptr, const PacketHeader &header,
                              const uint8_t *data) {
	uint32_t off, max_entries, msg_id;
	cltoma::fuseGetTrash::deserialize(data, header.length, msg_id, off, max_entries);
	std::vector<NamedInodeEntry> entries;
	fs_readtrash(off,
	             std::min<uint32_t>(max_entries, matocl::fuseGetDir::kMaxNumberOfDirectoryEntries),
	             entries);
	matoclserv_createpacket(eptr, matocl::fuseGetTrash::build(msg_id, entries));
}

void matoclserv_fuse_getdetachedattr(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	Attributes attr;
	uint32_t msgid;
	uint8_t dtype;
	uint8_t *ptr;
	uint8_t status;

	constexpr uint32_t kExpectedMinLength = sizeof(msgid) + sizeof(inode);
	constexpr uint32_t kExpectedMaxLength = kExpectedMinLength + sizeof(dtype);

	if (length < kExpectedMinLength || length > kExpectedMaxLength) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "CLTOMA_FUSE_GETDETACHEDATTR - wrong size (%" PRIu32 "/%u-%u)", length,
		                   kExpectedMinLength, kExpectedMaxLength);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}
	get32bit(&data, msgid);
	getINode(&data, inode);
	if (length == kExpectedMaxLength) {
		dtype = get8bit(&data);
	} else {
		dtype = DTYPE_UNKNOWN;
	}

	status = fs_getdetachedattr(eptr->sessionData->rootInode, eptr->sessionData->flags, inode, attr,
	                            dtype);

	constexpr uint32_t kFailedSize = sizeof(msgid) + sizeof(status);
	constexpr uint32_t kSuccessSize = sizeof(msgid) + attr.size();

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_GETDETACHEDATTR,
	                              (status != SAUNAFS_STATUS_OK) ? kFailedSize : kSuccessSize);

	put32bit(&ptr, msgid);

	if (status != SAUNAFS_STATUS_OK) {
		put8bit(&ptr, status);
	} else {
		memcpy(ptr, attr.data(), attr.size());
	}
}

void matoclserv_fuse_gettrashpath(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	std::string path;

	constexpr uint32_t kExpectedLength = sizeof(msgid) + sizeof(inode);

	if (length != kExpectedLength) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "CLTOMA_FUSE_GETTRASHPATH - wrong size (%" PRIu32 "/%" PRIu32 ")",
		                   length, kExpectedLength);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);
	getINode(&data, inode);

	status = fs_gettrashpath(eptr->sessionData->rootInode, eptr->sessionData->flags, inode, path);

	constexpr uint32_t kFailedSize = sizeof(msgid) + sizeof(status);
	const uint32_t kSuccessSize = sizeof(msgid) + sizeof(uint32_t) + path.length() + 1;

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_GETTRASHPATH,
	                              (status != SAUNAFS_STATUS_OK) ? kFailedSize : kSuccessSize);

	put32bit(&ptr, msgid);

	if (status != SAUNAFS_STATUS_OK) {
		put8bit(&ptr, status);
	} else {
		// Safe cast, the length should always fit
		put32bit(&ptr, static_cast<uint32_t>(path.length() + 1));
		if (path.length() > 0) {
			memcpy(ptr, path.c_str(), path.length());
		}
		ptr[path.length()] = 0;
	}
}

void matoclserv_fuse_settrashpath(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	const uint8_t *path;
	uint32_t pleng;
	uint32_t msgid;
	uint8_t status;
	uint8_t *ptr;

	constexpr uint32_t kExpectedMinLength = sizeof(msgid) + sizeof(inode) + sizeof(pleng);

	if (length < kExpectedMinLength) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "CLTOMA_FUSE_SETTRASHPATH - wrong size (%" PRIu32 "/<%" PRIu32 ")",
		                   length, kExpectedMinLength);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);
	getINode(&data, inode);
	get32bit(&data, pleng);

	if (length != kExpectedMinLength + pleng) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "CLTOMA_FUSE_SETTRASHPATH - wrong size (%" PRIu32 "/%" PRIu32 ")",
		                   length, kExpectedMinLength + pleng);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	path = data;
	data += pleng;
	while (pleng > 0 && path[pleng - 1] == 0) { pleng--; }

	status = fs_settrashpath(matoclserv_get_context(eptr), inode, std::string((char*)path, pleng));

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_SETTRASHPATH, sizeof(msgid) + sizeof(status));

	put32bit(&ptr, msgid);
	put8bit(&ptr, status);
}

void matoclserv_fuse_undel(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	uint32_t msgid;
	uint8_t status;
	uint8_t *ptr;

	constexpr uint32_t kExpectedLength = sizeof(msgid) + sizeof(inode);

	if (length != kExpectedLength) {
		safs_pretty_syslog(LOG_NOTICE, "CLTOMA_FUSE_UNDEL - wrong size (%" PRIu32 "/%" PRIu32 ")",
		                   length, kExpectedLength);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);
	getINode(&data, inode);

	status = fs_undel(matoclserv_get_context(eptr), inode);

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_UNDEL, sizeof(msgid) + sizeof(status));

	put32bit(&ptr, msgid);
	put8bit(&ptr, status);
}

void matoclserv_fuse_purge(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;

	constexpr uint32_t kExpectedLength = sizeof(msgid) + sizeof(inode);

	if (length != kExpectedLength) {
		safs_pretty_syslog(LOG_NOTICE, "CLTOMA_FUSE_PURGE - wrong size (%" PRIu32 "/%" PRIu32 ")",
		                   length, kExpectedLength);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);
	getINode(&data, inode);

	status = fs_purge(matoclserv_get_context(eptr), inode);

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_PURGE, sizeof(msgid) + sizeof(status));
	put32bit(&ptr, msgid);
	put8bit(&ptr, status);
}

void matoclserv_fuse_getreserved(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t msgid;
	uint8_t *ptr;
	uint8_t status;
	uint32_t dleng;

	constexpr uint32_t kExpectedSize = sizeof(msgid);

	if (length != kExpectedSize) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "CLTOMA_FUSE_GETRESERVED - wrong size (%" PRIu32 "/%" PRIu32 ")", length,
		                   kExpectedSize);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);

	status = fs_readreserved_size(eptr->sessionData->rootInode, eptr->sessionData->flags, &dleng);

	constexpr uint32_t kFailedSize = sizeof(msgid) + sizeof(status);
	const uint32_t kSuccessSize = sizeof(msgid) + dleng;
	const uint32_t answerSize = (status!=SAUNAFS_STATUS_OK) ? kFailedSize : kSuccessSize;

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_GETRESERVED, answerSize);

	put32bit(&ptr, msgid);

	if (status!=SAUNAFS_STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		fs_readreserved_data(eptr->sessionData->rootInode, eptr->sessionData->flags, ptr);
	}
}

void matoclserv_fuse_getreserved(matoclserventry *eptr, const PacketHeader &header,
                                 const uint8_t *data) {
	uint32_t off, max_entries, msg_id;
	cltoma::fuseGetReserved::deserialize(data, header.length, msg_id, off, max_entries);
	std::vector<NamedInodeEntry> entries;
	fs_readreserved(
	    off, std::min<uint32_t>(max_entries, matocl::fuseGetDir::kMaxNumberOfDirectoryEntries),
	    entries);
	matoclserv_createpacket(eptr, matocl::fuseGetReserved::build(msg_id, entries));
}

void matoclserv_fuse_deleteacl(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t messageId, uid, gid;
	inode_t inode;
	AclType type;
	cltoma::fuseDeleteAcl::deserialize(data, length, messageId, inode, uid, gid, type);

	uint8_t status = matoclserv_check_group_cache(eptr, gid);
	if (status == SAUNAFS_STATUS_OK) {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		status = fs_deleteacl(context, inode, type);
	}
	matoclserv_createpacket(eptr, matocl::fuseDeleteAcl::build(messageId, status));
}

void matoclserv_fuse_getacl(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t messageId, uid, gid;
	inode_t inode;
	AclType type;
	cltoma::fuseGetAcl::deserialize(data, length, messageId, inode, uid, gid, type);
	safs::log_trace("master.cltoma_fuse_getacl: {}", inode);

	MessageBuffer reply;
	RichACL acl;

	uint8_t status = matoclserv_check_group_cache(eptr, gid);

	if (status == SAUNAFS_STATUS_OK) {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		status = fs_getacl(context, inode, acl);
	}

	if (status == SAUNAFS_STATUS_OK) {
		if (eptr->version >= kRichACLVersion) {
			FSNode *node = fsnodes_id_to_node(inode);
			uint32_t owner_id = node ? node->uid : RichACL::Ace::kInvalidId;
			matocl::fuseGetAcl::serialize(reply, messageId, owner_id, acl);
		} else {
			std::pair<bool, AccessControlList> posix_acl;
			if (type == AclType::kDefault) {
				posix_acl = acl.convertToDefaultPosixACL();
			} else {
				// default behavior for unknown acl type.
				posix_acl = acl.convertToPosixACL();
			}

			if (posix_acl.first) {
				if (eptr->version >= kACL11Version) {
					matocl::fuseGetAcl::serialize(reply, messageId, posix_acl.second);
				} else {
					legacy::AccessControlList legacy_acl = posix_acl.second;
					matocl::fuseGetAcl::serialize(reply, messageId, legacy_acl);
				}
			} else {
				status = SAUNAFS_ERROR_ENOATTR;
			}
		}
	}

	if (status != SAUNAFS_STATUS_OK) {
		matocl::fuseGetAcl::serialize(reply, messageId, status);
	}

	matoclserv_createpacket(eptr, std::move(reply));
}

static void matoclserv_lock_wake_up(uint32_t sessionid, uint32_t messageId, safs_locks::Type type) {
	matoclserventry *eptr;
	MessageBuffer reply;

	eptr = matoclserv_find_connection(sessionid);

	if (eptr == nullptr) {
		return;
	}

	switch (type) {
	case safs_locks::Type::kFlock:
		matocl::fuseFlock::serialize(reply, messageId, SAUNAFS_STATUS_OK);
		break;
	case safs_locks::Type::kPosix:
		matocl::fuseSetlk::serialize(reply, messageId, SAUNAFS_STATUS_OK);
		break;
	default:
		safs::log_err("Incorrect lock type passed for lock wakeup: {}", (unsigned)type);
		return;
	}

	matoclserv_createpacket(eptr, std::move(reply));
}

static void matoclserv_lock_wake_up(std::vector<FileLocks::Owner> &owners, safs_locks::Type type) {
	for (auto owner : owners) {
		matoclserv_lock_wake_up(owner.sessionid, owner.msgid, type);
	}
}

void matoclserv_fuse_flock(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	FsContext context = FsContext::getForMaster(eventloop_time());
	uint32_t messageId;
	inode_t inode;
	uint64_t owner;

	uint32_t requestId;
	uint16_t op;
	MessageBuffer reply;
	PacketVersion version;
	uint8_t status;

	bool nonblocking = false;

	deserializePacketVersionNoHeader(data, length, version);

	if (version != 0) {
		safs::log_err("flock wrong message version\n");
		return;
	}
	cltoma::fuseFlock::deserialize(data, length, messageId, inode, owner, requestId, op);

	if (op & safs_locks::kNonblock) {
		nonblocking = true;
		op &= ~safs_locks::kNonblock;
	}

	std::vector<FileLocks::Owner> applied;
	status = fs_flock_op(context, inode, owner, eptr->sessionData->sessionId, requestId, messageId,
			op, nonblocking, applied);

	matoclserv_lock_wake_up(applied, safs_locks::Type::kFlock);

	// If it was a release request, do not respond
	if (op == safs_locks::kRelease) {
		return;
	}

	// Do not respond only if operation is blocking and status is WAITING
	if (nonblocking || status != SAUNAFS_ERROR_WAITING) {
		matocl::fuseFlock::serialize(reply, messageId, status);
		matoclserv_createpacket(eptr, std::move(reply));
	}
}

void matoclserv_fuse_getlk(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	FsContext context = FsContext::getForMaster(eventloop_time());
	uint32_t message_id;
	inode_t inode;
	uint64_t owner;

	safs_locks::FlockWrapper lock_info;
	uint8_t status;
	uint64_t lock_end;

	cltoma::fuseGetlk::deserialize(data, length, message_id, inode, owner, lock_info);

	if (lock_info.l_start < 0 || lock_info.l_len < 0) {
		matoclserv_createpacket(eptr, matocl::fuseGetlk::build(message_id, SAUNAFS_ERROR_EINVAL));
		return;
	}

	// Standard states that lock of length 0 is a lock till EOF
	if (lock_info.l_len == 0) {
		lock_end = std::numeric_limits<uint64_t>::max();
	} else {
		lock_end = (uint64_t)lock_info.l_start + (uint64_t)lock_info.l_len;
	}

	status = fs_posixlock_probe(context, inode, lock_info.l_start, lock_end, owner,
			eptr->sessionData->sessionId, 0, message_id, lock_info.l_type, lock_info);

	// Standard states that lock of length 0 is a lock till EOF
	if (lock_info.l_len == std::numeric_limits<int64_t>::max()) {
		lock_info.l_len = 0;
	}

	if (status == SAUNAFS_ERROR_WAITING || status == SAUNAFS_STATUS_OK) {
		matoclserv_createpacket(eptr, matocl::fuseGetlk::build(message_id, lock_info));
	} else {
		matoclserv_createpacket(eptr, matocl::fuseGetlk::build(message_id, status));
	}
}

void matoclserv_fuse_setlk(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	FsContext context = FsContext::getForMaster(eventloop_time());
	uint32_t message_id;
	inode_t inode;
	uint64_t owner;

	uint32_t request_id;
	uint16_t op;
	PacketVersion version;
	uint8_t status;
	safs_locks::FlockWrapper lock_info;
	uint64_t lock_end;

	bool nonblocking = false;
	deserializePacketVersionNoHeader(data, length, version);

	cltoma::fuseSetlk::deserialize(data, length, message_id, inode, owner, request_id, lock_info);

	if (lock_info.l_start < 0 || lock_info.l_len < 0) {
		matoclserv_createpacket(eptr, matocl::fuseSetlk::build(message_id, SAUNAFS_ERROR_EINVAL));
		return;
	}

	op = lock_info.l_type;

	if (op & safs_locks::kNonblock) {
		nonblocking = true;
		op &= ~safs_locks::kNonblock;
	}

	// Standard states that lock of length 0 is a lock till EOF
	if (lock_info.l_len == 0) {
		lock_end = std::numeric_limits<uint64_t>::max();
	} else {
		lock_end = (uint64_t)lock_info.l_start + (uint64_t)lock_info.l_len;
	}

	std::vector<FileLocks::Owner> applied;
	status = fs_posixlock_op(context, inode, lock_info.l_start, lock_end,
			owner, eptr->sessionData->sessionId, request_id, message_id, op, nonblocking, applied);

	matoclserv_lock_wake_up(applied, safs_locks::Type::kPosix);

	// If it was a release request, do not respond
	if (op == safs_locks::kRelease) {
		return;
	}

	// Do not respond only if operation is blocking and status is WAITING
	if (nonblocking || status != SAUNAFS_ERROR_WAITING) {
		matoclserv_createpacket(eptr, matocl::fuseSetlk::build(message_id, status));
	}
}

void matoclserv_list_defective_files(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	static const uint64_t kMaxNumberOfDefectiveEntries = 64 * 1024 * 1024;
	uint8_t flags;
	uint64_t entry_index, number_of_entries;
	cltoma::listDefectiveFiles::deserialize(data, length, flags, entry_index, number_of_entries);
	number_of_entries = std::min(number_of_entries, kMaxNumberOfDefectiveEntries);
	std::vector<DefectiveFileInfo> files_info =
	    fs_get_defective_nodes_info(flags, number_of_entries, entry_index);
	matoclserv_createpacket(eptr, matocl::listDefectiveFiles::build(entry_index, files_info));
}

void matoclserv_manage_locks_list(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	FsContext context = FsContext::getForMaster(eventloop_time());
	inode_t inode;
	safs_locks::Type type;
	bool pending;
	uint64_t start;
	uint64_t max;
	PacketVersion version;
	std::vector<safs_locks::Info> locks;
	int status;

	if (eptr->registered != ClientState::kAdmin) {
		safs::log_info("Listing file locks is available only for registered admins");
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	deserializePacketVersionNoHeader(data, length, version);

	if (version == cltoma::manageLocksList::kAll) {
		cltoma::manageLocksList::deserialize(data, length, type, pending, start, max);
		max = std::min(max, (uint64_t)SAU_CLTOMA_MANAGE_LOCKS_LIST_LIMIT);
		status = fs_locks_list_all(context, (uint8_t)type, pending, start, max, locks);
	} else if (version == cltoma::manageLocksList::kInode) {
		cltoma::manageLocksList::deserialize(data, length, inode, type, pending, start, max);
		max = std::min(max, (uint64_t)SAU_CLTOMA_MANAGE_LOCKS_LIST_LIMIT);
		status = fs_locks_list_inode(context, (uint8_t)type, pending, inode, start, max, locks);
	} else {
		throw IncorrectDeserializationException(
				"Unknown SAU_CLTOMA_MANAGE_LOCKS_LIST version: " + std::to_string(version));
	}

	if (status != SAUNAFS_STATUS_OK) {
		safs::log_warn(
		    "Master received invalid lock type {} from in SAU_CLTOMA_MANAGE_LOCKS_LIST packet",
		    (uint8_t)type);
	}

	MessageBuffer reply;
	matocl::manageLocksList::serialize(reply, locks);
	matoclserv_createpacket(eptr, std::move(reply));
}

void matoclserv_manage_locks_unlock(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	FsContext context = FsContext::getForMaster(eventloop_time());
	inode_t inode;
	uint64_t owner;
	uint32_t sessionid;
	safs_locks::Type type;
	uint64_t start;
	uint64_t end;
	PacketVersion version;
	uint8_t status = SAUNAFS_STATUS_OK;
	std::vector<FileLocks::Owner> flocks_applied, posix_applied;

	if (eptr->registered != ClientState::kAdmin) {
		safs::log_info("Removing file locks is available only for registered admins");
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	deserializePacketVersionNoHeader(data, length, version);

	if (version == cltoma::manageLocksUnlock::kSingle) {
		cltoma::manageLocksUnlock::deserialize(data, length, type, inode, sessionid, owner, start,
		                                       end);
		// Passing a 0 as lock's end is equivalent to passing 'till EOF'
		if (end == 0) {
			end = std::numeric_limits<decltype(end)>::max();
		}
		if (type == safs_locks::Type::kAll || type == safs_locks::Type::kFlock) {
			status = fs_flock_op(context, inode, owner, sessionid, 0, 0, safs_locks::kUnlock, true,
			                     flocks_applied);
		}
		if (status == SAUNAFS_STATUS_OK &&
		    (type == safs_locks::Type::kAll || type == safs_locks::Type::kPosix)) {
			status = fs_posixlock_op(context, inode, start, end, owner, sessionid, 0, 0,
			                         safs_locks::kUnlock, true, posix_applied);
		}
	} else if (version == cltoma::manageLocksUnlock::kInode) {
		cltoma::manageLocksUnlock::deserialize(data, length, type, inode);
		if (type == safs_locks::Type::kAll || type == safs_locks::Type::kFlock) {
			status = fs_locks_unlock_inode(context, (uint8_t)safs_locks::Type::kFlock, inode,
			                               flocks_applied);
		}
		if (status == SAUNAFS_STATUS_OK &&
		    (type == safs_locks::Type::kAll || type == safs_locks::Type::kPosix)) {
			status = fs_locks_unlock_inode(context, (uint8_t)safs_locks::Type::kPosix, inode,
			                               posix_applied);
		}
	} else {
		throw IncorrectDeserializationException("Unknown SAU_CLTOMA_MANAGE_LOCKS_UNLOCK version: " +
		                                        std::to_string(version));
	}

	for (auto sessionAndMsg : flocks_applied) {
		matoclserv_lock_wake_up(sessionAndMsg.sessionid, sessionAndMsg.msgid,
		                        safs_locks::Type::kFlock);
	}
	for (auto sessionAndMsg : posix_applied) {
		matoclserv_lock_wake_up(sessionAndMsg.sessionid, sessionAndMsg.msgid,
		                        safs_locks::Type::kPosix);
	}

	MessageBuffer reply;
	matocl::manageLocksUnlock::serialize(reply, status);
	matoclserv_createpacket(eptr, std::move(reply));
}

void matoclserv_list_tasks(matoclserventry *eptr) {
	std::vector<JobInfo> jobs_info = fs_get_current_tasks_info();
	matoclserv_createpacket(eptr, matocl::listTasks::build(jobs_info));
}

void matoclserv_stop_task(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t job_id, msgid;
	uint8_t status;
	cltoma::stopTask::deserialize(data, length, msgid, job_id);
	status = fs_cancel_job(job_id);
	matoclserv_createpacket(eptr, matocl::stopTask::build(msgid, status));
}

void matoclserv_fuse_locks_interrupt(matoclserventry *eptr, const uint8_t *data, uint32_t length,
				     uint8_t type) {
	FsContext context = FsContext::getForMaster(eventloop_time());
	uint32_t messageId;
	safs_locks::InterruptData interruptData;

	PacketVersion version;
	deserializePacketVersionNoHeader(data, length, version);

	if (version != 0) {
		safs::log_err("fuse_flock_interrupt wrong message version\n");
		return;
	}

	cltoma::fuseFlock::deserialize(data, length, messageId, interruptData);

	// we do not reply, so there is not need for checking status of this fs_operation
	fs_locks_remove_pending(context, type, interruptData.owner,
			   eptr->sessionData->sessionId, interruptData.ino, interruptData.reqid);
}

void matoclserv_update_credentials(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t messageId, index;
	FsContext::GroupsContainer gids;

	cltoma::updateCredentials::deserialize(data, length, messageId, index, gids);

	assert(eptr->sessionData);

	auto it = eptr->sessionData->groupsCache.find(index);
	if (it != eptr->sessionData->groupsCache.end()) {
		it->second.clear();
		it->second.insert(it->second.end(), gids.begin(), gids.end());
	} else {
		FsContext::GroupsContainer tmp(gids.begin(), gids.end());
		eptr->sessionData->groupsCache.insert(std::move(index), std::move(tmp));
	}

	matoclserv_createpacket(eptr, matocl::updateCredentials::build(messageId, SAUNAFS_STATUS_OK));
}

void matoclserv_fuse_setacl(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t messageId, uid, gid;
	inode_t inode;
	AclType type = AclType::kRichACL;
	RichACL rich_acl;
	AccessControlList posix_acl;
	bool use_posix = false;

	PacketVersion version;
	deserializePacketVersionNoHeader(data, length, version);

	if (version == cltoma::fuseSetAcl::kLegacyACL) {
		legacy::AccessControlList legacy_acl;
		cltoma::fuseSetAcl::deserialize(data, length, messageId, inode, uid, gid, type, legacy_acl);
		use_posix = true;
		posix_acl = (AccessControlList)legacy_acl;
	} else if (version == cltoma::fuseSetAcl::kPosixACL) {
		use_posix = true;
		cltoma::fuseSetAcl::deserialize(data, length, messageId, inode, uid, gid, type, posix_acl);
	} else if (version == cltoma::fuseSetAcl::kRichACL) {
		cltoma::fuseSetAcl::deserialize(data, length, messageId, inode, uid, gid, rich_acl);
	} else {
		safs::log_warn("SAU_CLTOMA_FUSE_SET_ACL: unknown packet version");
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	uint8_t status = matoclserv_check_group_cache(eptr, gid);
	if (status == SAUNAFS_STATUS_OK) {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		if (use_posix) {
			status = fs_setacl(context, inode, type, posix_acl);
		} else {
			status = fs_setacl(context, inode, rich_acl);
		}
	}
	matoclserv_createpacket(eptr, matocl::fuseSetAcl::build(messageId, status));
}

void matoclserv_fuse_setquota(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t messageId, uid, gid;
	std::vector<QuotaEntry> entries;
	cltoma::fuseSetQuota::deserialize(data, length, messageId, uid, gid, entries);

	uint8_t status = matoclserv_check_group_cache(eptr, gid);
	if (status == SAUNAFS_STATUS_OK) {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		status = fs_quota_set(context, entries);
	}

	MessageBuffer reply;
	matocl::fuseSetQuota::serialize(reply, messageId, status);
	matoclserv_createpacket(eptr, std::move(reply));
}

void matoclserv_fuse_getquota(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t version, messageId, uid, gid;
	std::vector<QuotaEntry> results;
	std::vector<std::string> info;
	uint8_t status;

	deserializePacketVersionNoHeader(data, length, version);

	if (version == cltoma::fuseGetQuota::kAllLimits) {
		cltoma::fuseGetQuota::deserialize(data, length, messageId, uid, gid);
		status = matoclserv_check_group_cache(eptr, gid);
		if (status == SAUNAFS_STATUS_OK) {
			FsContext context = matoclserv_get_context(eptr, uid, gid);
			status = fs_quota_get_all(context, results);
		}
	} else if (version == cltoma::fuseGetQuota::kSelectedLimits) {
		std::vector<QuotaOwner> owners;
		cltoma::fuseGetQuota::deserialize(data, length, messageId, uid, gid, owners);
		status = matoclserv_check_group_cache(eptr, gid);
		if (status == SAUNAFS_STATUS_OK) {
			FsContext context = matoclserv_get_context(eptr, uid, gid);
			status = fs_quota_get(context, owners, results);
		}
	} else {
		throw IncorrectDeserializationException(
				"Unknown SAU_CLTOMA_FUSE_GET_QUOTA version: " + std::to_string(version));
	}

	MessageBuffer reply;
	if (status == SAUNAFS_STATUS_OK) {
		status = fs_quota_get_info(matoclserv_get_context(eptr), results, info);
	}

	if (status == SAUNAFS_STATUS_OK) {
		matocl::fuseGetQuota::serialize(reply, messageId, results, info);
	} else {
		matocl::fuseGetQuota::serialize(reply, messageId, status);
	}

	matoclserv_createpacket(eptr, std::move(reply));
}

void matoclserv_iolimit(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t msgid;
	uint32_t configVersion;
	std::string groupId;
	uint64_t requestedBytes;

	cltoma::iolimit::deserialize(data, length, msgid, configVersion, groupId, requestedBytes);
	uint64_t grantedBytes;

	if (configVersion != gIoLimitsConfigId) {
		grantedBytes = 0;
	} else {
		try {
			grantedBytes = gIoLimitsDatabase.request(SteadyClock::now(), groupId, requestedBytes);
		} catch (IoLimitsDatabase::InvalidGroupIdException&) {
			safs::log_info("SAU_CLTOMA_IOLIMIT: Invalid group: {}", groupId);
			grantedBytes = 0;
		}
	}

	MessageBuffer reply;
	matocl::iolimit::serialize(reply, msgid, configVersion, groupId, grantedBytes);
	matoclserv_createpacket(eptr, std::move(reply));
}

void matoclserv_hostname(matoclserventry* eptr, const uint8_t* data, uint32_t length) {
	cltoma::hostname::deserialize(data, length);
	char hostname[257];
	memset(hostname, 0, 257);
	// Use 1 byte less then the array has in order to ensure that the name is null terminated:
	gethostname(hostname, 256);
	matoclserv_createpacket(eptr, matocl::hostname::build(std::string(hostname)));
}

void matoclserv_admin_register(matoclserventry* eptr, const uint8_t* data, uint32_t length) {
	cltoma::adminRegister::deserialize(data, length);
	if (!eptr->adminChallenge) {
		eptr->adminChallenge.reset(new SauMatoclAdminRegisterChallengeData());
		auto& array = *eptr->adminChallenge;
		for (uint32_t i = 0; i < array.size(); ++i) {
			array[i] = rnd<uint8_t>();
		}
		matoclserv_createpacket(eptr, matocl::adminRegisterChallenge::build(array));
	} else {
		safs::log_info("SAU_CLTOMA_ADMIN_REGISTER_CHALLENGE: retry not allowed");
		eptr->mode = ClientConnectionMode::KILL;
	}
}

void matoclserv_admin_register_response(matoclserventry* eptr, const uint8_t* data,
		uint32_t length) {
	SauCltomaAdminRegisterResponseData receivedDigest;
	cltoma::adminRegisterResponse::deserialize(data, length, receivedDigest);

	if (eptr->adminChallenge) {
		std::string password = cfg_getstring("ADMIN_PASSWORD", "");
		if (password == "") {
			matoclserv_createpacket(eptr, matocl::adminRegisterResponse::build(SAUNAFS_ERROR_EPERM));
			safs_pretty_syslog(LOG_WARNING, "admin access disabled");
			return;
		}

		auto digest = md5_challenge_response(*eptr->adminChallenge, password);
		if (receivedDigest == digest) {
			matoclserv_createpacket(eptr, matocl::adminRegisterResponse::build(SAUNAFS_STATUS_OK));
			eptr->registered = ClientState::kAdmin;
		} else {
			matoclserv_createpacket(eptr, matocl::adminRegisterResponse::build(SAUNAFS_ERROR_BADPASSWORD));
			safs_pretty_syslog(LOG_WARNING, "admin authentication error");
		}

		eptr->adminChallenge.reset();
	} else {
		safs::log_info("SAU_CLTOMA_ADMIN_REGISTER_RESPONSE: response without previous challenge");
		eptr->mode = ClientConnectionMode::KILL;
	}
}

void matoclserv_admin_become_master(matoclserventry* eptr, const uint8_t* data, uint32_t length) {
	cltoma::adminBecomeMaster::deserialize(data, length);

	if (eptr->registered == ClientState::kAdmin) {
		bool succ = metadataserver::promoteAutoToMaster();
		uint8_t status = succ ? SAUNAFS_STATUS_OK : SAUNAFS_ERROR_NOTPOSSIBLE;
		matoclserv_createpacket(eptr, matocl::adminBecomeMaster::build(status));
	} else {
		safs::log_info("SAU_CLTOMA_ADMIN_BECOME_MASTER: available only for registered admins");
		eptr->mode = ClientConnectionMode::KILL;
	}
}

void matoclserv_admin_stop_without_metadata_dump(matoclserventry *eptr, const uint8_t *data,
                                                 uint32_t length) {
	cltoma::adminStopWithoutMetadataDump::deserialize(data, length);

	if (eptr->registered == ClientState::kAdmin) {
		if (metadataserver::isMaster()) {
			if (matomlserv_shadows_count() == 0) {
				safs::log_warn(
				    "SAU_CLTOMA_ADMIN_STOP_WITHOUT_METADATA_DUMP: Trying to stop"
				    " master server with disabled metadata dump when no shadow servers are "
				    "connected.");
				matoclserv_createpacket(eptr, matocl::adminStopWithoutMetadataDump::build(EPERM));
			} else {
				fs_disable_metadata_dump_on_exit();
				uint8_t status = eventloop_want_to_terminate();
				if (status == SAUNAFS_STATUS_OK) {
					eptr->adminTask = AdminTask::kTerminate;
				} else {
					matoclserv_createpacket(
							eptr, matocl::adminStopWithoutMetadataDump::build(status));
				}
			}
		} else { // not Master
			fs_disable_metadata_dump_on_exit();
			uint8_t status = eventloop_want_to_terminate();
			matoclserv_createpacket(eptr, matocl::adminStopWithoutMetadataDump::build(status));
		}
	} else {
		safs::log_info(
		    "SAU_CLTOMA_ADMIN_STOP_WITHOUT_METADATA_DUMP:"
		    " available only for registered admins");
		eptr->mode = ClientConnectionMode::KILL;
	}
}

void matoclserv_admin_reload(matoclserventry* eptr, const uint8_t* data, uint32_t length) {
	cltoma::adminReload::deserialize(data, length);

	if (eptr->registered == ClientState::kAdmin) {
		eptr->adminTask = AdminTask::kReload; // mark, that this admin waits for response
		eventloop_want_to_reload();
		safs::log_info("reload of the config file requested using saunafs-admin by {}",
		               ipToString(eptr->peerIpAddress));
	} else {
		safs::log_info("SAU_CLTOMA_ADMIN_RELOAD: available only for registered admins");
		eptr->mode = ClientConnectionMode::KILL;
	}
}

std::string get_client_configs() {
	std::map<std::string, std::string> client_configs;

	for (const auto& sessionPtr : sessionVector) {
		if (sessionPtr->config.empty()) { continue; }
		NetworkAddress addr(sessionPtr->peerIpAddress, sessionPtr->peerPort);
		client_configs[addr.toString()] = sessionPtr->config;
	}

	return cfg_yaml_list("clients", client_configs);
}

void matoclserv_admin_dump_config(matoclserventry *eptr) {
	if (eptr->registered != ClientState::kAdmin) {
		safs::log_info("SAU_CLTOMA_ADMING_DUMP_CONFIG: available only for registered admins");
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	MessageBuffer reply;

	auto master_config = cfg_yaml_string((metadataserver::isMaster() ? "master" : "shadow"));
	auto chunkserver_configs = csdb_chunkserver_configs();
	auto metalogger_configs = get_metaloggers_config();
	auto client_configs = get_client_configs();

	matocl::adminDumpConfiguration::serialize(
	    reply, master_config + "\n" + chunkserver_configs + "\n" +
	               metalogger_configs + "\n" + client_configs);

	matoclserv_createpacket(eptr, reply);
}

void matoclserv_admin_save_metadata(matoclserventry* eptr, const uint8_t* data, uint32_t length) {
	bool asynchronous;
	cltoma::adminSaveMetadata::deserialize(data, length, asynchronous);

	if (eptr->registered == ClientState::kAdmin) {
		safs::log_info("saving metadata image requested using saunafs-admin by {}",
		               ipToString(eptr->peerIpAddress));
		uint8_t status = gMetadataBackend->fs_storeall(DumpType::kBackgroundDump);

		if (status != SAUNAFS_STATUS_OK || asynchronous) {
			matoclserv_createpacket(eptr, matocl::adminSaveMetadata::build(status));
		} else {
			// Mark the client; we will reply after metadata save process is finished
			eptr->adminTask = AdminTask::kSaveMetadata;
		}
	} else {
		safs::log_info("SAU_CLTOMA_ADMIN_SAVE_METADATA: available only for registered admins");
		eptr->mode = ClientConnectionMode::KILL;
	}
}

void matoclserv_broadcast_metadata_saved(uint8_t status) {
	if (exiting) {
		return;
	}

	for (const auto &eptr : matoclservList) {
		if (eptr->adminTask == AdminTask::kSaveMetadata) {
			matoclserv_createpacket(eptr.get(), matocl::adminSaveMetadata::build(status));
			eptr->adminTask = AdminTask::kNone;
		}
	}
}

void matoclserv_admin_recalculate_metadata_checksum(matoclserventry *eptr, const uint8_t *data,
                                                    uint32_t length) {
	bool asynchronous;
	cltoma::adminRecalculateMetadataChecksum::deserialize(data, length, asynchronous);

	if (eptr->registered == ClientState::kAdmin) {
		safs::log_info("metadata checksum recalculation requested using saunafs-admin by {}",
		               ipToString(eptr->peerIpAddress));
		uint8_t status = fs_start_checksum_recalculation();

		if (status != SAUNAFS_STATUS_OK || asynchronous) {
			matoclserv_createpacket(eptr, matocl::adminRecalculateMetadataChecksum::build(status));
		} else {
			// Mark the client; we will reply after checksum of metadata is recalculated
			eptr->adminTask = AdminTask::kRecalculateChecksums;
		}
	} else {
		safs::log_info(
		    "SAU_CLTOMA_ADMIN_RECALCULATE_METADATA_CHECKSUM: available only for registered admins");
		eptr->mode = ClientConnectionMode::KILL;
	}
}

void matoclserv_broadcast_metadata_checksum_recalculated(uint8_t status) {
	if (exiting) {
		return;
	}

	for (const auto &eptr : matoclservList) {
		if (eptr->adminTask == AdminTask::kRecalculateChecksums) {
			matoclserv_createpacket(eptr.get(),
			                        matocl::adminRecalculateMetadataChecksum::build(status));
			eptr->adminTask = AdminTask::kNone;
		}
	}
}

void matocl_locks_release(const FsContext &context, inode_t inode, uint32_t sessionId) {
	std::vector<FileLocks::Owner> applied;

	fs_locks_clear_session(context, (uint8_t)safs_locks::Type::kFlock, inode, sessionId, applied);

	for (auto candidate : applied) {
		matoclserv_lock_wake_up(candidate.sessionid, candidate.msgid, safs_locks::Type::kFlock);
	}

	applied.clear();
	fs_locks_clear_session(context, (uint8_t)safs_locks::Type::kPosix, inode, sessionId, applied);

	for (auto candidate : applied) {
		matoclserv_lock_wake_up(candidate.sessionid, candidate.msgid, safs_locks::Type::kPosix);
	}
}

void matocl_close_files(Session *currentSession) {
	FsContext context = FsContext::getForMaster(eventloop_time());

	for (const auto &openFileInode : currentSession->openFilesSet) {
		fs_release(context, openFileInode, currentSession->sessionId);
		matocl_locks_release(context, openFileInode, currentSession->sessionId);
	}

	currentSession->openFilesSet.clear();
}

uint32_t session_number_of_files(Session *currentSession) {
	return currentSession->openFilesSet.size();
}

void matoclserv_session_files(matoclserventry *eptr,
                              [[maybe_unused]] const uint8_t *data,
                              [[maybe_unused]] uint32_t length) {
	std::vector<SessionFiles> sessions;
	MessageBuffer reply;

	for (const auto &eaptr : matoclservList) {
		if (eaptr->mode == ClientConnectionMode::KILL || !eaptr->sessionData ||
		    eaptr->registered != ClientState::kRegistered) {
			continue;
		}

		SessionFiles session;
		session.sessionId = eaptr->sessionData->sessionId;
		session.peerIp = eaptr->sessionData->peerIpAddress;
		session.filesNumber = session_number_of_files(eaptr->sessionData);
		sessions.push_back(session);
	}

	matocl::listSessions::serialize(reply, sessions);
	matoclserv_createpacket(eptr, std::move(reply));
}

void matocl_session_timedout(Session *currentSession) {
	matocl_close_files(currentSession);
}

void matoclserv_session_delete(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint8_t status = SAUNAFS_STATUS_OK;
	uint64_t sessionId = 0;

	cltoma::deleteSession::deserialize(data, length, sessionId);

	// Find the session with the given sessionId
	matoclserventry *target = nullptr;

	for (const auto &eaptr : matoclservList) {
		if (eaptr->sessionData && eaptr->sessionData->sessionId == sessionId) {
			target = eaptr.get();
			break;
		}
	}

	if (!target) {
		safs::log_err("SAU_CLTOMA_DELETE_SESSION - session not found (session id: {})", sessionId);
		status = SAUNAFS_ERROR_BADSESSIONID;
	} else {
		matoclserv_close_session(sessionId);
		target->mode = ClientConnectionMode::KILL;
		// Force closure of files immediately.
		// Otherwise, it will take a few moments before the session
		// times out and the files are removed automatically, unless the
		// peer reconnects during that time
		matocl_close_files(target->sessionData);
	}

	MessageBuffer reply;
	matocl::deleteSession::serialize(reply, status);
	matoclserv_createpacket(eptr, reply);
}

void matocl_session_check() {
	uint32_t now = eventloop_time();

	for (auto sessionIt = sessionVector.begin(); sessionIt != sessionVector.end();) {
		auto& sessionPtr = *sessionIt;
		if (sessionPtr->connections == 0 &&
		    ((sessionPtr->newSession > 1 && sessionPtr->disconnectedTimestamp < now) ||
		     (sessionPtr->newSession == 1 &&
		      sessionPtr->disconnectedTimestamp + SessionSustainTime < now) ||
		     (sessionPtr->newSession == 0 && sessionPtr->disconnectedTimestamp + 7200 < now))) {
			matocl_session_timedout(sessionPtr.get());
			sessionIt = sessionVector.erase(sessionIt);
		} else {
			++sessionIt;
		}
	}
}

void matocl_session_stats_rotate() {
	for (auto& sessionPtr : sessionVector) {
		sessionPtr->prevHourOperationsStats = sessionPtr->currHourOperationsStats;
		sessionPtr->currHourOperationsStats.fill(0);
	}
	matoclserv_store_sessions();
}

void matocl_before_disconnect(matoclserventry *eptr) {
	// unlock locked chunks
	for (const auto &operation : eptr->delayedChunkOperations) {
		if (operation->type == FUSE_TRUNCATE) {
			fs_end_setlength(operation->chunkId);
		}
	}

	eptr->delayedChunkOperations.clear();

	if (eptr->sessionData) {
		if (eptr->sessionData->connections > 0) {
			eptr->sessionData->connections--;
		}

		if (eptr->sessionData->connections == 0) {
			eptr->sessionData->disconnectedTimestamp = eventloop_time();
		}
	}
}

void matoclserv_gotpacket(matoclserventry *eptr, uint32_t type, const uint8_t *data,
                          uint32_t length) {
	if (type == ANTOAN_NOP) {
		return;
	}

	if (type == ANTOAN_UNKNOWN_COMMAND) { // for future use
		return;
	}

	if (type == ANTOAN_BAD_COMMAND_SIZE) { // for future use
		return;
	}

	if (type == ANTOAN_PING) {
		matoclserv_ping(eptr, data, length);
		return;
	}

	try {
		if (!metadataserver::isMaster()) {     // shadow
			switch (type) {
				case SAU_CLTOMA_METADATASERVER_STATUS:
					matoclserv_metadataserver_status(eptr, data, length);
					break;
				case SAU_CLTOMA_HOSTNAME:
					matoclserv_hostname(eptr, data, length);
					break;
				case SAU_CLTOMA_ADMIN_REGISTER_CHALLENGE:
					matoclserv_admin_register(eptr, data, length);
					break;
				case SAU_CLTOMA_ADMIN_REGISTER_RESPONSE:
					matoclserv_admin_register_response(eptr, data, length);
					break;
				case SAU_CLTOMA_ADMIN_BECOME_MASTER:
					matoclserv_admin_become_master(eptr, data, length);
					break;
				case SAU_CLTOMA_ADMIN_STOP_WITHOUT_METADATA_DUMP:
					matoclserv_admin_stop_without_metadata_dump(eptr, data, length);
					break;
				case SAU_CLTOMA_ADMIN_RELOAD:
					matoclserv_admin_reload(eptr, data, length);
					break;
				case SAU_CLTOMA_ADMIN_SAVE_METADATA:
					matoclserv_admin_save_metadata(eptr, data, length);
					break;
				case SAU_CLTOMA_ADMIN_DUMP_CONFIG:
					matoclserv_admin_dump_config(eptr);
					break;
				default:
				    safs::log_info(
				        "main master server module: got invalid message in shadow state (type:{})",
				        type);
				    eptr->mode = ClientConnectionMode::KILL;
			}
		} else if (eptr->registered == ClientState::kUnregistered
				|| eptr->registered == ClientState::kAdmin) { // beware that in this context sesdata is NULL
			switch (type) {
				case CLTOMA_FUSE_REGISTER:
				    matoclserv_fuse_register(eptr, data, length);
				    break;
				case CLTOMA_CSERV_LIST:
				    matoclserv_cserv_list(eptr, data, length);
				    break;
				case SAU_CLTOMA_CSERV_LIST:
					matoclserv_sau_cserv_list(eptr, data, length);
					break;
				case CLTOMA_SESSION_LIST:
				    matoclserv_session_list(eptr, data, length);
				    break;
				case SAU_CLTOMA_MOUNT_INFO_LIST:
				    matoclserv_mount_info_list(eptr, data, length);
				    break;
				case SAU_CLTOMA_SESSION_FILES:
					matoclserv_session_files(eptr, data, length);
					break;
				case SAU_CLTOMA_DELETE_SESSION:
				    matoclserv_session_delete(eptr, data, length);
				    break;
				case CLTOAN_CHART:
				    matoclserv_chart(eptr, data, length);
				    break;
				case CLTOAN_CHART_DATA:
				    matoclserv_chart_data(eptr, data, length);
				    break;
				case CLTOMA_INFO:
				    matoclserv_info(eptr, data, length);
				    break;
				case CLTOMA_FSTEST_INFO:
				    matoclserv_fstest_info(eptr, data, length);
				    break;
				case CLTOMA_CHUNKSTEST_INFO:
				    matoclserv_chunkstest_info(eptr, data, length);
				    break;
				case CLTOMA_CHUNKS_MATRIX:
				    matoclserv_chunks_matrix(eptr, data, length);
				    break;
				case CLTOMA_EXPORTS_INFO:
				    matoclserv_exports_info(eptr, data, length);
				    break;
				case CLTOMA_MLOG_LIST:
				    matoclserv_mlog_list(eptr, data, length);
				    break;
				case CLTOMA_CSSERV_REMOVESERV:
				    matoclserv_cserv_removeserv(eptr, data, length);
				    break;
				case SAU_CLTOMA_IOLIMITS_STATUS:
					matoclserv_iolimits_status(eptr, data, length);
					break;
				case SAU_CLTOMA_METADATASERVERS_LIST:
					matoclserv_metadataservers_list(eptr, data, length);
					break;
				case SAU_CLTOMA_METADATASERVER_STATUS:
					matoclserv_metadataserver_status(eptr, data, length);
					break;
				case SAU_CLTOMA_LIST_GOALS:
					matoclserv_list_goals(eptr);
					break;
				case SAU_CLTOMA_CHUNKS_HEALTH:
					matoclserv_chunks_health(eptr, data, length);
					break;
				case SAU_CLTOMA_HOSTNAME:
					matoclserv_hostname(eptr, data, length);
					break;
				case SAU_CLTOMA_ADMIN_REGISTER_CHALLENGE:
					matoclserv_admin_register(eptr, data, length);
					break;
				case SAU_CLTOMA_ADMIN_REGISTER_RESPONSE:
					matoclserv_admin_register_response(eptr, data, length);
					break;
				case SAU_CLTOMA_ADMIN_STOP_WITHOUT_METADATA_DUMP:
					matoclserv_admin_stop_without_metadata_dump(eptr, data, length);
					break;
				case SAU_CLTOMA_ADMIN_RELOAD:
					matoclserv_admin_reload(eptr, data, length);
					break;
				case SAU_CLTOMA_ADMIN_DUMP_CONFIG:
					matoclserv_admin_dump_config(eptr);
					break;
				case SAU_CLTOMA_ADMIN_SAVE_METADATA:
					matoclserv_admin_save_metadata(eptr, data, length);
					break;
				case SAU_CLTOMA_ADMIN_RECALCULATE_METADATA_CHECKSUM:
					matoclserv_admin_recalculate_metadata_checksum(eptr, data, length);
					break;
				case SAU_CLTOMA_LIST_DEFECTIVE_FILES:
					matoclserv_list_defective_files(eptr, data, length);
					break;
				case SAU_CLTOMA_MANAGE_LOCKS_LIST:
				    matoclserv_manage_locks_list(eptr, data, length);
				    break;
				case SAU_CLTOMA_MANAGE_LOCKS_UNLOCK:
				    matoclserv_manage_locks_unlock(eptr, data, length);
				    break;
				case SAU_CLTOMA_LIST_TASKS:
					matoclserv_list_tasks(eptr);
					break;
				case SAU_CLTOMA_STOP_TASK:
					matoclserv_stop_task(eptr, data, length);
					break;
				default:
				    safs::log_info(
				        "main master server module: got unknown message from unregistered (type:{})",
				        type);
				    eptr->mode = ClientConnectionMode::KILL;
			}
		} else if (eptr->registered == ClientState::kRegistered) {      // mounts and new tools
			if (eptr->sessionData == nullptr) {
				safs::log_err("registered connection without sesdata !!!");
				eptr->mode = ClientConnectionMode::KILL;
				return;
			}
			switch (type) {
				case CLTOMA_FUSE_REGISTER:
				    matoclserv_fuse_register(eptr, data, length);
				    break;
				case SAU_CLTOMA_REGISTER_CONFIG:
				    matoclserv_register_config(eptr, data, length);
				    break;
				case SAU_CLTOMA_UPDATE_MOUNT_INFO:
				    matoclserv_update_mount_info(eptr, data, length);
				    break;
				case CLTOMA_FUSE_RESERVED_INODES:
				    matoclserv_fuse_reserved_inodes(eptr, data, length);
				    break;
				case CLTOMA_FUSE_STATFS:
				    matoclserv_fuse_statfs(eptr, data, length);
				    break;
				case CLTOMA_FUSE_ACCESS:
				    matoclserv_fuse_access(eptr, data, length);
				    break;
				case CLTOMA_FUSE_LOOKUP:
				    matoclserv_fuse_lookup(eptr, data, length);
				    break;
				case CLTOMA_FUSE_GETATTR:
				    matoclserv_fuse_getattr(eptr, data, length);
				    break;
				case CLTOMA_FUSE_SETATTR:
				    matoclserv_fuse_setattr(eptr, data, length);
				    break;
				case CLTOMA_FUSE_READLINK:
				    matoclserv_fuse_readlink(eptr, data, length);
				    break;
				case CLTOMA_FUSE_SYMLINK:
				    matoclserv_fuse_symlink(eptr, data, length);
				    break;
				case CLTOMA_FUSE_MKNOD:
				case SAU_CLTOMA_FUSE_MKNOD:
					matoclserv_fuse_mknod(eptr, PacketHeader(type, length), data);
					break;
				case CLTOMA_FUSE_MKDIR:
				case SAU_CLTOMA_FUSE_MKDIR:
					matoclserv_fuse_mkdir(eptr, PacketHeader(type, length), data);
					break;
				case CLTOMA_FUSE_UNLINK:
				    matoclserv_fuse_unlink(eptr, data, length);
				    break;
				case CLTOMA_FUSE_RMDIR:
				    matoclserv_fuse_rmdir(eptr, data, length);
				    break;
				case CLTOMA_FUSE_RENAME:
					matoclserv_fuse_rename(eptr,data,length);
					break;
				case CLTOMA_FUSE_LINK:
				    matoclserv_fuse_link(eptr, data, length);
				    break;
				case CLTOMA_FUSE_GETDIR:
				    matoclserv_fuse_getdir(eptr, data, length);
				    break;
				case SAU_CLTOMA_FUSE_GETDIR:
					matoclserv_fuse_getdir(eptr, PacketHeader(type, length), data);
					break;
				case CLTOMA_FUSE_OPEN:
				    matoclserv_fuse_open(eptr, data, length);
				    break;
				case SAU_CLTOMA_FUSE_READ_CHUNK:
				case CLTOMA_FUSE_READ_CHUNK:
					matoclserv_fuse_read_chunk(eptr, PacketHeader(type, length), data);
					break;
				case SAU_CLTOMA_CHUNKS_INFO:
					matoclserv_chunks_info(eptr, data, length);
					break;
				case SAU_CLTOMA_FUSE_WRITE_CHUNK:
				case CLTOMA_FUSE_WRITE_CHUNK:
					matoclserv_fuse_write_chunk(eptr, PacketHeader(type, length), data);
					break;
				case SAU_CLTOMA_FUSE_WRITE_CHUNK_END:
				case CLTOMA_FUSE_WRITE_CHUNK_END:
					matoclserv_fuse_write_chunk_end(eptr, PacketHeader(type, length), data);
					break;
					// fuse - meta
				case CLTOMA_FUSE_GETTRASH:
				    matoclserv_fuse_gettrash(eptr, data, length);
				    break;
				case SAU_CLTOMA_FUSE_GETTRASH:
					matoclserv_fuse_gettrash(eptr, PacketHeader(type, length), data);
					break;
				case CLTOMA_FUSE_GETDETACHEDATTR:
				    matoclserv_fuse_getdetachedattr(eptr, data, length);
				    break;
				case CLTOMA_FUSE_GETTRASHPATH:
				    matoclserv_fuse_gettrashpath(eptr, data, length);
				    break;
				case CLTOMA_FUSE_SETTRASHPATH:
				    matoclserv_fuse_settrashpath(eptr, data, length);
				    break;
				case CLTOMA_FUSE_UNDEL:
				    matoclserv_fuse_undel(eptr, data, length);
				    break;
				case CLTOMA_FUSE_PURGE:
				    matoclserv_fuse_purge(eptr, data, length);
				    break;
				case CLTOMA_FUSE_GETRESERVED:
				    matoclserv_fuse_getreserved(eptr, data, length);
				    break;
				case SAU_CLTOMA_FUSE_GETRESERVED:
					matoclserv_fuse_getreserved(eptr, PacketHeader(type, length), data);
					break;
				case CLTOMA_FUSE_CHECK:
				    matoclserv_fuse_check(eptr, data, length);
				    break;
				case CLTOMA_FUSE_GETTRASHTIME:
				    matoclserv_fuse_gettrashtime(eptr, data, length);
				    break;
				case CLTOMA_FUSE_SETTRASHTIME:
					matoclserv_fuse_settrashtime(eptr, PacketHeader(type, length), data);
					break;
				case CLTOMA_FUSE_GETGOAL:
				case SAU_CLTOMA_FUSE_GETGOAL:
					matoclserv_fuse_getgoal(eptr, PacketHeader(type, length), data);
					break;
				case CLTOMA_FUSE_SETGOAL:
				case SAU_CLTOMA_FUSE_SETGOAL:
					matoclserv_fuse_setgoal(eptr, PacketHeader(type, length), data);
					break;
				case CLTOMA_FUSE_APPEND:
				    matoclserv_fuse_append(eptr, data, length);
				    break;
				case CLTOMA_FUSE_GETDIRSTATS:
				    matoclserv_fuse_getdirstats_old(eptr, data, length);
				    break;
				case SAU_CLTOMA_FUSE_TRUNCATE_END:
				case SAU_CLTOMA_FUSE_TRUNCATE:
				case CLTOMA_FUSE_TRUNCATE:
					matoclserv_fuse_truncate(eptr, PacketHeader(type, length), data);
					break;
				case CLTOMA_FUSE_REPAIR:
				    matoclserv_fuse_repair(eptr, data, length);
				    break;
				case CLTOMA_FUSE_SNAPSHOT:
				case SAU_CLTOMA_FUSE_SNAPSHOT:
					matoclserv_fuse_snapshot(eptr, PacketHeader(type, length), data);
					break;
				case CLTOMA_FUSE_GETEATTR:
				    matoclserv_fuse_geteattr(eptr, data, length);
				    break;
				case CLTOMA_FUSE_SETEATTR:
				    matoclserv_fuse_seteattr(eptr, data, length);
				    break;
				case SAU_CLTOMA_FUSE_DELETE_ACL:
					matoclserv_fuse_deleteacl(eptr, data, length);
					break;
				case SAU_CLTOMA_FUSE_GET_ACL:
					matoclserv_fuse_getacl(eptr, data, length);
					break;
				case SAU_CLTOMA_FUSE_SET_ACL:
					matoclserv_fuse_setacl(eptr, data, length);
					break;
				case SAU_CLTOMA_FUSE_SET_QUOTA:
					matoclserv_fuse_setquota(eptr, data, length);
					break;
				case SAU_CLTOMA_FUSE_GET_QUOTA:
					matoclserv_fuse_getquota(eptr, data, length);
					break;
					/* do not use in version before 1.7.x */
				case CLTOMA_FUSE_GETXATTR:
				    matoclserv_fuse_getxattr(eptr, data, length);
				    break;
				case CLTOMA_FUSE_SETXATTR:
				    matoclserv_fuse_setxattr(eptr, data, length);
				    break;
					/* for tools - also should be available for registered clients */
				case CLTOMA_CSERV_LIST:
				    matoclserv_cserv_list(eptr, data, length);
				    break;
				case CLTOMA_SESSION_LIST:
				    matoclserv_session_list(eptr, data, length);
				    break;
				case CLTOAN_CHART:
				    matoclserv_chart(eptr, data, length);
				    break;
				case CLTOAN_CHART_DATA:
				    matoclserv_chart_data(eptr, data, length);
				    break;
				case CLTOMA_INFO:
				    matoclserv_info(eptr, data, length);
				    break;
				case CLTOMA_FSTEST_INFO:
				    matoclserv_fstest_info(eptr, data, length);
				    break;
				case CLTOMA_CHUNKSTEST_INFO:
				    matoclserv_chunkstest_info(eptr, data, length);
				    break;
				case CLTOMA_CHUNKS_MATRIX:
				    matoclserv_chunks_matrix(eptr, data, length);
				    break;
				case CLTOMA_EXPORTS_INFO:
				    matoclserv_exports_info(eptr, data, length);
				    break;
				case CLTOMA_MLOG_LIST:
				    matoclserv_mlog_list(eptr, data, length);
				    break;
				case CLTOMA_CSSERV_REMOVESERV:
				    matoclserv_cserv_removeserv(eptr, data, length);
				    break;
				case SAU_CLTOMA_IOLIMIT:
				    matoclserv_iolimit(eptr, data, length);
				    break;
				case SAU_CLTOMA_FUSE_SETLK:
				    matoclserv_fuse_setlk(eptr, data, length);
				    break;
				case SAU_CLTOMA_FUSE_GETLK:
				    matoclserv_fuse_getlk(eptr, data, length);
				    break;
				case SAU_CLTOMA_FUSE_FLOCK:
				    matoclserv_fuse_flock(eptr, data, length);
				    break;
				case SAU_CLTOMA_FUSE_FLOCK_INTERRUPT:
				    matoclserv_fuse_locks_interrupt(eptr, data, length,
				                                    (uint8_t)safs_locks::Type::kFlock);
				    break;
				case SAU_CLTOMA_FUSE_SETLK_INTERRUPT:
				    matoclserv_fuse_locks_interrupt(eptr, data, length,
				                                    (uint8_t)safs_locks::Type::kPosix);
				    break;
				case SAU_CLTOMA_RECURSIVE_REMOVE:
					matoclserv_fuse_recursive_remove(eptr, data, length);
					break;
				case SAU_CLTOMA_REQUEST_TASK_ID:
					matoclserv_fuse_request_task_id(eptr, data, length);
					break;
				case SAU_CLTOMA_STOP_TASK:
					matoclserv_stop_task(eptr, data, length);
					break;
				case SAU_CLTOMA_UPDATE_CREDENTIALS:
					matoclserv_update_credentials(eptr, data, length);
					break;
				case SAU_CLTOMA_WHOLE_PATH_LOOKUP:
					matoclserv_sau_whole_path_lookup(eptr, data, length);
					break;
				case SAU_CLTOMA_FULL_PATH_BY_INODE:
					matoclserv_sau_full_path_by_inode(eptr, data, length);
					break;
				case SAU_CLTOMA_FUSE_GET_SELF_QUOTA:
					matoclserv_sau_get_self_quota(eptr, data, length);
					break;
				case SAU_CLTOMA_CSERV_LIST:
					matoclserv_sau_cserv_list(eptr, data, length);
					break;
				default:
				    safs::log_info(
				        "main master server module: got unknown message from sfsmount (type:{})",
				        type);
				    eptr->mode=ClientConnectionMode::KILL;
			}
		} else if (eptr->registered == ClientState::kOldTools) {        // old sfstools
			if (eptr->sessionData == nullptr) {
				safs::log_err("registered connection (tools) without sesdata !!!");
				eptr->mode=ClientConnectionMode::KILL;
				return;
			}
			switch (type) {
				// extra (external tools)
				case CLTOMA_FUSE_REGISTER:
				    matoclserv_fuse_register(eptr, data, length);
				    break;
				case CLTOMA_FUSE_READ_CHUNK: // used in saunafs fileinfo
					matoclserv_fuse_read_chunk(eptr, PacketHeader(type, length), data);
					break;
				case CLTOMA_FUSE_CHECK:
					matoclserv_fuse_check(eptr, data, length);
					break;
				case CLTOMA_FUSE_GETTRASHTIME:
					matoclserv_fuse_gettrashtime(eptr, data, length);
					break;
				case CLTOMA_FUSE_SETTRASHTIME:
					matoclserv_fuse_settrashtime(eptr, PacketHeader(type, length), data);
					break;
				case CLTOMA_FUSE_GETGOAL:
					matoclserv_fuse_getgoal(eptr, PacketHeader(type, length), data);
					break;
				case CLTOMA_FUSE_SETGOAL:
					matoclserv_fuse_setgoal(eptr, PacketHeader(type, length), data);
					break;
				case CLTOMA_FUSE_APPEND:
				    matoclserv_fuse_append(eptr, data, length);
				    break;
				case CLTOMA_FUSE_GETDIRSTATS:
				    matoclserv_fuse_getdirstats(eptr, data, length);
				    break;
				case CLTOMA_FUSE_TRUNCATE:
					matoclserv_fuse_truncate(eptr, PacketHeader(type, length), data);
					break;
				case CLTOMA_FUSE_REPAIR:
				    matoclserv_fuse_repair(eptr, data, length);
				    break;
				case CLTOMA_FUSE_SNAPSHOT:
					matoclserv_fuse_snapshot(eptr, PacketHeader(type, length), data);
					break;
				case CLTOMA_FUSE_GETEATTR:
				    matoclserv_fuse_geteattr(eptr, data, length);
				    break;
				case CLTOMA_FUSE_SETEATTR:
				    matoclserv_fuse_seteattr(eptr, data, length);
				    break;
				default:
				    safs::log_info(
				        "main master server module: got unknown message from saunafs "
				        "<COMMAND> tools (type:{})", type);
				    eptr->mode = ClientConnectionMode::KILL;
			}
		}
	} catch (IncorrectDeserializationException& e) {
		safs::log_info(
				"main master server module: got inconsistent message from mount "
				"(type:{}, length:{}), {}", type, length, e.what());
		eptr->mode = ClientConnectionMode::KILL;
	}
}

void matoclserv_term() {
	packetstruct *pptr,*pptrn;

	safs::log_info("main master server module: closing {}:{}", ListenHost, ListenPort);
	tcpclose(masterSocket);

	for (const auto &eptr : matoclservList) {
		if (eptr->inputPacket.packet) {
			free(eptr->inputPacket.packet);
		}

		for (pptr = eptr->outputPacketHead ; pptr ; pptr = pptrn) {
			pptrn = pptr->next;
			if (pptr->packet) {
				free(pptr->packet);
			}
			free(pptr);
		}

		eptr->delayedChunkOperations.clear();
	}

	matoclservList.clear();
	matoclserv_session_unload();
}

void matoclserv_read(matoclserventry *eptr) {
	SignalLoopWatchdog watchdog;
	int32_t bytesRead;
	uint32_t type,size;
	const uint8_t *ptr;

	watchdog.start();
	while (eptr->mode != ClientConnectionMode::KILL) {
		bytesRead = read(eptr->socket, eptr->inputPacket.startPtr, eptr->inputPacket.bytesLeft);
		if (bytesRead == 0) {
			if (eptr->registered == ClientState::kRegistered) {       // show this message only for standard, registered clients
				safs::log_info("connection with client (ip:{}) has been closed by peer",
				               ipToString(eptr->peerIpAddress));
			}
			eptr->mode = ClientConnectionMode::KILL;
			return;
		}

		if (bytesRead < 0) {
			if (errno != EAGAIN) {
#ifdef ECONNRESET
				if (errno != ECONNRESET) {
#endif
					safs_silent_errlog(LOG_NOTICE, "main master server module: (ip:%s) read error",
					                   ipToString(eptr->peerIpAddress).c_str());
#ifdef ECONNRESET
				}
#endif
				eptr->mode = ClientConnectionMode::KILL;
			}
			return;
		}
		eptr->inputPacket.startPtr += bytesRead;
		eptr->inputPacket.bytesLeft -= bytesRead;
		metrics::Counter::increment(metrics::Counter::Master::CLIENT_RX_BYTES, bytesRead);
		statsBytesReceived += bytesRead;

		if (eptr->inputPacket.bytesLeft > 0) {
			return;
		}

		if (eptr->mode == ClientConnectionMode::HEADER) {
			ptr = eptr->headerBuffer + 4;
			get32bit(&ptr, size);
			if (size > 0) {
				if (size > MaxPacketSize) {
					safs::log_warn("main master server module: packet too long ({}/{})", size,
					               MaxPacketSize);
					eptr->mode = ClientConnectionMode::KILL;
					return;
				}
				eptr->inputPacket.packet = (uint8_t*) malloc(size);
				passert(eptr->inputPacket.packet);
				eptr->inputPacket.bytesLeft = size;
				eptr->inputPacket.startPtr = eptr->inputPacket.packet;
				eptr->mode = ClientConnectionMode::DATA;
				continue;
			}
			eptr->mode = ClientConnectionMode::DATA;
		}

		if (eptr->mode==ClientConnectionMode::DATA) {
			ptr = eptr->headerBuffer;
			get32bit(&ptr, type);
			get32bit(&ptr, size);

			eptr->mode = ClientConnectionMode::HEADER;
			eptr->inputPacket.bytesLeft = 8;
			eptr->inputPacket.startPtr = eptr->headerBuffer;
			matoclserv_gotpacket(eptr,type,eptr->inputPacket.packet,size);
			statsPacketsReceived++;
			metrics::Counter::increment(metrics::Counter::Master::CLIENT_RX_PACKETS);

			if (eptr->inputPacket.packet) {
				free(eptr->inputPacket.packet);
			}
			eptr->inputPacket.packet = nullptr;
			break;
		}

		if (watchdog.expired()) {
			break;
		}
	}
}

void matoclserv_write(matoclserventry *eptr) {
	SignalLoopWatchdog watchdog;
	packetstruct *pack;
	int32_t i;

	watchdog.start();
	for (;;) {
		pack = eptr->outputPacketHead;
		if (pack == nullptr) { return; }
		i = write(eptr->socket, pack->startPtr, pack->bytesLeft);
		if (i < 0) {
			if (errno != EAGAIN) {
				safs_silent_errlog(LOG_NOTICE, "main master server module: (ip:%s) write error",
				                   ipToString(eptr->peerIpAddress).c_str());
				eptr->mode = ClientConnectionMode::KILL;
			}
			return;
		}
		pack->startPtr += i;
		pack->bytesLeft -= i;
		metrics::Counter::increment(metrics::Counter::Master::CLIENT_TX_BYTES, i);
		statsBytesSent += i;
		if (pack->bytesLeft > 0) {
			return;
		}
		free(pack->packet);
		statsPacketsSent++;
		metrics::Counter::increment(metrics::Counter::Master::CLIENT_TX_PACKETS);
		eptr->outputPacketHead = pack->next;
		if (eptr->outputPacketHead == nullptr) {
			eptr->outputPacketTail = &(eptr->outputPacketHead);
		}
		free(pack);

		if (watchdog.expired()) {
			break;
		}
	}
}

void matoclserv_wantexit() {
	exiting = 1;
}

int matoclserv_canexit() {
	matoclserventry *adminTerminator = nullptr;
	static bool terminatorPacketSent = false;

	for (const auto &eptr : matoclservList) {
		if (eptr->outputPacketHead != nullptr) {
			return 0;
		}

		if (!eptr->delayedChunkOperations.empty()) {
			return 0;
		}

		if (eptr->adminTask == AdminTask::kTerminate) {
			adminTerminator = eptr.get();
		}
	}

	if (adminTerminator != nullptr && !terminatorPacketSent) {
		// Are we replying to termination request?
		if (!matomlserv_canexit()){  // make sure there are no ml connected
			safs_pretty_syslog(LOG_INFO, "Waiting for ml connections to close...");
			return 0;
		} else {  // Reply to admin
			matoclserv_createpacket(adminTerminator,
					matocl::adminStopWithoutMetadataDump::build(SAUNAFS_STATUS_OK));
			terminatorPacketSent = true;
		}
	}

	// Wait for the admin which requested termination (if exists) to disconnect.
	// This ensures that he received the response (or died and is no longer interested).
	for (const auto &eptr : matoclservList) {
		if (eptr->adminTask == AdminTask::kTerminate) {
			return 0;
		}
	}

	return 1;
}

void matoclserv_desc(std::vector<pollfd> &pdesc) {
	if (exiting == 0) {
		pdesc.push_back({masterSocket, POLLIN, 0});
		masterSocketDescPos = pdesc.size() - 1;
	} else {
		masterSocketDescPos = -1;
	}

	for (const auto &eptr : matoclservList) {
		pdesc.push_back({eptr->socket, 0, 0});
		eptr->pDescPos = pdesc.size() - 1;

		if (exiting == 0) {
			pdesc.back().events |= POLLIN;
		}

		if (eptr->outputPacketHead != nullptr) {
			pdesc.back().events |= POLLOUT;
		}
	}
}


void matoclserv_serve(const std::vector<pollfd> &pdesc) {
	uint32_t now = eventloop_time();
	packetstruct *pptr;
	packetstruct *paptr;

	if (masterSocketDescPos >= 0 && (pdesc[masterSocketDescPos].revents & POLLIN)) {
		int ns = tcpaccept(masterSocket);
		if (ns < 0) {
			safs_silent_errlog(LOG_NOTICE,"main master server module: accept error");
		} else {
			tcpnonblock(ns);
			tcpnodelay(ns);
			auto eptr = std::make_unique<matoclserventry>();
			eptr->socket = ns;
			eptr->pDescPos = -1;
			tcpgetpeer(ns, &(eptr->peerIpAddress), &(eptr->peerPort));
			eptr->registered = ClientState::kUnregistered;
			eptr->ioLimitsEnabled = false;
			eptr->version = 0;
			eptr->mode = ClientConnectionMode::HEADER;
			eptr->lastReadTimestamp = now;
			eptr->lastWriteTimestamp = now;
			eptr->inputPacket.next = nullptr;
			eptr->inputPacket.bytesLeft = 8;
			eptr->inputPacket.startPtr = eptr->headerBuffer;
			eptr->inputPacket.packet = nullptr;
			eptr->adminTask = AdminTask::kNone;
			eptr->outputPacketHead = nullptr;
			eptr->outputPacketTail = &(eptr->outputPacketHead);

			eptr->delayedChunkOperations.clear();
			eptr->sessionData = nullptr;
			memset(eptr->randomPassword, 0, 32);

			matoclservList.push_front(std::move(eptr));
		}
	}

// read
	for (const auto &eptr : matoclservList) {
		if (eptr->pDescPos >= 0) {
			if (pdesc[eptr->pDescPos].revents & (POLLERR | POLLHUP)) {
				eptr->mode = ClientConnectionMode::KILL;
			}

			if ((pdesc[eptr->pDescPos].revents & POLLIN) &&
			    eptr->mode != ClientConnectionMode::KILL) {
				eptr->lastReadTimestamp = now;
				matoclserv_read(eptr.get());
			}
		}
	}

// write
	for (const auto &eptr : matoclservList) {
		if (eptr->lastWriteTimestamp + 2 < now && eptr->registered != ClientState::kOldTools &&
		    eptr->outputPacketHead == nullptr) {
			// 4 byte length because of 'msgid'
			uint8_t *ptr = matoclserv_createpacket(eptr.get(), ANTOAN_NOP, 4);
			*((uint32_t *)ptr) = 0;
		}

		if (eptr->pDescPos >= 0) {
			if ((((pdesc[eptr->pDescPos].events & POLLOUT) == 0 && (eptr->outputPacketHead)) ||
			     (pdesc[eptr->pDescPos].revents & POLLOUT)) &&
			    eptr->mode != ClientConnectionMode::KILL) {
				eptr->lastWriteTimestamp = now;
				matoclserv_write(eptr.get());
			}
		}

		// Disconnect clients that have not requested any data for a while
		if (eptr->lastReadTimestamp + kClientInactivityTimeout < now && exiting == 0) {
			eptr->mode = ClientConnectionMode::KILL;
		}
	}

// close
	for (auto eptrIt = matoclservList.begin(); eptrIt != matoclservList.end();) {
		auto *eptr = eptrIt->get();
		if (eptr->mode == ClientConnectionMode::KILL) {
			matocl_before_disconnect(eptr);
			tcpclose(eptr->socket);

			if (eptr->inputPacket.packet) {
				free(eptr->inputPacket.packet);
			}

			pptr = eptr->outputPacketHead;
			while (pptr) {
				if (pptr->packet) {
					free(pptr->packet);
				}
				paptr = pptr;
				pptr = pptr->next;
				free(paptr);
			}

			eptrIt = matoclservList.erase(eptrIt);
		} else {
			++eptrIt;
		}
	}
}

void matoclserv_start_cond_check() {
	if (starting) {
		// very simple condition checking if all chunkservers have been connected
		// in the future master will know his chunkservers list and then this condition will be
		// changed
		if (chunk_get_missing_count() < 100) {
			starting = 0;
		} else {
			starting--;
		}
	}
}

int matoclserv_sessions_init() {
	sessionVector.clear();

	switch (matoclserv_load_sessions()) {
		case 0: // no file
		    safs::log_warn(
		        "sessions file {}/{} not found; if it is not a fresh installation "
		        "you have to restart all active mounts",
		        fs::getCurrentWorkingDirectoryNoThrow().c_str(), kSessionsFilename);
		    matoclserv_store_sessions();
			break;
		case 1: // file loaded
		    safs::log_info("initialized sessions from file {}/{}",
		                   fs::getCurrentWorkingDirectoryNoThrow().c_str(), kSessionsFilename);
		    break;
		default:
		    safs::log_err("due to missing sessions ({}/{}) you have to restart all active mounts",
		                  fs::getCurrentWorkingDirectoryNoThrow().c_str(), kSessionsFilename);
		    break;
	}

	SessionSustainTime = cfg_getuint32("SESSION_SUSTAIN_TIME", 86400);

	if (SessionSustainTime > 7 * 86400) {
		SessionSustainTime = 7 * 86400;
		safs::log_warn(
		    "SESSION_SUSTAIN_TIME too big (more than week) - setting this value to one week");
	}

	if (SessionSustainTime < 60) {
		SessionSustainTime = 60;
		safs::log_warn(
		    "SESSION_SUSTAIN_TIME too low (less than minute) - setting this value to one minute");
	}

	return 0;
}

int matoclserv_iolimits_reload() {
	std::string configFile = cfg_getstring("GLOBALIOLIMITS_FILENAME", "");
	gIoLimitsAccumulate_ms = cfg_get_minvalue("GLOBALIOLIMITS_ACCUMULATE_MS", 250U, 1U);

	if (!configFile.empty()) {
		try {
			IoLimitsConfigLoader configLoader;
			configLoader.load(std::ifstream(configFile));
			gIoLimitsSubsystem = configLoader.subsystem();
			gIoLimitsDatabase.setLimits(
					SteadyClock::now(), configLoader.limits(), gIoLimitsAccumulate_ms);
		} catch (Exception& ex) {
			safs::log_err("failed to process global I/O limits configuration file ({}): {}",
			              configFile, ex.message());
			return -1;
		}
	} else {
		gIoLimitsSubsystem = "";
		gIoLimitsDatabase.setLimits(
				SteadyClock::now(), IoLimitsConfigLoader::LimitsMap(), gIoLimitsAccumulate_ms);
	}

	gIoLimitsRefreshTime = cfg_get_minvalue(
			"GLOBALIOLIMITS_RENEGOTIATION_PERIOD_SECONDS", 0.1, 0.001);

	gIoLimitsConfigId++;

	matoclserv_broadcast_iolimits_cfg();

	return 0;
}

void matoclserv_become_master() {
	starting = 120;
	matoclserv_reset_session_timeouts();
	matoclserv_start_cond_check();

	if (starting) {
		eventloop_timeregister(TIMEMODE_RUN_LATE, 1, 0, matoclserv_start_cond_check);
	}

	eventloop_timeregister(TIMEMODE_RUN_LATE, 10, 0, matocl_session_check);
	eventloop_timeregister(TIMEMODE_RUN_LATE, 3600, 0, matocl_session_stats_rotate);
	return;
}

void matoclserv_reload() {
	// Notify admins that reload was performed - put responses in their packet queues
	for (const auto &eptr : matoclservList) {
		if (eptr->adminTask == AdminTask::kReload) {
			matoclserv_createpacket(eptr.get(), matocl::adminReload::build(SAUNAFS_STATUS_OK));
			eptr->adminTask = AdminTask::kNone;
		}
	}

	RejectOld = cfg_getuint32("REJECT_OLD_CLIENTS", 0);
	SessionSustainTime = cfg_getuint32("SESSION_SUSTAIN_TIME", 86400);

	if (SessionSustainTime > 7 * 86400) {
		SessionSustainTime = 7 * 86400;
		safs::log_warn(
		    "SESSION_SUSTAIN_TIME too big (more than week) - setting this value to one week");
	}

	if (SessionSustainTime < 60) {
		SessionSustainTime = 60;
		safs::log_warn(
		    "SESSION_SUSTAIN_TIME too low (less than minute) - setting this value to one minute");
	}

	matoclserv_iolimits_reload();

	std::string oldListenHost = ListenHost;
	std::string oldListenPort = ListenPort;

	auto host = cfg_getstr("MATOCL_LISTEN_HOST","*");
	auto port = cfg_getstr("MATOCL_LISTEN_PORT","9421");

	ListenHost = host;
	ListenPort = port;

	free(host); // to avoid memory leak allocated by strdup in cfg_getstr() function
	free(port); // to avoid memory leak allocated by strdup in cfg_getstr() function

	if (oldListenHost == ListenHost && oldListenPort == ListenPort) {
		safs::log_info("main master server module: socket address hasn't changed ({}:{})",
		               ListenHost, ListenPort);
		return;
	}

	int newlsock = tcpsocket();
	if (newlsock < 0) {
		safs::log_warn(
		    "main master server module: socket address has changed, but can't create new socket");
		ListenHost = oldListenHost;
		ListenPort = oldListenPort;
		return;
	}

	tcpnonblock(newlsock);
	tcpnodelay(newlsock);
	tcpreuseaddr(newlsock);

	if (tcpsetacceptfilter(newlsock) < 0 && errno != ENOTSUP) {
		safs_silent_errlog(LOG_NOTICE, "main master server module: can't set accept filter");
	}

	if (tcpstrlisten(newlsock, ListenHost.c_str(), ListenPort.c_str(), 100) < 0) {
		safs::log_err(
		    "main master server module: socket address has changed, but can't listen on socket ({}:{})",
		    ListenHost, ListenPort);
		ListenHost = oldListenHost;
		ListenPort = oldListenPort;
		tcpclose(newlsock);
		return;
	}

	safs::log_info("main master server module: socket address has changed, now listen on {}:{}",
	               ListenHost, ListenPort);
	tcpclose(masterSocket);
	masterSocket = newlsock;
}

int matoclserv_network_init() {
	auto host = cfg_getstr("MATOCL_LISTEN_HOST", "*");
	auto port = cfg_getstr("MATOCL_LISTEN_PORT", "9421");

	ListenHost = host;
	ListenPort = port;

	free(host);  // to avoid memory leak allocated by strdup in cfg_getstr() function
	free(port);  // to avoid memory leak allocated by strdup in cfg_getstr() function

	RejectOld = cfg_getuint32("REJECT_OLD_CLIENTS", 0);

	if (matoclserv_iolimits_reload() != 0) {
		return -1;
	}

	exiting = 0;
	masterSocket = tcpsocket();
	if (masterSocket < 0) {
		safs::log_err("main master server module: can't create socket");
		return -1;
	}

	tcpnonblock(masterSocket);
	tcpnodelay(masterSocket);
	tcpreuseaddr(masterSocket);

	if (tcpsetacceptfilter(masterSocket) < 0 && errno != ENOTSUP) {
		safs::log_info("main master server module: can't set accept filter");
	}

	if (tcpstrlisten(masterSocket, ListenHost.c_str(), ListenPort.c_str(), 100) < 0) {
		safs::log_err("main master server module: can't listen on {}:{}", ListenHost, ListenPort);
		return -1;
	}

	safs::log_info("main master server module: listen on {}:{}", ListenHost, ListenPort);

	matoclservList.clear();

	if (metadataserver::isMaster()) {
		matoclserv_become_master();
	}

	eventloop_reloadregister(matoclserv_reload);
	metadataserver::registerFunctionCalledOnPromotion(matoclserv_become_master);
	eventloop_destructregister(matoclserv_term);
	eventloop_pollregister(matoclserv_desc,matoclserv_serve);
	eventloop_wantexitregister(matoclserv_wantexit);
	eventloop_canexitregister(matoclserv_canexit);
	return 0;
}

void matoclserv_session_unload() {
	for (const auto& sessionPtr : sessionVector) {
		sessionPtr->openFilesSet.clear();
	}

	sessionVector.clear();
}
