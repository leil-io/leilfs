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

#include "master/chunks.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/chunk_copies_calculator.h"
#include "common/test_event_stream.h"
#include "common/chunk_version_with_todel_flag.h"
#include "common/chunks_availability_state.h"
#include "common/compact_vector.h"
#include "common/coroutine.h"
#include "common/counting_sort.h"
#include "common/datapack.h"
#include "common/event_loop.h"
#include "common/exceptions.h"
#include "common/flat_set.h"
#include "common/goal.h"
#include "common/hashfn.h"
#include "common/loop_watchdog.h"
#include "common/massert.h"
#include "common/saunafs_version.h"
#include "common/slice_traits.h"
#include "common/small_vector.h"
#include "master/checksum.h"
#include "master/chunk_goal_counters.h"
#include "master/chunk_operations_interface.h"
#include "master/chunk_metadata.h"
#include "master/chunkserver_db.h"
#include "master/filesystem.h"
#include "master/filesystem_operations_interface.h"
#include "master/get_servers_for_new_chunk.h"
#include "master/goal_cache.h"
#include "master/id_generator_incremental.h"
#include "metrics/metrics.h"
#include "protocol/SFSCommunication.h"
#include "slogger/slogger.h"

#ifdef METARESTORE
#  include <ctime>
#else
#  include "config/cfg.h"
#  include "common/main.h"
#  include "common/random.h"
#  include "master/matoclserv.h"
#  include "master/matocsserv.h"
#  include "master/topology.h"
#endif

#define MINLOOPTIME 1
#define MAXLOOPTIME 7200
#define MAXCPS 10000000
#if !defined(NDEBUG)
    #define MINCPS 1
#else
    #define MINCPS 500
#endif
#define MINCHUNKSLOOPPERIOD 40
#define MAXCHUNKSLOOPPERIOD 10000
#define MINCHUNKSLOOPCPU    10
#define MAXCHUNKSLOOPCPU    90

constexpr uint8_t kChunkHashBits = 22;
constexpr int32_t kChunkHashSize = (1 << kChunkHashBits);
constexpr uint32_t kChunkHashMask = kChunkHashSize - 1;
constexpr uint32_t chunkHashPos(uint64_t chunkid) {
	return static_cast<uint32_t>(chunkid) & kChunkHashMask;
}

#define CHECKSUMSEED 78765491511151883ULL

#ifndef METARESTORE

static uint32_t gRedundancyLevel;
static uint64_t gEndangeredChunksServingLimit;
static uint64_t gEndangeredChunksMaxCapacity;
static uint64_t gDisconnectedCounter = 0;
inline LinearAssignmentCache gLinearAssignmentCache;
inline bool gUseLinearAssignmentOptimizer;
static bool gUseChunkserverSideChunkLock;

struct ChunkPart {
	enum {
		INVALID =
		    0,  /*!< Wrong version / or got info from chunkserver (IO error etc.)  ->  to delete. */
		DEL,    /*!< Deletion in progress. */
		BUSY,   /*!< Operation in progress. */
		VALID,  /*!< Ok. */
		TDBUSY, /*!< To delete + BUSY. */
		TDVALID /*!< Want to be deleted. */
	};

	uint32_t version;   /*!< Part version. */
	ChunkPartType type; /*!< Part type. */
	uint16_t csid : 13; /*!< Chunkserver id. */
	uint16_t state : 3; /*!< Chunk part state. */
	bool beingWritten = false;  /*!< Indicates if the chunk part is being written, i.e locked. */

	ChunkPart() : version(0), type(), csid(0), state(INVALID) {}

	ChunkPart(uint16_t part_csid, int part_state, uint32_t part_version,
	          const ChunkPartType &part_type)
	    : version(part_version), type(part_type), csid(part_csid), state(part_state) {}

	bool is_busy() const {
		return state == BUSY || state == TDBUSY;
	}

	bool is_valid() const {
		return state != INVALID && state != DEL;
	}

	bool is_todel() const {
		return state == TDVALID || state == TDBUSY;
	}

	bool is_being_written() const { return beingWritten; }

	void mark_being_written() { beingWritten = true; }

	void unmark_being_written() { beingWritten = false; }

	void mark_busy() {
		switch (state) {
		case VALID:
			state = BUSY;
			break;
		case TDVALID:
			state = TDBUSY;
			break;
		default:
			sassert(!"ChunkPartInfo::mark_busy(): wrong state");
		}
	}

	void unmark_busy() {
		switch (state) {
		case BUSY:
			state = VALID;
			break;
		case TDBUSY:
			state = TDVALID;
			break;
		default:
			sassert(!"ChunkPartInfo::unmark_busy(): wrong state");
		}
	}

	void mark_todel() {
		switch (state) {
		case VALID:
			state = TDVALID;
			break;
		case BUSY:
			state = TDBUSY;
			break;
		default:
			sassert(!"ChunkPartInfo::mark_todel(): wrong state");
		}
	}

	void unmark_todel() {
		switch (state) {
		case TDVALID:
			state = VALID;
			break;
		case TDBUSY:
			state = BUSY;
			break;
		default:
			sassert(!"ChunkPartInfo::unmark_todel(): wrong state");
		}
	}

	matocsserventry *server() const {
		assert(csdb_find(csid));
		assert(csdb_find(csid)->eptr);
		return csdb_find(csid)->eptr;
	}
};

static void*                         gChunkLoopEventHandle = NULL;

static uint32_t gOperationsDelayDisconnect = 3600;
static uint32_t gOperationsDelayInit = 300;

static uint32_t MaxWriteRepl;
static uint32_t MaxReadRepl;
static uint32_t MaxDelSoftLimit;
static uint32_t MaxDelHardLimit;
static double   TmpMaxDelFrac;
static uint32_t TmpMaxDel;
static uint32_t HashSteps;
static uint32_t HashCPS;
static uint32_t ChunksLoopPeriod;
static uint32_t ChunksLoopTimeout;
static double   gAcceptableDifference;
static bool     RebalancingBetweenLabels = false;

static uint32_t jobsnorepbefore;

constexpr uint32_t kStartupGracePeriodSeconds = 60;
static uint32_t starttime;
#endif // METARESTORE

class Chunk {
	static constexpr int kMaxStatCount = 15;
	static_assert(CHUNK_MATRIX_SIZE <= kMaxStatCount, "stats matrix size too big for internal stats storage");
	static_assert(ChunksAvailabilityState::kStateCount <= 3, "not enough space for chunk state");

public:
	/// @brief Current operation being performed on the chunk by chunkservers
	enum ChunkOperation : uint8_t {
		NONE,
		CREATE,
		SET_VERSION,
		DUPLICATE,
		TRUNCATE,
		DUPTRUNC,
		LOCK
	};

	uint64_t chunkid;
	uint64_t checksum;
#ifndef METARESTORE
	compact_vector<ChunkPart> parts;
	/// Generation of the durable write ownership round open on this chunk, threaded into the
	/// fenced version command so a chunkserver can tell one round from the next. Zero when no
	/// round set it. In memory only; the durable identity is the CHUNK_OP_ record. Costs eight
	/// bytes per chunk; a transient side table would spare resting chunks that, and is left for
	/// later.
	uint64_t operationGeneration = 0;
#endif
private: // public/private sections are mixed here to make the struct as small as possible
	ChunkGoalCounters goalCounters_;
public:
	uint32_t version;
	uint32_t lockid;
	uint32_t lockedto;
#ifndef METARESTORE
	uint8_t inEndangeredQueue:1;
	/// @brief Indicates whether the chunk version needs to be increased. This may be needed due to
	///        a variety of reasons, e.g., disconnected parts, repair and replicate operations, etc.
	///        The use of this flag happens at the beginning of the next write operation, forcing
	///        the chunk version to be increased.
	uint8_t needVersionIncrease : 1;
	/// @brief Indicates whether the chunk operation or write was interrupted. This may happen when
	///        chunkserver disconnects during the operation or sends an error status. The use
	///        of this flag happens at the end of the current operation, forcing the chunk version
	///        to be increased to avoid inconsistencies.
	uint8_t interrupted : 1;
	/// @brief Current operation being performed on the chunk by chunkservers
	uint8_t operation : 3;

private:
	uint8_t allAvailabilityState_:2;
	uint8_t copiesInStats_:4;
	uint8_t allMissingParts_:4;
	uint8_t allRedundantParts_:4;
	uint8_t allFullCopies_:4;
#endif

public:
#ifndef METARESTORE
	static ChunksAvailabilityState allChunksAvailability;
	static ChunksReplicationState allChunksReplicationState;
	static uint64_t count;
	static uint64_t allFullChunkCopies[CHUNK_MATRIX_SIZE][CHUNK_MATRIX_SIZE];
	static std::deque<Chunk *> endangeredChunks;
	static GoalCache goalCache;
#endif

	void clear() {
		goalCounters_.clear();
		chunkid = 0;
		version = 0;
		lockid = 0;
		lockedto = 0;
		checksum = 0;
#ifndef METARESTORE
		inEndangeredQueue = 0;
		needVersionIncrease = 1;
		interrupted = 0;
		operation = Chunk::NONE;
		parts.clear();
		allMissingParts_ = 0;
		allRedundantParts_= 0;
		allFullCopies_ = 0;
		allAvailabilityState_ = ChunksAvailabilityState::kSafe;
		copiesInStats_ = 0;
		count++;
		updateStats(false);
#endif
	}

	// Highest id of the chunk's goal
	// This function is preserved only for backward compatibility of metadata checksums
	// and shouldn't be used anywhere else.
	uint8_t highestIdGoal() const {
		return goalCounters_.highestIdGoal();
	}

	// Number of files this chunk belongs to
	uint32_t fileCount() const {
		return goalCounters_.fileCount();
	}

	// Per-goal reference counts, for persisting the chunk record to a KV backend
	const ChunkGoalCounters &goalCounters() const { return goalCounters_; }

	// Called when this chunk becomes a part of a file with the given goal
	void addFileWithGoal(uint8_t goal) {
#ifndef METARESTORE
		removeFromStats();
#endif
		goalCounters_.addFile(goal);
#ifndef METARESTORE
		updateStats(false);
#endif
	}

	// Called when a file that this chunk belongs to is removed
	void removeFileWithGoal(uint8_t goal) {
#ifndef METARESTORE
		removeFromStats();
#endif
		goalCounters_.removeFile(goal);
#ifndef METARESTORE
		updateStats(false);
#endif
	}

	// Called when a file that this chunk belongs to changes goal
	void changeFileGoal(uint8_t prevGoal, uint8_t newGoal) {
#ifndef METARESTORE
		removeFromStats();
#endif
		goalCounters_.changeFileGoal(prevGoal, newGoal);
#ifndef METARESTORE
		updateStats(false);
#endif
	}

#ifndef METARESTORE
	Goal getGoal() {
		// Do not search for empty goalCounters in cache
		if (goalCounters_.size() == 0) {
			return Goal();
		}

		auto it = goalCache.find(goalCounters_);
		if (it != goalCache.end()) {
			return it->second;
		}

		Goal result;
		int prev_goal = -1;
		for (auto counter : goalCounters_) {
			const Goal &goal = gFSOperations->getGoalDefinition(counter.goal);
			if (prev_goal != (int)counter.goal) {
				result.mergeIn(goal);
				prev_goal = counter.goal;
			}
		}

		goalCache.insert(goalCounters_, result);
		return result;
	}

	// This method should be called when a chunk is removed
	void freeStats() {
		count--;
		removeFromStats();
	}

	// Updates statistics of all chunks
	void updateStats(bool remove_from_stats = true) {
		int oldAllMissingParts = allMissingParts_;

		if (remove_from_stats) {
			removeFromStats();
		}

		Goal g = getGoal();

		ChunkCopiesCalculator all(g);

		for (const auto &part : parts) {
			if (!part.is_valid()) {
				continue;
			}
			all.addPart(part.type, csdb_find(part.csid)->label);
		}

		all.optimize(gUseLinearAssignmentOptimizer, &gLinearAssignmentCache);

		allFullCopies_ = std::min(kMaxStatCount, all.getFullCopiesCount());
		allAvailabilityState_ = all.getState();
		allMissingParts_ = std::min(kMaxStatCount, all.countPartsToRecover());
		allRedundantParts_ = std::min(kMaxStatCount, all.countPartsToRemove());
		copiesInStats_ = std::min(kMaxStatCount, ChunkCopiesCalculator::getFullCopiesCount(g));

		/* Enqueue a chunk as endangered only if:
		 * 1. Endangered chunks prioritization is on (limit > 0)
		 * 2. Limit of endangered chunks in queue is not reached
		 * 3. Chunk has more missing parts than it used to
		 * 4. Chunk is endangered
		 * 5. It is not already in queue
		 * By checking conditions below we assert no repetitions in endangered queue. */
		if (gEndangeredChunksServingLimit > 0
				&& endangeredChunks.size() < gEndangeredChunksMaxCapacity
				&& allMissingParts_ > oldAllMissingParts
				&& allAvailabilityState_ == ChunksAvailabilityState::kEndangered
				&& !inEndangeredQueue) {
			inEndangeredQueue = 1;
			endangeredChunks.push_back(this);
		}

		addToStats();
	}

	bool isSafe() const {
		return allAvailabilityState_ == ChunksAvailabilityState::kSafe;
	}

	bool isEndangered() const {
		return allAvailabilityState_ == ChunksAvailabilityState::kEndangered;
	}

	bool isLost() const {
		return allAvailabilityState_ == ChunksAvailabilityState::kLost;
	}

	bool isWritable() {
		return !isLost();
	}

	int countMissingParts() const {
		return allMissingParts_;
	}

	bool countRedundantParts() const {
		return allRedundantParts_;
	}

	uint8_t getFullCopiesCount() const {
		return allFullCopies_;
	}

	bool isLocked() const {
		/// Chunk is considered locked if the current time is less than lockedto (not unlocked by
		/// client) or if it is being written and lockedto is 0: which means client has unlocked the
		/// chunk but the chunk parts are still being written.
		if (lockedto >= eventloop_time()) { return true; }

		bool isChunkBeingWritten = std::any_of(
		    parts.begin(), parts.end(),
		    [](const ChunkPart &part) { return part.is_being_written() && part.is_valid(); });
		return isChunkBeingWritten && lockedto == 0;
	}

	void markCopyAsHavingWrongVersion(ChunkPart &part) {
		part.state = ChunkPart::INVALID;
		updateStats();
	}

	void invalidateCopy(ChunkPart &part) {
		part.state = ChunkPart::INVALID;
		part.beingWritten = false;
		part.version = 0;
		updateStats();
	}

	void deleteCopy(ChunkPart &part) {
		part.state = ChunkPart::DEL;
		updateStats();
	}

private:
	ChunksAvailabilityState::State allCopiesState() const {
		return static_cast<ChunksAvailabilityState::State>(allAvailabilityState_);
	}

	void removeFromStats() {
		int prev_goal = -1;
		for (const auto& counter : goalCounters_) {
			if (prev_goal == (int)counter.goal) {
				continue;
			}
			prev_goal = counter.goal;
			allChunksAvailability.removeChunk(counter.goal, allCopiesState());
			allChunksReplicationState.removeChunk(counter.goal, allMissingParts_, allRedundantParts_);
		}

		uint8_t limitedGoal = std::min<uint8_t>(CHUNK_MATRIX_SIZE - 1, copiesInStats_);
		uint8_t limitedAll = std::min<uint8_t>(CHUNK_MATRIX_SIZE - 1, allFullCopies_);
		allFullChunkCopies[limitedGoal][limitedAll]--;
	}

	void addToStats() {
		int prev_goal = -1;
		for (const auto& counter : goalCounters_) {
			if (prev_goal == (int)counter.goal) {
				continue;
			}
			prev_goal = counter.goal;
			allChunksAvailability.addChunk(counter.goal, allCopiesState());
			allChunksReplicationState.addChunk(counter.goal, allMissingParts_, allRedundantParts_);
		}

		uint8_t limitedGoal = std::min<uint8_t>(CHUNK_MATRIX_SIZE - 1, copiesInStats_);
		uint8_t limitedAll = std::min<uint8_t>(CHUNK_MATRIX_SIZE - 1, allFullCopies_);
		allFullChunkCopies[limitedGoal][limitedAll]++;
	}
#endif
};

constexpr int Chunk::kMaxStatCount;

#ifndef METARESTORE

std::deque<Chunk *> Chunk::endangeredChunks;
GoalCache Chunk::goalCache(10000);
ChunksAvailabilityState Chunk::allChunksAvailability;
ChunksReplicationState Chunk::allChunksReplicationState;
uint64_t Chunk::count;
uint64_t Chunk::allFullChunkCopies[CHUNK_MATRIX_SIZE][CHUNK_MATRIX_SIZE];
#endif

#define CHUNK_BUCKET_SIZE 20000
struct ChunkBucket {
	std::array<Chunk, CHUNK_BUCKET_SIZE> bucket;
	uint32_t firstAvailableChunk{};
};

namespace {
struct ChunksMetadata {
	// chunks
	std::vector<std::unique_ptr<ChunkBucket>> chunkBuckets;
	std::vector<Chunk *> availableChunks;
	compact_vector<Chunk *> chunkhash[kChunkHashSize]{};
	uint64_t lastchunkid{};
	Chunk *lastchunkptr{};

	// other chunks metadata information
	uint64_t chunksChecksum{};
	uint64_t chunksChecksumRecalculated{};
	uint32_t checksumRecalculationPosition{};

	ChunksMetadata() = default;

	~ChunksMetadata() = default;

	/// Gets the id that will be assigned to the next created chunk without incrementing it.
	static uint64_t getNextChunkId() {
		return gChunkIdGenerator->getCurrentId();
	}

	/// Gets the id that will be assigned to the next created chunk and increments it.
	static uint64_t getAndIncrementNextChunkId() {
		return gChunkIdGenerator->getNextId();
	}

	/// Sets the id that will be assigned to the next created chunk.
	/// Useful when loading metadata from disk and on special occasions.
	static bool setNextChunkId(uint64_t nextId) {
		return gChunkIdGenerator->setCurrentId(nextId);
	}
};
} // anonymous namespace

static ChunksMetadata *gChunksMetadata;

#define LOCKTIMEOUT 120
#define UNUSED_DELETE_TIMEOUT (86400*7)

#ifndef METARESTORE

// Outer iteration index over chunkhash
static uint32_t gCurrentBucketInZombieLoop = 0;
// Index within the current bucket's vector
static size_t gCurrentChunkInZombieLoopIndex = 0;

class ReplicationDelayInfo {
public:
	ReplicationDelayInfo()
		: disconnectedServers_(0),
		  timestamp_() {}

	void serverDisconnected() {
		refresh();
		++disconnectedServers_;
		timestamp_ = eventloop_time() + gOperationsDelayDisconnect;
	}

	void serverConnected() {
		refresh();
		if (disconnectedServers_ > 0) {
			--disconnectedServers_;
		}
	}

	bool replicationAllowed(int missingCopies) {
		refresh();
		return missingCopies > disconnectedServers_;
	}

private:
	uint16_t disconnectedServers_;
	uint32_t timestamp_;

	void refresh() {
		if (eventloop_time() > timestamp_) {
			disconnectedServers_ = 0;
		}
	}

};

/*
 * Information about recently disconnected and connected servers
 * necessary for replication to unlabeled servers.
 */
static ReplicationDelayInfo replicationDelayInfoForAll;

/*
 * Information about recently disconnected and connected servers
 * necessary for replication to servers with specified label.
 */
static std::unordered_map<MediaLabel, ReplicationDelayInfo, MediaLabel::hash> replicationDelayInfoForLabel;

struct job_info {
	uint32_t del_invalid;
	uint32_t del_unused;
	uint32_t del_diskclean;
	uint32_t del_overgoal;
	uint32_t copy_undergoal;
};

struct loop_info {
	job_info done,notdone;
	uint32_t copy_rebalance;
};

static loop_info chunksinfo = {{0,0,0,0,0},{0,0,0,0,0},0};
static uint32_t chunksinfo_loopstart=0,chunksinfo_loopend=0;

// DEPRECATED: Use prometheus metrics instead
static uint32_t stats_deletions=0;
static uint32_t stats_replications=0;

void chunk_stats(uint32_t *del,uint32_t *repl) {
	*del = stats_deletions;
	*repl = stats_replications;
	stats_deletions = 0;
	stats_replications = 0;
}

#endif // ! METARESTORE

static uint64_t chunk_checksum(const Chunk *c) {
	if (c == nullptr || c->fileCount() == 0) {
		// We treat chunks with fileCount=0 as non-existent, so that we don't have to notify shadow
		// masters when we remove them from our structures.
		return 0;
	}
	uint64_t checksum = 64517419147637ULL;
	// Only highest id goal is taken into checksum for compatibility reasons
	hashCombine(checksum, c->chunkid, c->version, c->lockedto, c->highestIdGoal(), c->fileCount());

	return checksum;
}

static void chunk_checksum_add_to_background(Chunk *ch) {
	if (!ch) {
		return;
	}
	removeFromChecksum(gChunksMetadata->chunksChecksum, ch->checksum);
	ch->checksum = chunk_checksum(ch);
	addToChecksum(gChunksMetadata->chunksChecksumRecalculated, ch->checksum);
	addToChecksum(gChunksMetadata->chunksChecksum, ch->checksum);
}

static void chunk_update_checksum(Chunk *ch, bool isMetadataLoading = false) {
	if (!ch) {
		return;
	}
	if (chunkHashPos(ch->chunkid) <
	    gChunksMetadata->checksumRecalculationPosition) {
		removeFromChecksum(gChunksMetadata->chunksChecksumRecalculated, ch->checksum);
	}
	removeFromChecksum(gChunksMetadata->chunksChecksum, ch->checksum);
	ch->checksum = chunk_checksum(ch);
	if (chunkHashPos(ch->chunkid) < gChunksMetadata->checksumRecalculationPosition) {
		if (!isMetadataLoading) {
			safs::log_trace("master.fs.checksum.changing_recalculated_chunk");
		}
		addToChecksum(gChunksMetadata->chunksChecksumRecalculated, ch->checksum);
	} else if (!isMetadataLoading) {
		safs::log_trace("master.fs.checksum.changing_not_recalculated_chunk");
	}
	addToChecksum(gChunksMetadata->chunksChecksum, ch->checksum);
}

/*!
 * \brief update chunks checksum in the background
 * \param limit for processed chunks per function call
 * \return info whether all chunks were updated or not.
 */

ChecksumRecalculationStatus chunks_update_checksum_a_bit(uint32_t speedLimit) {
	if (gChunksMetadata->checksumRecalculationPosition == 0) {
		gChunksMetadata->chunksChecksumRecalculated = CHECKSUMSEED;
	}
	uint32_t recalculated = 0;
	while (gChunksMetadata->checksumRecalculationPosition < kChunkHashSize) {
		for (Chunk *chunk :
		     gChunksMetadata->chunkhash[gChunksMetadata->checksumRecalculationPosition]) {
			chunk_checksum_add_to_background(chunk);
			++recalculated;
		}
		++gChunksMetadata->checksumRecalculationPosition;
		if (recalculated >= speedLimit) {
			return ChecksumRecalculationStatus::kInProgress;
		}
	}
	// Recalculating chunks checksum finished
	gChunksMetadata->checksumRecalculationPosition = 0;
	if (gChunksMetadata->chunksChecksum != gChunksMetadata->chunksChecksumRecalculated) {
		safs_pretty_syslog(LOG_WARNING,"Chunks metadata checksum mismatch found, replacing with a new value.");
		safs_silent_syslog(LOG_DEBUG, "master.fs.checksum.mismatch");
		gChunksMetadata->chunksChecksum = gChunksMetadata->chunksChecksumRecalculated;
	}
	return ChecksumRecalculationStatus::kDone;
}

static void chunk_recalculate_checksum() {
	gChunksMetadata->chunksChecksum = CHECKSUMSEED;
	for (int i = 0; i < kChunkHashSize; ++i) {
		for (Chunk *chunk : gChunksMetadata->chunkhash[i]) {
			chunk->checksum = chunk_checksum(chunk);
			addToChecksum(gChunksMetadata->chunksChecksum, chunk->checksum);
		}
	}
}

static inline void emit_chunk_changed(const Chunk *c) {
	if (!gChunkChangedSignal.empty()) {
		gChunkChangedSignal.emit(c->chunkid, c->version, c->lockedto, c->lockid);
	}
}

uint64_t chunk_checksum(ChecksumMode mode) {
	uint64_t checksum = 46586918175221;
	addToChecksum(checksum, ChunksMetadata::getNextChunkId());
	if (mode == ChecksumMode::kForceRecalculate) {
		chunk_recalculate_checksum();
	}
	addToChecksum(checksum, gChunksMetadata->chunksChecksum);
	return checksum;
}

static inline Chunk *chunk_malloc() {
	Chunk *ret;
	if (!gChunksMetadata->availableChunks.empty()) {
		ret = gChunksMetadata->availableChunks.back();
		gChunksMetadata->availableChunks.pop_back();
		ret->clear();
		return ret;
	}

	if (gChunksMetadata->chunkBuckets.empty() ||
	    gChunksMetadata->chunkBuckets.back()->firstAvailableChunk == CHUNK_BUCKET_SIZE) {
		auto chunkBucket = std::make_unique<ChunkBucket>();
		chunkBucket->firstAvailableChunk = 0;
		gChunksMetadata->chunkBuckets.push_back(std::move(chunkBucket));
	}

	auto &currentBucket = gChunksMetadata->chunkBuckets.back();
	ret = &(currentBucket->bucket[currentBucket->firstAvailableChunk]);
	currentBucket->firstAvailableChunk++;
	ret->clear();
	return ret;
}

#ifndef METARESTORE
static inline void chunk_free(Chunk *p) {
	gChunksMetadata->availableChunks.push_back(p);
	p->inEndangeredQueue = 0;
}
#endif /* METARESTORE */

Chunk *chunk_new(uint64_t chunkid, uint32_t chunkversion) {
	uint32_t chunkpos = chunkHashPos(chunkid);
	Chunk *newchunk;
	newchunk = chunk_malloc();
	gChunksMetadata->chunkhash[chunkpos].push_back(newchunk);
	newchunk->chunkid = chunkid;
	newchunk->version = chunkversion;
	gChunksMetadata->lastchunkid = chunkid;
	gChunksMetadata->lastchunkptr = newchunk;
	chunk_update_checksum(newchunk);
	return newchunk;
}

#ifndef METARESTORE
void chunk_increase_version_operation(Chunk *targetChunk, bool needsLocking);

void chunk_emergency_increase_version(Chunk *c) {
	chunk_increase_version_operation(c, false);
	chunk_update_checksum(c);

	// The new version is already live by the time we get here: memory carries it and the
	// setversion commands are on the wire, so there is nothing to roll back and the durable
	// record has to be made to agree. A commit conflict at this point is ordinary rather than
	// exceptional, because a failing write is exactly what makes a chunkserver report on the
	// same chunk, and that report's transaction writes the same keys.
	//
	// Treating the conflict as terminal strands the chunk for good. Memory stays a version
	// ahead of the record, and nothing can close the gap from either side: the part set cannot
	// be republished, because publication only accepts a report whose version matches the
	// record, and no write can proceed, because the session gate refuses a set that does not
	// match the version memory holds. A retryable conflict must never be allowed to mean that.
	//
	// Replaying is safe because the body persists the version memory already holds instead of
	// incrementing whatever it reads, so a second attempt writes the same value. The harness
	// does not replay an unknown outcome, which matters here: that attempt may have applied,
	// and a repeated INCVERSION would raise the version twice on anything replaying the log.
	const uint8_t status =
	    matoclserv_commit_op_with_retry([c](FilesystemOperationContext &fsOpContext) -> uint8_t {
		    gFSOperations->increaseChunkVersion(fsOpContext, c->chunkid);
		    return SAUNAFS_STATUS_OK;
	    });
	if (status != SAUNAFS_STATUS_OK) {
		safs::log_critical("{}: Failed to commit transaction for increasing version of chunk {}.",
		                   __func__, c->chunkid);
	}

	emit_chunk_changed(c);
}

/// @brief This function should be called when an operation on a chunk fails (chunk not writable)
/// and the client should be notified about it after all the operation replies are received.
/// @param c The chunk on which the operation was performed.
void chunk_finalize_failed_operation(Chunk *c) {
	if (c->operation == Chunk::CREATE) {
		matoclserv_chunk_status(c->chunkid, SAUNAFS_ERROR_CHUNKLOST, true);
	} else if (c->operation == Chunk::LOCK) {
		// The client tried to lock the chunk, but it is not writable anymore. We need to notify the
		// client about it, so that it can retry the operation and get a chance to lock the chunk
		// when it becomes writable again.
		matoclserv_chunk_status(c->chunkid, SAUNAFS_ERROR_CHUNKLOST);
	} else {
		matoclserv_chunk_status(c->chunkid, SAUNAFS_ERROR_NOTDONE);
	}
	c->operation = Chunk::NONE;

	for (auto &part : c->parts) {
		if (!part.is_valid() || part.is_busy() || part.is_todel() || !part.is_being_written()) {
			continue;
		}
		// No valid parts should be busy, but some of them may be marked as being written if the
		// failure happened when expecting writes to start right away. We need to unlock the parts
		// in the chunkserver side and mark them as not being written to avoid inconsistencies.
		part.unmark_being_written();
		matocsserv_send_chunkunlock(part.server(), c->chunkid, part.type);
	}
}

void chunk_handle_disconnected_copies(Chunk *c) {
	bool any_lost_copy_being_written = false;
	auto it = std::remove_if(c->parts.begin(), c->parts.end(), [&](const ChunkPart &part) {
		if (csdb_find(part.csid)->eptr == nullptr) {
			if (part.is_being_written()) { any_lost_copy_being_written = true; }
			return true;
		}
		return false;
	});
	bool lost_copy_found = it != c->parts.end();

	if (lost_copy_found) {
		c->parts.erase(it, c->parts.end());
		c->needVersionIncrease = 1;
		c->updateStats();
	}

	if (any_lost_copy_being_written) {
		// The lost copy while being written may lead to inconsistencies, so we
		// mark the operation as interrupted to increase the chunk version later.
		c->interrupted = 1;
	}

	if (lost_copy_found && c->operation != Chunk::NONE) {
		bool any_copy_busy = std::any_of(c->parts.begin(), c->parts.end(),
		                                 [](const ChunkPart &part) { return part.is_busy(); });

		if (any_copy_busy) {
			// Wait for the remaining operation replies to arrive, but mark the operation as
			// interrupted to increase the chunk version later (when the other replies arrive).
			c->interrupted = 1;
		} else {
			if (c->isWritable()) {
				chunk_emergency_increase_version(c);
			} else {
				chunk_finalize_failed_operation(c);
			}
		}
	} else if (any_lost_copy_being_written) {
		// implies operation == NONE
		bool any_copy_being_written =
		    std::any_of(c->parts.begin(), c->parts.end(),
		                [](const ChunkPart &part) { return part.is_being_written(); });

		if (any_copy_being_written) {
			// Wait for remaining write replies to arrive, but mark the operation as interrupted
			// to increase the chunk version later (when the other replies arrive).
			c->interrupted = 1;
		} else if (c->interrupted) {
			if (c->isWritable()) {
				// If there was an interrupted write, we need to increase the version
				chunk_emergency_increase_version(c);
			} else {
				// If the chunk is not writable anymore, log the error
				safs::log_warn(
				    "{}: Chunk {} became not writable after losing copies during an operation.",
				    __func__, c->chunkid);
			}
		}
	}
}
#endif

Chunk *chunk_find(uint64_t chunkid) {
	uint32_t chunkpos = chunkHashPos(chunkid);
	if (gChunksMetadata->lastchunkid==chunkid) {
		return gChunksMetadata->lastchunkptr;
	}
	for (Chunk *chunk : gChunksMetadata->chunkhash[chunkpos]) {
		if (chunk->chunkid == chunkid) {
			gChunksMetadata->lastchunkid = chunkid;
			gChunksMetadata->lastchunkptr = chunk;
#ifndef METARESTORE
			chunk_handle_disconnected_copies(chunk);
#endif // METARESTORE
			return chunk;
		}
	}
	return nullptr;
}

#ifndef METARESTORE
void chunk_delete(Chunk *c) {
	if (gChunksMetadata->lastchunkptr==c) {
		gChunksMetadata->lastchunkid=0;
		gChunksMetadata->lastchunkptr=NULL;
	}
	c->freeStats();
	chunk_free(c);
}

uint32_t chunk_count(void) {
	return Chunk::count;
}

void chunk_info(uint32_t *allchunks,uint32_t *allcopies,uint32_t *regularvalidcopies) {
	*allchunks = Chunk::count;
	*allcopies = 0;
	*regularvalidcopies = 0;
	for (int actualCopies = 1; actualCopies < CHUNK_MATRIX_SIZE; actualCopies++) {
		uint32_t ag = 0;
		for (int expectedCopies = 0; expectedCopies < CHUNK_MATRIX_SIZE; expectedCopies++) {
			ag += Chunk::allFullChunkCopies[expectedCopies][actualCopies];
		}
		*allcopies += ag * actualCopies;
	}
}

uint32_t chunk_get_missing_count(void) {
	uint32_t res = 0;
	for (uint8_t goal = GoalId::kMin; goal <= GoalId::kMax; ++goal) {
		res += Chunk::allChunksAvailability.lostChunks(goal);
	}
	return res;
}

void chunk_store_chunkcounters(uint8_t *buff,uint8_t matrixid) {
	if (matrixid == MATRIX_ALL_COPIES) {
		for (int i = 0; i < CHUNK_MATRIX_SIZE; i++) {
			for (int j = 0; j < CHUNK_MATRIX_SIZE; j++) {
				// TODO(Guillex): possible truncation if the chunks number is greater than 2^32
				put32bit(&buff, static_cast<uint32_t>(Chunk::allFullChunkCopies[i][j]));
			}
		}
	} else {
		memset(buff, 0, CHUNK_MATRIX_SIZE * CHUNK_MATRIX_SIZE * sizeof(uint32_t));
	}
}
#endif

/// updates chunk's goal after a file goal has been changed
int chunk_change_file(uint64_t chunkid,uint8_t prevgoal,uint8_t newgoal) {
	Chunk *c;
	if (prevgoal==newgoal) {
		return SAUNAFS_STATUS_OK;
	}
	c = chunk_find(chunkid);
	if (c==NULL) {
		safs::log_err("chunk_change_file: could not find chunkid {}", chunkid);
		return SAUNAFS_ERROR_NOCHUNK;
	}
	try {
		c->changeFileGoal(prevgoal, newgoal);
	} catch (Exception& ex) {
		safs_pretty_syslog(LOG_WARNING, "chunk_change_file: %s", ex.what());
		return SAUNAFS_ERROR_CHUNKLOST;
	}
	chunk_update_checksum(c);
	return SAUNAFS_STATUS_OK;
}

/// updates chunk's goal after a file with goal `goal' has been removed
static inline int chunk_delete_file_int(Chunk *c, uint8_t goal) {
	try {
		c->removeFileWithGoal(goal);
	} catch (Exception& ex) {
		safs_pretty_syslog(LOG_WARNING, "chunk_delete_file_int: %s", ex.what());
		return SAUNAFS_ERROR_CHUNKLOST;
	}
	chunk_update_checksum(c);
	return SAUNAFS_STATUS_OK;
}

/// updates chunk's goal after a file with goal `goal' has been added
static inline int chunk_add_file_int(Chunk *c, uint8_t goal, bool isMetadataLoading = false) {
	try {
		c->addFileWithGoal(goal);
	} catch (Exception& ex) {
		safs_pretty_syslog(LOG_WARNING, "chunk_add_file_int: %s", ex.what());
		return SAUNAFS_ERROR_CHUNKLOST;
	}
	chunk_update_checksum(c, isMetadataLoading);
	return SAUNAFS_STATUS_OK;
}

int chunk_delete_file(uint64_t chunkid,uint8_t goal) {
	Chunk *c;
	c = chunk_find(chunkid);
	if (c==NULL) {
		safs::log_err("chunk_delete_file: could not find chunkid {}", chunkid);
		return SAUNAFS_ERROR_NOCHUNK;
	}
	return chunk_delete_file_int(c,goal);
}

int chunk_add_file(uint64_t chunkid, uint8_t goal, bool isMetadataLoading) {
	Chunk *c;
	c = chunk_find(chunkid);
	if (c==NULL) {
		safs::log_err("chunk_add_file: could not find chunkid {}", chunkid);
		return SAUNAFS_ERROR_NOCHUNK;
	}
	return chunk_add_file_int(c, goal, isMetadataLoading);
}

bool chunk_get_version_and_goal_counters(uint64_t chunkid, uint32_t &version,
                                         ChunkGoalCounters &counters) {
	Chunk *c = chunk_find(chunkid);
	if (c == nullptr) { return false; }
	version = c->version;
	counters = c->goalCounters();
	return true;
}

bool chunk_get_lock_state(uint64_t chunkid, uint32_t &lockid, uint32_t &lockedto) {
	Chunk *c = chunk_find(chunkid);
	if (c == nullptr) { return false; }
	lockid = c->lockid;
	lockedto = c->lockedto;
	return true;
}

void chunk_set_operation_generation(uint64_t chunkid, uint64_t generation) {
#ifndef METARESTORE
	Chunk *c = chunk_find(chunkid);
	if (c != nullptr) { c->operationGeneration = generation; }
#else
	(void)chunkid;
	(void)generation;
#endif
}

uint64_t chunk_operation_generation(uint64_t chunkid) {
#ifndef METARESTORE
	Chunk *c = chunk_find(chunkid);
	return c != nullptr ? c->operationGeneration : 0;
#else
	(void)chunkid;
	return 0;
#endif
}

bool chunk_exists(uint64_t chunkid) { return chunk_find(chunkid) != nullptr; }

void chunk_create_with_goal_counters(uint64_t chunkid, uint32_t version,
                                     const std::vector<ChunkGoalCounters::GoalCounter> &goals,
                                     uint32_t lockid, uint32_t lockedto) {
	if (chunk_find(chunkid) != nullptr) { return; }
	Chunk *c = chunk_new(chunkid, version);
	for (const auto &counter : goals) {
		for (uint32_t i = 0; i < counter.count; ++i) { c->addFileWithGoal(counter.goal); }
	}
	c->lockid = lockid;
	c->lockedto = lockedto;
	chunk_update_checksum(c);
}

int chunk_can_unlock(uint64_t chunkid, uint32_t lockid) {
	Chunk *c;
	c = chunk_find(chunkid);
	if (c==NULL) {
		safs::log_err("chunk_can_unlock: could not find chunkid {}", chunkid);
		return SAUNAFS_ERROR_NOCHUNK;
	}
	if (lockid == 0) {
		// lockid == 0 -> force unlock
		return SAUNAFS_STATUS_OK;
	}
	// We will let client unlock the chunk even if c->lockedto < eventloop_time()
	// if he provides lockId that was used to lock the chunk -- this means that nobody
	// else used this chunk since it was locked (operations like truncate or replicate
	// would remove such a stale lock before modifying the chunk)
	if (c->lockid == lockid) {
		return SAUNAFS_STATUS_OK;
	} else if (c->lockedto == 0) {
		return SAUNAFS_ERROR_NOTLOCKED;
	}
	// Case lockid != c->lockid
	return SAUNAFS_ERROR_WRONGLOCKID;
}

int chunk_unlock(uint64_t chunkid) {
	Chunk *c;
	c = chunk_find(chunkid);
	if (c==NULL) {
		safs::log_err("chunk_unlock: could not find chunkid {}", chunkid);
		return SAUNAFS_ERROR_NOCHUNK;
	}
	// Don't remove lockid to safely accept retransmission of FUSE_CHUNK_UNLOCK message
	c->lockedto = 0;
	chunk_update_checksum(c);
	emit_chunk_changed(c);

#ifndef METARESTORE
	if (!c->isLocked()) {
		// If the chunk is not locked anymore, we can try to send notices about the operation
		// status to clients waiting for the lock release.
		matoclserv_notify_unlock_list(chunkid);
	}
#endif
	return SAUNAFS_STATUS_OK;
}

bool should_increase_chunk_version_on_modification(uint8_t operation) {
	return operation == Chunk::CREATE || operation == Chunk::SET_VERSION ||
	       operation == Chunk::TRUNCATE;
}

#ifndef METARESTORE

int chunk_invalidate_goal_cache(){
	Chunk::goalCache.clear();
	return SAUNAFS_STATUS_OK;
}

bool chunk_has_only_invalid_copies(uint64_t chunkid) {
	if (chunkid == 0) {
		return false;
	}
	Chunk *c = chunk_find(chunkid);
	if (c == NULL || !c->isLost()) {
		return false;
	}
	// Chunk is lost, so it can only have INVALID or DEL copies.
	// Return true it there is at least one INVALID.
	return std::any_of(c->parts.begin(), c->parts.end(), [](const ChunkPart& part) {
		return part.state == ChunkPart::INVALID;
	});
}

int chunk_get_fullcopies(uint64_t chunkid,uint8_t *vcopies) {
	Chunk *c;
	*vcopies = 0;
	c = chunk_find(chunkid);
	if (c==NULL) {
		safs::log_err("chunk_get_fullcopies: could not find chunkid {}", chunkid);
		return SAUNAFS_ERROR_NOCHUNK;
	}

	*vcopies = c->getFullCopiesCount();

	return SAUNAFS_STATUS_OK;
}

int chunk_get_partstomodify(uint64_t chunkid, int &recover, int &remove) {
	Chunk *c;
	recover = 0;
	remove = 0;
	c = chunk_find(chunkid);
	if (c==NULL) {
		safs::log_err("chunk_get_partstomodify: could not find chunkid {}", chunkid);
		return SAUNAFS_ERROR_NOCHUNK;
	}
	recover = c->countMissingParts();
	remove = c->countRedundantParts();
	return SAUNAFS_STATUS_OK;
}

// Chunk operations

/// @brief Performs the chunk creation operation, which consists of creating a new chunk with
/// version 1, associating it with the given goal and sending create chunk messages to the provided
/// chunkservers. The parts in the chunk are marked as being written (it is expecteted that client
/// starts writing) if the corresponding chunkserver supports locking and the create chunk message
/// was sent with locking.
/// @param createdChunk A reference to a pointer where the created chunk will be stored.
/// @param goal The goal that will be associated with the created chunk.
/// @param serversWithChunkTypes The list of chunkservers to create the chunk on.
void chunk_create_operation(
    Chunk *&createdChunk, uint8_t goal,
    std::vector<std::pair<matocsserventry *, ChunkPartType>> &serversWithChunkTypes) {
	createdChunk = chunk_new(ChunksMetadata::getAndIncrementNextChunkId(), 1);
	createdChunk->interrupted = 0;
	createdChunk->operation = Chunk::CREATE;
	chunk_add_file_int(createdChunk, goal);

	for (const auto &server_with_type : serversWithChunkTypes) {
		createdChunk->parts.push_back(ChunkPart(matocsserv_get_csdb(server_with_type.first)->csid,
		                                        ChunkPart::BUSY, createdChunk->version,
		                                        server_with_type.second));
		bool sentChunkLock = false;
		matocsserv_send_createchunk(server_with_type.first, createdChunk->chunkid,
		                            server_with_type.second, createdChunk->version,
		                            gUseChunkserverSideChunkLock, sentChunkLock);

		if (sentChunkLock) { createdChunk->parts.back().mark_being_written(); }
		// If the chunk lock was not sent, it means that the chunkserver does not support locking,
		// so the part is not marked as being written.
	}

	createdChunk->updateStats();
}

/// @brief Performs the chunk version increase operation, which consists of increasing the chunk
/// version and sending setchunkversion messages to all valid parts in the chunk.
/// @param chunk A pointer to the chunk whose version will be increased.
/// @param needsLocking A boolean indicating whether locking is needed, i.e it is expected that
/// client will start writing right after the version increase.
void chunk_increase_version_operation(Chunk *chunk, bool needsLocking) {
	assert(chunk->isWritable());
	for (auto &part : chunk->parts) {
		if (part.is_valid()) {
			if (!part.is_busy()) { part.mark_busy(); }

			part.version = chunk->version + 1;
			// If part is already being written then we don't need to ask the chunkserver to lock
			// it again, and we can just increase the version.
			bool partNeedsLocking =
			    !part.is_being_written() && needsLocking && gUseChunkserverSideChunkLock;
			bool sentChunkLock = false;
			matocsserv_send_setchunkversion(part.server(), chunk->chunkid, chunk->version + 1,
			                                chunk->version, part.type, partNeedsLocking,
			                                sentChunkLock, chunk->operationGeneration);

			if (partNeedsLocking && sentChunkLock) { part.mark_being_written(); }
		}
	}

	chunk->interrupted = 0;
	chunk->operation = Chunk::SET_VERSION;
	chunk->version++;
}

/// @brief Performs the chunk lock operation, which consists of sending chunk lock messages to all
/// valid parts in the chunk and marking the parts as being written if the chunk lock message was
/// sent with locking.
/// @param chunk A pointer to the chunk to lock.
void chunk_lock_operation(Chunk *chunk) {
	bool mustWaitForReply = false;
	assert(chunk->isWritable());
	if (gUseChunkserverSideChunkLock) {
		for (auto &part : chunk->parts) {
			if (part.is_valid()) {
				if (part.is_busy()) { continue; }
				// No busy parts from now on

				bool sentChunkLock = false;
				matocsserv_send_chunklock(part.server(), chunk->chunkid, part.type,
				                          !part.is_being_written(), sentChunkLock);
				if (sentChunkLock) {
					part.mark_being_written();
					mustWaitForReply = true;
					part.mark_busy();
				}
			}
		}
	}

	chunk->interrupted = 0;
	if (mustWaitForReply) {
		// We'll need to wait for some replies
		chunk->operation = Chunk::LOCK;
	} else {
		// No need to wait, parts have been set to be written
		chunk->operation = Chunk::NONE;
	}
}

/// @brief Performs the chunk duplication operation, which consists of creating a new chunk with
/// version 1, associating it with the given goal, sending duplicate chunk messages to the
/// corresponding chunkservers and marking the parts in the new chunk as being written if the
/// corresponding duplicate chunk message was sent with locking. It is expected that client will
/// start writing to the new chunk right after the duplication.
/// @param originalChunk A pointer to the original chunk to duplicate.
/// @param goal The goal associated with the new chunk.
/// @param newChunk A reference to a pointer where the new chunk will be stored.
void chunk_duplicate_operation(Chunk *originalChunk, uint8_t goal, Chunk *&newChunk) {
	assert(originalChunk->isWritable());
	newChunk = chunk_new(ChunksMetadata::getAndIncrementNextChunkId(), 1);
	newChunk->interrupted = 0;
	newChunk->operation = Chunk::DUPLICATE;
	chunk_delete_file_int(originalChunk, goal);
	chunk_add_file_int(newChunk, goal);

	for (const auto &oldPart : originalChunk->parts) {
		if (oldPart.is_valid()) {
			newChunk->parts.push_back(
			    ChunkPart(oldPart.csid, ChunkPart::BUSY, newChunk->version, oldPart.type));

			bool sentChunkLock = false;
			matocsserv_send_duplicatechunk(oldPart.server(), newChunk->chunkid, newChunk->version,
			                               oldPart.type, originalChunk->chunkid,
			                               originalChunk->version, gUseChunkserverSideChunkLock,
			                               sentChunkLock);

			if (sentChunkLock) { newChunk->parts.back().mark_being_written(); }
		}
	}

	newChunk->updateStats();
}

/// @brief Performs the chunk truncate operation, which consists of increasing the chunk version and
/// sending truncate chunk messages to all valid parts in the chunk. It is not expected that client
/// will start writing right after the truncation.
/// @param chunk A pointer to the chunk to truncate.
/// @param length The new length of the chunk.
void chunk_truncate_operation(Chunk *chunk, uint32_t length) {
	assert(chunk->isWritable());
	for (auto &part : chunk->parts) {
		if (part.is_valid()) {
			if (!part.is_busy()) { part.mark_busy(); }
			part.version = chunk->version + 1;
			uint32_t chunkTypeLength =
			    slice_traits::chunkLengthToChunkPartLength(part.type, length);
			matocsserv_send_truncatechunk(part.server(), chunk->chunkid, part.type, chunkTypeLength,
			                              chunk->version + 1, chunk->version,
			                              chunk->operationGeneration);
		}
	}

	chunk->interrupted = 0;
	chunk->operation = Chunk::TRUNCATE;
	chunk->version++;
}

/// @brief Performs the chunk duplicate and truncate operation, which consists of creating a new
/// chunk with version 1, associating it with the given goal, sending duplicate and truncate chunk
/// messages to the corresponding chunkservers. It is not expected that client will start writing to
/// the new chunk right after the duplication and truncation.
/// @param originalChunk A pointer to the original chunk to duplicate and truncate.
/// @param goal The goal associated with the new chunk.
/// @param newChunk A reference to a pointer where the new chunk will be stored.
/// @param length The new length of the chunk.
void chunk_duplicate_and_truncate_operation(Chunk *originalChunk, uint8_t goal, Chunk *&newChunk,
                                            uint32_t length) {
	assert(originalChunk->isWritable());
	newChunk = chunk_new(ChunksMetadata::getAndIncrementNextChunkId(), 1);
	newChunk->interrupted = 0;
	newChunk->operation = Chunk::DUPTRUNC;
	chunk_delete_file_int(originalChunk, goal);
	chunk_add_file_int(newChunk, goal);

	for (const auto &oldPart : originalChunk->parts) {
		if (oldPart.is_valid()) {
			newChunk->parts.push_back(
			    ChunkPart(oldPart.csid, ChunkPart::BUSY, newChunk->version, oldPart.type));
			uint32_t chunkTypeLength =
			    slice_traits::chunkLengthToChunkPartLength(oldPart.type, length);
			matocsserv_send_duptruncchunk(oldPart.server(), newChunk->chunkid, newChunk->version,
			                              oldPart.type, originalChunk->chunkid,
			                              originalChunk->version, chunkTypeLength);
		}
	}

	newChunk->updateStats();
}

/// @brief Handles the chunk creation case of the chunk_multi_modify operation, which consists of
/// checking if the chunk can be created with the given goal and proceed if so.
/// @param quotaExceeded Whether the quota has been exceeded.
/// @param goal The goal for the chunk creation.
/// @param operation Pointer to the operation code.
/// @param newChunkId Pointer to the new chunk ID.
/// @param minServerVersion The minimum server version required.
/// @param createdChunk Pointer to the created chunk.
/// @return The status code of the operation.
uint8_t chunk_create(bool quotaExceeded, uint8_t goal, uint8_t *operation, uint64_t *newChunkId,
                     uint32_t minServerVersion, Chunk *&createdChunk) {
	// First check if quota is exceeded
	if (quotaExceeded) { return SAUNAFS_ERROR_QUOTA; }

	// Next check availability of chunkservers for the given goal
	uint16_t minServerCount = 0;
	auto serversWithChunkTypes =
	    matocsserv_getservers_for_new_chunk(goal, minServerCount, minServerVersion);
	if (serversWithChunkTypes.empty()) {
		uint16_t usableChunkservers, totalChunkservers;
		double minUsage, maxUsage;
		matocsserv_usagedifference(&minUsage, &maxUsage, &usableChunkservers, &totalChunkservers);

		if (usableChunkservers >= minServerCount &&
		    eventloop_time() > starttime + kStartupGracePeriodSeconds) {
			// if there are enough chunkservers and it's at least one minute after start then it
			// means that there is no space left
			return SAUNAFS_ERROR_NOSPACE;
		}

		return SAUNAFS_ERROR_NOCHUNKSERVERS;
	}

	// Check if the chunk would be safe to write with the current redundancy level
	ChunkCopiesCalculator calculator(gFSOperations->getGoalDefinition(goal));
	for (const auto &serverWithType : serversWithChunkTypes) {
		calculator.addPart(serverWithType.second, MediaLabel::kWildcard);
	}
	calculator.evalRedundancyLevel();
	if (!calculator.isSafeEnoughToWrite(gRedundancyLevel)) { return SAUNAFS_ERROR_NOCHUNKSERVERS; }

	// All checks passed, we can create the chunk
	chunk_create_operation(createdChunk, goal, serversWithChunkTypes);
	*operation = Chunk::CREATE;
	*newChunkId = createdChunk->chunkid;
	return SAUNAFS_STATUS_OK;
}

/// @brief Handles the chunk modification case of the chunk_multi_modify operation, which consists
/// of checking if the chunk can be modified with the given parameters and proceed if so.
/// @param currentChunkId The ID of the chunk to modify.
/// @param lockId Pointer to the lock ID for the chunk modification.
/// @param goal The goal for the chunk modification.
/// @param quotaExceeded Whether the quota has been exceeded.
/// @param operation Pointer to the operation code.
/// @param targetChunkId Pointer to the target chunk ID after modification.
/// @param targetChunk Reference to a pointer where the target chunk after modification will be
/// stored.
/// @return The status code of the operation.
uint8_t chunk_modify(uint64_t currentChunkId, uint32_t *lockId, uint8_t goal, bool quotaExceeded,
                     uint8_t *operation, uint64_t *targetChunkId, Chunk *&targetChunk) {
	// First find the chunk
	Chunk *currentChunk = chunk_find(currentChunkId);
	if (currentChunk == nullptr) { return SAUNAFS_ERROR_NOCHUNK; }

	// Next check if the chunk is locked and if the lockid matches.
	if (*lockId != 0 && *lockId != currentChunk->lockid) {
		// Adopt a client-presented lock id only for chunks restored without any live lock.
		// `lockedto == 0` alone is not enough: writeEnd clears it while chunkserver-side
		// writes may still be tracked by beingWritten, which is covered by isLocked().
		const bool restartRestoredNoLock = currentChunk->lockid == 0 &&
		                                   currentChunk->lockedto == 0 &&
		                                   !currentChunk->isLocked();
		if (!restartRestoredNoLock) {
			if (currentChunk->lockid == 0 || currentChunk->lockedto == 0) {
				// Lock was removed by some chunk operation or by a different client
				return SAUNAFS_ERROR_NOTLOCKED;
			}
			// Case *lockId != currentChunk->lockid
			return SAUNAFS_ERROR_WRONGLOCKID;
		}
	}
	if (*lockId == 0 && currentChunk->isLocked()) {
		*targetChunkId = currentChunkId;
		return SAUNAFS_ERROR_LOCKED;
	}

	// Check if the chunk is writable
	if (!currentChunk->isWritable()) { return SAUNAFS_ERROR_CHUNKLOST; }

	// Check if the chunk would be safe to write with the desired redundancy level
	ChunkCopiesCalculator calculator(currentChunk->getGoal());
	for (auto &part : currentChunk->parts) { calculator.addPart(part.type, MediaLabel::kWildcard); }
	calculator.evalRedundancyLevel();
	if (!calculator.isSafeEnoughToWrite(gRedundancyLevel)) { return SAUNAFS_ERROR_NOCHUNKSERVERS; }

	if (currentChunk->fileCount() == 1) {
		// Only one reference case
		*targetChunkId = currentChunkId;
		targetChunk = currentChunk;
		if (targetChunk->operation != Chunk::NONE) { return SAUNAFS_ERROR_CHUNKBUSY; }

		if (targetChunk->needVersionIncrease) {
			// We are expected to start writing to the chunk, but it has lost some copies and we
			// haven't increased its version yet, so we need to increase the version before allowing
			// the write operation to proceed.
			chunk_increase_version_operation(targetChunk, true);
		} else {
			chunk_lock_operation(targetChunk);
		}
	} else {
		if (currentChunk->fileCount() == 0) {  // it's serious structure error
			safs::log_warn("serious structure inconsistency: (chunkid:{:016X})", currentChunkId);
			return SAUNAFS_ERROR_CHUNKLOST;  // ERROR_STRUCTURE
		}
		// More than one reference case
		if (quotaExceeded) { return SAUNAFS_ERROR_QUOTA; }

		chunk_duplicate_operation(currentChunk, goal, targetChunk);
		*targetChunkId = targetChunk->chunkid;
	}
	*operation = targetChunk->operation;

	return SAUNAFS_STATUS_OK;
}

/// @brief Handles the chunk_multi_modify operation, which consists of performing either chunk
/// creation or modification. Called when writing on the chunk is needed.
///
/// Since the chunk is going to be modified, the chunk is locked and a lock ID is
/// assigned if the chunk is not already locked. After any of the operations, the chunk is expected
/// to be written to by the client, so if enabled, the chunkserver side locking is used to lock the
/// chunk, so the parts in the chunk are marked as being written and the corresponding chunk lock
/// messages are sent to the chunkservers.
///
/// @param currentChunkId The current chunk ID in the file layout, 0 means no chunk in current
/// index.
/// @param lockid Pointer to the lock ID, used to transmit the assigned lock ID to the caller and to
/// check the lock ID in case of modification. The lock ID is assigned in case of creation or
/// modification if the chunk is not already locked. If the chunk is already locked, then the lock
/// ID is used to verify the lock ownership.
/// @param goal The goal for the chunk creation or modification.
/// @param quotaExceeded Whether the quota has been exceeded, used to check if the operation can be
/// performed.
/// @param operation Pointer to the operation code, used to transmit the performed operation code to
/// the caller.
/// @param targetChunkId Pointer to the target chunk ID after modification, used to transmit the
/// target chunk ID to the caller in case of modification.
/// @param minServerVersion The minimum server version required for the chunk creation, used to
/// check if the operation can be performed in case of creation.
/// @return The status of the operation.
uint8_t chunk_multi_modify(uint64_t currentChunkId, uint32_t *lockid, uint8_t goal,
                           bool quotaExceeded, uint8_t *operation, uint64_t *targetChunkId,
                           uint32_t minServerVersion = 0) {
	Chunk *targetChunk = nullptr;
	uint8_t status = SAUNAFS_STATUS_OK;
	if (currentChunkId == 0) {
		// New chunk case
		status = chunk_create(quotaExceeded, goal, operation, targetChunkId, minServerVersion,
		                      targetChunk);
	} else {
		// Existing chunk case
		status = chunk_modify(currentChunkId, lockid, goal, quotaExceeded, operation, targetChunkId,
		                      targetChunk);
	}

	if (status != SAUNAFS_STATUS_OK) { return status; }

	// Set the lock if needed
	targetChunk->lockedto = eventloop_time() + LOCKTIMEOUT;
	if (*lockid == 0) {
		*lockid = 1 + rnd_ranged<uint32_t>(0xFFFFFFF0);  // some random number greater than 0
	}
	targetChunk->lockid = *lockid;

	chunk_update_checksum(targetChunk);
	emit_chunk_changed(targetChunk);
	return SAUNAFS_STATUS_OK;
}

/// @brief Handles the chunk_multi_truncate operation, which consists of performing either chunk
/// truncation or duplication and truncation.
///
/// Since the chunk is going to be modified, the chunk is locked and a lock ID is
/// assigned if the chunk is not already locked.
///
/// @param currentChunkId The current chunk ID in the file layout. Should be non-zero since
/// truncation of a non-existing chunk doesn't make sense.
/// @param lockid The lock ID, used to check if the chunk is locked and to assign a new lock ID if
/// needed.
/// @param length The length to truncate the chunk to.
/// @param goal The goal for the chunk truncation.
/// @param denyTruncatingParityParts Whether truncating parity parts is denied.
/// @param quotaExceeded Whether the quota has been exceeded, used to check if the operation can be
/// performed.
/// @param targetChunkId Pointer to the target chunk ID after truncation, used to transmit the
/// target chunk ID to the caller in case of truncation.
/// @return The status of the operation.
uint8_t chunk_multi_truncate(uint64_t currentChunkId, uint32_t lockid, uint32_t length,
                             uint8_t goal, bool denyTruncatingParityParts, bool quotaExceeded,
                             uint64_t *targetChunkId) {
	Chunk *currentChunk = nullptr;
	Chunk *targetChunk = nullptr;

	// First find the chunk
	currentChunk = chunk_find(currentChunkId);
	if (currentChunk == nullptr) {
		safs::log_err("chunk_multi_truncate: could not find chunkid {}", currentChunkId);
		return SAUNAFS_ERROR_NOCHUNK;
	}

	// Chunk must be writable to be truncated
	if (!currentChunk->isWritable()) { return SAUNAFS_ERROR_CHUNKLOST; }

	// Check if the chunk is locked and if the lockid matches
	if (currentChunk->isLocked() && (lockid == 0 || lockid != currentChunk->lockid)) {
		return SAUNAFS_ERROR_LOCKED;
	}

	// Deny truncating parity parts if initiating a truncate operation while reducing the file size
	if (denyTruncatingParityParts) {
		for (const auto &part : currentChunk->parts) {
			if (slice_traits::isParityPart(part.type)) { return SAUNAFS_ERROR_NOTPOSSIBLE; }
		}
	}

	if (currentChunk->fileCount() == 1) {
		// Only one reference case - we can truncate the chunk without duplication
		*targetChunkId = currentChunkId;
		targetChunk = currentChunk;
		if (targetChunk->operation != Chunk::NONE) { return SAUNAFS_ERROR_CHUNKBUSY; }

		chunk_truncate_operation(targetChunk, length);
	} else {
		if (currentChunk->fileCount() == 0) {  // it's serious structure error
			safs_pretty_syslog(LOG_WARNING,
			                   "serious structure inconsistency: (chunkid:%016" PRIX64 ")",
			                   currentChunkId);
			return SAUNAFS_ERROR_CHUNKLOST;  // ERROR_STRUCTURE
		}
		// More than one reference case - need to duplicate and truncate
		if (quotaExceeded) { return SAUNAFS_ERROR_QUOTA; }

		chunk_duplicate_and_truncate_operation(currentChunk, goal, targetChunk, length);
		*targetChunkId = targetChunk->chunkid;
	}

	targetChunk->lockedto = eventloop_time() + LOCKTIMEOUT;
	targetChunk->lockid = lockid;

	chunk_update_checksum(targetChunk);
	emit_chunk_changed(targetChunk);
	return SAUNAFS_STATUS_OK;
}
#endif // ! METARESTORE

uint8_t chunk_apply_modification(uint32_t ts, uint64_t oldChunkId, uint32_t lockid, uint8_t goal,
		bool doIncreaseVersion, uint64_t *newChunkId) {
	Chunk *c;
	if (oldChunkId == 0) { // new chunk
		c = chunk_new(ChunksMetadata::getAndIncrementNextChunkId(), 1);
		chunk_add_file_int(c, goal);
	} else {
		Chunk *oc = chunk_find(oldChunkId);
		if (oc == NULL) {
		    safs::log_err("chunk_apply_modification: could not find old chunkid {}", oldChunkId);
			return SAUNAFS_ERROR_NOCHUNK;
		}
		if (oc->fileCount() == 0) { // refcount == 0
			safs_pretty_syslog(LOG_WARNING,
					"serious structure inconsistency: (chunkid:%016" PRIX64 ")", oldChunkId);
			return SAUNAFS_ERROR_CHUNKLOST; // ERROR_STRUCTURE
		} else if (oc->fileCount() == 1) { // refcount == 1
			c = oc;
			if (doIncreaseVersion) {
				c->version++;
			}
		} else {
			c = chunk_new(ChunksMetadata::getAndIncrementNextChunkId(), 1);
			chunk_delete_file_int(oc, goal);
			chunk_add_file_int(c, goal);
		}
	}
	c->lockedto = ts + LOCKTIMEOUT;
	c->lockid = lockid;
	chunk_update_checksum(c);
	emit_chunk_changed(c);
	*newChunkId = c->chunkid;
	return SAUNAFS_STATUS_OK;
}

#ifndef METARESTORE
/// Declines a repair that cannot name who holds the version it would settle on.
///
/// Repairing anyway would move the chunk to a version whose holders cannot be published, and a
/// chunk whose published set describes a version it has left is one nothing may serve and nothing
/// may reconcile. Doing nothing leaves it readable at the version it is on, so this is the
/// conservative half of the choice and not the timid one. It is reported because a repair that
/// silently declines looks exactly like a repair that found nothing to do.
static ChunkRepairPlan chunk_refuse_unnameable_repair(uint64_t chunkId, uint32_t version,
                                                      const char *reason) {
	if (test_event_stream::enabled()) {
		test_event_stream::emit("repair_refused_unnameable_members",
		                        {{"chunk", chunkId}, {"version", version}, {"reason", reason}});
	}
	return {};
}

ChunkRepairPlan chunk_plan_repair(uint64_t ochunkid, uint8_t correct_only, bool nameMembers) {
	uint32_t best_version;
	Chunk *c;

	if (ochunkid == 0) { return {}; }

	c = chunk_find(ochunkid);
	if (c == NULL) {
		if (correct_only == 1) { return {}; }
		return {.action = ChunkRepairAction::kEraseReference, .version = 0, .members = {}};
	}
	if (c->isLocked()) { return {}; }

	// calculators will be sorted by decreasing keys, so highest version will be first.
	std::map<uint32_t, ChunkCopiesCalculator, std::greater<uint32_t>> calculators;
	best_version = 0;
	for (const auto &part : c->parts) {
		// ignore chunks which are being deleted
		if (part.state != ChunkPart::DEL) {
			ChunkCopiesCalculator &calculator = calculators[part.version];
			calculator.addPart(part.type, matocsserv_get_label(part.server()));
		}
	}
	// find best version which can be recovered
	// calculators are sorted by decreasing keys, so highest version will be first.
	for (auto &version_and_calculator : calculators) {
		uint32_t version = version_and_calculator.first;
		ChunkCopiesCalculator &calculator = version_and_calculator.second;
		calculator.optimize(gUseLinearAssignmentOptimizer, &gLinearAssignmentCache);
		// calculator.isRecoveryPossible() won't work below, because target goal is empty.
		if (calculator.getFullCopiesCount() > 0) {
			best_version = version;
			break;
		}
	}
	// current version is readable
	if (best_version == c->version) { return {}; }
	// didn't find sensible chunk
	if (best_version == 0) {
		if (correct_only == 1) { return {}; }
		return {.action = ChunkRepairAction::kEraseReference, .version = 0, .members = {}};
	}

	// Name who holds the version being settled on, while the parts that say so are in front of
	// us. A repair that cannot name its holders cannot publish them, and a repaired chunk whose
	// published set still describes the version it left is one nothing may serve and nothing may
	// reconcile, so it fails closed and repairs nothing rather than stranding the chunk. Only a
	// caller that publishes the member set needs this: the in-memory backend ignores members and
	// its connections carry no stable id, so it must plan without naming.
	std::vector<ChunkRepairMember> members;
	if (nameMembers) {
		for (const auto &part : c->parts) {
			if (part.state == ChunkPart::DEL || part.version != best_version) { continue; }
			uint32_t serverIp = 0;
			uint16_t serverPort = 0;
			MediaLabel serverLabel = MediaLabel::kWildcard;
			if (matocsserv_getlocation(part.server(), &serverIp, &serverPort, &serverLabel) != 0) {
				return chunk_refuse_unnameable_repair(ochunkid, best_version,
				                                      "location unresolved");
			}
			const uint32_t stableId = matocsserv_stable_id_by_address(serverIp, serverPort);
			if (stableId == 0) {
				return chunk_refuse_unnameable_repair(ochunkid, best_version, "no stable id");
			}
			members.push_back({stableId, static_cast<uint16_t>(part.type.getId())});
		}
		if (members.empty()) {
			return chunk_refuse_unnameable_repair(ochunkid, best_version,
			                                      "no holder of that version");
		}
	}

	return {.action = ChunkRepairAction::kSetVersion,
	        .version = best_version,
	        .members = std::move(members)};
}

bool chunk_apply_repair_plan(uint8_t goal, uint64_t ochunkid, const ChunkRepairPlan &plan) {
	if (plan.action == ChunkRepairAction::kUnchanged) { return true; }

	Chunk *c = chunk_find(ochunkid);
	if (plan.action == ChunkRepairAction::kEraseReference) {
		return c == nullptr || chunk_delete_file_int(c, goal) == SAUNAFS_STATUS_OK;
	}
	if (c == nullptr || plan.version == 0) { return false; }

	c->version = plan.version;
	for (auto &part : c->parts) {
		if (part.state == ChunkPart::INVALID && part.version == plan.version) {
			part.state = ChunkPart::VALID;
		}
	}
	c->needVersionIncrease = 1;
	c->updateStats();
	chunk_update_checksum(c);
	emit_chunk_changed(c);
	return true;
}

int chunk_repair(uint8_t goal, uint64_t ochunkid, uint32_t *nversion, uint8_t correct_only) {
	*nversion = 0;
	const ChunkRepairPlan plan = chunk_plan_repair(ochunkid, correct_only, false);
	if (plan.action == ChunkRepairAction::kUnchanged) { return 0; }
	if (!chunk_apply_repair_plan(goal, ochunkid, plan)) { return 0; }
	*nversion = plan.version;
	return 1;
}
#endif

int chunk_set_version(uint64_t chunkid,uint32_t version) {
	Chunk *c;
	c = chunk_find(chunkid);
	if (c==NULL) {
		safs::log_err("chunk_set_version: could not find chunkid {}", chunkid);
		return SAUNAFS_ERROR_NOCHUNK;
	}
	c->version = version;
	chunk_update_checksum(c);
	emit_chunk_changed(c);
	return SAUNAFS_STATUS_OK;
}

int chunk_increase_version(uint64_t chunkid) {
	Chunk *c;
	c = chunk_find(chunkid);
	if (c==NULL) {
		safs::log_err("chunk_increase_version: could not find chunkid {}", chunkid);
		return SAUNAFS_ERROR_NOCHUNK;
	}
	c->version++;
	chunk_update_checksum(c);
	emit_chunk_changed(c);
	return SAUNAFS_STATUS_OK;
}

uint8_t chunk_set_next_chunkid(uint64_t nextChunkIdToBeSet) {
	if (nextChunkIdToBeSet >= ChunksMetadata::getNextChunkId()) {
		ChunksMetadata::setNextChunkId(nextChunkIdToBeSet);
		return SAUNAFS_STATUS_OK;
	}

	safs::log_warn(
	    "chunk_set_next_chunkid: failed to set next chunk id from {} to value {}. Ignoring.",
	    ChunksMetadata::getNextChunkId(), nextChunkIdToBeSet);

	return SAUNAFS_ERROR_MISMATCH;
}

#ifndef METARESTORE

const ChunksReplicationState& chunk_get_replication_state() {
	return Chunk::allChunksReplicationState;
}

const ChunksAvailabilityState& chunk_get_availability_state() {
	return Chunk::allChunksAvailability;
}

struct ChunkLocation {
	ChunkLocation() : chunkType(slice_traits::standard::ChunkPartType()),
			chunkserver_version(0), distance(0), random(0) {
	}
	NetworkAddress address;
	ChunkPartType chunkType;
	uint32_t chunkserver_version;
	uint32_t distance;
	uint32_t random;
	MediaLabel label;
	bool operator<(const ChunkLocation& other) const {
		if (distance < other.distance) {
			return true;
		} else if (distance > other.distance) {
			return false;
		} else {
			return random < other.random;
		}
	}
};

// TODO deduplicate
int chunk_getversionandlocations(uint64_t chunkid, uint32_t currentIp, uint32_t& version,
		uint32_t maxNumberOfChunkCopies, std::vector<ChunkTypeWithAddress>& serversList) {
	Chunk *c;
	uint8_t cnt;

	sassert(serversList.empty());
	c = chunk_find(chunkid);

	if (c == NULL) {
		safs::log_err("chunk_getversionandlocations: could not find chunkid {}", chunkid);
		return SAUNAFS_ERROR_NOCHUNK;
	}
	version = c->version;
	cnt = 0;
	std::vector<ChunkLocation> chunkLocation;
	ChunkLocation chunkserverLocation;
	for (const auto &part : c->parts) {
		if (part.is_valid()) {
			if (cnt < maxNumberOfChunkCopies && matocsserv_getlocation(part.server(),
					&(chunkserverLocation.address.ip),
					&(chunkserverLocation.address.port),
					&(chunkserverLocation.label)) == 0) {
				chunkserverLocation.chunkType = part.type;
				chunkserverLocation.chunkserver_version = matocsserv_get_version(part.server());
				chunkserverLocation.distance =
						topology_distance(chunkserverLocation.address.ip, currentIp);
						// in the future prepare more sophisticated distance function
				chunkserverLocation.random = rnd<uint32_t>();
				chunkLocation.push_back(chunkserverLocation);
				cnt++;
			}
		}
	}
	std::sort(chunkLocation.begin(), chunkLocation.end());
	for (uint32_t i = 0; i < chunkLocation.size(); ++i) {
		const ChunkLocation& loc = chunkLocation[i];
		serversList.emplace_back(loc.address, loc.chunkType, loc.chunkserver_version);
	}
	return SAUNAFS_STATUS_OK;
}

int chunk_getversionandlocations(uint64_t chunkid, uint32_t currentIp, uint32_t& version,
		uint32_t maxNumberOfChunkCopies, std::vector<ChunkPartWithAddressAndLabel>& serversList) {
	Chunk *c;
	uint8_t cnt;

	sassert(serversList.empty());
	c = chunk_find(chunkid);

	if (c == NULL) {
		safs::log_err("chunk_getversionandlocations (2?): could not find chunkid {}", chunkid);
		return SAUNAFS_ERROR_NOCHUNK;
	}
	version = c->version;
	cnt = 0;
	std::vector<ChunkLocation> chunkLocation;
	ChunkLocation chunkserverLocation;
	for (const auto &part : c->parts) {
		if (part.is_valid()) {
			if (cnt < maxNumberOfChunkCopies && matocsserv_getlocation(part.server(),
					&(chunkserverLocation.address.ip),
					&(chunkserverLocation.address.port),
					&(chunkserverLocation.label)) == 0) {
				chunkserverLocation.chunkType = part.type;
				chunkserverLocation.distance =
						topology_distance(chunkserverLocation.address.ip, currentIp);
						// in the future prepare more sophisticated distance function
				chunkserverLocation.random = rnd<uint32_t>();
				chunkLocation.push_back(chunkserverLocation);
				cnt++;
			}
		}
	}
	std::sort(chunkLocation.begin(), chunkLocation.end());
	for (uint32_t i = 0; i < chunkLocation.size(); ++i) {
		const ChunkLocation& loc = chunkLocation[i];
		serversList.emplace_back(loc.address, static_cast<std::string>(loc.label), loc.chunkType);
	}
	return SAUNAFS_STATUS_OK;
}

bool chunk_has_any_parts(uint64_t chunkid) {
	Chunk *chunk = chunk_find(chunkid);
	return chunk != nullptr && !chunk->parts.empty();
}

bool chunk_has_live_parts(uint64_t chunkid) {
	Chunk *chunk = chunk_find(chunkid);
	if (chunk == nullptr) { return false; }
	for (const auto &part : chunk->parts) {
		if (part.is_valid() || part.is_being_written()) { return true; }
	}
	return false;
}

int chunk_part_memory_state(uint64_t chunkid, matocsserventry *server, ChunkPartType type) {
	Chunk *chunk = chunk_find(chunkid);
	if (chunk == nullptr) { return 0; }
	for (const auto &part : chunk->parts) {
		if (part.server() == server && part.type == type) {
			return part.is_valid() ? 1 : -1;
		}
	}
	return 0;
}

void chunk_refresh_from_record(uint64_t chunkid, uint32_t version, uint32_t lockid,
                               uint32_t lockedto) {
	// Another metadata server may have moved the durable version or lock while this one
	// held a stale in-memory view. Adopt the durable state only while nothing local is in
	// flight: a local operation or write in progress makes memory the authority.
	Chunk *chunk = chunk_find(chunkid);
	if (chunk == nullptr || chunk->operation != Chunk::NONE) { return; }

	// A session leaves write-in-progress tracking that only its own chunkserver acks
	// clear, and with several MDSs those acks can land elsewhere, pinning this chunk
	// locked here forever. Once the lock is gone both here (expired or cleared) and
	// durably, drop the tracking: the chunkserver-side chunk lock is what actually
	// serializes write chains.
	if (chunk->lockedto != 0 && chunk->lockedto < eventloop_time()) {
		for (auto &part : chunk->parts) { part.unmark_being_written(); }
		chunk->lockedto = 0;
	} else if (chunk->lockedto == 0 && lockedto == 0) {
		for (auto &part : chunk->parts) { part.unmark_being_written(); }
	}

	for (const auto &part : chunk->parts) {
		if (part.is_being_written() || part.is_busy()) { return; }
	}

	// Forward only: the version is monotonic, so a durable value BEHIND memory is a
	// commit of ours still in flight, never something to regress to; regressing would
	// hand out a version the chunkservers already left behind.
	if (version > chunk->version) {
		for (auto &part : chunk->parts) {
			if (part.is_valid() && part.version == chunk->version) { part.version = version; }
		}
		chunk->version = version;
	}

	// Same owner: adopt only a SHORTER lock (an unlock made durable elsewhere); a longer
	// durable value must never extend a lock this side already saw expire, and a renewal
	// still in flight locally must never be regressed into a stealable window. A different
	// owner is adopted whole: that lock was granted through another metadata server.
	if (chunk->lockid == lockid) {
		if (lockedto < chunk->lockedto) { chunk->lockedto = lockedto; }
	} else {
		chunk->lockid = lockid;
		chunk->lockedto = lockedto;
	}
}

void chunk_server_has_chunk(matocsserventry *ptr, uint64_t chunkid, uint32_t versionWithTodelFlag, ChunkPartType chunkType) {
	Chunk *c;
	const uint32_t new_version = common::getChunkVersion(versionWithTodelFlag);
	const bool todel = common::getTodelFlag(versionWithTodelFlag);
	c = chunk_find(chunkid);
	if (c==NULL) {
		// chunkserver has nonexistent chunk, so create it for future deletion
		if (chunkid >= ChunksMetadata::getNextChunkId() &&
		    gChunkIdGenerator->isStrictlyMonotonic()) {
			gFSOperations->setNextChunkId(FsContext::getForMaster(eventloop_time()), chunkid + 1);
		}
		c = chunk_new(chunkid, new_version);
		c->lockedto = (uint32_t)eventloop_time()+UNUSED_DELETE_TIMEOUT;
		c->lockid = 0;
		chunk_update_checksum(c);
		emit_chunk_changed(c);
	}
	auto server_csid = matocsserv_get_csdb(ptr)->csid;
	for (auto &part : c->parts) {
		if (part.csid == server_csid && part.type == chunkType) {
			// This server already notified us about its copy.
			// We normally don't get repeated notifications about the same copy, but
			// they can arrive after chunkserver configuration reload (particularly,
			// when folders change their 'to delete' status) or due to bugs.
			// Let's try to handle them as well as we can.
			switch (part.state) {
			case ChunkPart::DEL:
				// We requested deletion, but the chunkserver 'has' this copy again.
				// Repeat deletion request.
				c->invalidateCopy(part);
				// fallthrough
			case ChunkPart::INVALID:
				// leave this copy alone
				return;
			default:
				break;
			}
			if (part.version != new_version) {
				safs_pretty_syslog(LOG_WARNING, "chunk %016" PRIX64 ": master data indicated "
						"version %08" PRIX32 ", chunkserver reports %08"
						PRIX32 "!!! Updating master data.", c->chunkid,
						part.version, new_version);
				part.version = new_version;
			}
			if (part.version != c->version) {
				c->markCopyAsHavingWrongVersion(part);
				return;
			}
			if (!part.is_todel() && todel) {
				part.mark_todel();
				c->updateStats();
			}
			if (part.is_todel() && !todel) {
				part.unmark_todel();
				c->updateStats();
			}
			return;
		}
	}
	const uint8_t state = (new_version == c->version) ? (todel ? ChunkPart::TDVALID : ChunkPart::VALID) : ChunkPart::INVALID;
	c->parts.push_back(ChunkPart(server_csid, state, new_version, chunkType));
	c->updateStats();
}

void chunk_damaged(matocsserventry *ptr, uint64_t chunkid, ChunkPartType chunk_type) {
	Chunk *c;
	c = chunk_find(chunkid);
	if (c == NULL) {
		safs::log_warn(
		    "Chunkserver has nonexistent chunk ({:016X}), creating it for future deletion",
		    chunkid);

		if (chunkid >= ChunksMetadata::getNextChunkId() &&
		    gChunkIdGenerator->isStrictlyMonotonic()) {
			// Ensure nextChunkId is always greater than any known id
			ChunksMetadata::setNextChunkId(chunkid + 1);
		}

		c = chunk_new(chunkid, 0);
	}
	auto server_csid = matocsserv_get_csdb(ptr)->csid;
	for (auto &part : c->parts) {
		if (part.csid == server_csid && part.type == chunk_type) {
			c->invalidateCopy(part);
			c->needVersionIncrease = 1;
			return;
		}
	}
	c->parts.push_back(ChunkPart(server_csid, ChunkPart::INVALID, 0, slice_traits::standard::ChunkPartType()));
	c->updateStats();
	c->needVersionIncrease = 1;
}

void chunk_lost(matocsserventry *ptr,uint64_t chunkid, ChunkPartType chunk_type) {
	Chunk *c = chunk_find(chunkid);
	if (c == nullptr) {
		return;
	}
	auto server_csid = matocsserv_get_csdb(ptr)->csid;
	auto it = std::remove_if(c->parts.begin(), c->parts.end(), [server_csid, chunk_type](const ChunkPart &part) {
		return part.csid == server_csid && part.type == chunk_type;
	});
	if (it != c->parts.end()) {
		c->parts.erase(it, c->parts.end());
		c->updateStats();
		c->needVersionIncrease = 1;
	}
}

void chunk_server_disconnected(matocsserventry */*ptr*/, const MediaLabel &label) {
	replicationDelayInfoForAll.serverDisconnected();
	if (label != MediaLabel::kWildcard) {
		replicationDelayInfoForLabel[label].serverDisconnected();
	}
	// If chunkserver disconnects, we can assure it was processed by zombie server loop
	// only if the loop was executed at least twice. Reset the scan position so the
	// next call starts from the beginning, ensuring both passes cover all chunks.
	gDisconnectedCounter = 2;
	gCurrentBucketInZombieLoop = 0;
	gCurrentChunkInZombieLoopIndex = 0;
	eventloop_make_next_poll_nonblocking();
	fs_cs_disconnected();
	gChunksMetadata->lastchunkid = 0;
	gChunksMetadata->lastchunkptr = NULL;
}

void chunk_server_unlabelled_connected() {
	replicationDelayInfoForAll.serverConnected();
}

void chunk_server_label_changed(const MediaLabel &previousLabel, const MediaLabel &newLabel) {
	/*
	 * Only server with no label can be considered as newly connected
	 * and it was added to replicationDelayInfoForAll earlier
	 * in chunk_server_unlabelled_connected call.
	 */
	if (previousLabel == MediaLabel::kWildcard) {
		replicationDelayInfoForLabel[newLabel].serverConnected();
	}
}

static Chunk *getCurrentChunkInBucket(uint32_t currentBucket, size_t currentBucketIndex) {
	if (currentBucket >= kChunkHashSize) { return nullptr; }

	const auto &bucket = gChunksMetadata->chunkhash[currentBucket];
	if (currentBucketIndex >= bucket.size()) { return nullptr; }

	return bucket[currentBucketIndex];
}

/*
 * A function that is called in every main loop iteration, that cleans chunk structs
 */
void chunk_clean_zombie_servers_a_bit() {
	SignalLoopWatchdog watchdog;

	if (gDisconnectedCounter == 0) { return; }

	watchdog.start();

	while (gCurrentBucketInZombieLoop < kChunkHashSize) {
		const auto &bucket = gChunksMetadata->chunkhash[gCurrentBucketInZombieLoop];

		while (gCurrentChunkInZombieLoopIndex < bucket.size()) {
			chunk_handle_disconnected_copies(bucket[gCurrentChunkInZombieLoopIndex++]);

			if (watchdog.expired()) {
				eventloop_make_next_poll_nonblocking();
				return;
			}
		}

		++gCurrentBucketInZombieLoop;

		if (gCurrentBucketInZombieLoop < kChunkHashSize) { gCurrentChunkInZombieLoopIndex = 0; }
	}

	--gDisconnectedCounter;
	gCurrentBucketInZombieLoop = 0;
	gCurrentChunkInZombieLoopIndex = 0;

	eventloop_make_next_poll_nonblocking();
}

void chunk_got_delete_status(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType, uint8_t /*status*/) {
	Chunk *c = chunk_find(chunkId);
	if (c==NULL) {
		return ;
	}
	auto server_csid = matocsserv_get_csdb(ptr)->csid;
	auto it = std::remove_if(c->parts.begin(), c->parts.end(), [server_csid, chunkType](const ChunkPart& part) {
		if (part.csid == server_csid && part.type == chunkType) {
			if (part.state != ChunkPart::DEL) {
				safs_pretty_syslog(LOG_WARNING, "got unexpected delete status");
			}
			return true;
		}
		return false;
	});
	if (it != c->parts.end()) {
		c->parts.erase(it, c->parts.end());
		c->updateStats();
	}
}

void chunk_got_replicate_status(matocsserventry *ptr, uint64_t chunkId, uint32_t chunkVersion,
		ChunkPartType chunkType, uint8_t status) {
	Chunk *c = chunk_find(chunkId);
	if (c == NULL || status != 0) {
		return;
	}

	auto server_csid = matocsserv_get_csdb(ptr)->csid;
	for (auto &part : c->parts) {
		if (part.type == chunkType && part.csid == server_csid) {
			safs_pretty_syslog(LOG_WARNING,
					"got replication status from server which had had that chunk before (chunk:%016"
					PRIX64 "_%08" PRIX32 ")", chunkId, chunkVersion);
			if (part.state == ChunkPart::VALID && chunkVersion != c->version) {
				part.version = chunkVersion;
				c->markCopyAsHavingWrongVersion(part);
			}
			return;
		}
	}
	const uint8_t state = (c->isLocked() || chunkVersion != c->version) ? ChunkPart::INVALID : ChunkPart::VALID;
	c->parts.push_back(ChunkPart(server_csid, state, chunkVersion, chunkType));
	c->updateStats();
}

void chunk_operation_status(Chunk *c, ChunkPartType chunkType, uint8_t status,
                            matocsserventry *ptr) {
	bool any_copy_busy = false;
	auto server_csid = matocsserv_get_csdb(ptr)->csid;
	for (auto &part : c->parts) {
		if (part.csid == server_csid && part.type == chunkType) {
			if (status != SAUNAFS_STATUS_OK) {
				c->interrupted = 1;  // increase version after finish, just in case
				c->invalidateCopy(part);
			} else {
				if (part.is_busy()) { part.unmark_busy(); }
			}
		}

		any_copy_busy |= part.is_busy();
	}

	if (!any_copy_busy) {
		if (c->isWritable()) {
			if (c->interrupted) {
				chunk_emergency_increase_version(c);
			} else {
				matoclserv_chunk_status(c->chunkid, SAUNAFS_STATUS_OK);
				c->operation = Chunk::NONE;
				c->needVersionIncrease = 0;
			}
		} else {
			chunk_finalize_failed_operation(c);
		}
		// Every part has reported this operation's status, so the operation is complete:
		// close the durable ownership round it ran under, exactly as the write end path
		// does. Backends that keep no such round do nothing here.
		gChunkOperations->finalizeWriteRound(c->chunkid);
	}
}

/// @brief Handles the end of a chunk write operation.
/// This function is called when a chunkserver reports the status of a write operation on a chunk
/// part. It checks the status and updates the chunk's state accordingly. If the write operation was
/// not successful, it marks the corresponding copy as invalid and sets the interrupted flag on the
/// chunk. The status sent by the chunkserver is expected to be the one not told to the clients.
/// @param chunk The chunk that was written.
/// @param chunkType The type of the chunk part.
/// @param status The status of the write operation.
/// @param ptr The server entry associated with the operation.
void chunk_write_end_status(Chunk *chunk, ChunkPartType chunkType, uint8_t status,
                            matocsserventry *ptr) {
	bool anyCopyBeingWritten = false;
	auto server_csid = matocsserv_get_csdb(ptr)->csid;
	for (auto &part : chunk->parts) {
		if (part.csid == server_csid && part.type == chunkType) {
			if (status != SAUNAFS_STATUS_OK) {
				chunk->interrupted = 1;  // increase version after finish, just in case
				chunk->invalidateCopy(part);
			} else {
				if (part.is_being_written()) { part.unmark_being_written(); }
			}
		}

		anyCopyBeingWritten |= part.is_being_written();
	}

	if (!anyCopyBeingWritten && chunk->interrupted) {
		if (chunk->isWritable()) {
			chunk_emergency_increase_version(chunk);
		} else {
			// If chunk is not writable, we probably have a lost chunk here
			safs::log_warn("{}: chunk {} is not writable. Lost chunk?", __func__, chunk->chunkid);
		}
	}

	if (!anyCopyBeingWritten) {
		// Every part has reported its write end, so the client write is complete: close the
		// durable ownership round this chunk was opened under. Backends that keep no such round
		// do nothing here.
		gChunkOperations->finalizeWriteRound(chunk->chunkid);
	}

	if (!anyCopyBeingWritten && !chunk->isLocked()) {
		// If the chunk is not locked anymore, we can try to send notices about the operation
		// status to clients waiting for the lock release.
		matoclserv_notify_unlock_list(chunk->chunkid);
	}
}

void chunk_got_create_status(matocsserventry *ptr,uint64_t chunkId, ChunkPartType chunkType, uint8_t status) {
	Chunk *c;
	c = chunk_find(chunkId);
	if (c==NULL) {
		return ;
	}
	chunk_operation_status(c, chunkType, status, ptr);
}

void chunk_got_duplicate_status(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType, uint8_t status) {
	Chunk *c;
	c = chunk_find(chunkId);
	if (c==NULL) {
		return ;
	}
	chunk_operation_status(c, chunkType, status, ptr);
}

/// @brief Handles the status of a chunk lock operation.
/// @param ptr The server entry associated with the operation.
/// @param chunkId The ID of the chunk.
/// @param chunkType The type of the chunk part.
/// @param status The status of the chunk lock operation.
void chunk_got_chunklock_status(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType,
                                uint8_t status) {
	Chunk *chunk;
	chunk = chunk_find(chunkId);
	if (chunk == nullptr) { return; }

	chunk_operation_status(chunk, chunkType, status, ptr);
}

/// @brief Handles the status of a chunk write end operation.
/// @param ptr The server entry associated with the operation.
/// @param chunkId The ID of the chunk.
/// @param chunkType The type of the chunk part.
/// @param status The status of the chunk write end operation.
void chunk_got_writeend_status(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType,
                               uint8_t status) {
	Chunk *chunk;
	chunk = chunk_find(chunkId);
	if (chunk == nullptr) { return; }

	chunk_write_end_status(chunk, chunkType, status, ptr);
}

void chunk_got_setversion_status(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType, uint8_t status) {
	Chunk *c;
	c = chunk_find(chunkId);
	if (c==NULL) {
		return ;
	}
	chunk_operation_status(c, chunkType, status, ptr);
}

void chunk_got_truncate_status(matocsserventry *ptr, uint64_t chunkid, ChunkPartType chunkType, uint8_t status) {
	Chunk *c;
	c = chunk_find(chunkid);
	if (c==NULL) {
		return ;
	}
	chunk_operation_status(c, chunkType, status, ptr);
}

void chunk_got_duptrunc_status(matocsserventry *ptr, uint64_t chunkId, ChunkPartType chunkType, uint8_t status) {
	Chunk *c;
	c = chunk_find(chunkId);
	if (c==NULL) {
		return ;
	}
	chunk_operation_status(c, chunkType, status, ptr);
}

/* ----------------------- */
/* JOBS (DELETE/REPLICATE) */
/* ----------------------- */

uint32_t get_chunk_info_serialized_size() {
	// Some fields are multiplied by 2 because they are stored in both 'done' and 'not done' states.
	// See `chunk_store_info` function for details.
	constexpr uint32_t kInfoSize =
	    sizeof(chunksinfo_loopstart) + sizeof(chunksinfo_loopend) +
	    (2 * sizeof(job_info::del_invalid)) + (2 * sizeof(job_info::del_unused)) +
	    (2 * sizeof(job_info::del_diskclean)) + (2 * sizeof(job_info::del_overgoal)) +
	    (2 * sizeof(job_info::copy_undergoal)) + sizeof(loop_info::copy_rebalance);

	return kInfoSize;
}

void chunk_store_info(uint8_t *buff) {
	put32bit(&buff,chunksinfo_loopstart);
	put32bit(&buff,chunksinfo_loopend);
	put32bit(&buff,chunksinfo.done.del_invalid);
	put32bit(&buff,chunksinfo.notdone.del_invalid);
	put32bit(&buff,chunksinfo.done.del_unused);
	put32bit(&buff,chunksinfo.notdone.del_unused);
	put32bit(&buff,chunksinfo.done.del_diskclean);
	put32bit(&buff,chunksinfo.notdone.del_diskclean);
	put32bit(&buff,chunksinfo.done.del_overgoal);
	put32bit(&buff,chunksinfo.notdone.del_overgoal);
	put32bit(&buff,chunksinfo.done.copy_undergoal);
	put32bit(&buff,chunksinfo.notdone.copy_undergoal);
	put32bit(&buff,chunksinfo.copy_rebalance);
}

//jobs state: jobshpos

class ChunkWorker : public coroutine {
public:
	ChunkWorker();
	void doEveryLoopTasks();
	void doEverySecondTasks();
	void doChunkJobs(Chunk *c, uint16_t serverCount);
	void mainLoop();

private:
	using ServersWithUsage = std::vector<ServerWithUsage>;

	struct MainLoopStack {
		uint32_t current_bucket;
		size_t current_bucket_index;
		uint16_t usable_server_count;
		uint32_t chunks_done_count;
		uint32_t buckets_done_count;
		std::size_t endangered_to_serve;
		Chunk *node;
		ActiveLoopWatchdog work_limit;
		ActiveLoopWatchdog watchdog;
	};

	bool deleteUnusedChunks();

	uint32_t getMinChunkserverVersion(Chunk *c, ChunkPartType type);
	bool tryReplication(Chunk *c, ChunkPartType type, matocsserventry *destinationServer);

	void deleteInvalidChunkParts(Chunk *c);
	void deleteAllChunkParts(Chunk *c);
	bool replicateChunkPart(Chunk *c, Goal::Slice::Type slice_type, int slice_part, ChunkCopiesCalculator& calc, const IpCounter &ip_counter);
	bool removeUnneededChunkPart(Chunk *c, Goal::Slice::Type slice_type, int slice_part,
	                             ChunkCopiesCalculator& calc, const IpCounter &ip_counter);
	bool rebalanceChunkParts(Chunk *c, ChunkCopiesCalculator& calc, bool only_todel, const IpCounter &ip_counter);
	bool rebalanceChunkPartsWithSameIp(Chunk *c, ChunkCopiesCalculator &calc, const IpCounter &ip_counter);
	void updateSortedServers();

	loop_info inforec_;
	uint32_t deleteNotDone_;
	uint32_t deleteDone_;
	uint32_t prevToDeleteCount_;
	uint32_t deleteLoopCount_;

	/// All chunkservers sorted by disk usage.
	ServersWithUsage sortedServers_;

	/// For each label, all servers with this label sorted by disk usage.
	std::map<MediaLabel, ServersWithUsage> labeledSortedServers_;

	MainLoopStack stack_;
};

ChunkWorker::ChunkWorker()
		: deleteNotDone_(0),
		  deleteDone_(0),
		  prevToDeleteCount_(0),
		  deleteLoopCount_(0) {
	memset(&inforec_,0,sizeof(loop_info));
	stack_.current_bucket = 0;
	stack_.current_bucket_index = 0;
}

void ChunkWorker::doEveryLoopTasks() {
	deleteLoopCount_++;
	if (deleteLoopCount_ >= 16) {
		uint32_t toDeleteCount = deleteDone_ + deleteNotDone_;
		deleteLoopCount_ = 0;
		if ((deleteNotDone_ > deleteDone_) && (toDeleteCount > prevToDeleteCount_)) {
			TmpMaxDelFrac *= 1.5;
			if (TmpMaxDelFrac>MaxDelHardLimit) {
				safs_pretty_syslog(LOG_NOTICE,"DEL_LIMIT hard limit (%" PRIu32 " per server) reached",MaxDelHardLimit);
				TmpMaxDelFrac=MaxDelHardLimit;
			}
			TmpMaxDel = TmpMaxDelFrac;
			safs_pretty_syslog(LOG_NOTICE,"DEL_LIMIT temporary increased to: %" PRIu32 " per server",TmpMaxDel);
		}
		if ((toDeleteCount < prevToDeleteCount_) && (TmpMaxDelFrac > MaxDelSoftLimit)) {
			TmpMaxDelFrac /= 1.5;
			if (TmpMaxDelFrac<MaxDelSoftLimit) {
				safs_pretty_syslog(LOG_NOTICE,"DEL_LIMIT back to soft limit (%" PRIu32 " per server)",MaxDelSoftLimit);
				TmpMaxDelFrac = MaxDelSoftLimit;
			}
			TmpMaxDel = TmpMaxDelFrac;
			safs_pretty_syslog(LOG_NOTICE,"DEL_LIMIT decreased back to: %" PRIu32 " per server",TmpMaxDel);
		}
		prevToDeleteCount_ = toDeleteCount;
		deleteNotDone_ = 0;
		deleteDone_ = 0;
	}
	chunksinfo = inforec_;
	memset(&inforec_,0,sizeof(inforec_));
	chunksinfo_loopstart = chunksinfo_loopend;
	chunksinfo_loopend = eventloop_time();
}

void ChunkWorker::updateSortedServers() {
	sortedServers_ = matocsserv_getservers_sorted();
	labeledSortedServers_.clear();
	for (const ServerWithUsage& sw : sortedServers_) {
		labeledSortedServers_[sw.label].push_back(sw);
	}
}

void ChunkWorker::doEverySecondTasks() {
	updateSortedServers();
}

static bool chunkPresentOnServer(Chunk *c, matocsserventry *server) {
	auto server_csid = matocsserv_get_csdb(server)->csid;
	return std::any_of(c->parts.begin(), c->parts.end(), [server_csid](const ChunkPart &part) {
		return part.csid == server_csid;
	});
}

static bool chunkPresentOnServer(Chunk *c, Goal::Slice::Type slice_type, matocsserventry *server) {
	auto server_csid = matocsserv_get_csdb(server)->csid;
	return std::any_of(c->parts.begin(), c->parts.end(), [server_csid, slice_type](const ChunkPart &part) {
		return part.csid == server_csid && part.type.getSliceType() == slice_type;
	});
}

uint32_t ChunkWorker::getMinChunkserverVersion(Chunk */*c*/, ChunkPartType /*type*/) {
	return kFirstECVersion;
}

bool ChunkWorker::tryReplication(Chunk *c, ChunkPartType part_to_recover,
				matocsserventry *destination_server) {
	// TODO(msulikowski) Prefer VALID over TDVALID copies.
	std::vector<matocsserventry *> standard_servers;
	std::vector<matocsserventry *> all_servers;
	std::vector<ChunkPartType> all_parts;
	ChunkCopiesCalculator calc(c->getGoal());

	// Implies matocsserv_get_version(destination_server) >= kFirstECVersion
	assert(matocsserv_get_version(destination_server) >=
	       getMinChunkserverVersion(c, part_to_recover));

	for (const auto &part : c->parts) {
		if (!part.is_valid() || part.is_busy() || matocsserv_replication_read_counter(part.server()) >= MaxReadRepl) {
			continue;
		}

		if (slice_traits::isStandard(part.type)) {
			standard_servers.push_back(part.server());
		}

		all_servers.push_back(part.server());
		all_parts.push_back(part.type);
		calc.addPart(part.type, matocsserv_get_label(part.server()));
	}

	calc.evalRedundancyLevel();
	if (!calc.isRecoveryPossible()) {
		return false;
	}

	matocsserv_send_sau_replicatechunk(destination_server, c->chunkid, c->version, part_to_recover,
	                                   all_servers, all_parts);
	stats_replications++;
	metrics::Counter::increment(metrics::Counter::Master::CHUNK_REPLICATE);
	c->needVersionIncrease = 1;
	return true;
}

void ChunkWorker::deleteInvalidChunkParts(Chunk *c) {
	for (auto &part : c->parts) {
		if (matocsserv_deletion_counter(part.server()) < TmpMaxDel) {
			if (!part.is_valid()) {
				if (part.state == ChunkPart::DEL) {
					safs_pretty_syslog(LOG_WARNING,
					       "chunk hasn't been deleted since previous loop - "
					       "retry");
				}
				part.state = ChunkPart::DEL;
				stats_deletions++;
				metrics::Counter::increment(metrics::Counter::Master::CHUNK_DELETE);
				matocsserv_send_deletechunk(part.server(), c->chunkid, 0, part.type);
				inforec_.done.del_invalid++;
				deleteDone_++;
			}
		} else {
			if (part.state == ChunkPart::INVALID) {
				inforec_.notdone.del_invalid++;
				deleteNotDone_++;
			}
		}
	}
}

void ChunkWorker::deleteAllChunkParts(Chunk *c) {
	for (auto &part : c->parts) {
		if (matocsserv_deletion_counter(part.server()) < TmpMaxDel) {
			if (part.is_valid() && !part.is_busy()) {
				c->deleteCopy(part);
				c->needVersionIncrease = 1;
				stats_deletions++;
				metrics::Counter::increment(metrics::Counter::Master::CHUNK_DELETE);
				matocsserv_send_deletechunk(part.server(), c->chunkid, c->version,
				                            part.type);
				inforec_.done.del_unused++;
				deleteDone_++;
			}
		} else {
			if (part.state == ChunkPart::VALID || part.state == ChunkPart::TDVALID) {
				inforec_.notdone.del_unused++;
				deleteNotDone_++;
			}
		}
	}
}

bool ChunkWorker::replicateChunkPart(Chunk *c, Goal::Slice::Type slice_type, int slice_part,
					ChunkCopiesCalculator &calc, const IpCounter &ip_counter) {
	std::vector<matocsserventry *> servers;
	int skipped_replications = 0, valid_parts_count = 0, expected_copies = 0;
	bool tried_to_replicate = false;
	Goal::Slice::Labels replicate_labels;

	replicate_labels = calc.getLabelsToRecover(slice_type, slice_part);

	if (calc.getAvailable().find(slice_type) != calc.getAvailable().end()) {
		valid_parts_count =
		        Goal::Slice::countLabels(calc.getAvailable()[slice_type][slice_part]);
	}

	expected_copies = Goal::Slice::countLabels(calc.getTarget()[slice_type][slice_part]);

	uint32_t min_chunkserver_version = getMinChunkserverVersion(c, ChunkPartType(slice_type, slice_part));

	for (const auto &label_and_count : replicate_labels) {
		tried_to_replicate = true;

		if (jobsnorepbefore >= eventloop_time()) {
			break;
		}

		if (label_and_count.first == MediaLabel::kWildcard) {
			if (!replicationDelayInfoForAll.replicationAllowed(
			            label_and_count.second)) {
				continue;
			}
		} else if (!replicationDelayInfoForLabel[label_and_count.first].replicationAllowed(
		                   label_and_count.second)) {
			skipped_replications += label_and_count.second;
			continue;
		}

		// Get a list of possible destination servers
		int total_matching, returned_matching, temporarily_unavailable;
		matocsserv_getservers_lessrepl(label_and_count.first, min_chunkserver_version, MaxWriteRepl,
		                               ip_counter, servers, total_matching, returned_matching,
		                               temporarily_unavailable);

		// Find a destination server for replication -- the first one without a copy of 'c'
		matocsserventry *destination = nullptr;
		matocsserventry *backup_destination = nullptr;
		for (const auto &server : servers) {
			if (!chunkPresentOnServer(c, server)) {
				destination = server;
				break;
			}
			if (backup_destination == nullptr && !chunkPresentOnServer(c, slice_type, server)) {
				backup_destination = server;
			}
		}

		// If destination was not found, use a backup one, i.e. server where
		// there is a copy of this chunk, but from different slice.
		// Do it only if there are no available chunkservers in the system,
		// not if they merely reached their replication limit.
		if (destination == nullptr && temporarily_unavailable == 0) {
			// there are no servers which are expected to be available soon,
			// so backup server must be used
			destination = backup_destination;
		}

		if (destination == nullptr) {
			// there is no server suitable for replication to be written to
			break;
		}

		if (!(label_and_count.first == MediaLabel::kWildcard ||
		      matocsserv_get_label(destination) == label_and_count.first)) {
			// found server doesn't match requested label
			if (total_matching > returned_matching) {
				// There is a server which matches the current label, but it has
				// exceeded the
				// replication limit. In this case we won't try to use servers with
				// non-matching
				// labels as our destination -- we will wait for that server to be
				// ready.
				skipped_replications += label_and_count.second;
				continue;
			}
			if (!RebalancingBetweenLabels && !c->isEndangered()
			    && calc.isSafeEnoughToWrite(gRedundancyLevel)) {
				// Chunk is not endangered, so we should prevent label spilling.
				// Only endangered chunks will be replicated across labels.
				skipped_replications += label_and_count.second;
				continue;
			}
			if (valid_parts_count + skipped_replications >= expected_copies) {
				// Don't create copies on non-matching servers if there already are
				// enough replicas.
				continue;
			}
		}

		if (tryReplication(c, ChunkPartType(slice_type, slice_part), destination)) {
			inforec_.done.copy_undergoal++;
			return true;
		} else {
			// There is no server suitable for replication to be read from
			skipped_replications += label_and_count.second;
			break;  // there's no need to analyze other labels if there's no free source
			        // server
		}
	}
	if (tried_to_replicate) {
		inforec_.notdone.copy_undergoal++;
		// Enqueue chunk again only if it was taken directly from endangered chunks queue
		// to avoid repetitions. If it was taken from chunk hashmap, inEndangeredQueue bit
		// would be still up.
		if (gEndangeredChunksServingLimit > 0 && Chunk::endangeredChunks.size() < gEndangeredChunksMaxCapacity
			&& !c->inEndangeredQueue && calc.getState() == ChunksAvailabilityState::kEndangered) {
			c->inEndangeredQueue = 1;
			Chunk::endangeredChunks.push_back(c);
		}
	}

	return false;
}

bool ChunkWorker::removeUnneededChunkPart(Chunk *c, Goal::Slice::Type slice_type, int slice_part,
					ChunkCopiesCalculator &calc, const IpCounter &ip_counter) {
	Goal::Slice::Labels remove_pool = calc.getRemovePool(slice_type, slice_part);
	if (remove_pool.empty()) {
		return false;
	}

	ChunkPart *candidate = nullptr;
	bool candidate_todel = false;
	int candidate_occurrence = 0;
	double candidate_usage = std::numeric_limits<double>::lowest();

	for (auto &part : c->parts) {
		if (!part.is_valid() || part.type != ChunkPartType(slice_type, slice_part)) {
			continue;
		}
		if (matocsserv_deletion_counter(part.server()) >= TmpMaxDel) {
			continue;
		}

		MediaLabel server_label = matocsserv_get_label(part.server());
		if (remove_pool.find(server_label) == remove_pool.end()) {
			continue;
		}

		bool is_todel = part.is_todel();
		double usage = matocsserv_get_usage(part.server());
		int occurrence = ip_counter.empty() ? 1 : ip_counter.at(matocsserv_get_servip(part.server()));

		if (std::make_tuple(is_todel, occurrence, usage) >
		      std::make_tuple(candidate_todel, candidate_occurrence, candidate_usage)) {
			candidate = &part;
			candidate_usage = usage;
			candidate_todel = is_todel;
			candidate_occurrence = occurrence;
		}
	}

	if (candidate &&
	    calc.canRemovePart(slice_type, slice_part, matocsserv_get_label(candidate->server()))) {
		// A backend that keeps durable published sets retires the copy through its own
		// coordinated transition: the set narrows first and the physical delete follows from
		// the durable debt, never ahead of the set that still names the copy.
		if (gChunkOperations->retireSurplusPart(candidate->server(), c->chunkid, candidate->type)) {
			inforec_.done.del_overgoal++;
			deleteDone_++;
			return true;
		}
		c->deleteCopy(*candidate);
		c->needVersionIncrease = 1;
		stats_deletions++;
		metrics::Counter::increment(metrics::Counter::Master::CHUNK_DELETE);
		matocsserv_send_deletechunk(candidate->server(), c->chunkid, 0, candidate->type);

		int overgoal_copies = calc.countPartsToMove(slice_type, slice_part).second;

		inforec_.done.del_overgoal++;
		deleteDone_++;
		inforec_.notdone.del_overgoal += overgoal_copies - 1;
		deleteNotDone_ += overgoal_copies - 1;

		return true;
	}

	return false;
}

bool ChunkWorker::rebalanceChunkParts(Chunk *c, ChunkCopiesCalculator &calc, bool only_todel, const IpCounter &ip_counter) {
	if(!only_todel) {
		double min_usage = sortedServers_.front().disk_usage;
		double max_usage = sortedServers_.back().disk_usage;
		if ((max_usage - min_usage) <= gAcceptableDifference) {
			return false;
		}
	}

	// Avoid same part types landing in the same chunkservers by randomizing...
	size_t partsSize = c->parts.size();
	std::vector<int> part_indices(partsSize);
	std::iota(part_indices.begin(), part_indices.end(), 0);
	std::shuffle(part_indices.begin(), part_indices.end(), kRandomEngine);

	// Consider each copy to be moved to a server with disk usage much less than actual.
	// There are at least two servers with a disk usage difference grater than
	// gAcceptableDifference, so it's worth checking.
	for (size_t i = 0; i < partsSize; ++i) {
		const auto &part = c->parts[part_indices[i]];
		if (!part.is_valid()) {
			continue;
		}

		if(only_todel && !part.is_todel()) {
			continue;
		}

		auto current_ip = matocsserv_get_servip(part.server());
		auto it = ip_counter.find(current_ip);
		auto current_ip_count = it != ip_counter.end() ? it->second : 0;

		MediaLabel current_copy_label = matocsserv_get_label(part.server());
		double current_copy_disk_usage = matocsserv_get_usage(part.server());
		// Look for a server that has disk usage much less than currentCopyDiskUsage.
		// If such a server exists consider creating a new copy of this chunk there.
		// First, choose all possible candidates for the destination server: we consider
		// only
		// servers with the same label is rebalancing between labels if turned off or the
		// goal
		// requires our copy to exist on a server labeled 'currentCopyLabel'.
		bool multi_label_rebalance =
		        RebalancingBetweenLabels &&
		        (current_copy_label == MediaLabel::kWildcard ||
		         calc.canMovePartToDifferentLabel(part.type.getSliceType(),
		                                          part.type.getSlicePart(),
		                                          current_copy_label));

		uint32_t min_chunkserver_version = getMinChunkserverVersion(c, part.type);

		const ServersWithUsage &sorted_servers =
		        multi_label_rebalance ? sortedServers_
		                              : labeledSortedServers_[current_copy_label];

		for (const auto &empty_server : sorted_servers) {
			if (matocsserv_is_killed(empty_server.server)) { continue; }

			if (!only_todel && gAvoidSameIpChunkservers) {
				auto empty_server_ip = matocsserv_get_servip(empty_server.server);
				auto it = ip_counter.find(empty_server_ip);
				auto empty_server_ip_count = it != ip_counter.end() ? it->second : 0;
				if (empty_server_ip != current_ip && empty_server_ip_count >= current_ip_count) {
					continue;
				}
			}

			if (!only_todel && empty_server.disk_usage >
			    current_copy_disk_usage - gAcceptableDifference) {
				break;  // No more suitable destination servers (next servers have
				        // higher usage)
			}
			if (matocsserv_get_version(empty_server.server) < min_chunkserver_version) {
				continue;
			}
			if (chunkPresentOnServer(c, part.type.getSliceType(), empty_server.server)) {
				continue;  // A copy is already here
			}
			if (matocsserv_replication_write_counter(empty_server.server) >= MaxWriteRepl) {
				continue;  // We can't create a new copy here
			}
			if (tryReplication(c, part.type, empty_server.server)) {
				inforec_.copy_rebalance++;
				return true;
			}
		}
	}

	return false;
}

bool ChunkWorker::rebalanceChunkPartsWithSameIp(Chunk *c, ChunkCopiesCalculator &calc, const IpCounter &ip_counter) {
	if (!gAvoidSameIpChunkservers) {
		return false;
	}

	for (const auto &part : c->parts) {
		if (!part.is_valid()) {
			continue;
		}

		auto current_ip = matocsserv_get_servip(part.server());
		auto it = ip_counter.find(current_ip);
		auto current_ip_count = it != ip_counter.end() ? it->second : 0;

		MediaLabel current_copy_label = matocsserv_get_label(part.server());

		bool multi_label_rebalance =
		        RebalancingBetweenLabels &&
		        (current_copy_label == MediaLabel::kWildcard ||
		         calc.canMovePartToDifferentLabel(part.type.getSliceType(),
		                                          part.type.getSlicePart(),
		                                          current_copy_label));

		uint32_t min_chunkserver_version = getMinChunkserverVersion(c, part.type);

		const ServersWithUsage &sorted_servers =
		        multi_label_rebalance ? sortedServers_
		                              : labeledSortedServers_[current_copy_label];

		ServersWithUsage sorted_by_ip_count;
		sorted_by_ip_count.resize(sorted_servers.size());
		counting_sort_copy(sorted_servers.begin(), sorted_servers.end(), sorted_by_ip_count.begin(),
			           [&ip_counter](const ServerWithUsage& elem) {
			                  auto ip = matocsserv_get_servip(elem.server);
			                  auto it = ip_counter.find(ip);
			                  return it != ip_counter.end() ? it->second : 0;
			           });

		for (const auto &empty_server : sorted_by_ip_count) {
			if (matocsserv_is_killed(empty_server.server)) { continue; }

			auto empty_server_ip = matocsserv_get_servip(empty_server.server);
			auto it = ip_counter.find(empty_server_ip);
			auto empty_server_ip_count = it != ip_counter.end() ? it->second : 0;
			if (empty_server_ip_count >= (current_ip_count - 1)) {
				break;
			}

			if (matocsserv_get_version(empty_server.server) < min_chunkserver_version) {
				continue;
			}
			if (chunkPresentOnServer(c, part.type.getSliceType(), empty_server.server)) {
				continue;  // A copy is already here
			}
			if (matocsserv_replication_write_counter(empty_server.server) >= MaxWriteRepl) {
				continue;  // We can't create a new copy here
			}
			if (tryReplication(c, part.type, empty_server.server)) {
				inforec_.copy_rebalance++;
				return true;
			}
		}
	}

	return false;
}


void ChunkWorker::doChunkJobs(Chunk *c, uint16_t serverCount) {
	// step 0. Update chunk's statistics
	// Useful e.g. if definitions of goals did change.
	chunk_handle_disconnected_copies(c);
	c->updateStats();
	if (serverCount == 0) {
		return;
	}

	int invalid_parts = 0;
	ChunkCopiesCalculator calc(c->getGoal());

	// Chunk is in degenerate state if it has more than 1 part
	// on the same chunkserver (i.e. 1 std and 1 xor)
	// TODO(sarna): this flat_set should be removed after
	// 'slists' are rewritten to use sensible data structures
	bool degenerate = false;
	flat_set<matocsserventry *, small_vector<matocsserventry *, 64>> servers;

	// step 1. calculate number of valid and invalid copies
	for (const auto &part : c->parts) {
		if (part.is_valid()) {
			calc.addPart(part.type, matocsserv_get_label(part.server()));
			if (!degenerate) {
				degenerate = servers.count(part.server()) > 0;
				servers.insert(part.server());
			}
		} else {
			++invalid_parts;
		}
	}
	calc.optimize(gUseLinearAssignmentOptimizer, &gLinearAssignmentCache);

	// step 1a. count number of chunk parts on servers with the same ip
	IpCounter ip_occurrence;
	if (gAvoidSameIpChunkservers) {
		for (auto &part : c->parts) {
			if (part.is_valid()) {
				++ip_occurrence[matocsserv_get_servip(part.server())];
			}
		}
	}

	// step 2. check number of copies
	if (c->isLost() && invalid_parts > 0 && c->fileCount() > 0) {
		safs_pretty_syslog(LOG_WARNING, "chunk %016" PRIx64 " has not enough valid parts (%d)"
		                   " consider repairing it manually", c->chunkid, invalid_parts);
		for (const auto &part : c->parts) {
			if (!part.is_valid()) {
				safs_pretty_syslog(LOG_NOTICE, "chunk %016" PRIx64 "_%08x - invalid part on (%s - ver:%08x)",
				c->chunkid, c->version, matocsserv_getstrip(part.server()), part.version);
			}
		}
		return;
	}

	// step 3. delete invalid parts
	deleteInvalidChunkParts(c);

	// step 4. return if chunk is during some operation
	if (c->operation != Chunk::NONE || (c->isLocked())) {
		return;
	}

	// step 5. check busy count
	for (const auto &part : c->parts) {
		if (part.is_busy()) {
			safs_pretty_syslog(LOG_WARNING, "chunk %016" PRIX64 " has unexpected BUSY copies",
			       c->chunkid);
			return;
		}
	}

	// step 6. delete unused chunk
	if (c->fileCount() == 0) {
		deleteAllChunkParts(c);
		return;
	}

	if (c->isLost()) {
		return;
	}

	// step 7. check if chunk needs any replication
	for (const auto &slice : calc.getTarget()) {
		for (int i = 0; i < slice.size(); ++i) {
			if (replicateChunkPart(c, slice.getType(), i, calc, ip_occurrence)) {
				return;
			}
		}
	}

	// Do not remove any parts if more than 1 part resides on 1 chunkserver
	if (degenerate && calc.countPartsToRecover() > 0) {
		return;
	}

	// step 8. if chunk has too many copies then delete some of them
	for (const auto &slice : calc.getAvailable()) {
		for (int i = 0; i < slice.size(); ++i) {
			std::pair<int, int> operations = calc.countPartsToMove(slice.getType(), i);
			if (operations.first > 0 || operations.second == 0) {
				// do not remove elements if some are missing
				continue;
			}

			if (removeUnneededChunkPart(c, slice.getType(), i, calc, ip_occurrence)) {
				return;
			}
		}
	}

	// step 9. If chunk has parts marked as "to delete" then move them to other servers
	if(rebalanceChunkParts(c, calc, true, ip_occurrence)) {
		return;
	}

	if (chunksinfo.notdone.copy_undergoal > 0 && chunksinfo.done.copy_undergoal > 0) {
		return;
	}

	// step 10. Move chunk parts residing on chunkservers with the same ip.
	if (rebalanceChunkPartsWithSameIp(c, calc, ip_occurrence)) {
		return;
	}

	// step 11. if there is too big difference between chunkservers then make copy of chunk from
	// a server with a high disk usage on a server with low disk usage
	if (rebalanceChunkParts(c, calc, false, ip_occurrence)) {
		return;
	}
}

bool ChunkWorker::deleteUnusedChunks() {
	while (stack_.node != nullptr) {
		chunk_handle_disconnected_copies(stack_.node);
		auto &bucket = gChunksMetadata->chunkhash[stack_.current_bucket];
		if (stack_.node->fileCount() == 0 && stack_.node->parts.empty()) {
			assert(stack_.current_bucket_index < bucket.size());
			assert(bucket[stack_.current_bucket_index] == stack_.node);

			if (stack_.current_bucket == gCurrentBucketInZombieLoop &&
			    stack_.current_bucket_index < gCurrentChunkInZombieLoopIndex) {
				--gCurrentChunkInZombieLoopIndex;
			}

			Chunk *toDelete = bucket[stack_.current_bucket_index];
			bucket.erase(bucket.begin() + static_cast<std::ptrdiff_t>(stack_.current_bucket_index));
			chunk_delete(toDelete);

			stack_.node =
			    getCurrentChunkInBucket(stack_.current_bucket, stack_.current_bucket_index);
		} else {
			++stack_.current_bucket_index;
			stack_.node =
			    getCurrentChunkInBucket(stack_.current_bucket, stack_.current_bucket_index);
		}

		if (stack_.watchdog.expired()) {
			return false;
		}
	}

	return true;
}

void ChunkWorker::mainLoop() {
	Chunk *c;

	auto updateSortedServersIfNeeded = [&]() {
		if (matocsserv_sorted_servers_need_refresh()) {
			updateSortedServers();
			matocsserv_sorted_servers_refresh_done();
		}
	};

	reenter(this) {
		stack_.work_limit.setMaxDuration(std::chrono::milliseconds(ChunksLoopTimeout));
		stack_.work_limit.start();
		stack_.watchdog.start();
		stack_.chunks_done_count = 0;
		stack_.buckets_done_count = 0;

		if (starttime + gOperationsDelayInit > eventloop_time()) {
			return;
		}

		double min_usage, max_usage;
		matocsserv_usagedifference(&min_usage, &max_usage, &stack_.usable_server_count,
		                           nullptr);

		if (min_usage > max_usage) {
			return;
		}

		doEverySecondTasks();

		if (jobsnorepbefore < eventloop_time()) {
			stack_.endangered_to_serve = gEndangeredChunksServingLimit;
			while (stack_.endangered_to_serve > 0 && !Chunk::endangeredChunks.empty()) {
				c = Chunk::endangeredChunks.front();
				Chunk::endangeredChunks.pop_front();
				// If queued chunk is obsolete (e.g. was freed while in queue),
				// do not proceed with chunk jobs.
				if (c->inEndangeredQueue == 1) {
					c->inEndangeredQueue = 0;
					doChunkJobs(c, stack_.usable_server_count);
				}
				--stack_.endangered_to_serve;

				if (stack_.watchdog.expired()) {
					yield;
					stack_.watchdog.start();
					updateSortedServersIfNeeded();
				}
			}
		}

		while (stack_.buckets_done_count < HashSteps &&
		       stack_.chunks_done_count < HashCPS) {
			if (stack_.current_bucket == 0) {
				doEveryLoopTasks();
			}

			if (stack_.watchdog.expired()) {
				yield;
				stack_.watchdog.start();
			}

			// delete unused chunks
			stack_.current_bucket_index = 0;
			stack_.node =
			    getCurrentChunkInBucket(stack_.current_bucket, stack_.current_bucket_index);
			while (!deleteUnusedChunks()) {
				yield;
				stack_.watchdog.start();
			}

			// regenerate usable_server_count
			matocsserv_usagedifference(nullptr, nullptr, &stack_.usable_server_count,
			                           nullptr);
			updateSortedServersIfNeeded();

			stack_.current_bucket_index = 0;
			stack_.node =
			    getCurrentChunkInBucket(stack_.current_bucket, stack_.current_bucket_index);
			while (stack_.node) {
				doChunkJobs(stack_.node, stack_.usable_server_count);
				++stack_.chunks_done_count;
				++stack_.current_bucket_index;
				stack_.node =
				    getCurrentChunkInBucket(stack_.current_bucket, stack_.current_bucket_index);

				if (stack_.watchdog.expired()) {
					yield;
					stack_.watchdog.start();
					matocsserv_usagedifference(nullptr, nullptr,
					                           &stack_.usable_server_count,
					                           nullptr);
					updateSortedServersIfNeeded();
				}
			}

			stack_.current_bucket +=
			        123;  // if HASHSIZE is any power of 2 then any odd number is
			              // good here
			stack_.current_bucket %= kChunkHashSize;
			++stack_.buckets_done_count;

			if (stack_.work_limit.expired()) {
				break;
			}
		}
	}
}

static std::unique_ptr<ChunkWorker> gChunkWorker;

void chunk_jobs_main(void) {
	if (gChunkWorker->is_complete()) {
		gChunkWorker->reset();
	}
}

void chunk_jobs_process_bit(void) {
	if (!gChunkWorker->is_complete()) {
		gChunkWorker->mainLoop();
		if (!gChunkWorker->is_complete()) {
			eventloop_make_next_poll_nonblocking();
		}
	}
}

#endif

[[maybe_unused]] constexpr uint32_t kSerializedChunkSizeNoLockId = 16;
constexpr uint32_t kSerializedChunkSizeWithLockId = 20;
#define CHUNKCNT 1000

#ifdef METARESTORE

void chunk_dump(void) {
	uint32_t i;

	for (i = 0; i < kChunkHashSize; i++) {
		for (Chunk *chunk : gChunksMetadata->chunkhash[i]) {
			printf("*|i:%016" PRIX64 "|v:%08" PRIX32 "|g:%" PRIu8 "|t:%10" PRIu32 "\n",
			       chunk->chunkid, chunk->version, chunk->highestIdGoal(), chunk->lockedto);
		}
	}
}

#endif

void chunk_add_from_initial_metadata_load(uint64_t chunkId, uint32_t chunkVersion,
                                          uint32_t lockedTo, uint32_t lockId) {
	Chunk *chunk = chunk_new(chunkId, chunkVersion);
	chunk->lockedto = lockedTo;
	chunk->lockid = lockId;
}

bool chunksLoadFromFile(MetadataLoader::Options options) {
	const uint8_t *ptr = options.metadataFile->seek(options.offset);
	uint64_t nextChunkId = get64bit(&ptr);
	if (!ChunksMetadata::setNextChunkId(nextChunkId)) {
		safs::log_warn(
		    "Failed to set next chunk ID from metadata file, stored value: {} is less than current: {}",
		    nextChunkId, ChunksMetadata::getNextChunkId());
	}
	options.offset = options.metadataFile->offset(ptr);

	while (true) {
		uint64_t chunkId = get64bit(&ptr);
		uint32_t version;
		get32bit(&ptr, version);
		uint32_t lockedTo;
		get32bit(&ptr, lockedTo);
		uint32_t lockId = 0;
		if (options.loadLockIds) {
			get32bit(&ptr, lockId);
		}
		if (chunkId > 0) {
			Chunk * chunk = chunk_new(chunkId, version);
			chunk->lockedto = lockedTo;
			chunk->lockid = lockId;
			continue;
		}
		options.offset = options.metadataFile->offset(ptr);
		return (version == 0 && lockedTo == 0);
	}
}

void chunk_store(FILE *fd) {
	passert(gChunksMetadata);
	uint8_t hdr[8];
	uint8_t storebuff[kSerializedChunkSizeWithLockId * CHUNKCNT];
	uint8_t *ptr;
	uint32_t i, j;
	// chunkdata
	uint64_t chunkid;
	uint32_t version;
	uint32_t lockedto, lockid;
	ptr = hdr;
	put64bit(&ptr, ChunksMetadata::getNextChunkId());
	if (fwrite(hdr,1,8,fd)!=(size_t)8) {
		return;
	}
	j=0;
	ptr = storebuff;
	for (i=0 ; i<kChunkHashSize ; i++) {
		for (Chunk *c : gChunksMetadata->chunkhash[i]) {
#ifndef METARESTORE
			chunk_handle_disconnected_copies(c);
#endif
			chunkid = c->chunkid;
			put64bit(&ptr,chunkid);
			version = c->version;
			put32bit(&ptr,version);
			lockedto = c->lockedto;
			lockid = c->lockid;
			put32bit(&ptr,lockedto);
			put32bit(&ptr,lockid);
			j++;
			if (j==CHUNKCNT) {
				size_t writtenBlockSize = kSerializedChunkSizeWithLockId * CHUNKCNT;
				if (fwrite(storebuff, 1, writtenBlockSize, fd) != writtenBlockSize) {
					return;
				}
				j=0;
				ptr = storebuff;
			}
		}
	}
	memset(ptr, 0, kSerializedChunkSizeWithLockId);
	j++;
	size_t writtenBlockSize = kSerializedChunkSizeWithLockId * j;
	if (fwrite(storebuff, 1, writtenBlockSize, fd) != writtenBlockSize) {
		return;
	}
}

void chunk_unload(void) {
	delete gChunksMetadata;
	gChunksMetadata = nullptr;
}

void chunk_newfs(void) {
#ifndef METARESTORE
	Chunk::count = 0;
#endif
	ChunksMetadata::setNextChunkId(1);
}

#ifndef METARESTORE
void chunk_become_master() {
	starttime = eventloop_time();
	jobsnorepbefore = starttime + gOperationsDelayInit;
	gChunkWorker = std::unique_ptr<ChunkWorker>(new ChunkWorker());
	gChunkLoopEventHandle = eventloop_timeregister_ms(ChunksLoopPeriod, chunk_jobs_main);
	eventloop_eachloopregister(chunk_jobs_process_bit);
	return;
}

void chunk_reload(void) {
	uint32_t repl;
	uint32_t looptime;

	// Set deprecated values first and override them if newer definition is found
	gOperationsDelayInit = cfg_getuint32("REPLICATIONS_DELAY_INIT", 300);
	gOperationsDelayDisconnect = cfg_getuint32("REPLICATIONS_DELAY_DISCONNECT", 3600);
	gOperationsDelayInit = cfg_getuint32("OPERATIONS_DELAY_INIT", gOperationsDelayInit);
	gOperationsDelayDisconnect = cfg_getuint32("OPERATIONS_DELAY_DISCONNECT", gOperationsDelayDisconnect);
	gAvoidSameIpChunkservers = cfg_getuint32("AVOID_SAME_IP_CHUNKSERVERS", 0);
	gRedundancyLevel = cfg_getuint32("REDUNDANCY_LEVEL", 0);
	gUseLinearAssignmentOptimizer = cfg_getuint32("USE_LINEAR_ASSIGNMENT_OPTIMIZER", 1);
	gUseChunkserverSideChunkLock = cfg_getuint32("USE_CHUNKSERVER_SIDE_CHUNK_LOCK", 0);

	uint32_t disableChunksDel = cfg_getuint32("DISABLE_CHUNKS_DEL", 0);
	if (disableChunksDel) {
		MaxDelSoftLimit = MaxDelHardLimit = 0;
	} else {
		uint32_t oldMaxDelSoftLimit = MaxDelSoftLimit;
		uint32_t oldMaxDelHardLimit = MaxDelHardLimit;

		MaxDelSoftLimit = cfg_getuint32("CHUNKS_SOFT_DEL_LIMIT",10);
		if (cfg_isdefined("CHUNKS_HARD_DEL_LIMIT")) {
			MaxDelHardLimit = cfg_getuint32("CHUNKS_HARD_DEL_LIMIT",25);
			if (MaxDelHardLimit<MaxDelSoftLimit) {
				MaxDelSoftLimit = MaxDelHardLimit;
				safs_pretty_syslog(LOG_WARNING,"CHUNKS_SOFT_DEL_LIMIT is greater than CHUNKS_HARD_DEL_LIMIT - using CHUNKS_HARD_DEL_LIMIT for both");
			}
		} else {
			MaxDelHardLimit = 3 * MaxDelSoftLimit;
		}
		if (MaxDelSoftLimit==0) {
			MaxDelSoftLimit = oldMaxDelSoftLimit;
			MaxDelHardLimit = oldMaxDelHardLimit;
		}
	}
	if (TmpMaxDelFrac<MaxDelSoftLimit) {
		TmpMaxDelFrac = MaxDelSoftLimit;
	}
	if (TmpMaxDelFrac>MaxDelHardLimit) {
		TmpMaxDelFrac = MaxDelHardLimit;
	}
	if (TmpMaxDel<MaxDelSoftLimit) {
		TmpMaxDel = MaxDelSoftLimit;
	}
	if (TmpMaxDel>MaxDelHardLimit) {
		TmpMaxDel = MaxDelHardLimit;
	}

	repl = cfg_getuint32("CHUNKS_WRITE_REP_LIMIT", 2);
	if (repl > 0) {
		MaxWriteRepl = repl;
	}

	repl = cfg_getuint32("CHUNKS_READ_REP_LIMIT", 10);
	if (repl > 0) {
		MaxReadRepl = repl;
	}

	ChunksLoopPeriod = cfg_get_minmaxvalue<uint32_t>("CHUNKS_LOOP_PERIOD", 1000, MINCHUNKSLOOPPERIOD, MAXCHUNKSLOOPPERIOD);
	if (gChunkLoopEventHandle) {
		eventloop_timechange_ms(gChunkLoopEventHandle, ChunksLoopPeriod);
	}

	repl = cfg_get_minmaxvalue<uint32_t>("CHUNKS_LOOP_MAX_CPU", 60, MINCHUNKSLOOPCPU, MAXCHUNKSLOOPCPU);
	ChunksLoopTimeout = repl * ChunksLoopPeriod / 100;

	if (cfg_isdefined("CHUNKS_LOOP_TIME")) {
		looptime = cfg_get_minmaxvalue<uint32_t>("CHUNKS_LOOP_TIME", 300, MINLOOPTIME, MAXLOOPTIME);
		uint64_t scaled_looptime = std::max((uint64_t)1000 * looptime / ChunksLoopPeriod, (uint64_t)1);
		HashSteps = 1 + ((kChunkHashSize) / scaled_looptime);
		HashCPS   = 0xFFFFFFFF;
	} else {
		looptime = cfg_get_minmaxvalue<uint32_t>("CHUNKS_LOOP_MIN_TIME", 300, MINLOOPTIME, MAXLOOPTIME);
		HashCPS = cfg_get_minmaxvalue<uint32_t>("CHUNKS_LOOP_MAX_CPS", 100000, MINCPS, MAXCPS);
		uint64_t scaled_looptime = std::max((uint64_t)1000 * looptime / ChunksLoopPeriod, (uint64_t)1);
		HashSteps = 1 + ((kChunkHashSize) / scaled_looptime);
		HashCPS   = (uint64_t)ChunksLoopPeriod * HashCPS / 1000;
	}
	double endangeredChunksPriority = cfg_ranged_get("ENDANGERED_CHUNKS_PRIORITY", 0.0, 0.0, 1.0);
	gEndangeredChunksServingLimit = HashSteps * endangeredChunksPriority;
	gEndangeredChunksMaxCapacity = cfg_get("ENDANGERED_CHUNKS_MAX_CAPACITY", static_cast<uint64_t>(1024*1024UL));
	gAcceptableDifference = cfg_ranged_get("ACCEPTABLE_DIFFERENCE",0.1, 0.001, 10.0);
	RebalancingBetweenLabels = cfg_getuint32("CHUNKS_REBALANCING_BETWEEN_LABELS", 0) == 1;
}
#endif

int chunk_strinit(void) {
	gChunksMetadata = new ChunksMetadata;

#ifndef METARESTORE
	Chunk::count = 0;
	for (int i = 0; i < CHUNK_MATRIX_SIZE; ++i) {
		for (int j = 0; j < CHUNK_MATRIX_SIZE; ++j) {
			Chunk::allFullChunkCopies[i][j] = 0;
		}
	}
	Chunk::allChunksAvailability = ChunksAvailabilityState();
	Chunk::allChunksReplicationState = ChunksReplicationState();

	uint32_t disableChunksDel = cfg_getuint32("DISABLE_CHUNKS_DEL", 0);
	gOperationsDelayInit = cfg_getuint32("REPLICATIONS_DELAY_INIT", 300);
	gOperationsDelayDisconnect = cfg_getuint32("REPLICATIONS_DELAY_DISCONNECT", 3600);
	gOperationsDelayInit = cfg_getuint32("OPERATIONS_DELAY_INIT", gOperationsDelayInit);
	gOperationsDelayDisconnect = cfg_getuint32("OPERATIONS_DELAY_DISCONNECT", gOperationsDelayDisconnect);
	gAvoidSameIpChunkservers = cfg_getuint32("AVOID_SAME_IP_CHUNKSERVERS", 0);
	gRedundancyLevel = cfg_getuint32("REDUNDANCY_LEVEL", 0);
	gUseLinearAssignmentOptimizer = cfg_getuint32("USE_LINEAR_ASSIGNMENT_OPTIMIZER", 1);
	gUseChunkserverSideChunkLock = cfg_getuint32("USE_CHUNKSERVER_SIDE_CHUNK_LOCK", 0);

	if (disableChunksDel) {
		MaxDelHardLimit = MaxDelSoftLimit = 0;
	} else {
		MaxDelSoftLimit = cfg_getuint32("CHUNKS_SOFT_DEL_LIMIT",10);
		if (cfg_isdefined("CHUNKS_HARD_DEL_LIMIT")) {
			MaxDelHardLimit = cfg_getuint32("CHUNKS_HARD_DEL_LIMIT",25);
			if (MaxDelHardLimit<MaxDelSoftLimit) {
				MaxDelSoftLimit = MaxDelHardLimit;
				safs_pretty_syslog(LOG_WARNING, "%s: CHUNKS_SOFT_DEL_LIMIT is greater than "
					"CHUNKS_HARD_DEL_LIMIT - using CHUNKS_HARD_DEL_LIMIT for both",
					cfg_filename().c_str());
			}
		} else {
			MaxDelHardLimit = 3 * MaxDelSoftLimit;
		}
		if (MaxDelSoftLimit == 0) {
			throw InitializeException(cfg_filename() + ": CHUNKS_SOFT_DEL_LIMIT is zero");
		}
	}
	TmpMaxDelFrac = MaxDelSoftLimit;
	TmpMaxDel = MaxDelSoftLimit;
	MaxWriteRepl = cfg_getuint32("CHUNKS_WRITE_REP_LIMIT",2);
	MaxReadRepl = cfg_getuint32("CHUNKS_READ_REP_LIMIT",10);
	if (MaxReadRepl==0) {
		throw InitializeException(cfg_filename() + ": CHUNKS_READ_REP_LIMIT is zero");
	}
	if (MaxWriteRepl==0) {
		throw InitializeException(cfg_filename() + ": CHUNKS_WRITE_REP_LIMIT is zero");
	}

	ChunksLoopPeriod  = cfg_get_minmaxvalue<uint32_t>("CHUNKS_LOOP_PERIOD", 1000, MINCHUNKSLOOPPERIOD, MAXCHUNKSLOOPPERIOD);
	uint32_t repl = cfg_get_minmaxvalue<uint32_t>("CHUNKS_LOOP_MAX_CPU", 60, MINCHUNKSLOOPCPU, MAXCHUNKSLOOPCPU);
	ChunksLoopTimeout = repl * ChunksLoopPeriod / 100;

	uint32_t looptime;
	if (cfg_isdefined("CHUNKS_LOOP_TIME")) {
		safs_pretty_syslog(LOG_WARNING,
				"%s: defining loop time by CHUNKS_LOOP_TIME option is "
				"deprecated - use CHUNKS_LOOP_MAX_CPS and CHUNKS_LOOP_MIN_TIME",
				cfg_filename().c_str());
		looptime = cfg_get_minmaxvalue<uint32_t>("CHUNKS_LOOP_TIME", 300, MINLOOPTIME, MAXLOOPTIME);
		uint64_t scaled_looptime = std::max((uint64_t)1000 * looptime / ChunksLoopPeriod, (uint64_t)1);
		HashSteps = 1 + ((kChunkHashSize) / scaled_looptime);
		HashCPS   = 0xFFFFFFFF;
	} else {
		looptime = cfg_get_minmaxvalue<uint32_t>("CHUNKS_LOOP_MIN_TIME", 300, MINLOOPTIME, MAXLOOPTIME);
		HashCPS = cfg_get_minmaxvalue<uint32_t>("CHUNKS_LOOP_MAX_CPS", 100000, MINCPS, MAXCPS);
		uint64_t scaled_looptime = std::max((uint64_t)1000 * looptime / ChunksLoopPeriod, (uint64_t)1);
		HashSteps = 1 + ((kChunkHashSize) / scaled_looptime);
		HashCPS   = (uint64_t)ChunksLoopPeriod * HashCPS / 1000;
	}
	double endangeredChunksPriority = cfg_ranged_get("ENDANGERED_CHUNKS_PRIORITY", 0.0, 0.0, 1.0);
	gEndangeredChunksServingLimit = HashSteps * endangeredChunksPriority;
	gEndangeredChunksMaxCapacity = cfg_get("ENDANGERED_CHUNKS_MAX_CAPACITY", static_cast<uint64_t>(1024*1024UL));
	gAcceptableDifference = cfg_ranged_get("ACCEPTABLE_DIFFERENCE", 0.1, 0.001, 10.0);
	RebalancingBetweenLabels = cfg_getuint32("CHUNKS_REBALANCING_BETWEEN_LABELS", 0) == 1;
	eventloop_reloadregister(chunk_reload);
	metadataserver::registerFunctionCalledOnPromotion(chunk_become_master);
	eventloop_eachloopregister(chunk_clean_zombie_servers_a_bit);
	if (metadataserver::isMaster()) {
		chunk_become_master();
	}
#endif
	return 1;
}
