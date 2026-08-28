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

#include "chunkserver/masterconn.h"

#include <netinet/in.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <syslog.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <ctime>
#include <chrono>
#include <optional>
#include <random>

#include "chunkserver/bgjobs.h"
#include "chunkserver/evidence_outbox.h"
#include "chunkserver/hddspacemgr.h"
#include "chunkserver/master_connection.h"
#include "chunkserver/network_main_thread.h"
#include "chunkserver/test_job_faults.h"
#include "common/event_loop.h"
#include "common/human_readable_format.h"
#include "common/massert.h"
#include "common/network_address.h"
#include "common/random.h"
#include "common/session_authority_clock.h"
#include "common/sockets.h"
#include "common/test_event_stream.h"
#include "common/test_packet_faults.h"
#include "config/cfg.h"
#include "devtools/request_log.h"
#include "protocol/SFSCommunication.h"
#include "protocol/cstoma.h"
#include "slogger/slogger.h"

//  From config
static std::string gMasterHost;
static std::string gMasterPort;
static bool gEnableLoadFactor;
static bool gDistributedMode;

static const uint64_t kSendStatusDelay = 5;

//  JobPool shared between all connections to MDSs
static std::shared_ptr<MasterJobPool> gJobPool;
static std::shared_ptr<MasterJobPool> gReplicationJobPool;

//  Connections to every known MDS. Index 0 is the configured one (MASTER_HOST); the rest
//  are dialed from the cluster views the MDSs push. Entries are never erased once created
//  (job callbacks may hold pointers): a connection whose identity moved elsewhere is
//  retired instead and never redials.
static std::vector<std::unique_ptr<MasterConn>> gMasterConns;

//  Connections created while iterating gMasterConns (a cluster view arrives inside
//  servePoll): admitted into gMasterConns between event-loop iterations, never during
//  one, so no iterator over the live set is ever invalidated.
static std::vector<std::unique_ptr<MasterConn>> gPendingMasterConns;

static int gJobFD{-1};              ///< File descriptor for the job pool notifications
static int32_t gJobFDpDescPos{-1};  ///< Position in the pollfd array for the job pool notifications
static int gReplicationJobFD{-1};   ///< File descriptor for the replication job pool notifications
/// Position in the pollfd array for the replication job pool notifications
static int32_t gReplicationJobFDpDescPos{-1};

constexpr uint32_t kDefaultNumberOfWorkers = 10;
constexpr uint32_t kMinNumberOfWorkers = 2;
static uint32_t gNumberOfWorkers = kDefaultNumberOfWorkers;

constexpr uint32_t kDefaultReplicationNumberOfWorkers = 5;
constexpr uint32_t kMinReplicationNumberOfWorkers = 1;
static uint32_t gReplicationNumberOfWorkers = kDefaultReplicationNumberOfWorkers;

static void *gReconnectHook;

static void masterconn_admit_pending();

//  Stats
static uint32_t stats_maxjobscnt = 0;

//  Durable stable identity. The daemon runs chdir'ed into its DATA_PATH (csstats.sfs is
//  opened the same way), so the file is relative. The final design stamps every chunk
//  data directory; the working directory stand-in keeps the same observable contract.
static const char *kStableIdFilename = "chunkserverid";
static uint32_t gStableId = 0;
static uint64_t gChunkserverIncarnation = 0;

uint32_t masterconn_stable_id() { return gStableId; }
uint64_t masterconn_incarnation() { return gChunkserverIncarnation; }

static void masterconn_load_stable_id() {
	FILE *file = fopen(kStableIdFilename, "re");
	if (file == nullptr) { return; }

	unsigned long parsed = 0;
	const int fields = fscanf(file, "%lu", &parsed);
	fclose(file);

	if (fields != 1 || parsed == 0 || parsed > UINT32_MAX) {
		safs::log_err("MasterConn: unparseable stable id file '{}'", kStableIdFilename);
		return;
	}
	gStableId = static_cast<uint32_t>(parsed);
	safs::log_info("MasterConn: loaded stable id {}", gStableId);
}

/// Stamps the id with the write-fsync-rename idiom so a crash never leaves a torn file.
static bool masterconn_persist_stable_id(uint32_t stableId) {
	const std::string tmpPath = std::string(kStableIdFilename) + ".tmp";
	FILE *file = fopen(tmpPath.c_str(), "we");
	if (file == nullptr) {
		safs::log_err("MasterConn: cannot write stable id file '{}'", tmpPath);
		return false;
	}

	const bool written =
	    fprintf(file, "%u\n", stableId) > 0 && fflush(file) == 0 && fsync(fileno(file)) == 0;
	fclose(file);
	if (!written || rename(tmpPath.c_str(), kStableIdFilename) != 0) {
		safs::log_err("MasterConn: cannot persist stable id file '{}'", kStableIdFilename);
		unlink(tmpPath.c_str());
		return false;
	}
	return true;
}

bool masterconn_adopt_stable_id(uint32_t stableId) {
	if (stableId == 0) { return true; }
	if (gStableId == stableId) { return true; }
	if (gStableId != 0) {
		safs::log_err("MasterConn: registration assigned stable id {} but this server owns {}",
		              stableId, gStableId);
		return false;
	}

	if (!masterconn_persist_stable_id(stableId)) { return false; }
	gStableId = stableId;
	test_event_stream::setIdentity(gStableId, gChunkserverIncarnation);
	safs::log_info("MasterConn: adopted stable id {}", gStableId);
	return true;
}

//  Session authority state: the last accepted claim tuple and its cutoff reserve. The
//  serving flag is the one value the client-plane worker threads read; everything else
//  is event-loop only. Legacy mode never flips the flag off.
static std::optional<ChunkserverSessionLease> gAcceptedLease;
static uint64_t gCutoffReserveSeconds = 0;

/// The furthest ahead of this chunkserver's own clock an incoming lease deadline may sit. A
/// cluster is only as safe against clock error as the receiver's willingness to refuse a promise
/// it cannot justify, and a sender with a wrong clock believes its own.
static uint64_t gMaxAcceptedLeaseSeconds = 900;

/// How long a renewer nomination may stay in flight before it is treated as failed. An
/// election answered by silence is indistinguishable from one answered slowly, so the only
/// thing that separates them is a bound this chunkserver sets for itself.
static uint64_t gRenewerNominationTimeoutSeconds = 15;

/// The shortest an attempt may ever be given, however little of the serving window is left.
static constexpr uint64_t kMinNominationAttemptSeconds = 2;

/// The MDS whose nomination most recently expired unanswered, passed over by the next round
/// so a silent candidate cannot absorb every attempt. Cleared once a renewer is confirmed.
static uint32_t gLastExpiredRenewerMdsId = 0;
static std::atomic<bool> gSessionServingAllowed{true};

// Cutoff is one way. A deadline this process has already retired stays retired, because
// its peers and the durable row have counted on that, so only a newly accepted committed
// tuple may serve again. Without this a clock that moves back would resume work alone.
static bool gSessionCutoffLatched = false;

/// The era admitting work, and the counter that mints them. Legacy mode never leaves era 1,
/// because masterconn_session_cutoff_check() returns before touching either of these.
static std::atomic<uint64_t> gServingEra{1};
static uint64_t gNextServingEra = 2;

/// The claim sequence at which the current era began, which is how an era gets a name the
/// metadata server also knows.
///
/// An era is minted here and counted here, so no metadata server can name one. A claim sequence
/// is durable and both sides see it, so a command stamped with the claim it was issued under can
/// be placed relative to the cutoff: anything stamped with a claim older than the one that
/// readmitted this process was issued before the cutoff and must not run. Anything stamped at or
/// after it was issued by an authority this era still holds, which is what keeps an ordinary
/// renewal from aborting healthy work.
static uint64_t gEraStartClaimSequence = 0;

bool masterconn_session_serving_allowed() {
	return gSessionServingAllowed.load(std::memory_order_relaxed);
}

uint64_t masterconn_serving_era() { return gServingEra.load(std::memory_order_relaxed); }

bool masterconn_era_is_current(uint64_t era) {
	return era != 0 && era == gServingEra.load(std::memory_order_relaxed);
}

bool masterconn_claim_admits_work(uint64_t claimSequence) {
	if (gServingEra.load(std::memory_order_relaxed) == 0) { return false; }
	return claimSequence >= gEraStartClaimSequence;
}

uint64_t masterconn_claim_sequence() {
	return gAcceptedLease.has_value() ? gAcceptedLease->claimSequence : 0;
}

/// The conservative holder cutoff from D4: serving requires the session authority
/// clock to be strictly before the accepted deadline minus the cutoff reserve. A
/// missing tuple or an unreadable clock fails closed.
static bool masterconn_session_window_open() {
	if (!gAcceptedLease.has_value()) { return false; }
	const auto now = session_authority_clock::now();
	if (!now.has_value()) { return false; }
	const uint64_t deadline = gAcceptedLease->leaseDeadline;
	return deadline > gCutoffReserveSeconds && *now < deadline - gCutoffReserveSeconds;
}

/// The session authority instant at which the conservative holder cutoff retires the current
/// claim. Empty when no accepted tuple bounds it yet, which is the bootstrap case.
static std::optional<uint64_t> masterconn_session_cutoff_instant() {
	if (!gAcceptedLease.has_value()) { return std::nullopt; }
	const uint64_t deadline = gAcceptedLease->leaseDeadline;
	if (deadline <= gCutoffReserveSeconds) { return std::nullopt; }
	return deadline - gCutoffReserveSeconds;
}

/// Each-loop transition detector: flips the serving flag and emits the H9 boundary
/// events exactly on the transitions, so cutoff entry and readmission are observable.
static void masterconn_session_cutoff_check() {
	if (!gDistributedMode) { return; }
	const bool allowed = !gSessionCutoffLatched && masterconn_session_window_open();
	const bool wasAllowed = gSessionServingAllowed.exchange(allowed, std::memory_order_relaxed);
	if (wasAllowed == allowed) { return; }
	if (!allowed) {
		gSessionCutoffLatched = true;
		// The era ends here, permanently. Work already admitted under it may finish
		// physically; what it may no longer do is produce a positive result, and that is a
		// question about the work's own era rather than about this process.
		const uint64_t retired = gServingEra.exchange(0, std::memory_order_relaxed);
		safs::log_warn("MasterConn: session authority lost; refusing new client and control work");
		if (test_event_stream::enabled()) {
			test_event_stream::emit(
			    "session_cutoff_entered",
			    {{"claim_sequence", gAcceptedLease ? gAcceptedLease->claimSequence : 0},
			     {"claim_deadline", gAcceptedLease ? gAcceptedLease->leaseDeadline : 0},
			     {"retired_era", retired}});
		}
		return;
	}

	// Exhaustion fails closed. Staying retired costs this process its share of the cluster's
	// work; handing out an era number a previous stretch of authority already used would cost
	// the cluster the only thing that tells the two apart.
	if (gNextServingEra == std::numeric_limits<uint64_t>::max()) {
		gSessionCutoffLatched = true;
		gSessionServingAllowed.store(false, std::memory_order_relaxed);
		gServingEra.store(0, std::memory_order_relaxed);
		safs::log_err("MasterConn: serving eras exhausted; refusing to serve until restart");
		if (test_event_stream::enabled()) {
			test_event_stream::emit("serving_era_exhausted", {{"next_era", gNextServingEra}});
		}
		return;
	}
	const uint64_t era = gNextServingEra++;
	gServingEra.store(era, std::memory_order_relaxed);
	gEraStartClaimSequence = gAcceptedLease->claimSequence;
	safs::log_info("MasterConn: session authority restored; serving again under era {}", era);
	if (test_event_stream::enabled()) {
		test_event_stream::emit("session_readmitted",
		                        {{"claim_sequence", gAcceptedLease->claimSequence},
		                         {"claim_deadline", gAcceptedLease->leaseDeadline},
		                         {"serving_era", era}});
	}
}

static const char *leaseTupleAcceptanceName(LeaseTupleAcceptance verdict) {
	switch (verdict) {
	case LeaseTupleAcceptance::kAcceptNewer: return "accept_newer";
	case LeaseTupleAcceptance::kAcceptDuplicate: return "accept_duplicate";
	case LeaseTupleAcceptance::kRejectMalformed: return "reject_malformed";
	case LeaseTupleAcceptance::kRejectWrongHolder: return "reject_wrong_holder";
	case LeaseTupleAcceptance::kRejectWrongSender: return "reject_wrong_sender";
	case LeaseTupleAcceptance::kRejectLowerSequence: return "reject_lower_sequence";
	case LeaseTupleAcceptance::kRejectSameSequenceConflict: return "reject_same_sequence_conflict";
	case LeaseTupleAcceptance::kRejectImplausibleDeadline: return "reject_implausible_deadline";
	}
	return "reject_unknown";
}

LeaseTupleAcceptance masterconn_accept_session_lease(const ChunkserverSessionLease &incoming,
                                                     uint64_t cutoffReserveSeconds,
                                                     uint32_t senderMdsId,
                                                     uint64_t senderMdsIncarnation,
                                                     const char *source) {
	// The reserve arrives beside the tuple on the wire but is judged as part of it, so a
	// redelivery carrying a different one is a conflict rather than a quiet cutoff change.
	ChunkserverSessionLease judged = incoming;
	judged.cutoffReserveSeconds = cutoffReserveSeconds;

	const auto verdict = evaluateLeaseTuple(gAcceptedLease, judged, gStableId,
	                                        gChunkserverIncarnation, senderMdsId,
	                                        senderMdsIncarnation, session_authority_clock::now(),
	                                        gMaxAcceptedLeaseSeconds);
	if (test_event_stream::enabled()) {
		test_event_stream::emit(
		    leaseTupleAccepted(verdict) ? "lease_packet_accepted" : "lease_packet_rejected",
		    {{"source", source},
		     {"verdict", leaseTupleAcceptanceName(verdict)},
		     {"claim_sequence", incoming.claimSequence},
		     {"claim_deadline", incoming.leaseDeadline},
		     {"sender_mds_id", senderMdsId}});
	}
	if (verdict == LeaseTupleAcceptance::kAcceptNewer) {
		gAcceptedLease = judged;
		gCutoffReserveSeconds = judged.cutoffReserveSeconds;
		// The only way out of cutoff: a committed tuple this process has just accepted.
		gSessionCutoffLatched = false;
		masterconn_session_cutoff_check();
	} else if (verdict == LeaseTupleAcceptance::kAcceptDuplicate) {
		// Nothing to adopt: a duplicate is equal to what is already accepted, reserve included.
	} else {
		safs::log_warn("MasterConn: {} lease tuple from mds {} rejected: {}", source, senderMdsId,
		               leaseTupleAcceptanceName(verdict));
	}
	return verdict;
}

/// Applies the H5 and H9 test knobs; called at init and on every reload so a harness
/// can cross an authority deadline in a live process. Production configs leave all of
/// these unset and get zero behavior change.
static void masterconn_configure_test_hooks() {
	std::string streamPath = cfg_getstring("TEST_EVENT_STREAM_PATH", "");
	if (streamPath.empty() && cfg_getuint32("TEST_EVENT_STREAM", 0) != 0) {
		streamPath = "./test-events.jsonl";
	}
	test_event_stream::configure("chunkserver", streamPath);
	if (gStableId != 0) { test_event_stream::setIdentity(gStableId, gChunkserverIncarnation); }

	const bool testClocks = cfg_getuint32("TEST_AUTHORITY_CLOCKS", 0) != 0;
	const int64_t offset = cfg_getint64("TEST_SESSION_AUTHORITY_CLOCK_OFFSET_SECONDS", 0);
	session_authority_clock::configure(testClocks, offset);

	const bool testFaults = cfg_getuint32("TEST_PACKET_FAULTS_ENABLED", 0) != 0;
	test_packet_faults::configure(testFaults, cfg_getstring("TEST_PACKET_FAULTS", ""));

	const bool testJobHolds = cfg_getuint32("TEST_JOB_HOLD_ENABLED", 0) != 0;
	test_job_faults::configure(testJobHolds, cfg_getstring("TEST_JOB_HOLD", ""));
	test_job_faults::configureMaster(testJobHolds,
	                                 cfg_getstring("TEST_MASTER_JOB_HOLD", ""));
}

/// Bounds this chunkserver places on the session authority it accepts, none of which are
/// test seams: an operator lowers them, never a peer.
static void masterconn_configure_session_bounds() {
	gMaxAcceptedLeaseSeconds = cfg_getuint64("CS_SESSION_MAX_ACCEPTED_LEASE_SECONDS", 900);
	gRenewerNominationTimeoutSeconds =
	    cfg_getuint64("CS_SESSION_RENEWER_NOMINATION_TIMEOUT_SECONDS", 15);
}

void masterconn_stats(uint64_t *bin, uint64_t *bout, uint32_t *maxjobscnt) {
	//  For each connection, add the statistics
	uint64_t totalBytesIn = 0;
	uint64_t totalBytesOut = 0;
	for (auto &conn : gMasterConns) {
		totalBytesIn += conn->bytesIn();
		totalBytesOut += conn->bytesOut();
		conn->resetStats();
	}

	*bin = totalBytesIn;
	*bout = totalBytesOut;

	// Get the stats non dependent on specific connections
	*maxjobscnt = stats_maxjobscnt;
	stats_maxjobscnt = 0;
}

void masterconn_check_hdd_reports() {
	// The hdd queues drain once, so gather the recipients first: every report goes to
	// every fully registered MDS. With no recipient the queues stay untouched, exactly
	// as the single-connection code left them until registration completed.
	std::vector<MasterConn *> registered;
	for (auto &conn : gMasterConns) {
		if (conn->mode() == ConnectionMode::CONNECTED &&
		    conn->registrationStatus() == RegistrationStatus::kChunksRegistered) {
			registered.push_back(conn.get());
		}
	}
	if (registered.empty()) { return; }

	if (hddGetAndResetSpaceChanged()) {
		uint64_t usedspace, totalspace, tdusedspace, tdtotalspace;
		uint32_t chunkcount, tdchunkcount;
		hddGetTotalSpace(&usedspace, &totalspace, &chunkcount, &tdusedspace, &tdtotalspace,
		                 &tdchunkcount);
		for (auto *eptr : registered) {
			eptr->createAttachedNoVersionPacket(CSTOMA_SPACE, usedspace, totalspace, chunkcount,
			                                    tdusedspace, tdtotalspace, tdchunkcount);
		}
	}

	uint32_t errorcounter = hddGetAndResetErrorCounter();
	while (errorcounter) {
		for (auto *eptr : registered) {
			eptr->createAttachedNoVersionPacket(CSTOMA_ERROR_OCCURRED);
		}
		errorcounter--;
	}

	const auto chunkBulkSize = gChunkBulkSize.load(std::memory_order_relaxed);

	std::vector<ChunkWithType> chunks_with_type;
	hddGetDamagedChunks(chunks_with_type, chunkBulkSize);
	if (!chunks_with_type.empty()) {
		for (auto *eptr : registered) {
			eptr->createAttachedPacket(cstoma::chunkDamaged::build(chunks_with_type));
		}
	}

	hddGetLostChunks(chunks_with_type, chunkBulkSize);
	if (!chunks_with_type.empty()) {
		for (auto *eptr : registered) {
			eptr->createAttachedPacket(cstoma::chunkLost::build(chunks_with_type));
		}
		if (test_event_stream::enabled()) {
			for (const auto &chunk : chunks_with_type) {
				test_event_stream::emit(
				    "loss_report_packet_queued",
				    {{"chunk", chunk.id},
				     {"part_type", static_cast<uint64_t>(chunk.type.getId())},
				     {"destinations", static_cast<uint64_t>(registered.size())}});
			}
		}
	}

	std::vector<ChunkWithVersionAndType> chunks_with_version;
	hddGetNewChunks(chunks_with_version, chunkBulkSize);
	if (!chunks_with_version.empty()) {
		for (auto *eptr : registered) {
			eptr->createAttachedPacket(cstoma::chunkNew::build(chunks_with_version));
		}
	}

	// The acknowledged evidence channel: the outbox's unacknowledged tail goes to every
	// registered distributed session until an acknowledgement names it. Unlike the legacy queues
	// above, which empty themselves as they are read, the outbox keeps its items by design, so
	// the resend is paced by a clock rather than by this function's call rate: once a second is
	// a retry, every call would be a flood.
	static uint32_t lastEvidenceSend = 0;
	const uint32_t nowSeconds = eventloop_time();
	if (nowSeconds != lastEvidenceSend) {
		const auto evidenceItems = evidence_outbox::unacked(chunkBulkSize);
		if (!evidenceItems.empty()) {
			lastEvidenceSend = nowSeconds;
			for (auto *eptr : registered) {
				if (!eptr->distributedMode()) { continue; }
				eptr->createAttachedPacket(cstoma::evidenceItems::build(evidenceItems));
			}
		}
	}
}

void masterconn_unwantedjobfinished(uint8_t status, void *packet) {
	(void)status;
	MasterConn::deletePacket(packet);
}

std::function<void(uint8_t, void *)> masterconn_jobDeleteAfterErrorFinished(
    ChunkWithType chunkWithType) {
	return [chunkWithType](uint8_t status, void *packet) {
		(void)packet;
		// packet should be nullptr

		bool anyConnected = false;
		for (auto &conn : gMasterConns) {
			if (conn->mode() == ConnectionMode::CONNECTED) {
				anyConnected = true;
				break;
			}
		}

		if (status == SAUNAFS_STATUS_OK && anyConnected) {
			// Report the chunk as lost to the master server, so it won't be registered again and
			// won't cause any inconsistencies. If the mode is connected, it means that registration
			// with the master server was successful, so we can safely report the chunk as lost. If
			// the mode is not connected, it means that registration with the master server was not
			// successful, so we can skip reporting the chunk as lost, as it won't be registered
			// anyway.
			hddReportLostChunk(chunkWithType.id, chunkWithType.type);
		}
	};
}

std::function<void(uint8_t, void *)> masterconn_unwantedLockJobFinished(ChunkWithType chunkWithType,
                                                                        uint32_t listenerId) {
	return [chunkWithType, listenerId](uint8_t status, void *packet) {
		MasterConn::deletePacket(packet);

		if (status == SAUNAFS_STATUS_OK) { return; }

		// If there was an error while writing, which is passed to the callback as status, we want
		// to remove the chunk itself and avoid registering it again with the master server, as it
		// might contain broken data. To do that, we add a delete job to the job pool, which will be
		// processed and will remove the chunk from the chunk server.
		job_delete(*gJobPool, masterconn_jobDeleteAfterErrorFinished(chunkWithType), nullptr,
		           chunkWithType.id, 0, chunkWithType.type, listenerId);
	};
}

MasterJobPool *masterconn_get_job_pool() { return gJobPool.get(); }

bool masterconn_canexit() {
	bool anyConnected = false;
	bool outputQueuesEmpty = true;
	for (auto &conn : gMasterConns) {
		if (conn->mode() == ConnectionMode::CONNECTED) {
			anyConnected = true;
			outputQueuesEmpty = outputQueuesEmpty && conn->isOutputQueueEmpty();
		}
	}

	return !anyConnected ||
	       (gJobPool->isEmpty() && gReplicationJobPool->isEmpty() && outputQueuesEmpty);
}

void masterconn_term(void) {
	//  For each connection, release its resources.
	for (auto &conn : gMasterConns) { conn->releaseResources(); }
	gMasterConns.clear();
	gPendingMasterConns.clear();

	//  Now reset the last reference to the job pools.
	gReplicationJobPool.reset();
	gJobPool.reset();
}

void masterconn_desc(std::vector<pollfd> &pdesc) {
	LOG_AVG_TILL_END_OF_SCOPE0("master_desc");

	// Add the descriptor for listening for background jobs finishing.
	gJobFDpDescPos = -1;
	gReplicationJobFDpDescPos = -1;

	bool anyConnected = false;
	for (auto &conn : gMasterConns) {
		if (conn->mode() == ConnectionMode::CONNECTED) {
			anyConnected = true;
			break;
		}
	}

	if (anyConnected) {
		if (gJobFD >= 0) {
			pdesc.emplace_back(gJobFD, POLLIN, 0);
			gJobFDpDescPos = static_cast<int32_t>(pdesc.size() - 1);
		}
		if (gReplicationJobFD >= 0) {
			pdesc.emplace_back(gReplicationJobFD, POLLIN, 0);
			gReplicationJobFDpDescPos = static_cast<int32_t>(pdesc.size() - 1);
		}
	}

	// For each connection to an MDS, add its socket to the pollfd array.
	for (auto &conn : gMasterConns) { conn->providePollDescriptors(pdesc, doTerminate()); }
}

void masterconn_send_status() {
	static uint8_t prev_factor = 0;

	if (gEnableLoadFactor) {
		uint8_t load_factor = hddGetLoadFactor();
		if (load_factor != prev_factor) {
			bool sent = false;
			for (auto &conn : gMasterConns) {
				if (conn->mode() == ConnectionMode::CONNECTED) {
					conn->createAttachedPacket(cstoma::status::build(load_factor));
					sent = true;
				}
			}
			if (sent) { prev_factor = load_factor; }
		}
	}
}

/// The instant a nomination started now stops being worth waiting for. Zero when the authority
/// clock cannot be read, which leaves the attempt unbounded but only in the state where serving
/// is already refused for the same reason.
///
/// A cutoff still ahead of us shortens the attempt, so that one silent candidate cannot spend
/// the whole remaining serving window. It never shortens it to nothing: an attempt retired on
/// the pass that created it was measured re-electing on every event loop iteration, cycling
/// through candidates at twenty nominations a second and converging on none of them, because
/// no reply can arrive inside zero seconds. Past the cutoff the window it protected is already
/// spent and the plain timeout governs, since the election is now what readmission depends on.
static uint64_t masterconn_nomination_attempt_deadline() {
	const auto now = session_authority_clock::now();
	if (!now.has_value()) { return 0; }
	const uint64_t deadline = *now + gRenewerNominationTimeoutSeconds;
	const auto cutoff = masterconn_session_cutoff_instant();
	if (cutoff.has_value() && *cutoff > *now && *cutoff < deadline) {
		return std::max(*cutoff, *now + kMinNominationAttemptSeconds);
	}
	return deadline;
}

/// Keeps exactly one admitted distributed connection nominated as the session
/// renewer. A failed renewer is replaced deterministically by the lowest stable
/// MDS id already admitted as an observer. Re-registration then performs the
/// conditional FDB renewer move; connection liveness alone grants no authority.
static void masterconn_reconcile_session_renewer() {
	if (!gDistributedMode || gStableId == 0) { return; }

	// Only an MDS that answered holds the role. Reading the locally set role as a result
	// made an in-place upgrade look complete the instant it was requested, so a nomination
	// nobody answered ended the election permanently.
	for (const auto &conn : gMasterConns) {
		if (conn->sessionRenewerConfirmed() && conn->mode() == ConnectionMode::CONNECTED &&
		    conn->registrationStatus() == RegistrationStatus::kChunksRegistered) {
			gLastExpiredRenewerMdsId = 0;
			return;
		}
	}
	// An attempt is only worth waiting for while it can still finish inside the serving
	// window. Past that the stickiness below would hold the election open against a reply
	// that can no longer arrive in time, so retire the attempt and arbitrate again.
	if (const auto now = session_authority_clock::now(); now.has_value()) {
		for (const auto &conn : gMasterConns) {
			if (conn->expireSessionRenewerNomination(*now)) {
				gLastExpiredRenewerMdsId = conn->mdsId();
			}
		}
	}
	// Keep one nomination sticky across its intentional disconnect and handshake.
	// A failed attempt clears this flag in MasterConn, permitting a new election.
	for (const auto &conn : gMasterConns) {
		if (!conn->retired() && conn->sessionRenewerNominationPending()) { return; }
	}

	// Lowest admitted stable MDS id wins, except that whoever just went silent is passed
	// over. Re-electing the candidate that did not answer is bounded but makes no progress,
	// and with no one else to try it is chosen again rather than leaving the session
	// unrenewed.
	auto lowestAdmitted = [](uint32_t skipMdsId) -> MasterConn * {
		MasterConn *best = nullptr;
		for (const auto &conn : gMasterConns) {
			if (conn->retired() || conn->mdsId() == 0 || conn->mdsId() == skipMdsId ||
			    conn->mode() != ConnectionMode::CONNECTED ||
			    conn->registrationStatus() != RegistrationStatus::kChunksRegistered) {
				continue;
			}
			if (best == nullptr || conn->mdsId() < best->mdsId()) { best = conn.get(); }
		}
		return best;
	};
	MasterConn *candidate = lowestAdmitted(gLastExpiredRenewerMdsId);
	if (candidate == nullptr && gLastExpiredRenewerMdsId != 0) { candidate = lowestAdmitted(0); }
	const bool candidateAdmitted = candidate != nullptr;
	if (candidate == nullptr) {
		// Bootstrap: with NO admitted connection anywhere, only a CLAIM_RENEWER
		// registration can reopen the serving window (observers are refused while the
		// durable claim has no window), so waiting for an admitted candidate would
		// deadlock. Nominate the lowest known connection; its next registration
		// attempt presents the role and the conditional FDB claim still arbitrates.
		for (const auto &conn : gMasterConns) {
			if (conn->retired()) { continue; }
			if (candidate == nullptr ||
			    (conn->mdsId() != 0 &&
			     (candidate->mdsId() == 0 || conn->mdsId() < candidate->mdsId()))) {
				candidate = conn.get();
			}
		}
	}
	if (candidate == nullptr) { return; }

	for (auto &conn : gMasterConns) {
		if (conn->distributedRole() == DistributedRegistrationRole::kClaimRenewer) {
			conn->setDistributedRole(DistributedRegistrationRole::kObserver);
		}
	}
	candidate->nominateSessionRenewer(masterconn_nomination_attempt_deadline());
	safs::log_info("MasterConn: nominating MDS {} as chunkserver-session renewer",
	               candidate->mdsId());
	if (test_event_stream::enabled()) {
		test_event_stream::emit("renewer_nominated", {{"mds_id", candidate->mdsId()}});
	}
	if (candidateAdmitted) {
		// The role is still part of the FDB-gated handshake, but it is re-presented on
		// the LIVE admitted connection: the survivor keeps serving this chunkserver
		// during the move instead of dropping it for a reconnect.
		candidate->requestRenewerUpgrade();
	} else if (candidate->mode() == ConnectionMode::CONNECTED) {
		// Mid-registration under the old role: re-register with the new one.
		candidate->setMode(ConnectionMode::KILL);
	}
	// A FREE or CONNECTING candidate presents the role on its next registration.
}

void masterconn_serve(const std::vector<pollfd> &pdesc) {
	LOG_AVG_TILL_END_OF_SCOPE0("master_serve");

	for (auto &conn : gMasterConns) { conn->handlePollErrors(pdesc); }

	// Check if there are any background jobs to process.
	if (gJobFDpDescPos >= 0 && (pdesc[gJobFDpDescPos].revents & POLLIN)) {
		gJobPool->processCompletedJobs();
	}
	if (gReplicationJobFDpDescPos >= 0 && (pdesc[gReplicationJobFDpDescPos].revents & POLLIN)) {
		gReplicationJobPool->processCompletedJobs();
	}

	// For each connection to an MDS, process its socket.
	for (auto &conn : gMasterConns) { conn->servePoll(pdesc); }

	// Update general statistics
	uint32_t totalJobCount = gJobPool->getJobCount() + gReplicationJobPool->getJobCount();
	stats_maxjobscnt = std::max(totalJobCount, stats_maxjobscnt);

	masterconn_reconcile_session_renewer();

	// If a connection is in KILL mode, disable the job pool and close the socket. The job
	// pools are shared between the connections, so losing one MDS aborts in-flight jobs
	// for all of them; every MDS re-issues its commands after the re-registration.
	for (auto &conn : gMasterConns) {
		if (conn->mode() == ConnectionMode::KILL) {
			gJobPool->disableAndChangeCallbackAll(masterconn_unwantedjobfinished);
			gJobPool->changeLockJobsCallback(masterconn_unwantedLockJobFinished);
			gReplicationJobPool->disableAndChangeCallbackAll(masterconn_unwantedjobfinished);
			tcpclose(conn->socketFD());
			conn->resetPackets();
			conn->setMode(ConnectionMode::FREE);
		}
	}
}

void masterconn_reconnect(void) {
	for (auto &conn : gMasterConns) {
		if (conn->mode() == ConnectionMode::FREE && !conn->retired()) { conn->initConnect(); }
	}
}

static uint32_t get_cfg_timeout() {
	return 1000 * cfg_get_minmaxvalue<double>("MASTER_TIMEOUT", 60, 0.01, 1000 * 1000);
}

/// Read the label from configuration file and return true if it's changed to a valid one
bool masterconn_load_label() {
	std::string oldLabel = gLabel;
	gLabel = cfg_getstring("LABEL", MediaLabelManager::kWildcard);
	if (!MediaLabelManager::isLabelValid(gLabel)) {
		safs::log_warn("invalid label '{}'", gLabel);
		return false;
	}
	return gLabel != oldLabel;
}

void masterconn_reload(void) {
	//  Read the common configuration from file.
	gBindHostStr = cfg_getstring("BIND_HOST", "*");
	gEnableLoadFactor = static_cast<bool>(cfg_getuint32("ENABLE_LOAD_FACTOR", 0));
	masterconn_configure_test_hooks();
	masterconn_configure_session_bounds();

	uint32_t bip = 0;

	if (tcpresolve(gBindHostStr.c_str(), nullptr, &bip, nullptr, 1) < 0) { bip = 0; }

	// For each connection, reload the configuration and reconnect if needed.
	for (auto &conn : gMasterConns) {
		if (conn->isMasterAddressValid() && conn->mode() != ConnectionMode::FREE) {
			if (conn->bindHostAddress().ip != bip) {
				conn->setBindHostAddress(bip, conn->bindHostAddress().port);
				conn->setMode(ConnectionMode::KILL);
			}

			conn->reloadConfig();
		} else {
			conn->setMasterAddressValid(false);
		}
	}

	gTimeout_ms = get_cfg_timeout();

	if (masterconn_load_label()) {
		for (auto &conn : gMasterConns) { conn->sendRegisterLabel(); }
	}

	for (auto &conn : gMasterConns) { conn->sendConfig(); }

	uint32_t reconnectionDelay = cfg_getuint32("MASTER_RECONNECTION_DELAY", 5);
	eventloop_timechange(gReconnectHook, TIMEMODE_RUN_LATE, reconnectionDelay, 0);
}

/// Mints this process's identity, which exists to tell two processes that share a stable id
/// apart, so it is drawn from its own source rather than from the shared engine: a draw taken
/// before that engine is seeded is the same number in every process, which is the one collision
/// this value may never have. The pid and both clocks are mixed in so that a platform whose
/// random_device is deterministic still yields a value of its own.
static uint64_t masterconn_mint_incarnation() {
	std::random_device device;
	uint64_t value = (static_cast<uint64_t>(device()) << 32) ^ static_cast<uint64_t>(device());
	value ^= static_cast<uint64_t>(getpid()) << 48;
	value ^= static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
	value ^= static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count())
	         << 16;
	return value == 0 ? 1 : value;
}

int masterconn_init(void) {
	// Read the configuration
	uint32_t reconnectionDelay = cfg_getuint32("MASTER_RECONNECTION_DELAY", 5);
	const std::string connectionMode = cfg_getstring("METADATA_SERVER_CONNECTION_MODE", "LEGACY");
	if (connectionMode == "LEGACY") {
		gDistributedMode = false;
		gMasterHost = cfg_getstring("MASTER_HOST", "sfsmaster");
		gMasterPort = cfg_getstring("MASTER_PORT", "9420");
	} else if (connectionMode == "DISTRIBUTED") {
		gDistributedMode = true;
		gMasterHost = cfg_getstring("MDS_SEED_HOST", "");
		gMasterPort = cfg_getstring("MDS_SEED_PORT", "");
		if (gMasterHost.empty() || gMasterPort.empty()) {
			safs::log_err("distributed mode requires explicit MDS_SEED_HOST and MDS_SEED_PORT");
			return -1;
		}
	} else {
		safs::log_err(
		    "METADATA_SERVER_CONNECTION_MODE must be exactly LEGACY or DISTRIBUTED, got '{}'",
		    connectionMode);
		return -1;
	}
	// One CLUSTER_ID key for both planes: the cluster identity is the same concept in
	// legacy and distributed registration, and a separate distributed-only key would
	// silently bypass every cluster-mismatch refusal driven by CLUSTER_ID.
	std::string clusterId = cfg_getstring("CLUSTER_ID", "default");
	gBindHostStr = cfg_getstring("BIND_HOST", "*");
	gTimeout_ms = get_cfg_timeout();
	gEnableLoadFactor = static_cast<bool>(cfg_getuint32("ENABLE_LOAD_FACTOR", 0));

	if (!masterconn_load_label()) { return -1; }

	if (gDistributedMode) {
		masterconn_load_stable_id();
		evidence_outbox::init();
		gChunkserverIncarnation = masterconn_mint_incarnation();
		// Deny-by-default: no client or distributed control work before the first
		// accepted claim opens a serving window, and no era to bind it to either.
		gSessionServingAllowed.store(false, std::memory_order_relaxed);
		gServingEra.store(0, std::memory_order_relaxed);
	}
	masterconn_configure_test_hooks();
	masterconn_configure_session_bounds();

	// Create the configured connection; the rest arrive with the pushed cluster views.
	gMasterConns.push_back(std::make_unique<MasterConn>(
	    gMasterHost, gMasterPort, clusterId, gJobPool, gReplicationJobPool, gDistributedMode, true,
	    DistributedRegistrationRole::kClaimRenewer));
	MasterConn *eptr = gMasterConns.front().get();
	passert(eptr);

	// Init the connections
	if (eptr->initConnect() < 0) { return -1; }

	// Register the callbacks in the event loop
	eventloop_eachloopregister(masterconn_admit_pending);
	eventloop_eachloopregister(masterconn_session_cutoff_check);
	eventloop_eachloopregister(masterconn_check_hdd_reports);
	eventloop_timeregister(TIMEMODE_RUN_LATE, kSendStatusDelay,
	                       rnd_ranged<uint32_t>(kSendStatusDelay), masterconn_send_status);
	gReconnectHook =
	    eventloop_timeregister(TIMEMODE_RUN_LATE, reconnectionDelay,
	                           rnd_ranged<uint32_t>(reconnectionDelay), masterconn_reconnect);

	eventloop_destructregister(masterconn_term);
	eventloop_pollregister(masterconn_desc, masterconn_serve);
	eventloop_reloadregister(masterconn_reload);

	return 0;
}

void masterconn_apply_cluster_view(MasterConn *from, uint32_t senderMdsId,
                                   const std::vector<MetadataserverClusterEntry> &members) {
	if (!from->distributedMode() || senderMdsId == 0) { return; }

	// Bind the sender's id to the connection the view arrived on. If another connection
	// already owns that id, two routes reached the same MDS: keep the established one and
	// retire this one so it never redials.
	if (from->mdsId() != senderMdsId) {
		for (auto &conn : gMasterConns) {
			if (conn.get() != from && conn->mdsId() == senderMdsId && !conn->retired()) {
				safs::log_warn(
				    "MasterConn: two connections reached mds_id={}; retiring the duplicate to {}",
				    senderMdsId, from->address().toString());
				from->retire();
				return;
			}
		}
		from->setMdsId(senderMdsId);
	}

	// Stage a connection for every member nothing covers yet. The dial itself happens in
	// masterconn_admit_pending, outside the poll dispatch that delivered this view.
	for (const auto &member : members) {
		if (member.mdsId == 0) { continue; }

		bool known = false;
		for (auto &conn : gMasterConns) {
			if (conn->mdsId() == member.mdsId && !conn->retired()) {
				known = true;
				break;
			}
		}
		for (auto &conn : gPendingMasterConns) {
			if (conn->mdsId() == member.mdsId) { known = true; }
		}
		if (known) { continue; }

		safs::log_info("MasterConn: cluster view names mds_id={} at {}:{}; connecting",
		               member.mdsId, ipToString(member.ip), member.matocsPort);
		gPendingMasterConns.push_back(std::make_unique<MasterConn>(
		    ipToString(member.ip), std::to_string(member.matocsPort), from->clusterId(), gJobPool,
		    gReplicationJobPool, true, false, DistributedRegistrationRole::kObserver));
		gPendingMasterConns.back()->setMdsId(member.mdsId);
	}
}

/// Runs between event-loop iterations: admits staged connections into the live set and
/// dials them. A failed dial stays FREE and the reconnection timer keeps retrying it.
static void masterconn_admit_pending() {
	for (auto &conn : gPendingMasterConns) {
		gMasterConns.push_back(std::move(conn));
		gMasterConns.back()->initConnect();
	}
	gPendingMasterConns.clear();
}

int masterconn_init_threads(void) {
	gNumberOfWorkers = cfg_get_minvalue<uint32_t>("MASTER_NR_OF_WORKERS", kDefaultNumberOfWorkers,
	                                              kMinNumberOfWorkers);

	try {
		// Create the JobPool instance with the specified number of workers, it would be serving
		// only this master network thread, thus the number of listeners is 1.
		std::vector<int> bgJobPoolFDs(1);
		gJobPool = std::make_shared<MasterJobPool>("ma", gNumberOfWorkers, kMaxBackgroundJobsCount,
		                                           1, bgJobPoolFDs);
		gJobFD = bgJobPoolFDs[0];
	} catch (const std::exception &e) {
		safs::log_err("masterconn_init_threads: Failed to create JobPool instance: {}", e.what());
		return -1;
	}

	if (gJobPool == nullptr) {
		safs::log_err("masterconn_init_threads: jobPool is null. Unable to create worker threads.");
		return -1;
	}

	safs::log_info("master connection: {} background workers created", gNumberOfWorkers);

	gReplicationNumberOfWorkers = cfg_get_minvalue<uint32_t>("MASTER_REPLICATION_NR_OF_WORKERS",
	                                                         kDefaultReplicationNumberOfWorkers,
	                                                         kMinReplicationNumberOfWorkers);

	try {
		// Create the ReplicationJobPool instance with the specified number of workers, it would be
		// serving only this master network thread, thus the number of listeners is 1.
		std::vector<int> replicationJobPoolFDs(1);
		gReplicationJobPool =
		    std::make_shared<MasterJobPool>("ma_repl", gReplicationNumberOfWorkers,
		                                    kMaxBackgroundJobsCount, 1, replicationJobPoolFDs);
		gReplicationJobFD = replicationJobPoolFDs[0];
	} catch (const std::exception &e) {
		safs::log_err("masterconn_init_threads: Failed to create ReplicationJobPool instance: {}",
		              e.what());
		return -1;
	}

	if (gReplicationJobPool == nullptr) {
		safs::log_err(
		    "masterconn_init_threads: replicationJobPool is null. Unable to create "
		    "replication worker threads.");
		return -1;
	}

	safs::log_info("master connection: {} replication background workers created",
	               gReplicationNumberOfWorkers);

	return 0;
}
