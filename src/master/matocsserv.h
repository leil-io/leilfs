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

#pragma once

#include "common/platform.h"

#include <cstdint>
#include <vector>

#include "common/chunk_part_type.h"
#include "common/media_label.h"
#include "common/time_utils.h"
#include "master/get_servers_for_new_chunk.h"
#include "protocol/chunkserver_list_entry.h"

/// Expires a short interval after the last chunk-registration message; while it
/// is unexpired, chunkserver chunk registration is treated as still in progress.
extern Timeout gTimeoutSinceLastChunkRegistration;

/// A struct representing a chunkserver.
struct matocsserventry;

struct csdbentry;

/// A list of chunkservers.
using Chunkservers = std::vector<matocsserventry*>;

/// A struct used in matocsserv_getservers_sorted
struct ServerWithUsage {
	ServerWithUsage() : server(nullptr), disk_usage(), label() {
	}

	ServerWithUsage(matocsserventry* server, double disk_usage, const MediaLabel& label)
			: server(server),
			  disk_usage(disk_usage),
			  label(label) {
	}

	matocsserventry* server;
	double disk_usage;
	MediaLabel label;
};

using IpCounter = flat_map<uint32_t, int, small_vector<std::pair<uint32_t, int>, 16>>;

/*! \brief Get list of chunkservers for replication with the given label.
 *
 * This function returns a list of chunkservers that currently don't exceed the given limit of
 * chunks replicated into them. Servers with 99% disk usage are treated as non-existing, thus not
 * returned. The returned servers are randomly shuffled, but if the \p label is not a
 * \p MediaLabel::kWildcard, then servers with this label would be placed in front of the returned
 * list and \p returnedMatching would be set to the number of them.
 *
 * \param label - the requested label.
 * \param min_chunkserver_version - return only chunkservers with higher (or equal) version.
 * \param replicationWriteLimit - return only chunkservers with fewer ongoing replicatons.
 * \param servers[out] - list of chunkservers for replication.
 * \param totalMatching[out] - number of existing chunkservers that matched the requested label.
 * \param returnedMatching[out] - number of returned chunkservers that matched the requested label.
 * \return Number of valid entries in \p servers.
 */
void matocsserv_getservers_lessrepl(const MediaLabel &label, uint32_t min_chunkserver_version,
		uint16_t replication_write_limit, const IpCounter &ip_counter,
		std::vector<matocsserventry*> &servers,
		int &total_matching, int &returned_matching, int &temporarily_unavailable);

/*! \brief Get chunkserver's label. */
const MediaLabel& matocsserv_get_label(matocsserventry* eptr);

/*! \brief Get chunkserver's disk usage. */
double matocsserv_get_usage(matocsserventry* eptr);

/*! \brief Get chunkservers ordered by disk usage. */
std::vector<ServerWithUsage> matocsserv_getservers_sorted();

/*! \brief Check if chunkserver is killed. */
bool matocsserv_is_killed(matocsserventry* eptr);

/*! \brief Mark a chunkserver connection for disconnection by the event loop. */
void matocsserv_request_disconnect(matocsserventry *eptr);

uint32_t matocsserv_get_version(matocsserventry* eptr);
void matocsserv_usagedifference(double *minusage, double *maxusage, uint16_t *usablescount,
                                uint16_t *totalscount);

/*! \brief Check if sorted servers need refresh. */
bool matocsserv_sorted_servers_need_refresh();

/*! \brief Acknowledge that sorted servers have been refreshed. */
void matocsserv_sorted_servers_refresh_done();

/*! \brief Get chunkservers for a new chunk.
 *
 * This function returns a list of chunkservers that can be used for a new chunk creation. The
 * returned servers are randomly shuffled and sorted by disk usage, so the least used and least
 * loaded servers are returned first.
 *
 * \param goalId - the id of the goal for which the chunk is being created.
 * \param min_server_count[out] - the minimum number of servers that should be returned for each
 *                                slice in the goal. This is needed to determine if there are
 *                                enough servers to create a chunk with the given goal.
 * \param min_server_version - return only chunkservers with higher (or equal) version.
 * \return List of chunkservers for new chunk creation with their types.
 */
std::vector<std::pair<matocsserventry *, ChunkPartType>> matocsserv_getservers_for_new_chunk(
    uint8_t goalId, uint16_t &min_server_count, uint32_t min_server_version = 0);
void matocsserv_getspace(uint64_t* totalspace, uint64_t* availspace);
/// Returns the port this instance is actually listening on for chunkserver connections,
/// resolved the same way the listener resolved it at bind time (a number or a service
/// name); 0 means no concrete port (the ephemeral "*", or a string naming no port).
/// Can differ from MATOCS_LISTEN_PORT's raw config value right after a reload that failed
/// to rebind: the listener then keeps its previous, working port, but config already shows
/// the new one.
uint16_t matocsserv_get_listen_port();
const char* matocsserv_getstrip(matocsserventry* eptr);
uint32_t matocsserv_get_servip(matocsserventry *eptr);
int matocsserv_getlocation(matocsserventry* eptr, uint32_t* servip, uint16_t* servport,
		MediaLabel* label);
uint16_t matocsserv_replication_read_counter(matocsserventry* eptr);
uint16_t matocsserv_replication_write_counter(matocsserventry* eptr);
uint16_t matocsserv_deletion_counter(matocsserventry* eptr);
int matocsserv_send_sau_replicatechunk(matocsserventry* eptr,
		uint64_t chunkid, uint32_t version, ChunkPartType type,
		const std::vector<matocsserventry*> &sourcePointers,
		const std::vector<ChunkPartType> &sourceTypes);

int matocsserv_send_deletechunk(matocsserventry* eptr,
		uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType);
int matocsserv_send_createchunk(matocsserventry *eptr, uint64_t chunkid, ChunkPartType chunkType,
                                uint32_t version, bool needsLock, bool &sentChunkLock);
int matocsserv_send_chunklock(matocsserventry *eptr, uint64_t chunkId, ChunkPartType chunkType,
                              bool needsLock, bool &sentChunkLock);
int matocsserv_send_chunkunlock(matocsserventry *eptr, uint64_t chunkId, ChunkPartType chunkType);
int matocsserv_send_setchunkversion(matocsserventry *eptr, uint64_t chunkId, uint32_t newVersion,
                                    uint32_t chunkVersion, ChunkPartType chunkType, bool needsLock,
                                    bool &sentChunkLock);
int matocsserv_send_duplicatechunk(matocsserventry *eptr, uint64_t newChunkId,
                                   uint32_t newChunkVersion, ChunkPartType chunkType,
                                   uint64_t chunkId, uint32_t chunkVersion, bool needsLock,
                                   bool &sentChunkLock);
void matocsserv_send_truncatechunk(matocsserventry* eptr,
		uint64_t chunkid, ChunkPartType chunkType, uint32_t length,
		uint32_t version, uint32_t oldversion);
int matocsserv_send_duptruncchunk(matocsserventry* eptr,
		uint64_t newChunkId, uint32_t newChunkVersion,
		ChunkPartType chunkType, uint64_t chunkId, uint32_t chunkVersion, uint32_t length);
int matocsserv_init();
void matocsserv_getserverdata(const matocsserventry* eptr, ChunkserverListEntry &result);
csdbentry *matocsserv_get_csdb(matocsserventry* eptr);

/*! \brief Starts an on-demand chunk-location query for \p chunkId.
 *
 * Broadcast to every connected chunkserver that supports location queries.
 * This includes chunkservers that have finished their registration handshake:
 * an ongoing disk scan can still announce chunks through CHUNK_NEW, whose
 * locations may not be known to the master yet.
 * When all queried chunkservers answered -- or the query timed out -- the
 * waiters are completed through matoclserv_chunk_locations_resolved().
 * Queries for the same chunk id are coalesced.
 *
 * \return true if the query is pending and the caller should defer its reply
 *         until matoclserv_chunk_locations_resolved(chunkId) fires; false if
 *         nothing can be queried (no eligible chunkserver, or too many
 *         queries in flight) and the caller should reply immediately.
 */
bool matocsserv_query_chunk_location(uint64_t chunkId);

/*! \brief Maximum number of deferred client waits. */
uint32_t matocsserv_get_on_demand_chunk_query_waiter_limit();

/// Maximum number of chunk records one pull-registration window may admit per
/// chunkserver. This bounds the per-connection queue independently of the
/// operator-selected bulk size and window.
constexpr uint32_t kMaxChunkRegistrationInFlightChunks = 100000;

/// Limits a requested pull-registration window so it never admits more than
/// kMaxChunkRegistrationInFlightChunks records for the given bulk size.
/// Exposed to pin the configuration safety limit in unit tests.
uint32_t matocsserv_limit_chunk_registration_window(uint32_t bulkSize, uint32_t requestedWindow);

/*!
 * \brief Groups chunk ids for SAU_MATOCS_QUERY_CHUNKS packets.
 *
 * A chunkserver drops the connection on any packet above
 * kMaxMasterToChunkserverPacketSize, and the number of chunks waiting on a
 * location is bounded only by ON_DEMAND_CHUNK_QUERY_LIMIT, so the ids cannot
 * all go out together. Each returned group is small enough that the packet
 * built from it fits; every id appears exactly once, in the order given.
 *
 * Exposed for testing: the volume needed to overflow a packet is a cluster's
 * worth of clients blocking at the same instant, which a system test cannot
 * reproduce.
 */
std::vector<std::vector<uint64_t>> matocsserv_split_chunk_query_ids(
    const std::vector<uint64_t> &chunkIds);
