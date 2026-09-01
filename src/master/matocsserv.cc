/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
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

#include "master/matocsserv.h"

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
#include <list>
#include <vector>

#include "common/counting_sort.h"
#include "common/datapack.h"
#include "common/event_loop.h"
#include "common/goal.h"
#include "common/input_packet.h"
#include "common/loop_watchdog.h"
#include "common/massert.h"
#include "common/output_packet.h"
#include "common/random.h"
#include "common/saunafs_version.h"
#include "common/slice_traits.h"
#include "common/sockets.h"
#include "common/time_utils.h"
#include "common/tls_session.h"
#include "config/cfg.h"
#include "errors/saunafs_error_codes.h"
#include "master/chunk_metadata.h"
#include "master/chunk_operations_interface.h"
#include "master/chunks.h"
#include "master/chunkserver_db.h"
#include "master/filesystem.h"
#include "master/filesystem_operations_interface.h"
#include "master/get_servers_for_new_chunk.h"
#include "master/metadataserver_hooks.h"
#include "master/personality.h"
#include "protocol/SFSCommunication.h"
#include "protocol/cstoma.h"
#include "protocol/matocs.h"
#include "protocol/packet.h"
#include "slogger/slogger.h"

constexpr uint32_t kMaxPacketSize = 500'000'000;

// matocsserventry.mode
enum class ChunkserverConnectionMode : std::uint8_t {
	KILL,			/// Connection is terminated
	CONNECTED,		/// Connection is active
	HANDSHAKE		/// TLS handshake in progress
};

double gLoadFactorPenalty = 0.;
bool gPrioritizeDataParts = true;
bool gSortedServersNeedsRefresh = false;

// A safe threshold of 15 seconds to determine whether chunks registration is in progress and
// prevent dumping metadata.
constexpr auto kTimeoutForChunkRegistration = std::chrono::seconds(15);

// Default value of 1 second to initialize the Timeout instance.
// This value expires fast enough to not affect metadata dump when the cluster starts empty and
// there is no chunks registration.
Timeout gTimeoutSinceLastChunkRegistration(std::chrono::seconds(1));

// Max pending connections in accept queue
constexpr int kMaxListenBacklog = 100;

// Default CS timeout (ms)
constexpr uint32_t kDefaultChunkserverTimeoutMs = 60'000;

// Bytes per GiB
constexpr double kBytesPerGiB = 1024.0 * 1024.0 * 1024.0;

// Minimum allowed register timeout (ms)
constexpr uint32_t kMinRegisterTimeoutMs = 10;

// Keep-alive timeout divisor
constexpr uint32_t kDefaultKeepAliveTimeoutDivisor = 3;

// CSTOMA_SPACE packet sizes
constexpr uint32_t kSpacePktUsedTotal = 16;           // usedspace(8) + totalspace(8)
constexpr uint32_t kSpacePktWithToDelSpace = 32;      // + todelusedspace(8) + todeltotalspace(8)
constexpr uint32_t kSpacePktWithCountsAndToDel = 40;  // + chunkscount(4) + todelchunkscount(4)

struct matocsserventry {
	matocsserventry() : inputPacket(kMaxPacketSize) {}

	ChunkserverConnectionMode mode;
	int sock;
	int32_t pdescpos;
	Timer lastread,lastwrite;
	InputPacket inputPacket;
	std::list<OutputPacket> outputPackets;
	std::string serviceStrIp;       // human readable version of servip
	uint32_t version;
	uint32_t servip;                // ip to connect to
	uint16_t servport;              // port to connect to
	uint32_t timeout;               // communication timeout
	MediaLabel label;               // server label, empty if not set
	uint64_t usedspace;             // used hdd space in bytes
	uint64_t totalspace;            // total hdd space in bytes
	uint32_t chunkscount;
	uint64_t todelusedspace;
	uint64_t todeltotalspace;
	uint32_t todelchunkscount;
	uint32_t errorcounter;
	uint16_t rrepcounter;
	uint16_t wrepcounter;
	uint16_t delcounter;
	uint8_t load_factor;
	bool passive{false};
	bool chunkserverAccountingStarted{false};

	/// Context of the TLS channel used for communication with chunkserver.
	///
	/// If no TLS is used, this is `nullptr`.
	std::unique_ptr<TlsSession> tlsSession;
	int lastHandshakeError{0};

	csdbentry *csdb; /*!< Pointer to database entry for chunkserver. */

	static bool lessUsedAndLoaded(matocsserventry *first, matocsserventry *second) {
		double first_load_penalty = gLoadFactorPenalty * (double)first->load_factor / 100.;
		double second_load_penalty = gLoadFactorPenalty * (double)second->load_factor / 100.;

		double first_usage = double(first->usedspace) / double(first->totalspace) + first_load_penalty;
		double second_usage = double(second->usedspace) / double(second->totalspace) + second_load_penalty;

		return first_usage < second_usage;
	}
};

static std::list<std::unique_ptr<matocsserventry>> matocsservList;
static int lsock;
static int32_t lsockpdescpos;

// from config
static std::string gListenHost;
static std::string gListenPort;

void matocsserv_getserverdata(const matocsserventry *eptr, ChunkserverListEntry &result) {
	if (eptr) { result = *eptr; }
}

csdbentry *matocsserv_get_csdb(matocsserventry *eptr) {
	assert(eptr);
	return eptr->csdb;
}

bool matocsserv_sorted_servers_need_refresh() {
	return gSortedServersNeedsRefresh;
}

void matocsserv_sorted_servers_refresh_done() {
	gSortedServersNeedsRefresh = false;
}

/* replications DB */

// Replication hash parameters
constexpr uint32_t kReplicationHashSize = 256;

// Simple, fast hash for (chunkId, version) pairing.
// Uses XOR mix and a right-shift to spread bits, then maps into the bucket range.
inline uint32_t replicationHash(uint64_t chunkId, uint32_t version) noexcept {
	const uint64_t mix = chunkId ^ static_cast<uint64_t>(version) ^ (chunkId >> 8);

	return static_cast<uint32_t>(mix % kReplicationHashSize);
}

struct ReplicaSourceEntry {
	matocsserventry *src;
};

struct repdst {
	uint64_t chunkId;
	uint32_t chunkVersion;
	ChunkPartType chunkType;
	matocsserventry *destinationCs;
	std::list<std::unique_ptr<ReplicaSourceEntry>> replicaSourceList;
};

static std::list<std::unique_ptr<repdst>> rephash[kReplicationHashSize];

void matocsserv_replication_init() {
	uint32_t hash;

	for (hash = 0; hash < kReplicationHashSize; hash++) { rephash[hash].clear(); }
}

int matocsserv_replication_find(uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                                matocsserventry *dst) {
	uint32_t hash = replicationHash(chunkId, chunkVersion);
	for (const auto &replica : rephash[hash]) {
		if (replica->chunkId == chunkId
				&& replica->chunkVersion == chunkVersion
				&& replica->chunkType == chunkType
				&& replica->destinationCs == dst) {
			return 1;
		}
	}
	return 0;
}

void matocsserv_replication_begin(uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                                  matocsserventry *destination, uint8_t srccnt,
                                  matocsserventry *const *src) {
	if (srccnt == 0) {
		return;
	}

	uint32_t hash = replicationHash(chunkId, chunkVersion);

	std::unique_ptr<ReplicaSourceEntry> replicaSource;

	auto replica = std::make_unique<repdst>();
	replica->chunkId = chunkId;
	replica->chunkVersion = chunkVersion;
	replica->chunkType = chunkType;
	replica->destinationCs = destination;

	for (uint8_t i = 0 ; i < srccnt ; i++) {
		replicaSource = std::make_unique<ReplicaSourceEntry>();
		replicaSource->src = src[i];
		replica->replicaSourceList.push_back(std::move(replicaSource));
		static_cast<matocsserventry *>(src[i])->rrepcounter++;
	}

	rephash[hash].push_back(std::move(replica));
	destination->wrepcounter++;
}

void matocsserv_replication_end(uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType,
                                matocsserventry *destination) {
	uint32_t hash = replicationHash(chunkId, chunkVersion);

	for (auto replicaIt = rephash[hash].begin(); replicaIt != rephash[hash].end();) {
		auto &replica = *replicaIt;
		if (replica->chunkId == chunkId && replica->chunkVersion == chunkVersion &&
		    replica->chunkType == chunkType && replica->destinationCs == destination) {

			for (auto &replicaSource : replica->replicaSourceList) {
				replicaSource->src->rrepcounter--;
			}

			replica->replicaSourceList.clear();
			destination->wrepcounter--;

			replicaIt = rephash[hash].erase(replicaIt);
		} else {
			++replicaIt;
		}
	}
}

void matocsserv_replication_disconnected(matocsserventry *server) {
	for (auto & hash : rephash) {
		// Iterate through the list of replicas in the hash bucket.
		for (auto replicaIterator = hash.begin(); replicaIterator != hash.end();) {
			auto &replica = *replicaIterator;
			if (replica->destinationCs == server) {
				// If the destination server is the one we are disconnecting,
				// remove all sources and the replica itself.
				for (auto &replicaSource : replica->replicaSourceList) {
					replicaSource->src->rrepcounter--;
				}

				replica->replicaSourceList.clear();
				server->wrepcounter--;

				replicaIterator = hash.erase(replicaIterator);
			} else {
				auto replicaSrcIterator = replica->replicaSourceList.begin();
				while (replicaSrcIterator != replica->replicaSourceList.end()) {
					auto &replicaSource = *replicaSrcIterator;
					if (replicaSource->src == server) {
						server->rrepcounter--;
						replicaSrcIterator = replica->replicaSourceList.erase(replicaSrcIterator);
					} else {
						++replicaSrcIterator;
					}
				}
				++replicaIterator;
			}
		}
	}
}

/* replication DB END */
void matocsserv_usagedifference(double *minusage, double *maxusage, uint16_t *usablescount,
                                uint16_t *totalscount) {
	double minspace = 1.0;
	double maxspace = 0.0;
	double space;
	uint32_t chunkserversWithAvailableSpace = 0;
	uint32_t chunkserversAvailable = 0;
	constexpr uint32_t kMaxChunkservers = 65'535;

	for (const auto &eptr : matocsservList) {
		if (!eptr->chunkserverAccountingStarted) { continue; }

		if (eptr->mode != ChunkserverConnectionMode::KILL) {
			if (eptr->totalspace > 0 && eptr->usedspace <= eptr->totalspace) {
				space = (double)(eptr->usedspace) / (double)(eptr->totalspace);
				minspace = std::min(minspace, space);
				maxspace = std::max(maxspace, space);
				chunkserversWithAvailableSpace++;
			}
			chunkserversAvailable++;
		}

		if (chunkserversAvailable >= kMaxChunkservers) { break; }
	}

	if (usablescount) { *usablescount = chunkserversWithAvailableSpace; }

	if (totalscount) { *totalscount = chunkserversAvailable; }

	if (minusage) { *minusage = minspace; }

	if (maxusage) { *maxusage = maxspace; }
}

std::vector<ServerWithUsage> matocsserv_getservers_sorted() {
	std::vector<ServerWithUsage> result;
	for (const auto &eptr : matocsservList) {
		if (eptr->chunkserverAccountingStarted && eptr->mode != ChunkserverConnectionMode::KILL &&
		    eptr->totalspace > 0 && eptr->usedspace <= eptr->totalspace) {
			double usage = double(eptr->usedspace) / double(eptr->totalspace);
			result.emplace_back(eptr.get(), usage, eptr->label);
		}
	}

	std::sort(result.begin(), result.end(), [](const ServerWithUsage &u1, const ServerWithUsage &u2){
		return matocsserventry::lessUsedAndLoaded(u1.server, u2.server);
	});

	return result;
}

std::vector<std::pair<matocsserventry *, ChunkPartType>> matocsserv_getservers_for_new_chunk(
    uint8_t goal_id, uint16_t &min_server_count, uint32_t min_server_version) {
	static std::array<ChunkCreationHistory, GoalId::kMax + 1> history;
	GetServersForNewChunk getter;
	const Goal &goal(gFSOperations->getGoalDefinition(goal_id));

	min_server_count = std::numeric_limits<uint16_t>::max();

	for (const auto &eptr : matocsservList) {
		if (eptr->chunkserverAccountingStarted && eptr->mode != ChunkserverConnectionMode::KILL &&
		    eptr->totalspace > 0 && eptr->usedspace <= eptr->totalspace &&
		    (eptr->totalspace - eptr->usedspace) >= SFSCHUNKSIZE) {
			// A good weight formula will do the following:
			//   * Agree with the chunk balancing algorithm, i.e. avoid creating a distribution which
			//     immediately needs to be re-balanced.
			//   * Be coarse enough that when the cluster is balanced, weights are generally equal.
			//   * Keep weights constant across the cluster for long enough periods
			//     that the 'history' can also do its job.
			//
			// weight = percent free spaces
			const int64_t weight = 1024 * 1024 * (1. - matocsserv_get_usage(eptr.get()));
			getter.addServer(eptr.get(), eptr->label, weight, eptr->version, eptr->load_factor);
		}
	}

	getter.prepareData(history[goal_id]);

	std::vector<matocsserventry *> used_servers, servers;
	std::vector<std::pair<matocsserventry *, ChunkPartType>> ret;

	// At some point in future there might be a need to create chunks with more than one slice.
	// It would be good to clear used servers for each new slice (so we don't exhaust
	// chunkserver pool to fast). But it should be done on the need only basis so
	// we don't use any chunkserver twice if not necessary.
	for (const auto &slice : goal) {
		std::vector<std::pair<matocsserventry *, ChunkPartType>> slice_ret;
		min_server_count =
		    std::min(min_server_count,
		             static_cast<uint16_t>(slice_traits::getNumberOfDataParts(slice.getType())));

		// add parts index permutation here
		std::array<int, Goal::Slice::kMaxPartsCount> shuffle;

		std::iota(shuffle.begin(), shuffle.begin() + slice.size(), 0);

		if (gPrioritizeDataParts) {
			// Move xor parity to the end of a list
			if (slice_traits::isXor(slice)) {
				std::swap(shuffle[0], shuffle[slice.size() - 1]);
			}

			// Shuffle data before parity to prioritize data parts before parity
			// in partial writes
			int data_count = slice_traits::getNumberOfDataParts(slice);
			assert(std::all_of(shuffle.begin(), shuffle.begin() + data_count,
			                   [&slice](int i) {
				                   return slice_traits::isDataPart(
				                       ChunkPartType(slice.getType(), i));
			                   }));
			std::shuffle(shuffle.begin(), shuffle.begin() + data_count,
			             kRandomEngine);
			std::shuffle(shuffle.begin() + data_count,
			             shuffle.begin() + slice.size(), kRandomEngine);
		} else {
			std::shuffle(shuffle.begin(), shuffle.begin() + slice.size(),
			             kRandomEngine);
		}

		uint32_t min_version = std::max({
			slice_traits::isXor(slice) ? kFirstXorVersion : 0,
			slice_traits::isEC(slice) ? kFirstECVersion : 0,
			slice_traits::isEC(slice) && slice_traits::ec::isEC2(slice) ? kEC2Version : 0,
			min_server_version
		});

		int count_full_parts = 0;
		for (int i = 0; i < slice.size(); ++i) {
			servers = getter.chooseServersForLabels(
			        history[goal_id], slice[shuffle[i]],
			        min_version, used_servers);

			if (servers.empty()) {
				continue;
			}

			++count_full_parts;
			for (const auto &server : servers) {
				slice_ret.push_back(std::make_pair(
				        server, ChunkPartType(slice.getType(), shuffle[i])));
			}
		}

		if (count_full_parts < slice_traits::getNumberOfDataParts(slice.getType())) {
			continue; // do not create any parts if servers are missing
		}

		ret.insert(ret.end(), slice_ret.begin(), slice_ret.end());
	}

	return ret;
}

void matocsserv_getservers_lessrepl(const MediaLabel &label, uint32_t min_chunkserver_version,
		uint16_t replication_write_limit, const IpCounter &ip_counter, std::vector<matocsserventry *> &servers,
		int &total_matching, int &returned_matching, int &temporarily_unavailable) {
	total_matching = 0;
	returned_matching = 0;
	temporarily_unavailable = 0;
	servers.clear();
	for (const auto &eptr : matocsservList) {
		if (!eptr->chunkserverAccountingStarted || eptr->mode == ChunkserverConnectionMode::KILL ||
		    eptr->totalspace == 0 || eptr->usedspace > eptr->totalspace ||
		    (eptr->totalspace - eptr->usedspace) <= (eptr->totalspace / 100)) {
			// Skip full and disconnected chunkservers
			continue;
		}

		if (eptr->version < min_chunkserver_version) {
			continue;
		}

		bool matchesRequestedLabel = false;
		if (label != MediaLabel::kWildcard && eptr->label == label) {
			++total_matching;
			matchesRequestedLabel = true;
		}

		if (eptr->wrepcounter < replication_write_limit) {
			servers.push_back(eptr.get());
			if (matchesRequestedLabel) {
				++returned_matching;
			}
		} else {
			temporarily_unavailable++;
		}
	}

	std::shuffle(servers.begin(), servers.end(), kRandomEngine);
	std::sort(servers.begin(), servers.end(), matocsserventry::lessUsedAndLoaded);
	if (gAvoidSameIpChunkservers) {
		counting_sort(servers, [&ip_counter](matocsserventry *server) {
			auto it = ip_counter.find(server->servip);
			return it != ip_counter.end() ? it->second : 0;
		});
	}

	if (returned_matching > 0) {
		// Move servers matching the requested label to the front of the servers array
		std::stable_partition(servers.begin(), servers.end(), [&label](matocsserventry* cs) {
			return cs->label == label;
		});
	}
}

const MediaLabel& matocsserv_get_label(matocsserventry* eptr) {
	assert(eptr);
	return eptr->label;
}

double matocsserv_get_usage(matocsserventry* eptr) {
	if (eptr->usedspace > eptr->totalspace) {
		return 1.0;
	}

	return double(eptr->usedspace) / double(eptr->totalspace);
}

void matocsserv_getspace(uint64_t *totalspace,uint64_t *availspace) {
	uint64_t tspace = 0;
	uint64_t uspace = 0;
	for (const auto &eptr : matocsservList) {
		if (eptr->chunkserverAccountingStarted && eptr->mode != ChunkserverConnectionMode::KILL &&
		    eptr->totalspace > 0) {
			tspace += eptr->totalspace;
			uspace += eptr->usedspace;
		}
	}
	*totalspace = tspace;
	*availspace = tspace-uspace;
}

uint16_t matocsserv_get_listen_port() {
	// Resolved through the same getaddrinfo path the listener bound with, so a service name
	// (e.g. from /etc/services) reports the port it actually resolved to at bind time.
	uint16_t port = 0;
	if (tcpresolve(nullptr, gListenPort.c_str(), nullptr, &port, 1) < 0) { return 0; }
	return port;
}

const char* matocsserv_getstrip(matocsserventry *eptr) {
	static const char *empty = "???";
	if (eptr->mode != ChunkserverConnectionMode::KILL && !eptr->serviceStrIp.empty()) {
		return eptr->serviceStrIp.c_str();
	}
	return empty;
}

uint32_t matocsserv_get_servip(matocsserventry *eptr) {
	assert(eptr);
	return eptr->servip;
}

int matocsserv_getlocation(matocsserventry *eptr,uint32_t *servip,uint16_t *servport,
		MediaLabel* label) {
	if (eptr->mode != ChunkserverConnectionMode::KILL) {
		*servip = eptr->servip;
		*servport = eptr->servport;
		*label = eptr->label;
		return 0;
	}
	return -1;
}


uint16_t matocsserv_replication_write_counter(matocsserventry *eptr) {
	return eptr->wrepcounter;
}

uint16_t matocsserv_replication_read_counter(matocsserventry *eptr) {
	return eptr->rrepcounter;
}

uint16_t matocsserv_deletion_counter(matocsserventry *eptr) {
	return eptr->delcounter;
}

uint8_t* matocsserv_createpacket(matocsserventry *eptr,uint32_t type,uint32_t size) {
	eptr->outputPackets.emplace_back(PacketHeader(type, size));
	return eptr->outputPackets.back().packet.data() + PacketHeader::kSize;
}

/* for future use */
void matocsserv_got_chunk_checksum(matocsserventry *eptr, const uint8_t *data, uint32_t length) {
	uint64_t chunkid;
	uint32_t version, checksum;
	uint8_t status;
	constexpr uint32_t kChecksumPacketSizeStatusOnly =
	    sizeof(chunkid) + sizeof(version) + sizeof(status);
	constexpr uint32_t kChecksumPacketSizeWithChecksum =
	    sizeof(chunkid) + sizeof(version) + sizeof(checksum);

	if (length != kChecksumPacketSizeStatusOnly && length != kChecksumPacketSizeWithChecksum) {
		safs::log_info("CSTOAN_CHUNK_CHECKSUM - wrong size ({}/13|16)", length);
		eptr->mode = ChunkserverConnectionMode::KILL;
		return;
	}

	passert(data);
	chunkid = get64bit(&data);
	get32bit(&data, version);

	if (length == kChecksumPacketSizeStatusOnly) {
		status = get8bit(&data);
		safs::log_info("({}:{}) chunk: {:016X} calculate checksum status: {}", eptr->serviceStrIp,
		               eptr->servport, chunkid, saunafs_error_string(status));
	} else {
		get32bit(&data, checksum);
		safs::log_info("({}:{}) chunk: {:016X} calculate checksum: {:08X}", eptr->serviceStrIp,
		               eptr->servport, chunkid, checksum);
	}

	(void)version;
}

int matocsserv_send_createchunk(matocsserventry *eptr, uint64_t chunkId, ChunkPartType chunkType,
                                uint32_t chunkVersion, bool needsLock, bool &sentChunkLock) {
	sentChunkLock = false;
	if (eptr->mode != ChunkserverConnectionMode::KILL) {
		eptr->outputPackets.push_back(OutputPacket());
		sassert(eptr->version >= kFirstECVersion);
		if (eptr->version >= kFirstVersionWithChunkserverSideChunkLock && needsLock) {
			// For newer chunkservers, create and lock part
			matocs::createAndLockChunk::serialize(eptr->outputPackets.back().packet, chunkId,
			                                      chunkType, chunkVersion);
			sentChunkLock = true;
		} else {
			// For older chunkservers, create chunk without chunk lock, as they don't support it.
			matocs::createChunk::serialize(eptr->outputPackets.back().packet, chunkId, chunkType,
			                               chunkVersion);
		}
	}

	return 0;
}

void matocsserv_got_createchunk_status(matocsserventry *eptr, const std::vector<uint8_t> &data) {
	uint64_t chunkId;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	uint8_t status;

	sassert(eptr->version >= kFirstECVersion);
	PacketVersion v;
	deserializePacketVersionNoHeader(data, v);
	sassert(v == cstoma::createChunk::kECChunks);
	cstoma::createChunk::deserialize(data, chunkId, chunkType, status);

	gChunkOperations->gotCreateStatus(eptr, chunkId, chunkType, status);

	if (status != 0) {
		safs::log_info("({}:{}) chunk: {:016X} creation status: {}", eptr->serviceStrIp,
		               eptr->servport, chunkId, saunafs_error_string(status));
	}
}

int matocsserv_send_deletechunk(matocsserventry *eptr, uint64_t chunkId, uint32_t chunkVersion,
		ChunkPartType chunkType) {
	if (eptr->mode != ChunkserverConnectionMode::KILL) {
		eptr->outputPackets.push_back(OutputPacket());
		sassert(eptr->version >= kFirstECVersion);
		matocs::deleteChunk::serialize(eptr->outputPackets.back().packet,
				chunkId, chunkType, chunkVersion);
		eptr->delcounter++;
	}
	return 0;
}

void matocsserv_got_deletechunk_status(matocsserventry *eptr, const std::vector<uint8_t>& data) {
	uint64_t chunkId;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	uint8_t status;

	sassert(eptr->version >= kFirstECVersion);
	PacketVersion v;
	deserializePacketVersionNoHeader(data, v);
	sassert(v == cstoma::deleteChunk::kECChunks);
	cstoma::deleteChunk::deserialize(data, chunkId, chunkType, status);

	gChunkOperations->gotDeleteStatus(eptr, chunkId, chunkType, status);
	eptr->delcounter--;
	if (status != 0) {
		safs::log_info("({}:{}) chunk: {:016X} deletion status: {}", eptr->serviceStrIp,
		               eptr->servport, chunkId, saunafs_error_string(status));
	}
}

int matocsserv_send_sau_replicatechunk(matocsserventry *eptr, uint64_t chunkid, uint32_t version, ChunkPartType type,
		const std::vector<matocsserventry*> &sourcePointers, const std::vector<ChunkPartType> &sourceTypes) {
	if (matocsserv_replication_find(chunkid, version, type, eptr)) {
		return -1;
	}
	if (sourcePointers.size() != sourceTypes.size()) {
		safs::log_err("Inconsistent arguments for sau_replicatechunk ({} != {})",
		              sourcePointers.size(), sourceTypes.size());
		return -1;
	}
	if (eptr->mode == ChunkserverConnectionMode::KILL) {
		return 0;
	}
	for (auto source : sourcePointers) {
		if (source->mode == ChunkserverConnectionMode::KILL) {
			return 0;
		}
	}

	sassert(eptr->version >= kFirstECVersion);
	std::vector<ChunkTypeWithAddress> sources;
	for (size_t i = 0; i < sourcePointers.size(); ++i) {
		matocsserventry *src = sourcePointers[i];
		sources.emplace_back(NetworkAddress(src->servip, src->servport), sourceTypes[i],
		                     src->version);
	}
	eptr->outputPackets.emplace_back();
	matocs::replicateChunk::serialize(eptr->outputPackets.back().packet, chunkid, version, type,
	                                  sources);

	matocsserv_replication_begin(chunkid, version, type,
			eptr, sourcePointers.size(), sourcePointers.data());
	return 0;
}

void matocsserv_got_replicatechunk_status(matocsserventry *eptr, const std::vector<uint8_t> &data,
		 uint32_t /*packetType*/) {
	uint64_t chunkId;
	uint32_t chunkVersion;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	uint8_t status;

	PacketVersion v;
	deserializePacketVersionNoHeader(data, v);
	sassert(v == cstoma::replicateChunk::kECChunks);
	cstoma::replicateChunk::deserialize(data, chunkId, chunkType, status, chunkVersion);

	matocsserv_replication_end(chunkId, chunkVersion, chunkType, eptr);
	gChunkOperations->gotReplicateStatus(eptr, chunkId, chunkVersion, chunkType, status);
	if (status != 0 && status != SAUNAFS_ERROR_WAITING) {
		safs::log_info("({}:{}) chunk: {:016X} replication status: {}", eptr->serviceStrIp,
		               eptr->servport, chunkId, saunafs_error_string(status));
	}
}

int matocsserv_send_chunklock(matocsserventry *eptr, uint64_t chunkId, ChunkPartType chunkType,
                              bool needsLock, bool &sentChunkLock) {
	sentChunkLock = false;
	if (eptr->mode != ChunkserverConnectionMode::KILL) {
		sassert(eptr->version >= kFirstECVersion);
		if (eptr->version >= kFirstVersionWithChunkserverSideChunkLock && needsLock) {
			eptr->outputPackets.emplace_back();
			matocs::chunkLock::serialize(eptr->outputPackets.back().packet, chunkId, chunkType);
			sentChunkLock = true;
		}
		// For older chunkservers or if lock is not needed, we don't send chunk lock packet, as they
		// don't support it or it's not needed.
	}
	return 0;
}

void matocsserv_got_chunklock_status(matocsserventry *eptr, const std::vector<uint8_t> &data) {
	uint64_t chunkId;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	uint8_t status;

	sassert(eptr->version >= kFirstECVersion);
	PacketVersion v;
	deserializePacketVersionNoHeader(data, v);
	sassert(v == cstoma::chunkLock::kECChunks);
	cstoma::chunkLock::deserialize(data, chunkId, chunkType, status);

	gChunkOperations->gotChunkLockStatus(eptr, chunkId, chunkType, status);
	if (status != SAUNAFS_STATUS_OK) {
		safs::log_info("({}:{}) chunk: {:016X} chunk lock status: {}", eptr->serviceStrIp,
		               eptr->servport, chunkId, saunafs_error_string(status));
	}
}

void matocsserv_got_writeend_status(matocsserventry *eptr, const std::vector<uint8_t> &data) {
	uint64_t chunkId;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	uint8_t status;

	sassert(eptr->version >= kFirstECVersion);
	PacketVersion v;
	deserializePacketVersionNoHeader(data, v);
	sassert(v == cstoma::writeEndStatus::kECChunks);
	cstoma::writeEndStatus::deserialize(data, chunkId, chunkType, status);

	gChunkOperations->gotWriteEndStatus(eptr, chunkId, chunkType, status);
	if (status != SAUNAFS_STATUS_OK) {
		safs::log_info("({}:{}) chunk: {:016X} chunk write end status: {}", eptr->serviceStrIp,
		               eptr->servport, chunkId, saunafs_error_string(status));
	}
}

int matocsserv_send_chunkunlock(matocsserventry *eptr, uint64_t chunkId, ChunkPartType chunkType) {
	if (eptr->mode != ChunkserverConnectionMode::KILL) {
		sassert(eptr->version >= kFirstECVersion);
		if (eptr->version >= kFirstVersionWithChunkserverSideChunkLock) {
			eptr->outputPackets.emplace_back();
			matocs::chunkUnlock::serialize(eptr->outputPackets.back().packet, chunkId, chunkType);
		}
		// For older chunkservers, we don't send chunk unlock packet, as they don't support it.
	}
	return 0;
}

int matocsserv_send_setchunkversion(matocsserventry *eptr, uint64_t chunkId, uint32_t newVersion,
		uint32_t chunkVersion, ChunkPartType chunkType, bool needsLock, bool &sentChunkLock) {
	sentChunkLock = false;
	if (eptr->mode != ChunkserverConnectionMode::KILL) {
		eptr->outputPackets.emplace_back();
		sassert(eptr->version >= kFirstECVersion);
		if (eptr->version >= kFirstVersionWithChunkserverSideChunkLock && needsLock) {
			// For newer chunkservers, set version with chunk lock
			matocs::setVersionAndLock::serialize(eptr->outputPackets.back().packet, chunkId,
			                                     chunkType, chunkVersion, newVersion);
			sentChunkLock = true;
		} else {
			// For older chunkservers or if lock is not needed, set version without chunk lock, as
			// they don't support it or it's not needed.
			matocs::setVersion::serialize(eptr->outputPackets.back().packet, chunkId, chunkType,
			                              chunkVersion, newVersion);
		}
	}
	return 0;
}

void matocsserv_got_setchunkversion_status(matocsserventry *eptr,
		const std::vector<uint8_t>& data) {
	uint64_t chunkId;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	uint8_t status;

	sassert(eptr->version >= kFirstECVersion);
	PacketVersion v;
	deserializePacketVersionNoHeader(data, v);
	sassert(v == cstoma::setVersion::kECChunks);
	cstoma::setVersion::deserialize(data, chunkId, chunkType, status);

	gChunkOperations->gotSetVersionStatus(eptr, chunkId, chunkType, status);
	if (status != 0) {
		safs::log_info("({}:{}) chunk: {:016X} set version status: {}",
				eptr->serviceStrIp, eptr->servport, chunkId, saunafs_error_string(status));
	}
}

int matocsserv_send_duplicatechunk(matocsserventry *eptr, uint64_t newChunkId,
                                   uint32_t newChunkVersion, ChunkPartType chunkType,
                                   uint64_t chunkId, uint32_t chunkVersion, bool needsLock,
                                   bool &sentChunkLock) {
	sentChunkLock = false;
	if (eptr->mode == ChunkserverConnectionMode::KILL) { return 0; }

	OutputPacket outPacket;
	sassert(eptr->version >= kFirstECVersion);
	if (eptr->version >= kFirstVersionWithChunkserverSideChunkLock && needsLock) {
		// For newer chunkservers, duplicate with chunk lock
		matocs::duplicateAndLockChunk::serialize(outPacket.packet, newChunkId, newChunkVersion,
		                                         chunkType, chunkId, chunkVersion);
		sentChunkLock = true;
	} else {
		// For older chunkservers, duplicate without chunk lock, as they don't support it.
		matocs::duplicateChunk::serialize(outPacket.packet, newChunkId, newChunkVersion, chunkType,
		                                  chunkId, chunkVersion);
	}
	eptr->outputPackets.push_back(std::move(outPacket));
	return 0;
}

void matocsserv_got_duplicatechunk_status(matocsserventry* eptr, const std::vector<uint8_t>& data) {
	uint64_t chunkId;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	uint8_t status;

	sassert(eptr->version >= kFirstECVersion);
	PacketVersion v;
	deserializePacketVersionNoHeader(data, v);
	sassert(v == cstoma::duplicateChunk::kECChunks);
	cstoma::duplicateChunk::deserialize(data, chunkId, chunkType, status);

	gChunkOperations->gotDuplicateStatus(eptr, chunkId, chunkType, status);
	if (status != 0) {
		safs::log_info("({}:{}) chunk: {:016X}, type: {} duplication status: {}",
		               eptr->serviceStrIp, eptr->servport, chunkId, chunkType.getId(),
		               saunafs_error_string(status));
	}
}

void matocsserv_send_truncatechunk(matocsserventry* eptr, uint64_t chunkid, ChunkPartType chunkType, uint32_t length,
		uint32_t newVersion,uint32_t oldVersion) {
	if (eptr->mode == ChunkserverConnectionMode::KILL) {
		return;
	}

	sassert(eptr->version >= kFirstECVersion);
	eptr->outputPackets.emplace_back();
	matocs::truncateChunk::serialize(eptr->outputPackets.back().packet, chunkid, chunkType, length,
	                                 newVersion, oldVersion);
}

void matocsserv_got_sau_truncatechunk_status(matocsserventry *eptr,
		const std::vector<uint8_t>& data) {
	uint64_t chunkId;
	ChunkPartType chunkType;
	uint8_t status;
	PacketVersion v;

	deserializePacketVersionNoHeader(data, v);
	if (v == cstoma::truncate::kECChunks) {
		cstoma::truncate::deserialize(data, chunkId, chunkType, status);
	} else {
		legacy::ChunkPartType legacy_type;
		cstoma::truncate::deserialize(data, chunkId, legacy_type, status);
		chunkType = legacy_type;
	}

	gChunkOperations->gotTruncateStatus(eptr, chunkId, chunkType, status);
	if (status!=0) {
		safs::log_info("({}:{}) chunk: {:016X}, type: {:08X} truncate status: {}",
		               eptr->serviceStrIp, eptr->servport, chunkId, chunkType.getId(),
		               saunafs_error_string(status));
	}
}

int matocsserv_send_duptruncchunk(matocsserventry* eptr, uint64_t newChunkId, uint32_t newChunkVersion,
		ChunkPartType chunkType, uint64_t chunkId, uint32_t chunkVersion, uint32_t newChunkLength) {
	if (eptr->mode == ChunkserverConnectionMode::KILL) {
		return 0;
	}

	OutputPacket outPacket;
	sassert(eptr->version >= kFirstECVersion);
	matocs::duptruncChunk::serialize(outPacket.packet, newChunkId, newChunkVersion, chunkType,
	                                 chunkId, chunkVersion, newChunkLength);

	eptr->outputPackets.push_back(std::move(outPacket));
	return 0;
}

void matocsserv_got_duptruncchunk_status(matocsserventry* eptr, const std::vector<uint8_t>& data) {
	uint64_t chunkId;
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType();
	uint8_t status;

	sassert(eptr->version >= kFirstECVersion);
	PacketVersion v;
	deserializePacketVersionNoHeader(data, v);
	sassert(v == cstoma::duptruncChunk::kECChunks);
	cstoma::duptruncChunk::deserialize(data, chunkId, chunkType, status);

	gChunkOperations->gotDuptruncStatus(eptr, chunkId, chunkType, status);
	if (status != 0) {
		safs::log_info("({}:{}) chunk: {:016X}, type: {} duplication with truncate status: {}",
		               eptr->serviceStrIp, eptr->servport, chunkId, chunkType.getId(),
		               saunafs_error_string(status));
	}
}

void matocsserv_send_cluster_members(matocsserventry *eptr) {
	if (eptr->version < kFirstVersionWithPassiveMdsDiscovery ||
	    !gMetadataserverClusterMembersHook) {
		return;
	}

	try {
		auto members = gMetadataserverClusterMembersHook();
		if (members.size() > kMaxMetadataserverClusterEntries) {
			safs::log_warn("MDS discovery has {} members, refusing an oversized snapshot",
			               members.size());
			return;
		}

		OutputPacket discoveryPacket;
		matocs::clusterMembers::serialize(discoveryPacket.packet, members);
		eptr->outputPackets.push_back(std::move(discoveryPacket));
	} catch (const std::exception &exception) {
		safs::log_warn("MDS discovery snapshot failed: {}", exception.what());
	}
}

void matocsserv_register_host(matocsserventry *eptr, uint32_t version, uint32_t servip,
                              uint16_t servport, uint32_t timeout, const std::string &clusterId) {
	if (eptr->passive || eptr->csdb != nullptr) {
		safs::log_warn("SAU_CSTOMA_REGISTER_HOST received on an already registered connection");
		eptr->mode = ChunkserverConnectionMode::KILL;
		return;
	}

	eptr->version  = version;
	eptr->servip   = servip;
	eptr->servport = servport;
	eptr->timeout  = timeout;

	if (eptr->version < kFirstECVersion) {
		safs::log_err(
		    "SAU_CSTOMA_REGISTER_HOST received too old version: {} (minimum supported version is {})",
		    saunafsVersionToString(eptr->version), saunafsVersionToString(kFirstECVersion));
		eptr->mode = ChunkserverConnectionMode::KILL;
		return;
	}

	if (eptr->version >= kFirstVersionWithClusterId) {
		if (clusterId.empty()) {
			safs::log_err("SAU_CSTOMA_REGISTER_HOST received empty cluster ID");
			eptr->mode = ChunkserverConnectionMode::KILL;
			return;
		}

		if (clusterId != gClusterId) {
			safs::log_err(
			    "SAU_CSTOMA_REGISTER_HOST received mismatched cluster ID: expected {}, got {}",
			    gClusterId, clusterId);
			eptr->mode = ChunkserverConnectionMode::KILL;
			return;
		}
	}

	if (eptr->timeout < kMinRegisterTimeoutMs) {
		safs::log_info(
		    "SAU_CSTOMA_REGISTER communication timeout too small ({} milliseconds - should be at least 10 milliseconds)",
		    eptr->timeout);
		eptr->mode = ChunkserverConnectionMode::KILL;
		return;
	}

	if (eptr->servip == 0) { tcpgetpeer(eptr->sock, &(eptr->servip), nullptr); }

	eptr->serviceStrIp = ipToString(eptr->servip);

	if (isLoopbackAddress(eptr->servip)) {
		safs::log_warn("Chunkserver loopback IP addresses are experimental; consider assigning a non-loopback IP address to chunkserver (via /etc/hosts or some other way)");
	}

	if (csdb_new_connection(eptr->servip, eptr->servport, eptr) < 0) {
		safs::log_warn("chunk-server already connected !!!");
		eptr->mode = ChunkserverConnectionMode::KILL;
		return;
	}

	eptr->csdb = csdb_find(eptr->servip, eptr->servport);
	if (!eptr->chunkserverAccountingStarted) {
		gChunkOperations->serverUnlabelledConnected();
		eptr->chunkserverAccountingStarted = true;
	}

	safs::log_info("chunkserver register begin (packet version: 5) - ip: {}, port: {}",
	               eptr->serviceStrIp, eptr->servport);

	// Send the answer with the status
	if (eptr->version >= kFirstVersionWithClusterId) {
		OutputPacket outPacket;
		matocs::registerHost::serialize(outPacket.packet, SAUNAFS_STATUS_OK, SAUNAFS_VERSHEX,
		                                gClusterId);
		eptr->outputPackets.push_back(std::move(outPacket));
	}

	matocsserv_send_cluster_members(eptr);
}

void matocsserv_sau_register_passive(matocsserventry *eptr, const std::vector<uint8_t> &data) {
	if (!gMetadataserverClusterMembersHook) {
		safs::log_warn("SAU_CSTOMA_REGISTER_PASSIVE is not supported by Master");
		eptr->mode = ChunkserverConnectionMode::KILL;
		return;
	}

	if (eptr->passive || eptr->csdb != nullptr) {
		safs::log_warn("SAU_CSTOMA_REGISTER_PASSIVE received on an already registered connection");
		eptr->mode = ChunkserverConnectionMode::KILL;
		return;
	}

	uint32_t servip = 0;
	uint16_t servport = 0;
	uint32_t timeout = 0;
	uint32_t version = 0;
	std::string clusterId;
	cstoma::registerPassive::deserialize(data, servip, servport, timeout, version, clusterId);

	if (version < kFirstVersionWithPassiveMdsDiscovery || servport == 0 ||
	    timeout < kMinRegisterTimeoutMs || clusterId.empty() || clusterId != gClusterId) {
		safs::log_warn("SAU_CSTOMA_REGISTER_PASSIVE received invalid registration data");
		eptr->mode = ChunkserverConnectionMode::KILL;
		return;
	}
	if (servip == 0) { tcpgetpeer(eptr->sock, &servip, nullptr); }

	eptr->version = version;
	eptr->servip = servip;
	eptr->servport = servport;
	eptr->timeout = timeout;
	eptr->serviceStrIp = ipToString(servip);
	eptr->passive = true;

	eptr->outputPackets.emplace_back();
	matocs::registerPassive::serialize(eptr->outputPackets.back().packet, SAUNAFS_STATUS_OK,
	                                   SAUNAFS_VERSHEX, gClusterId);
	safs::log_info("passive chunkserver registered, ip: {}, port: {}, request bytes: {}",
	               eptr->serviceStrIp, eptr->servport, data.size());
}

void register_space(matocsserventry* eptr) {
	double us = (double)(eptr->usedspace) / kBytesPerGiB;
	double ts = (double)(eptr->totalspace) / kBytesPerGiB;
	safs::log_info(
	    "chunkserver register end (packet version: 5) - ip: {}, port: {}, usedspace: {} ({:.2f} GiB), totalspace: {} ({:.2f} GiB)",
	    eptr->serviceStrIp, eptr->servport, eptr->usedspace, us, eptr->totalspace, ts);
}

void matocsserv_space(matocsserventry *eptr, const uint8_t *data, uint32_t length) {
	if (length != kSpacePktUsedTotal && length != kSpacePktWithToDelSpace &&
	    length != kSpacePktWithCountsAndToDel) {
		safs::log_info("CSTOMA_SPACE - wrong size ({}/{}|{}|{})", length, kSpacePktUsedTotal,
		               kSpacePktWithToDelSpace, kSpacePktWithCountsAndToDel);
		eptr->mode = ChunkserverConnectionMode::KILL;
		return;
	}

	passert(data);
	eptr->usedspace = get64bit(&data);
	eptr->totalspace = get64bit(&data);

	// Optional counts present only in the 40-byte variant
	if (length == kSpacePktWithCountsAndToDel) { get32bit(&data, eptr->chunkscount); }

	// To-delete space fields present in 32- and 40-byte variants
	if (length >= kSpacePktWithToDelSpace) {
		eptr->todelusedspace = get64bit(&data);
		eptr->todeltotalspace = get64bit(&data);
		if (length == kSpacePktWithCountsAndToDel) { get32bit(&data, eptr->todelchunkscount); }
	}
}

void matocsserv_sau_register_host(matocsserventry *eptr, const std::vector<uint8_t> &data) {
	uint32_t version;
	uint32_t servip;
	uint16_t servport;
	uint32_t timeout;
	std::string clusterId;

	PacketVersion packetVersion;
	deserializePacketVersionNoHeader(data, packetVersion);

	if (packetVersion == cstoma::registerHost::kWithClusterId) {
		cstoma::registerHost::deserialize(data, servip, servport, timeout, version, clusterId);
	} else {
		cstoma::registerHost::deserialize(data, servip, servport, timeout, version);
	}

	matocsserv_register_host(eptr, version, servip, servport, timeout, clusterId);
}

void matocsserv_sau_register_chunks(matocsserventry *eptr, const std::vector<uint8_t>& data) {
	PacketVersion v;
	deserializePacketVersionNoHeader(data, v);

	sassert(v == cstoma::registerChunks::kECChunks);
	std::vector<ChunkWithVersionAndType> chunks;
	cstoma::registerChunks::deserialize(data, chunks);
	gChunkOperations->serverHasChunks(eptr, chunks);

	// Chunks registration is in progress, so we reset the timeout
	gTimeoutSinceLastChunkRegistration = Timeout(kTimeoutForChunkRegistration);
}

void matocsserv_sau_register_space(matocsserventry *eptr, const std::vector<uint8_t>& data) {
	cstoma::registerSpace::deserialize(data, eptr->usedspace, eptr->totalspace, eptr->chunkscount,
			eptr->todelusedspace, eptr->todeltotalspace, eptr->todelchunkscount);
	register_space(eptr);
}

void matocsserv_sau_register_label(matocsserventry *eptr, const std::vector<uint8_t>& data) {
	std::string label;
	cstoma::registerLabel::deserialize(data, label);
	if (!MediaLabelManager::isLabelValid(label)) {
		safs::log_info(
		    "SAU_CSTOMA_REGISTER_LABEL - wrong label '{}' of chunkserver "
		    "(ip: {}, port {})",
		    label.c_str(), eptr->serviceStrIp, eptr->servport);
		eptr->mode = ChunkserverConnectionMode::KILL;
		return;
	}

	if (eptr->csdb == nullptr) {
		safs::log_info(
		    "SAU_CSTOMA_REGISTER_LABEL - setting label is possible for registered connections only "
		    "(ip: {}, port {})",
		    eptr->serviceStrIp, eptr->servport);
		eptr->mode = ChunkserverConnectionMode::KILL;
		return;
	}

	if (label != static_cast<std::string>(eptr->label)) {
		safs::log_info("chunkserver (ip: {}, port {}) changed its label from '{}' to '{}'",
		    eptr->serviceStrIp, eptr->servport, static_cast<std::string>(eptr->label), label);
		gChunkOperations->serverLabelChanged(eptr->label, MediaLabel(label));
		eptr->label = MediaLabel(label);
		eptr->csdb->label = eptr->label;
	}
}

void matocsserv_sau_register_config(matocsserventry *eptr,
                                    const std::vector<uint8_t>& data) {
	std::string config;
	cstoma::registerConfig::deserialize(data, config);
	if (eptr->csdb == nullptr) {
		safs::log_info(
		    "SAU_CSTOMA_REGISTER_CONFIG - setting config is possible for registered connections only (ip: {}, port {})",
		    eptr->serviceStrIp, eptr->servport);
		eptr->mode = ChunkserverConnectionMode::KILL;
		return;
	}
	eptr->csdb->config = std::move(config);
}

void matocsserv_sau_status(matocsserventry *eptr, const std::vector<uint8_t> &data) {
	uint8_t load_factor;
	cstoma::status::deserialize(data, load_factor);
	eptr->load_factor = load_factor;
}

void matocsserv_sau_chunk_damaged(matocsserventry *eptr, const std::vector<uint8_t>& data) {
	PacketVersion v;
	deserializePacketVersionNoHeader(data, v);

	sassert(v == cstoma::chunkDamaged::kECChunks);
	std::vector<ChunkWithType> chunks;
	cstoma::chunkDamaged::deserialize(data, chunks);
	for (const auto &chunk : chunks) { gChunkOperations->damaged(eptr, chunk.id, chunk.type); }
}

void matocsserv_sau_chunks_lost(matocsserventry *eptr, const std::vector<uint8_t>& data) {
	PacketVersion v;
	deserializePacketVersionNoHeader(data, v);

	sassert(v == cstoma::chunkLost::kECChunks);
	std::vector<ChunkWithType> chunks;
	cstoma::chunkLost::deserialize(data, chunks);
	for (const auto &chunk : chunks) { gChunkOperations->lost(eptr, chunk.id, chunk.type); }
}

void matocsserv_sau_chunk_new(matocsserventry *eptr, const std::vector<uint8_t>& data) {
	PacketVersion v;
	deserializePacketVersionNoHeader(data, v);

	sassert(v == cstoma::chunkNew::kECChunks);
	std::vector<ChunkWithVersionAndType> chunks;
	cstoma::chunkNew::deserialize(data, chunks);
	gChunkOperations->serverHasChunks(eptr, chunks);
}

void matocsserv_error_occurred(matocsserventry *eptr, const uint8_t *data, uint32_t length) {
	(void)data;
	if (length != 0) {
		safs::log_info("CSTOMA_ERROR_OCCURRED - wrong size ({}/0)", length);
		eptr->mode = ChunkserverConnectionMode::KILL;
		return;
	}
	eptr->errorcounter++;
}

/// Starts/continues a TLS handshake (non-blocking)
/// @param eptr Pointer to the chunkserver connection in the master
void matocsserv_tlshandshake(matocsserventry *eptr) {
	sassert(eptr->mode == ChunkserverConnectionMode::HANDSHAKE);

	int ret = SSL_accept(eptr->tlsSession->session());

	if (ret == 1) {
		safs::log_info("TLS handshake completed with chunkserver from {}:{}",
		               eptr->serviceStrIp, eptr->servport);
		eptr->mode = ChunkserverConnectionMode::CONNECTED;
		return;
	}

	int err = SSL_get_error(eptr->tlsSession->session(), ret);
	eptr->lastHandshakeError = err;

	if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
		safs::log_info("TLS handshake in progress with chunkserver from {}:{}: {}",
		               eptr->serviceStrIp, eptr->servport, opensslErrorString(err));
		return;  // retry later
	}

	eptr->mode = ChunkserverConnectionMode::KILL;
	safs::log_err("TLS handshake failed: {}", opensslErrorString(err));
}

/// Initiate a TLS connection with the chunkserver.
/// @param eptr Pointer to the chunkserver connection in the master
void matocsserv_starttls(matocsserventry *eptr) {
	// Initialize a TLS session for the peer.
	std::string keyFile = cfg_getstring("TLS_KEY_FILE", std::string(TlsSession::kNoFile));
	std::string certFile = cfg_getstring("TLS_CERT_FILE", std::string(TlsSession::kNoFile));
	std::string trustFile =
	    cfg_getstring("TLS_CA_CERT_FILE", std::string(TlsSession::kNoFile));

	try {
		eptr->tlsSession =
		    std::make_unique<TlsSession>(eptr->sock, true, keyFile, certFile, trustFile);
		safs::log_info("Starting TLS session with chunkserver from {}:{}", eptr->serviceStrIp,
		               eptr->servport);
		eptr->mode = ChunkserverConnectionMode::HANDSHAKE;
		matocsserv_tlshandshake(eptr);
	} catch (const std::exception &e) {
		eptr->mode = ChunkserverConnectionMode::KILL;
		safs::log_err("Failed to start TLS session with chunkserver from {}:{}: {}",
		              eptr->serviceStrIp, eptr->servport, e.what());
	}
}

void matocsserv_gotpacket(matocsserventry *eptr, PacketHeader header, const MessageBuffer& data) {
	uint32_t length = data.size();
	if (eptr->passive && header.type != ANTOAN_NOP) {
		safs::log_warn("passive chunkserver connection sent packet type {}, closing it",
		               header.type);
		eptr->mode = ChunkserverConnectionMode::KILL;
		return;
	}

	try {
		switch (header.type) {
			case ANTOAN_NOP:
				break;
			case ANTOAN_UNKNOWN_COMMAND: // for future use
				break;
			case ANTOAN_BAD_COMMAND_SIZE: // for future use
				break;
			case CSTOMA_SPACE:
				matocsserv_space(eptr, data.data(), length);
				break;
			case CSTOMA_ERROR_OCCURRED:
				matocsserv_error_occurred(eptr, data.data(), length);
				break;
			case CSTOAN_CHUNK_CHECKSUM:
				matocsserv_got_chunk_checksum(eptr, data.data(), length);
				break;
			case SAU_CSTOMA_CREATE_CHUNK:
				matocsserv_got_createchunk_status(eptr, data);
				break;
			case SAU_CSTOMA_DELETE_CHUNK:
				matocsserv_got_deletechunk_status(eptr, data);
				break;
			case SAU_CSTOMA_REPLICATE_CHUNK:
				matocsserv_got_replicatechunk_status(eptr, data, header.type);
				break;
			case SAU_CSTOMA_DUPLICATE_CHUNK:
				matocsserv_got_duplicatechunk_status(eptr, data);
				break;
			case SAU_CSTOMA_LOCK_CHUNK:
				matocsserv_got_chunklock_status(eptr, data);
				break;
			case SAU_CSTOMA_WRITE_END_STATUS:
				matocsserv_got_writeend_status(eptr, data);
				break;
			case SAU_CSTOMA_SET_VERSION:
				matocsserv_got_setchunkversion_status(eptr, data);
				break;
			case SAU_CSTOMA_TRUNCATE:
				matocsserv_got_sau_truncatechunk_status(eptr, data);
				break;
			case SAU_CSTOMA_DUPTRUNC_CHUNK:
				matocsserv_got_duptruncchunk_status(eptr, data);
				break;
			case SAU_CSTOMA_CHUNK_DAMAGED:
				matocsserv_sau_chunk_damaged(eptr, data);
				break;
			case SAU_CSTOMA_CHUNK_LOST:
				matocsserv_sau_chunks_lost(eptr, data);
				break;
			case SAU_CSTOMA_CHUNK_NEW:
				matocsserv_sau_chunk_new(eptr, data);
				break;
			case SAU_CSTOMA_REGISTER_HOST:
				matocsserv_sau_register_host(eptr, data);
				break;
			case SAU_CSTOMA_REGISTER_PASSIVE:
				matocsserv_sau_register_passive(eptr, data);
				break;
			case SAU_CSTOMA_REGISTER_CHUNKS:
				matocsserv_sau_register_chunks(eptr, data);
				break;
			case SAU_CSTOMA_REGISTER_SPACE:
				matocsserv_sau_register_space(eptr, data);
				break;
			case SAU_CSTOMA_REGISTER_LABEL:
				matocsserv_sau_register_label(eptr, data);
				break;
			case SAU_CSTOMA_REGISTER_CONFIG:
				matocsserv_sau_register_config(eptr, data);
				break;
			case SAU_CSTOMA_STATUS:
				matocsserv_sau_status(eptr, data);
				break;
			case SAU_CSTOMA_STARTTLS:
				matocsserv_starttls(eptr);
				break;
			default:
				safs::log_info("master <-> chunkservers module: got unknown message "
						"(type:{})", header.type);
				eptr->mode = ChunkserverConnectionMode::KILL;
				break;
		}
	} catch (IncorrectDeserializationException& e) {
		safs::log_info("master <-> chunkservers module: got inconsistent message "
				"(type:{}, length:{}), {}", header.type, length, e.what());
		eptr->mode = ChunkserverConnectionMode::KILL;
	}
}

void matocsserv_term() {
	safs::log_info("master <-> chunkservers module: closing {}:{}", gListenHost, gListenPort);
	tcpclose(lsock);

	for (const auto &eptr : matocsservList) {
		if (eptr->tlsSession != nullptr) {
			int ret = SSL_shutdown(eptr->tlsSession->session());
			if (ret < 0) {
				safs::log_warn("TLS shutdown failed: {}",
				               opensslErrorString(SSL_get_error(eptr->tlsSession->session(), ret)));
			}
			eptr->tlsSession.reset();
		}
	}

	matocsservList.clear();
}

void matocsserv_read(matocsserventry *eptr) {
	SignalLoopWatchdog watchdog;

	watchdog.start();
	while (eptr->mode != ChunkserverConnectionMode::KILL) {
		uint32_t bytesToRead = eptr->inputPacket.bytesToBeRead();
		ssize_t ret;

		if (eptr->tlsSession != nullptr && eptr->mode != ChunkserverConnectionMode::HANDSHAKE) {
			ret =
			    SSL_read(eptr->tlsSession->session(), eptr->inputPacket.pointerToBeReadInto(),
			             eptr->inputPacket.bytesToBeRead());
		} else {
			ret = read(eptr->sock, eptr->inputPacket.pointerToBeReadInto(),
			                 eptr->inputPacket.bytesToBeRead());
		}

		if (ret == 0) {
			safs::log_info("connection with CS({}) has been closed by peer", eptr->serviceStrIp);
			eptr->mode = ChunkserverConnectionMode::KILL;
			return;
		}

		if (ret < 0) {
			if (eptr->tlsSession != nullptr && eptr->mode != ChunkserverConnectionMode::HANDSHAKE) {
				int err = SSL_get_error(eptr->tlsSession->session(), ret);
				if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
					safs::log_info("read from CS({}) error: {}", eptr->serviceStrIp,
					               opensslErrorString(err));
					eptr->mode = ChunkserverConnectionMode::KILL;
				}
				return;
			} else {
				int err = errno;
				if (err != EAGAIN) {
					safs::log_info("read from CS({}) error: {}", eptr->serviceStrIp, strerr(err));
					eptr->mode = ChunkserverConnectionMode::KILL;
				}
			}
			return;
		}

		try {
			eptr->inputPacket.increaseBytesRead(ret);
		} catch (InputPacketTooLongException& ex) {
			safs::log_warn("reading from CS({}): {}", eptr->serviceStrIp, ex.what());
			eptr->mode = ChunkserverConnectionMode::KILL;
			return;
		}

		if (ret == bytesToRead && !eptr->inputPacket.hasData()) {
			// there might be more data to read in socket's buffer
			continue;
		}

		if (!eptr->inputPacket.hasData()) { return; }

		matocsserv_gotpacket(eptr, eptr->inputPacket.getHeader(), eptr->inputPacket.getData());
		eptr->inputPacket.reset();

		if (watchdog.expired()) {
			break;
		}
	}
}

void matocsserv_write(matocsserventry *eptr) {
	SignalLoopWatchdog watchdog;

	watchdog.start();
	while (!eptr->outputPackets.empty()) {
		OutputPacket &pack = eptr->outputPackets.front();
		ssize_t bytesWritten;

		if (eptr->tlsSession != nullptr) {
			bytesWritten = SSL_write(eptr->tlsSession->session(),
			                         pack.packet.data() + pack.bytesSent,
			                         pack.packet.size() - pack.bytesSent);
			if (bytesWritten < 0) {
				int err = SSL_get_error(eptr->tlsSession->session(), bytesWritten);
				if (err != SSL_ERROR_WANT_WRITE && err != SSL_ERROR_WANT_READ) {
					safs::log_info("write to CS({}) error: {}", eptr->serviceStrIp,
					               opensslErrorString(err));
					eptr->mode = ChunkserverConnectionMode::KILL;
				}
				return;
			}
		} else {
			bytesWritten = write(eptr->sock, pack.packet.data() + pack.bytesSent,
			                     pack.packet.size() - pack.bytesSent);
			if (bytesWritten < 0) {
				int err = errno;
				if (err != EAGAIN) {
					safs::log_info("write to CS({}) error: {}", eptr->serviceStrIp.c_str(),
					               strerr(err));
					eptr->mode = ChunkserverConnectionMode::KILL;
				}
				return;
			}
		}

		pack.bytesSent += bytesWritten;
		if (pack.packet.size() != pack.bytesSent) {
			return;
		}

		eptr->outputPackets.pop_front();

		if (watchdog.expired()) {
			break;
		}
	}
}

void matocsserv_desc(std::vector<pollfd> &pdesc) {
	pdesc.push_back({lsock, POLLIN, 0});
	lsockpdescpos = pdesc.size() - 1;

	for (const auto &eptr : matocsservList) {
		pdesc.push_back({eptr->sock, POLLIN, 0});
		eptr->pdescpos = pdesc.size() - 1;

		if (eptr->mode == ChunkserverConnectionMode::HANDSHAKE) {
			short events = 0;
			switch (eptr->lastHandshakeError) {
			case SSL_ERROR_WANT_READ:
				events = POLLIN;
				break;
			case SSL_ERROR_WANT_WRITE:
				events = POLLOUT;
				break;
			default:
				events = POLLIN | POLLOUT;
				break;
			}
			pdesc.back().events = events;
		} else {
			if (!eptr->outputPackets.empty()) { pdesc.back().events |= POLLOUT; }
		}
	}
}

void matocsserv_serve(const std::vector<pollfd> &pdesc) {
	uint32_t peerip;
	std::unique_ptr<matocsserventry> eptr;
	int newSocket;

	if (lsockpdescpos >= 0 && (pdesc[lsockpdescpos].revents & POLLIN)) {
		newSocket = tcpaccept(lsock);

		if (newSocket < 0) {
			int err = errno;
			safs::log_info("master<->CS socket: accept error: {}", strerr(err));
		} else if (metadataserver::isMaster()) {
			tcpnonblock(newSocket);
			tcpnodelay(newSocket);
			eptr = std::make_unique<matocsserventry>();
			passert(eptr);
			eptr->sock = newSocket;
			eptr->pdescpos = -1;
			eptr->mode = ChunkserverConnectionMode::CONNECTED;
			eptr->lastread.reset();
			eptr->lastwrite.reset();

			tcpgetpeer(eptr->sock, &peerip, nullptr);
			eptr->serviceStrIp = ipToString(peerip);
			eptr->version = 0;
			eptr->servip = 0;
			eptr->servport = 0;
			eptr->timeout = kDefaultChunkserverTimeoutMs;
			eptr->label = MediaLabel::kWildcard;
			eptr->usedspace = 0;
			eptr->totalspace = 0;
			eptr->chunkscount = 0;
			eptr->todelusedspace = 0;
			eptr->todeltotalspace = 0;
			eptr->todelchunkscount = 0;
			eptr->errorcounter = 0;
			eptr->rrepcounter = 0;
			eptr->wrepcounter = 0;
			eptr->delcounter = 0;
			eptr->csdb = nullptr;
			eptr->load_factor = 0;
			eptr->passive = false;
			eptr->chunkserverAccountingStarted = false;
			eptr->tlsSession = nullptr;

			matocsservList.emplace_back(std::move(eptr));
			if (!gMetadataserverClusterMembersHook) {
				gChunkOperations->serverUnlabelledConnected();
				matocsservList.back()->chunkserverAccountingStarted = true;
			}
		} else {
			tcpclose(newSocket);
		}
	}

	for (const auto &eptr : matocsservList) {
		if (eptr->pdescpos >= 0) {
			if (pdesc[eptr->pdescpos].revents & (POLLERR | POLLHUP)) {
				eptr->mode = ChunkserverConnectionMode::KILL;
			}

			if (eptr->mode == ChunkserverConnectionMode::HANDSHAKE) {
				eptr->lastread.reset();
				matocsserv_tlshandshake(eptr.get());
			} else {
				if ((pdesc[eptr->pdescpos].revents & POLLIN) &&
				    eptr->mode != ChunkserverConnectionMode::KILL) {
					eptr->lastread.reset();
					matocsserv_read(eptr.get());
				}

				if ((pdesc[eptr->pdescpos].revents & POLLOUT) &&
				    eptr->mode != ChunkserverConnectionMode::KILL) {
					eptr->lastwrite.reset();
					matocsserv_write(eptr.get());
				}
			}
		}

		if (eptr->lastread.elapsed_ms() > eptr->timeout) {
			eptr->mode = ChunkserverConnectionMode::KILL;
		}

		if (eptr->lastwrite.elapsed_ms() > (eptr->timeout / kDefaultKeepAliveTimeoutDivisor) &&
		    eptr->outputPackets.empty()) {
			matocsserv_createpacket(eptr.get(), ANTOAN_NOP, 0);
		}
	}

	for (auto entriesIterator = matocsservList.begin(); entriesIterator != matocsservList.end();) {
		auto *eptr = entriesIterator->get();
		if (eptr->mode == ChunkserverConnectionMode::KILL) {
			if (eptr->passive) {
				safs::log_info("passive chunkserver disconnected, ip: {}, port: {}",
				               eptr->serviceStrIp, eptr->servport);
			} else {
				double usedSpace = (double)(eptr->usedspace) / kBytesPerGiB;
				double totalSpace = (double)(eptr->totalspace) / kBytesPerGiB;
				safs::log_info(
				    "chunkserver disconnected - ip: {}, port: {}, usedspace: {} ({:.2f} GiB), "
				    "totalspace: {} ({:.2f} GiB)",
				    eptr->serviceStrIp, eptr->servport, eptr->usedspace, usedSpace,
				    eptr->totalspace, totalSpace);
			}

			if (eptr->chunkserverAccountingStarted) {
				matocsserv_replication_disconnected(eptr);
				gChunkOperations->serverDisconnected(eptr, eptr->label);
			}

			if (eptr->csdb) { csdb_lost_connection(eptr->servip, eptr->servport); }

			tcpclose(eptr->sock);
			if (eptr->chunkserverAccountingStarted) { gSortedServersNeedsRefresh = true; }

			entriesIterator = matocsservList.erase(entriesIterator);
		} else {
			++entriesIterator;
		}
	}
}

void matocsserv_reload() {
	std::string oldListenHost = gListenHost;
	std::string oldListenPort = gListenPort;
	gListenHost = cfg_getstring("MATOCS_LISTEN_HOST","*");
	gListenPort = cfg_getstring("MATOCS_LISTEN_PORT","9420");
	gLoadFactorPenalty = cfg_get_minmaxvalue<double>("LOAD_FACTOR_PENALTY", 0., 0., 0.5);

	auto previousValue = gPrioritizeDataParts;
	gPrioritizeDataParts =
	    static_cast<bool>(cfg_getuint32("PRIORITIZE_DATA_PARTS", 1));

	if (previousValue != gPrioritizeDataParts) {
		safs::log_info(
		    "master <-> chunkservers module: "
		    "PRIORITIZE_DATA_PARTS has changed to {}",
		    static_cast<int>(gPrioritizeDataParts));
	}

	if (oldListenHost == gListenHost && oldListenPort == gListenPort) {
		safs::log_info("master <-> chunkservers module: socket address hasn't changed ({}:{})",
		               gListenHost, gListenPort);
		return;
	}

	int newlsock = tcpsocket();
	if (newlsock < 0) {
		int err = errno;
		safs::log_warn(
		    "master <-> chunkservers module: socket address has changed, but can't create new socket: {}",
		    strerr(err));
		gListenHost = oldListenHost;
		gListenPort = oldListenPort;
		return;
	}

	tcpnonblock(newlsock);
	tcpnodelay(newlsock);
	tcpreuseaddr(newlsock);

	int err = errno;
	if (tcpsetacceptfilter(newlsock) < 0 && err != ENOTSUP) {
		safs::log_info("master <-> chunkservers module: can't set accept filter: {}", strerr(err));
	}

	if (tcpstrlisten(newlsock, gListenHost.c_str(), gListenPort.c_str(), kMaxListenBacklog) < 0) {
		int err = errno;
		safs::log_err(
		    "master <-> chunkservers module: socket address has changed, but can't listen on socket ({}:{}): {}",
		    gListenHost, gListenPort, strerr(err));
		gListenHost = oldListenHost;
		gListenPort = oldListenPort;
		tcpclose(newlsock);
		return;
	}

	safs::log_info(
	    "master <-> chunkservers module: socket address has changed, now listen on {}:{}",
	    gListenHost, gListenPort);

	tcpclose(lsock);
	lsock = newlsock;
}

bool matocsserv_is_killed(matocsserventry* eptr) {
	return eptr != nullptr && eptr->mode == ChunkserverConnectionMode::KILL;
}

void matocsserv_request_disconnect(matocsserventry *eptr) {
	if (eptr != nullptr) { eptr->mode = ChunkserverConnectionMode::KILL; }
}

uint32_t matocsserv_get_version(matocsserventry *eptr) {
	return eptr->version;
}

int matocsserv_init() {
	gListenHost = cfg_getstring("MATOCS_LISTEN_HOST","*");
	gListenPort = cfg_getstring("MATOCS_LISTEN_PORT","9420");
	gLoadFactorPenalty = cfg_get_minmaxvalue<double>("LOAD_FACTOR_PENALTY", 0., 0., 0.5);
	gPrioritizeDataParts = static_cast<bool>(cfg_getuint32("PRIORITIZE_DATA_PARTS", 1));

	lsock = tcpsocket();
	if (lsock < 0) {
		safs::log_err("master <-> chunkservers module: can't create socket");
		return -1;
	}

	tcpnonblock(lsock);
	tcpnodelay(lsock);
	tcpreuseaddr(lsock);

	if (tcpsetacceptfilter(lsock) < 0 && errno != ENOTSUP) {
		safs::log_info("master <-> chunkservers module: can't set accept filter");
	}

	if (tcpstrlisten(lsock, gListenHost.c_str(), gListenPort.c_str(), kMaxListenBacklog) < 0) {
		safs::log_err("master <-> chunkservers module: can't listen on {}:{}", gListenHost,
		              gListenPort);
		return -1;
	}

	safs::log_info("master <-> chunkservers module: listen on {}:{}", gListenHost, gListenPort);

	matocsserv_replication_init();

	eventloop_reloadregister(matocsserv_reload);
	eventloop_destructregister(matocsserv_term);
	eventloop_pollregister(matocsserv_desc, matocsserv_serve);
	return 0;
}
