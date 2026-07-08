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
#include "master/matoclserv_serializer.h"
#include "master/matoclserv_sessions.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
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
#include "common/input_packet.h"
#include "common/io_limits_config_loader.h"
#include "common/io_limits_database.h"
#include "common/legacy_vector.h"
#include "common/loop_watchdog.h"
#include "common/massert.h"
#include "common/md5.h"
#include "common/network_address.h"
#include "common/output_packet.h"
#include "common/random.h"
#include "common/saunafs_statistics.h"
#include "common/saunafs_version.h"
#include "common/serialized_goal.h"
#include "common/sessions_file.h"
#include "common/sockets.h"
#include "common/tls_session.h"
#include "common/type_defs.h"
#include "common/user_groups.h"
#include "config/cfg.h"
#include "kv/ifuture.h"
#include "kv/itransaction.h"
#include "master/changelog.h"
#include "master/chartsdata.h"
#include "master/chunk_operations_interface.h"
#include "master/chunks.h"
#include "master/chunkserver_db.h"
#include "master/datacachemgr.h"
#include "master/exports.h"
#include "master/filesystem.h"
#include "master/filesystem_node.h"
#include "master/filesystem_node_types.h"
#include "master/filesystem_operation_context.h"
#include "master/filesystem_operations.h"
#include "master/filesystem_operations_interface.h"
#include "master/filesystem_periodic.h"
#include "master/filesystem_snapshot.h"
#include "master/masterconn.h"
#include "master/matocsserv.h"
#include "master/matomlserv.h"
#include "master/matontserv.h"
#include "master/metadata_backend_common.h"
#include "master/metadata_backend_interface.h"
#include "master/personality.h"
#include "master/settrashtime_task.h"
#include "metrics/metrics.h"
#include "protocol/SFSCommunication.h"
#include "protocol/cltoma.h"
#include "protocol/matocl.h"
#include "protocol/packet.h"
#include "slogger/slogger.h"

#define MaxPacketSize 1000000

// matoclserventry.mode
enum class ClientConnectionMode : std::uint8_t {
	KILL,			/// Connection is terminated,
	HEADER,			/// Read header
	DATA,			/// Read data packet
	HANDSHAKE		/// TLS handshake in progress
};

const uint32_t kMaxNumberOfChunkCopies = 100U;
constexpr uint8_t kClientInactivityTimeout = 10;

/// Batch size for committing transactions.
/// Used to avoid transaction too old errors in KV backends.
constexpr size_t kTransactionBatchSize = 100;

struct matoclserventry;

// locked chunks

struct DelayedChunkOperation {
	uint64_t chunkId = 0;     ///< Chunk ID
	uint64_t fileLength = 0;  ///< File length
	uint32_t lockId = 0;      ///< Lock ID
	uint32_t messageId = 0;   ///< Message ID for reply
	inode_t inode = 0;        ///< Inode
	uint32_t uid = 0;         ///< Remapped uid of the user which will run the operation
	uint32_t gid = 0;         ///< Remapped gid of the user which will run the operation
	uint32_t auid = 0;        ///< Real uid (not remapped) of the user who will run the operation
	uint32_t agid = 0;        ///< Real gid (not remapped) of the user who will run the operation
	uint8_t type = 0;         ///< Delayed operation type: FUSE_WRITE, FUSE_TRUNCATE,
	                          ///< FUSE_TRUNCATE_BEGIN or FUSE_TRUNCATE_END
	const PacketSerializer* serializer = nullptr;  ///< Packet serializer for the operation

	// Deferred-commit handshake. The op body that initiates a chunkserver operation
	// enqueues this entry BEFORE its own commit is durable (the deferred commit no
	// longer blocks the event loop, so the chunkserver's status can arrive first). While
	// commitPending, matoclserv_chunk_status stashes the status here instead of
	// processing it; the commit continuation then clears the flag and processes any
	// stashed status (or drops the entry if the commit failed).
	bool commitPending = false;     ///< enqueuing op's commit still in flight
	bool statusArrived = false;     ///< chunkserver status arrived while commitPending
	uint8_t pendingStatus = 0;      ///< stashed chunkserver status
	bool pendingIsFailedCreate = false;  ///< stashed isFailedCreateOperation flag
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
	InputPacket inputPacket{MaxPacketSize};  ///< InputPacket for reading data from the client
	std::list<OutputPacket> outputPackets;  ///< List of output packets

	/// Context of the TLS channel used for communication with client.
	///
	/// If no TLS is used, this is `nullptr`.
	std::unique_ptr<TlsSession> tlsSession;
	int lastHandshakeError = 0;
	static constexpr uint8_t kPasswordSize = 32;
	uint8_t randomPassword[kPasswordSize];  ///< Random password for authentication
	Session *sessionData;                   ///< Pointer to the session data for this client
	///< Challenge data for admin authentication
	std::unique_ptr<SauMatoclAdminRegisterChallengeData> adminChallenge;
	AdminTask adminTask;  ///< admin task requested by this client
	///< Delayed chunk operations for this client
	std::vector<std::unique_ptr<DelayedChunkOperation>> delayedChunkOperations;
	///< Number of async commits still in flight that will write a reply to this
	///< client. The entry must not be freed while this is non-zero (see the close
	///< loop in matoclserv_serve), so deferred replies never touch a dangling eptr.
	uint32_t pendingCommits = 0;
};

using WaitEntry = std::tuple<matoclserventry *, inode_t, uint32_t>;

struct WaitEntryCmp {
	bool operator()(const WaitEntry &a, const WaitEntry &b) const noexcept {
		matoclserventry *pa = std::get<0>(a), *pb = std::get<0>(b);
		if (pa != pb) { return std::less<matoclserventry *>()(pa, pb); }
		inode_t ia = std::get<1>(a), ib = std::get<1>(b);
		if (ia != ib) { return ia < ib; }
		return std::get<2>(a) < std::get<2>(b);
	}
};

// This map stores, for each chunk ID, the list of clients that are waiting for this chunk to be
// unlocked and the inode and chunk index of the chunk they are waiting for. When a chunk is
// unlocked, all clients in the corresponding list are notified and removed from the map.
std::unordered_map<uint64_t, std::set<WaitEntry, WaitEntryCmp>> gWaitForUnlockMap;

static std::list<std::unique_ptr<matoclserventry>> matoclservList;

// ---------------------------------------------------------------------------
// Op submission
//
// A kReadWrite client RPC's body runs on the single event-loop thread and its
// mutations are committed as part of a batch (see "Group commit" below); the
// client reply is gated on that batch's durability. The op body is replayable so
// the batch can be re-run on a retryable conflict, and op bodies still run
// sequentially, so versions stay monotonic and out-of-order completion is safe.
// ---------------------------------------------------------------------------

/// Re-runs a deferred op's body on a fresh read-write transaction, returning the
/// op status and refreshing the captured reply data (via state the closure holds,
/// e.g. a shared reply struct). Empty when an op is not replayable, in which case a
/// retryable commit conflict falls through to an error reply instead of a retry.
/// Runs on the single event-loop thread, so it is serialized against every other
/// op; only this op's backend writes are re-issued. The closure must NOT re-apply any
/// in-process persistent side effect (session open-file sets, etc.) -- those belong
/// in the finish continuation, which runs once on success.
using OpReplay = std::function<uint8_t(FilesystemOperationContext &)>;

// Bounded retries for a batch commit that fails with a retryable backend conflict
// and for retryable read errors thrown while running an op body. Each retry replays on
// a fresh transaction. 0 disables retrying (a conflict becomes an immediate error reply).
static uint32_t gMaxCommitRetries = 5;
// TEST-ONLY fault injection (MATOCL_DEBUG_INJECT_COMMIT_CONFLICTS, default 0):
// synthesize a retryable commit conflict for a batch's first N attempts WITHOUT
// committing (the transaction is dropped uncommitted, so there is no orphan), forcing
// the replay path. Reloadable. Leave 0 in production.
static uint32_t gDebugInjectConflicts = 0;
// TEST-ONLY fault injection (MATOCL_DEBUG_INJECT_READ_CONFLICTS, default 0): throw a
// retryable read error (kv::RetryableTransactionError, as a timed-out backend read would) on
// the first N attempts of a replayable op body, BEFORE the body runs, so no backend write or
// inode is burned. N < MATOCL_MAX_COMMIT_RETRIES recovers; N >= it exhausts to a bounded
// EIO with no abort. Reloadable. Leave 0 in production.
static uint32_t gDebugInjectReadConflicts = 0;

// eventfd the commit future's ready callback writes to (from the backend network
// thread, or inline from the event-loop thread) so poll() wakes immediately to
// finalize the commit instead of waiting out the poll timeout. -1 until
// matoclserv_network_init creates it. Linux-only; without it the group commit path
// still works, just serviced at the poll-timeout cadence.
static std::atomic<int> gCommitWakeupFd{-1};

/// Wakes the event loop when a batch commit becomes durable. Invoked from the
/// backend network thread, so it stays minimal and touches only the eventfd. The
/// write is best-effort: if the 64-bit counter is somehow saturated the existing
/// pending value still keeps poll() readable, and a dropped wakeup only defers
/// finalization to the next poll.
static void matoclserv_commit_wakeup(void * /*arg*/) {
	int fd = gCommitWakeupFd.load(std::memory_order_acquire);
	if (fd < 0) { return; }
	uint64_t one = 1;
	ssize_t written = write(fd, &one, sizeof(one));
	(void)written;
}

/// pollregister desc: have poll() watch the wakeup eventfd for readability.
static void matoclserv_commit_wakeup_desc(std::vector<pollfd> &pdesc) {
	int fd = gCommitWakeupFd.load(std::memory_order_acquire);
	if (fd < 0) { return; }
	pdesc.push_back(pollfd{fd, POLLIN, 0});
}

/// pollregister serve: drain the eventfd so it stops signalling. The matching
/// commit finalization runs in matoclserv_poll_batch (an eachloop hook
/// that fires every iteration regardless), so this only clears the wakeup.
static void matoclserv_commit_wakeup_serve(const std::vector<pollfd> &pdesc) {
	int fd = gCommitWakeupFd.load(std::memory_order_acquire);
	if (fd < 0) { return; }
	for (const auto &pfd : pdesc) {
		if (pfd.fd == fd && (pfd.revents & POLLIN) != 0) {
			uint64_t drain = 0;
			ssize_t got = read(fd, &drain, sizeof(drain));
			(void)got;
			break;
		}
	}
}

/// Publishes all changelog sinks after a deferring transaction commits.
static void matoclserv_publish_deferred_changelog(const FilesystemOperationContext &ctx) {
	auto entries = ctx.takeDeferredChangelogEntries();
	for (auto &deferred : entries) {
		// Preserve inline payload bytes, including the trailing NUL.
		changelog(deferred.version, deferred.entry.c_str());
		if (!getChangelogSignal().empty()) {
			getChangelogSignal().emit(
			    {.version = deferred.version, .entry = deferred.entry.c_str()});
		}
		matomlserv_broadcast_logstring(deferred.version,
		                               reinterpret_cast<uint8_t *>(deferred.entry.data()),
		                               deferred.entry.size());
		matontserv_broadcast_message(deferred.version, std::string_view(deferred.entry));
	}
}

/// Confirms a durable deferring commit and drains its changelog buffer.
/// confirmCommitted() arms the destructor tripwire before publication drains entries.
static void matoclserv_commit_confirmed(const FilesystemOperationContext &ctx) {
	ctx.confirmCommitted();
	matoclserv_publish_deferred_changelog(ctx);
}

/// Runs a replayable op body on a fresh read-write transaction and commits it
/// SYNCHRONOUSLY, replaying the whole op (body + commit) on a fresh transaction on a
/// retryable backend error -- either a read error thrown from the body
/// (kv::RetryableTransactionError) or a retryable commit conflict (surfaced by
/// getResult's retryable flag). Bounded by
/// gMaxCommitRetries. Used by the few standalone post-durability cleanup commits that do
/// not go through the group batch (e.g. the writeEnd cleanup in the write_chunk
/// continuation), which still need conflict-retry because batch commits run concurrently
/// and can conflict on a shared inode -- without it those sites reply EIO on every such
/// conflict.
///
/// Returns the op status. A non-OK body status is returned WITHOUT committing (e.g.
/// permission error, or SAUNAFS_ERROR_LOCKED from writeChunk). A non-retryable commit
/// failure or retry exhaustion returns SAUNAFS_ERROR_IO. Commit is intentionally NOT
/// retried on commit_unknown_result (non-retryable here): the commit may have applied,
/// so a blind replay could double-apply (e.g. burn a second chunk).
///
/// The body must refresh any reply/out state it owns on EACH call (it may run more than
/// once) and must NOT apply in-process persistent side effects -- do those only after
/// this returns SAUNAFS_STATUS_OK. On the KV backend the body may only read, check, and
/// stage backend writes; it must not mutate in-memory MDS structures, because a replay
/// reruns it on a fresh transaction that does not roll those back. Returns OK with no
/// commit when there is no read-write transaction (in-memory Master, applied in place).
static uint8_t matoclserv_commit_op_with_retry(const OpReplay &body) {
	for (uint32_t attempt = 0;; ++attempt) {
		FilesystemOperationContext ctx = gFSOperations->createFilesystemOperationContext(
		    FilesystemOperationContext::TransactionType::kReadWrite);
		if (ctx.hasReadWriteTransaction()) { ctx.setDeferChangelog(); }
		uint8_t status;
		try {
			if (attempt < gDebugInjectReadConflicts) {
				// Fault injection (no commit, no write): force the retry path.
				throw kv::RetryableTransactionError(
				    1031, "injected retryable read conflict (MATOCL_DEBUG_INJECT_READ_CONFLICTS)");
			}
			status = body(ctx);
		} catch (const kv::RetryableTransactionError &e) {
			if (attempt >= gMaxCommitRetries) {
				safs::log_warn(
				    "matoclserv: op body read retry exhausted after {} attempts (err {}): {}",
				    attempt, e.errorCode(), e.what());
				return SAUNAFS_ERROR_IO;
			}
			safs::log_info(
			    "matoclserv: retryable read error in op body (err {}), retrying, attempt {}",
			    e.errorCode(), attempt + 1);
			continue;
		}

		if (status != SAUNAFS_STATUS_OK) { return status; }  // op error/status, do not commit
		auto *txn = ctx.getReadWriteTransaction();
		if (txn == nullptr) { return status; }  // in-memory Master: already applied in place

		int commitError = 0;
		bool retryable = false;
		if (txn->commitAsync()->getResult(&commitError, &retryable)) {
			matoclserv_commit_confirmed(ctx);
			return SAUNAFS_STATUS_OK;  // durable
		}
		if (retryable && attempt < gMaxCommitRetries) {
			safs::log_info(
			    "matoclserv: sync commit conflict (err {}), replaying op, attempt {}",
			    commitError, attempt + 1);
			continue;
		}
		safs::log_err("matoclserv: sync commit failed (err {}, retryable {}), giving up", commitError,
		              retryable);
		return SAUNAFS_ERROR_IO;
	}
}

// ---------------------------------------------------------------------------
// Group commit. Many concurrently-arrived write ops share ONE backend
// transaction; one durability wait is amortized across the whole batch and ops
// inside a batch cannot conflict with each other. At most one batch commit is
// in flight (pipeline depth 1); converted ops arriving while it is in flight
// are HELD with their body unrun and drained into the next batch once the
// commit resolves. Bodies therefore only ever run against durable backend
// state, so in-memory side effects made by a body (chunk locks, session sets)
// can never be orphaned by the rollback of an EARLIER op's commit -- the
// divergence class behind the lock livelock / WRONGLOCKID / reserved-EINVAL
// findings dies by construction, and same-key conflict storms collapse into
// sequential batches.
// ---------------------------------------------------------------------------

/// One op inside a batch: the callback that replies to the client, plus the op body that
/// can be rerun on a fresh transaction. A held op (one still waiting for the in-flight
/// batch to finish, its body not run yet) has the same shape and is carried as one too.
struct BatchMember {
	matoclserventry *eptr;                       ///< held alive via eptr->pendingCommits
	/// Serialize + queue the client reply (kill-gated). Receives SAUNAFS_STATUS_OK when the
	/// batch became durable, the replay's op status on an op error, or SAUNAFS_ERROR_IO on a
	/// non-retryable/exhausted commit failure.
	std::function<void(uint8_t status)> finish;
	OpReplay replay;                             ///< the op body; rerunnable on a fresh txn
	uint32_t attempts = 0;                       ///< read-retry attempts for THIS member
	/// Durable side effect run with the final status BEFORE the kill-gated `finish`, even when
	/// the client is being torn down (empty for ops with no post-commit memo).
	std::function<void(uint8_t status)> onCommit;
};

/// One batch, used in two phases. While OPEN it accumulates members, whose bodies run on
/// the shared transaction in `ctx`; `future` is null. Once submitted it is IN FLIGHT: the
/// batch's single commit runs on `future`.
struct OpBatch {
	FilesystemOperationContext ctx;             ///< owns the txn; must outlive `future`
	std::unique_ptr<kv::ICommitFuture> future;  ///< declared after ctx: destroyed first
	std::vector<BatchMember> members;
	uint32_t attempt = 0;     ///< whole-batch commit attempts (conflict replays)
	bool hasContext = false;  ///< ctx is created lazily for the open batch
};

static OpBatch gOpenBatch;
static std::optional<OpBatch> gInFlightBatch;
static std::deque<BatchMember> gHeldOps;
// True while matoclserv_poll_batch is resolving an in-flight batch (running
// finishes / replaying members). A finish can submit a follow-up op (e.g. the
// truncate finalize via matoclserv_resolve_pending_delayed_op); it must be HELD
// even in the window where gInFlightBatch is already reset, or its body would
// run against state the resolution is still rebuilding.
static bool gBatchResolving = false;

// Group-commit batch-stats accounting. Off by default; the master
// flips it on with FDB_OP_PROFILING. Touched only on the single event-loop thread (the batch
// poller and submit_op), so plain integers need no atomics.
static bool gBatchStatsEnabled = false;
static MatoclBatchStats gBatchStats;

void matoclserv_set_batch_stats_enabled(bool enabled) { gBatchStatsEnabled = enabled; }

MatoclBatchStats matoclserv_get_batch_stats() { return gBatchStats; }

/// Records a committed batch of `size` members in the stats (size-histogram bucketed by
/// power of two as 1, 2-3, 4-7, 8-15, 16+). No-op unless accounting is enabled.
static void matoclserv_record_committed_batch(size_t size) {
	if (!gBatchStatsEnabled || size == 0) { return; }
	gBatchStats.committedBatches++;
	gBatchStats.committedOps += size;
	gBatchStats.maxBatchSize = std::max<uint64_t>(gBatchStats.maxBatchSize, size);
	size_t bucket = std::min<size_t>(std::bit_width(size) - 1, 4);
	gBatchStats.sizeBuckets[bucket]++;
}

/// Releases a member's connection refcount, runs its durable on-commit hook, then its
/// reply unless the client is already being torn down.
static void matoclserv_finish_member(BatchMember &member, uint8_t status) {
	if (member.eptr->pendingCommits > 0) { member.eptr->pendingCommits--; }
	// The durable side effect (recording an open file, resolving a delayed chunk op) runs
	// with the final status even when the client is being torn down: the connection is not
	// freed until pendingCommits drains, and its session-file release then walks openFilesSet,
	// so skipping it would orphan a persisted acquire or a chunk lock. The reply, in contrast,
	// is dropped for a killed client.
	if (member.onCommit) { member.onCommit(status); }
	if (member.eptr->mode != ClientConnectionMode::KILL && member.finish) { member.finish(status); }
}

/// Runs one member's body at `idx` on the shared `ctx` transaction. On success, advances
/// `idx` to the next member; on an op error, replies to that member and removes it; on a
/// retryable read error still within budget, keeps it for the rebuild. Returns true when
/// the shared transaction can no longer be used: a read error was thrown, or an op error
/// already wrote and the backend cannot undo a single op. The caller then restarts the
/// replay on a fresh transaction. Shared by the whole-batch replay and the single-member
/// join.
static bool matoclserv_replay_members_iteration(std::vector<BatchMember> &members, size_t &idx,
                                                FilesystemOperationContext &ctx) {
	auto *txn = ctx.getReadWriteTransaction();
	BatchMember &member = members[idx];
	const uint64_t mutationsBefore = txn != nullptr ? txn->mutationCount() : 0;
	try {
		if (member.attempts < gDebugInjectReadConflicts) {
			// Fault injection (no write burned): force the read-retry path.
			throw kv::RetryableTransactionError(
			    1031, "injected retryable read conflict (MATOCL_DEBUG_INJECT_READ_CONFLICTS)");
		}
		uint8_t status = member.replay(ctx);
		if (status == SAUNAFS_STATUS_OK) {
			++idx;
			return false;
		}
		const bool poisoned = txn != nullptr && txn->mutationCount() != mutationsBefore;
		matoclserv_finish_member(member, status);
		members.erase(members.begin() + idx);
		return poisoned;
	} catch (const kv::RetryableTransactionError &e) {
		// The shared transaction is invalid after a thrown read error:
		// restart on a fresh one regardless of what this member did.
		if (member.attempts >= gMaxCommitRetries) {
			safs::log_warn(
			    "matoclserv: batch member read retry exhausted after {} attempts (err {}): {}",
			    member.attempts, e.errorCode(), e.what());
			matoclserv_finish_member(member, SAUNAFS_ERROR_IO);
			members.erase(members.begin() + idx);
		} else {
			member.attempts++;
			safs::log_info(
			    "matoclserv: retryable read error in batch member (err {}), replaying batch, "
			    "attempt {}",
			    e.errorCode(), member.attempts);
		}
		return true;
	}
}

/// Replays `members` in order onto a fresh shared transaction and returns the
/// resulting context, ready to commit. Runs the per-member step on each member,
/// starting over on a fresh transaction whenever a step reports the current one
/// can no longer be used. Terminates: every restart either drops a member or
/// consumes one member's bounded retry budget.
///
/// Replayed bodies rerun changeLog(); discarded contexts drop their buffered entries.
/// Only the durable context publishes, so each entry appears once after commit.
static FilesystemOperationContext matoclserv_replay_members(std::vector<BatchMember> &members) {
	for (;;) {
		FilesystemOperationContext ctx = gFSOperations->createFilesystemOperationContext(
		    FilesystemOperationContext::TransactionType::kReadWrite);
		if (ctx.hasReadWriteTransaction()) { ctx.setDeferChangelog(); }
		bool restart = false;
		for (size_t idx = 0; idx < members.size() && !restart;) {
			restart = matoclserv_replay_members_iteration(members, idx, ctx);
		}
		if (!restart) { return ctx; }
	}
}

/// Drops the open batch's shared transaction when no member is using it. An idle
/// context must not linger: its read snapshot ages, and a later join would run
/// against state that predates interleaved synchronous commits (e.g. a create
/// joining a context older than the mkdir that made its parent -> ENOENT).
static void matoclserv_release_idle_open_batch() {
	if (gOpenBatch.members.empty() && gOpenBatch.hasContext) {
		gOpenBatch.hasContext = false;
		gOpenBatch.ctx = FilesystemOperationContext();
	}
}

/// Runs a new member's body on the open batch's shared transaction and appends it
/// on success. An op error is replied immediately; if the failed body already
/// wrote, or its read threw (invalidating the shared txn), the surviving members
/// are replayed onto a fresh transaction.
static void matoclserv_join_open_batch(BatchMember member) {
	if (!gOpenBatch.hasContext) {
		gOpenBatch.ctx = gFSOperations->createFilesystemOperationContext(
		    FilesystemOperationContext::TransactionType::kReadWrite);
		// Batched KV ops buffer changelog entries until commit, avoiding uncommitted
		// publication and duplicate entries from replayed batches.
		if (gOpenBatch.ctx.hasReadWriteTransaction()) {
			gOpenBatch.ctx.setDeferChangelog();
		}
		gOpenBatch.hasContext = true;
	}
	auto *txn = gOpenBatch.ctx.getReadWriteTransaction();
	if (txn == nullptr) {
		// In-memory Master: the body applies in place; reply on its status.
		uint8_t status = member.replay(gOpenBatch.ctx);
		matoclserv_finish_member(member, status);
		matoclserv_release_idle_open_batch();
		return;
	}

	// Add the member to the open batch, then run it through the same per-member step the
	// batch replay uses. If that step reports the shared transaction is no longer usable,
	// rebuild the context by replaying the members that remain.
	size_t idx = gOpenBatch.members.size();
	gOpenBatch.members.push_back(std::move(member));
	if (matoclserv_replay_members_iteration(gOpenBatch.members, idx, gOpenBatch.ctx)) {
		gOpenBatch.ctx = matoclserv_replay_members(gOpenBatch.members);
	}
	matoclserv_release_idle_open_batch();
}

/// Entry point for converted write-op handlers: routes the op into the group-commit
/// batch. `finish` always runs exactly once with the op/commit status; the optional
/// `onCommit` durable hook runs first, with the same status, even for a killed client.
static void matoclserv_submit_op(matoclserventry *eptr, OpReplay body,
                                 std::function<void(uint8_t status)> finish,
                                 std::function<void(uint8_t status)> onCommit = {}) {
	eptr->pendingCommits++;
	if (gInFlightBatch.has_value() || gBatchResolving) {
		// Hold the body until the in-flight batch is durable so it runs against
		// committed state (the soundness core of this design).
		if (gBatchStatsEnabled) { gBatchStats.heldOps++; }
		gHeldOps.push_back(
		    BatchMember{eptr, std::move(finish), std::move(body), 0, std::move(onCommit)});
		return;
	}
	matoclserv_join_open_batch(
	    BatchMember{eptr, std::move(finish), std::move(body), 0, std::move(onCommit)});
}

/// Runs once per event-loop iteration (after the serve pass): finalizes an
/// in-flight batch commit, drains held ops into the next batch, and submits the
/// open batch. Ops arriving in the same poll pass share one commit.
static void matoclserv_poll_batch() {
	// Build the commit future and move a batch in-flight. The txn is non-null by
	// construction: members accumulate only when a read-write txn exists (in-memory ops
	// finish inline in join), so every caller here passes a context that owns one.
	auto launchInFlightBatch = [](FilesystemOperationContext ctx,
	                              std::vector<BatchMember> members, uint32_t attempt) {
		std::unique_ptr<kv::ICommitFuture> future;
		if (attempt < gDebugInjectConflicts) {
			// Fault injection: keep failing without committing.
			future = std::make_unique<kv::ImmediateCommitFuture>(false, true);
		} else {
			future = ctx.getReadWriteTransaction()->commitAsync();
		}
		future->setReadyCallback(&matoclserv_commit_wakeup, nullptr);
		gInFlightBatch.emplace(
		    OpBatch{std::move(ctx), std::move(future), std::move(members), attempt, true});
	};

	if (gInFlightBatch.has_value() && gInFlightBatch->future->isReady()) {
		// Finishes (and replay-time finishes) can submit follow-up ops; hold them
		// until this resolution completes so their bodies see only durable state.
		gBatchResolving = true;
		int commitError = 0;
		bool retryable = false;
		const bool ok = gInFlightBatch->future->getResult(&commitError, &retryable);
		if (ok) {
			matoclserv_record_committed_batch(gInFlightBatch->members.size());
			matoclserv_commit_confirmed(gInFlightBatch->ctx);
			for (BatchMember &member : gInFlightBatch->members) {
				matoclserv_finish_member(member, SAUNAFS_STATUS_OK);
			}
			gInFlightBatch.reset();
		} else if (retryable && gInFlightBatch->attempt < gMaxCommitRetries) {
			// A conflict can only come from a writer outside the batch (sync
			// handlers, chunkserver-driven writes, other masters): replay the whole
			// batch on a fresh transaction and resubmit.
			if (gBatchStatsEnabled) { gBatchStats.batchReplays++; }
			const uint32_t attempt = gInFlightBatch->attempt + 1;
			safs::log_info(
			    "matoclserv: batch commit conflict (err {}), replaying {} member(s), attempt {}",
			    commitError, gInFlightBatch->members.size(), attempt);
			std::vector<BatchMember> members = std::move(gInFlightBatch->members);
			gInFlightBatch.reset();  // destroy the old future before its txn
			FilesystemOperationContext ctx = matoclserv_replay_members(members);
			// The replay may have replied and dropped every member; relaunch a commit only
			// when some survived (this conflict path is KV only, so the ctx owns a txn).
			if (!members.empty()) {
				launchInFlightBatch(std::move(ctx), std::move(members), attempt);
			}
		} else {
			safs::log_err("matoclserv: batch commit failed (err {}, retryable {}), giving up",
			              commitError, retryable);
			for (BatchMember &member : gInFlightBatch->members) {
				matoclserv_finish_member(member, SAUNAFS_ERROR_IO);
			}
			gInFlightBatch.reset();
		}
	}

	gBatchResolving = false;

	// Drain held ops into the open batch once nothing is in flight. Bodies run
	// here, against the now-durable state. FIFO keeps per-connection op order.
	while (!gInFlightBatch.has_value() && !gHeldOps.empty()) {
		BatchMember held = std::move(gHeldOps.front());
		gHeldOps.pop_front();
		if (held.eptr->mode == ClientConnectionMode::KILL) {
			if (held.eptr->pendingCommits > 0) { held.eptr->pendingCommits--; }
			continue;
		}
		matoclserv_join_open_batch(std::move(held));
	}

	// Submit the open batch.
	if (!gInFlightBatch.has_value() && !gOpenBatch.members.empty()) {
		launchInFlightBatch(std::move(gOpenBatch.ctx), std::move(gOpenBatch.members), 0);
		gOpenBatch.members.clear();
		gOpenBatch.hasContext = false;
		gOpenBatch.ctx = FilesystemOperationContext();
	}
}

static void matoclserv_process_chunk_status(matoclserventry *eptr,
                                            const DelayedChunkOperation &operation, uint8_t status,
                                            bool isFailedCreateOperation);

/// Removes `op` from eptr's delayed-chunk queue if still present (matched by identity).
/// Used by replayable op bodies to drop the entry a previous attempt enqueued, and by
/// commit continuations when the commit failed or the final attempt took a
/// non-delayed path.
static void matoclserv_drop_queued_delayed_op(matoclserventry *eptr, DelayedChunkOperation *op) {
	if (op == nullptr) { return; }
	auto &queue = eptr->delayedChunkOperations;
	auto it = std::find_if(queue.begin(), queue.end(),
	                       [op](const std::unique_ptr<DelayedChunkOperation> &entry) {
		                       return entry.get() == op;
	                       });
	if (it != queue.end()) { queue.erase(it); }
}

/// Marks a commit-pending delayed op durable. If its chunkserver status already arrived
/// (stashed by matoclserv_chunk_status while the commit was in flight), processes it
/// now; otherwise the entry stays queued and matoclserv_chunk_status processes it on
/// arrival as usual.
static void matoclserv_resolve_pending_delayed_op(matoclserventry *eptr,
                                                  DelayedChunkOperation *op) {
	auto &queue = eptr->delayedChunkOperations;
	auto it = std::find_if(queue.begin(), queue.end(),
	                       [op](const std::unique_ptr<DelayedChunkOperation> &entry) {
		                       return entry.get() == op;
	                       });
	if (it == queue.end()) {
		safs::log_warn("matoclserv: pending delayed chunk op vanished before its commit landed");
		return;
	}
	op->commitPending = false;
	if (op->statusArrived) {
		auto holder = std::move(*it);
		queue.erase(it);
		matoclserv_process_chunk_status(eptr, *holder, holder->pendingStatus,
		                                holder->pendingIsFailedCreate);
	}
}

static int masterSocket;             ///< Master socket for accepting new connections
static int32_t masterSocketDescPos;  ///< Position in the poll descriptors array for masterSocket
static int exiting;   ///< Flag indicating whether the server is exiting (1) or running (0)
static int starting;  ///< Flag indicating whether the server is starting (1) or not (0)

// from config
static std::string gListenHost;
static std::string gListenPort;

static uint32_t gIoLimitsAccumulate_ms;
static double gIoLimitsRefreshTime;
static uint32_t gIoLimitsConfigId;
static std::string gIoLimitsSubsystem;
static IoLimitsDatabase gIoLimitsDatabase;

static uint32_t statsPacketsReceived = 0;
static uint32_t statsPacketsSent = 0;
static uint64_t statsBytesReceived = 0;
static uint64_t statsBytesSent = 0;

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

/// Creates a new output packet for a given session entry.
/// @param eptr Pointer to the client connection in the master
/// @param type The type of the packet
/// @param size The size of the packet data
/// @return Pointer to the start of the packet data
uint8_t *matoclserv_createpacket(matoclserventry *eptr, uint32_t type, uint32_t size) {
	eptr->outputPackets.emplace_back(PacketHeader(type, size));
	return eptr->outputPackets.back().packet.data() + PacketHeader::kSize;
}

/// Creates a new output packet for a given session entry.
/// @param eptr Pointer to the client connection in the master
/// @param buffer The message buffer containing the packet data
void matoclserv_createpacket(matoclserventry *eptr, const MessageBuffer &buffer) {
	eptr->outputPackets.emplace_back(buffer);
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

/// Adds a client connection to the wait-for-unlock list for a given chunk ID.
/// @param eptr Pointer to the client connection in the master
/// @param chunkId The ID of the chunk to wait for
/// @param inode The inode of the chunk to wait for
/// @param chunkIndex The index of the chunk to wait for
static inline void matoclserv_add_to_wait_for_unlock_list(matoclserventry *eptr, uint64_t chunkId,
                                                          inode_t inode, uint32_t chunkIndex) {
	gWaitForUnlockMap[chunkId].emplace(eptr, inode, chunkIndex);
}

/// Removes a client connection from the wait-for-unlock list for all chunk IDs.
/// @param eptr Pointer to the client connection in the master
static inline void matoclserv_remove_entry_from_unlock_list(matoclserventry *eptr) {
	for (auto waitMapIt = gWaitForUnlockMap.begin(); waitMapIt != gWaitForUnlockMap.end();) {
		auto& waitSet = waitMapIt->second;

		for (auto it = waitSet.begin(); it != waitSet.end();) {
			if (std::get<0>(*it) == eptr) {
				it = waitSet.erase(it);
			} else {
				++it;
			}
		}

		if (waitSet.empty()) {
			waitMapIt = gWaitForUnlockMap.erase(waitMapIt);
		} else {
			++waitMapIt;
		}
	}
}

void matoclserv_notify_unlock_list(uint64_t chunkId) {
	auto it = gWaitForUnlockMap.find(chunkId);
	if (it != gWaitForUnlockMap.end()) {
		auto &clientsWaitingForUnlock = it->second;
		for (auto &[waitingClient, inode, chunkIndex] : clientsWaitingForUnlock) {
			// Don't send notices to clients that are being killed or that don't support notices
			// about unlocked chunks
			if (waitingClient->mode == ClientConnectionMode::KILL ||
			    waitingClient->version < kFirstVersionWithUnlockChunkNotice) {
				continue;
			}

			std::vector<uint8_t> outMessage;
			matocl::unlockChunkNotice::serialize(outMessage, inode, chunkIndex);
			matoclserv_createpacket(waitingClient, outMessage);
		}
		gWaitForUnlockMap.erase(it);
	}
	// If nothing is found, do nothing
}

/// Clears the wait-for-unlock list for all chunk IDs. This is called periodically to remove chunks
/// that are no longer relevant or to clean up stale entries.
static void matocl_clean_unlock_chunks_list() {
	gWaitForUnlockMap.clear();
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
	uint8_t status = gChunkOperations->getVersionAndLocations(
	    chunkId, eptr->peerIpAddress, chunkVersion, kMaxNumberOfChunkCopies, allChunkCopies);

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

void matoclserv_chunk_status(uint64_t chunkId, uint8_t status, bool isFailedCreateOperation) {
	for (const auto &entryPtr : matoclservList) {
		matoclserventry *eaptr = entryPtr.get();
		auto &queue = eaptr->delayedChunkOperations;
		auto it = std::find_if(queue.begin(), queue.end(),
		                       [chunkId](const std::unique_ptr<DelayedChunkOperation> &op) {
			                       return op->chunkId == chunkId;
		                       });
		if (it == queue.end()) { continue; }
		if (eaptr->mode == ClientConnectionMode::KILL) {
			// Client gone: no reply will be sent, but the chunk lock this write op holds still
			// must be released (and a failed create's speculative chunk dropped). Stash the
			// status instead of dropping it, so that cleanup acts on the real outcome. Cleanup
			// runs at disconnect teardown, or sooner via the commit-resolve hook if the
			// enqueuing commit is still pending. Truncate ops need no stash: teardown unlocks
			// them unconditionally.
			if ((*it)->type == FUSE_WRITE) {
				(*it)->statusArrived = true;
				(*it)->pendingStatus = status;
				(*it)->pendingIsFailedCreate = isFailedCreateOperation;
			}
			return;
		}
		if ((*it)->commitPending) {
			// The commit of the op that enqueued this entry is still in flight
			// (deferred): stash the status; the commit continuation processes it via
			// matoclserv_resolve_pending_delayed_op once the commit is durable.
			(*it)->statusArrived = true;
			(*it)->pendingStatus = status;
			(*it)->pendingIsFailedCreate = isFailedCreateOperation;
			return;
		}
		auto holder = std::move(*it);
		queue.erase(it);
		matoclserv_process_chunk_status(eaptr, *holder, status, isFailedCreateOperation);
		return;
	}
	safs_pretty_syslog(LOG_WARNING, "got chunk status, but don't want it");
}

/// Handles a completed chunkserver operation for a delayed op already removed from its
/// client's queue: builds and sends the client reply, committing any metadata
/// finalization. The truncate finalize commits while concurrent writers' async commits
/// on the same inode are in flight, so a retryable conflict replays on a fresh
/// transaction (it was the residual failure of the {ec,xor}_truncate_atomicity tests
/// under async commit); the commit itself is deferred, with the reply in the commit
/// continuation, so finalization never blocks the event loop on backend durability.
static void matoclserv_process_chunk_status(matoclserventry *eptr,
                                            const DelayedChunkOperation &operation, uint8_t status,
                                            bool isFailedCreateOperation) {
	const PacketSerializer *serializer = operation.serializer;
	const uint32_t messageId = operation.messageId;
	const uint64_t fileLength = operation.fileLength;
	const uint32_t lockId = operation.lockId;
	const uint8_t operationType = operation.type;
	const inode_t inode = operation.inode;
	const uint64_t chunkId = operation.chunkId;

	if (status == SAUNAFS_STATUS_OK) { dcm_modify(inode, eptr->sessionData->sessionId); }

	std::vector<uint8_t> reply;
	FsContext context = FsContext::getForMasterWithSession(
	    eventloop_time(), eptr->sessionData->rootInode, eptr->sessionData->flags, operation.uid,
	    operation.gid, operation.auid, operation.agid);

	// This can run for a client that disconnected after enqueuing the delayed op: the
	// delayed-op onCommit hook resolves a stashed status even for a KILL client, so the
	// durable chunk cleanup still happens. Client packets are suppressed for a killed client;
	// the lock-releasing commits below still run.
	const bool alive = eptr->mode != ClientConnectionMode::KILL;

	switch (operationType) {
	case FUSE_WRITE: {
		bool removeChunk = false;
		if (status != SAUNAFS_STATUS_OK) {
			// The chunkserver write failed: report the error (unless the client is gone) and,
			// on a failed create, drop the speculative chunk from the file in the cleanup
			// commit below (which runs regardless, to release the lock).
			removeChunk = isFailedCreateOperation;
			if (alive) {
				serializer->serializeFuseWriteChunk(reply, messageId, status);
				matoclserv_createpacket(eptr, std::move(reply));
			}
		} else if (alive) {
			// The chunkserver write succeeded: reply with the chunk version and locations.
			// This can still fail (e.g. NOCHUNK), in which case the lock is released below.
			status = matoclserv_fuse_write_chunk_respond(eptr, serializer,
					chunkId, messageId, fileLength, lockId);
			if (status == SAUNAFS_STATUS_OK) {
				return;  // success: the chunk stays locked for the active writer
			}
		}

		// Cleanup: release the chunk lock (writeEnd) and, on a failed create, drop the
		// chunk from the file. A killed client on a successful write reaches here too (via
		// matoclserv_resolve_pending_delayed_op): no write_chunk_end will arrive, so the
		// lock must be released now instead of stranded. Both mutate metadata that KV
		// backends persist, so they must run in a COMMITTED transaction; the previous bare
		// context was dropped uncommitted, leaking the lock and a stale chunk reference
		// after a master restart. Any client reply was already sent (or suppressed for a
		// killed client) and does not depend on this commit, so the on-commit hook only
		// logs a commit failure (mirrors the FUSE_TRUNCATE_END path); the log lives in
		// onCommit, not finish, so a killed client cannot suppress it.
		OpReplay runCleanup = [context, inode, chunkId,
		                       removeChunk](FilesystemOperationContext &ctx) -> uint8_t {
			if (removeChunk) {
				gFSOperations->removeChunkFromFile(context, ctx, inode, chunkId);
			}
			gFSOperations->writeEnd(ctx, 0, 0, chunkId, 0);  // ignore status, just release
			return SAUNAFS_STATUS_OK;
		};
		matoclserv_submit_op(
		    eptr, std::move(runCleanup), {},
		    [inode, chunkId](uint8_t commitStatus) {
			    if (commitStatus != SAUNAFS_STATUS_OK) {
				    safs::log_err(
				        "matoclserv_process_chunk_status: transaction failed to commit "
				        "while releasing the write chunk lock: inode {}, chunkId {}",
				        inode, chunkId);
			    }
		    });
		return;
	}
	case FUSE_TRUNCATE_BEGIN:
		if (alive) {
			if (status != SAUNAFS_STATUS_OK) {
				matocl::fuseTruncate::serialize(reply, messageId, status);
			} else {
				matocl::fuseTruncate::serialize(reply, messageId, fileLength, lockId);
			}
			matoclserv_createpacket(eptr, std::move(reply));
		}
		return;
	case FUSE_TRUNCATE:
	case FUSE_TRUNCATE_END:
		if (status != SAUNAFS_STATUS_OK) {
			// Commit endSetLength's unlock changelog even on error, so KV backends
			// persist the unlock regardless of the chunk operation outcome. The reply
			// (the chunk operation's error) does not depend on this commit's outcome,
			// so the continuation only logs a commit failure.
			OpReplay runUnlock = [chunkId](FilesystemOperationContext &ctx) -> uint8_t {
				gFSOperations->endSetLength(ctx, chunkId);
				return SAUNAFS_STATUS_OK;
			};
			// Reply (kill-gated) lives in finish; the commit-failure log lives in the
			// always-run onCommit hook, so a killed client cannot suppress it.
			matoclserv_submit_op(
			    eptr, std::move(runUnlock),
			    [eptr, serializer, operationType, messageId, status](uint8_t) {
				    std::vector<uint8_t> reply;
				    serializer->serializeFuseTruncate(reply, operationType, messageId, status);
				    matoclserv_createpacket(eptr, std::move(reply));
			    },
			    [inode, chunkId](uint8_t commitStatus) {
				    if (commitStatus != SAUNAFS_STATUS_OK) {
					    safs::log_err(
					        "matoclserv_process_chunk_status: transaction failed to commit "
					        "after endSetLength: inode {}, chunkId {}",
					        inode, chunkId);
				    }
			    });
			return;
		}
		{
			auto replyAttr = std::make_shared<Attributes>();
			OpReplay runFinalize = [context, inode, chunkId, fileLength,
			                        replyAttr](FilesystemOperationContext &ctx) -> uint8_t {
				gFSOperations->endSetLength(ctx, chunkId);
				return gFSOperations->doSetLength(context, ctx, inode, fileLength, *replyAttr);
			};
			auto sendFinalizeReply = [eptr, serializer, operationType, messageId,
			                          replyAttr](uint8_t finalStatus) {
				std::vector<uint8_t> reply;
				if (finalStatus == SAUNAFS_STATUS_OK) {
					serializer->serializeFuseTruncate(reply, operationType, messageId, *replyAttr);
				} else {
					serializer->serializeFuseTruncate(reply, operationType, messageId, finalStatus);
				}
				matoclserv_createpacket(eptr, std::move(reply));
			};
			matoclserv_submit_op(
			    eptr, std::move(runFinalize),
			    [sendFinalizeReply](uint8_t commitStatus) { sendFinalizeReply(commitStatus); });
			return;
		}
	default:
		safs_pretty_syslog(LOG_WARNING,"got chunk status, but operation type is unknown");
	}
}

/// Starts/continues a TLS handshake (non-blocking)
/// @param eptr Pointer to the client connection in the master
void matoclserv_tlshandshake(matoclserventry *eptr) {
	sassert(eptr->mode == ClientConnectionMode::HANDSHAKE);

	int ret = SSL_accept(eptr->tlsSession->session());

	if (ret == 1) {
		safs::log_info("TLS handshake completed with client from {}:{}",
		               ipToString(eptr->peerIpAddress), eptr->peerPort);
		eptr->mode = ClientConnectionMode::HEADER;
		return;
	}

	int err = SSL_get_error(eptr->tlsSession->session(), ret);
	eptr->lastHandshakeError = err;

	if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
		safs::log_info("TLS handshake in progress with client from {}:{}: {}",
		               ipToString(eptr->peerIpAddress), eptr->peerPort, opensslErrorString(err));
		return;  // retry later
	}

	eptr->mode = ClientConnectionMode::KILL;
	safs::log_err("TLS handshake failed: {}", opensslErrorString(err));
}

/// Initiate a TLS connection with the mount.
/// @param eptr Pointer to the client connection in the master
void matoclserv_starttls(matoclserventry *eptr) {
	if (eptr->tlsSession != nullptr) {
		safs::log_warn(
		    "Attempted to start TLS session with client from {}:{}, but TLS session already exists",
		    ipToString(eptr->peerIpAddress), eptr->peerPort);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	// Initialize a TLS session for the peer.
	std::string keyFile = cfg_getstring("TLS_KEY_FILE", std::string(TlsSession::kNoFile));
	std::string certFile = cfg_getstring("TLS_CERT_FILE", std::string(TlsSession::kNoFile));
	std::string trustFile = cfg_getstring("TLS_CA_CERT_FILE", std::string(TlsSession::kNoFile));

	try {
		eptr->tlsSession =
		    std::make_unique<TlsSession>(eptr->socket, true, keyFile, certFile, trustFile);
		safs::log_info("Starting TLS session with client from {}:{}",
		               ipToString(eptr->peerIpAddress), eptr->peerPort);
		eptr->mode = ClientConnectionMode::HANDSHAKE;
		matoclserv_tlshandshake(eptr);
	} catch (const std::exception &e) {
		eptr->mode = ClientConnectionMode::KILL;
		safs::log_err("Failed to start TLS session with client from {}:{}: {}",
		              ipToString(eptr->peerIpAddress), eptr->peerPort, e.what());
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
		metadataVersion = gFSOperations->getMetadataVersion();
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
	const std::map<int, Goal> &goalsMap = gFSOperations->getAllGoalDefinitions();

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
	auto message =
	    matocl::chunksHealth::build(regularChunksOnly, gChunkOperations->getAvailabilityState(),
	                                gChunkOperations->getReplicationState());

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

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);

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
				size += gFSOperations->getDirPathSize(fsOpContext, eaptr->sessionData->rootInode);
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
				pathLength =
				    gFSOperations->getDirPathSize(fsOpContext, eaptr->sessionData->rootInode);
				put32bit(&ptr, pathLength);
				if (pathLength > 0) {
					gFSOperations->getDirPathData(fsOpContext, eaptr->sessionData->rootInode, ptr,
					                              pathLength);
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

	gFSOperations->getFSStats(&statistics.totalSpace, &statistics.availableSpace,
	                          &statistics.trashSpace, &statistics.trashNodes,
	                          &statistics.reservedSpace, &statistics.reservedNodes,
	                          &statistics.allNodes, &statistics.dirNodes, &statistics.fileNodes,
	                          &statistics.symlinkNodes);

	gChunkOperations->info(&statistics.chunks, &statistics.chunkCopies, &statistics.regularCopies);

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

	auto chunksInfoSize = gChunkOperations->getChunkInfoSerializedSize();

	ptr = matoclserv_createpacket(eptr, MATOCL_CHUNKSTEST_INFO, chunksInfoSize);
	gChunkOperations->storeChunkInfo(ptr);
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

	gChunkOperations->storeChunkCounters(ptr, matrixId);
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

void matoclserv_inotifier_list(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	cltoma::inotifierList::deserialize(data, length);
	matoclserv_createpacket(eptr, matocl::inotifierList::build(matontserv_inotifiers()));
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
		safs::log_info("CLTOMA_FUSE_REGISTER/NOACL - rejected");
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	auto checkMinimumVersion = [](matoclserventry *eptr) {
		if (eptr->version < kFirstECVersion) {
			safs::log_info("Got register packet from client ({}) older than {} - rejecting",
			               saunafsVersionToString(eptr->version),
			               saunafsVersionToString(kFirstECVersion));
			eptr->mode = ClientConnectionMode::KILL;
			return false;
		}
		return true;
	};

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

			if (!checkMinimumVersion(eptr)) {
				return;
			}

			get32bit(&rptr, infoLength);
			if (length < kRegisterNewSessionMinSize + infoLength) {
				safs::log_info("CLTOMA_FUSE_REGISTER/ACL.2 - wrong size ({}/>={} + infoLength({}))",
				               length, kRegisterNewSessionMinSize, infoLength);
				eptr->mode = ClientConnectionMode::KILL;
				return;
			}

			info = reinterpret_cast<const char*>(rptr);
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
				status = gFSOperations->getRootInode(&rootInode, path);
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

				matoclserv_persist_session(eptr->sessionData);
			}

			// answer

			wptr = matoclserv_createpacket(eptr, MATOCL_FUSE_REGISTER,
			                               (status == SAUNAFS_STATUS_OK) ? 35 : sizeof(status));

			if (status != SAUNAFS_STATUS_OK) {
				put8bit(&wptr, status);
				return;
			}
			sessionId = eptr->sessionData->sessionId;

			put16bit(&wptr, SAUNAFS_PACKAGE_VERSION_MAJOR);
			put8bit(&wptr, SAUNAFS_PACKAGE_VERSION_MINOR);
			put8bit(&wptr, SAUNAFS_PACKAGE_VERSION_MICRO);
			put32bit(&wptr, sessionId);
			put8bit(&wptr, sessionFlags);
			put32bit(&wptr, rootUid);
			put32bit(&wptr, rootGid);
			put32bit(&wptr, mapAllUid);
			put32bit(&wptr, mapAllGid);
			put8bit(&wptr, minGoal);
			put8bit(&wptr, maxGoal);
			put32bit(&wptr, minTrashTime);
			put32bit(&wptr, maxTrashTime);

			eptr->ioLimitsEnabled = true;
			matoclserv_send_iolimits_cfg(eptr);
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

			if (!checkMinimumVersion(eptr)) {
				return;
			}

			get32bit(&rptr, infoLength);

			if (length != kRegisterNewMetaSessionMinSize + infoLength &&
			    length != kRegisterNewMetaSessionMinSize + 16 + infoLength) {
				safs::log_info("CLTOMA_FUSE_REGISTER/ACL.5 - wrong size ({}/{} + ileng({}) + 16)",
				               length, kRegisterNewMetaSessionMinSize, infoLength);
				eptr->mode = ClientConnectionMode::KILL;
				return;
			}

			info = reinterpret_cast<const char*>(rptr);
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

				matoclserv_persist_session(eptr->sessionData);
			}

			// answer

			wptr = matoclserv_createpacket(eptr, MATOCL_FUSE_REGISTER,
			                               (status == SAUNAFS_STATUS_OK) ? 19 : sizeof(status));
			if (status!=SAUNAFS_STATUS_OK) {
				put8bit(&wptr,status);
				return;
			}
			sessionId = eptr->sessionData->sessionId;

			put16bit(&wptr, SAUNAFS_PACKAGE_VERSION_MAJOR);
			put8bit(&wptr, SAUNAFS_PACKAGE_VERSION_MINOR);
			put8bit(&wptr, SAUNAFS_PACKAGE_VERSION_MICRO);
			put32bit(&wptr, sessionId);
			put8bit(&wptr, sessionFlags);
			put8bit(&wptr, minGoal);
			put8bit(&wptr, maxGoal);
			put32bit(&wptr, minTrashTime);
			put32bit(&wptr, maxTrashTime);

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

			if (!checkMinimumVersion(eptr)) {
				return;
			}

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
				if (eptr->sessionData->rootInode != 0) {
					eptr->ioLimitsEnabled = true;
					matoclserv_send_iolimits_cfg(eptr);
				}
				eptr->registered = ClientState::kRegistered;
			} else {
				eptr->registered = ClientState::kOldTools;
				safs::log_info("Registered old tools from {}:{}, rejecting ...",
				               ipToString(eptr->peerIpAddress), eptr->peerPort);
				eptr->mode = ClientConnectionMode::KILL;  // old tools disconnect after register
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

/// Helper function to commit transaction batches for bulk operations.
/// Committing in batches avoids transaction too old issues in KV backends.
/// @param fsOpContext The filesystem operation context to commit and replace with a fresh
/// context.
/// @param operationCount Reference to the current operation count; reset after commit.
[[nodiscard]] bool commitTransactionBatch(FilesystemOperationContext &fsOpContext,
                                          size_t &operationCount) {
	assert(fsOpContext.hasReadWriteTransaction());

	auto commitResult = fsOpContext.getReadWriteTransaction()->commit();

	fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);
	operationCount = 0;

	return commitResult;
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

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	changelog_disable_flush();

	// The in-memory openFilesSet mutation for a batch is applied only after that
	// batch's transaction commits. A failed commit rolls the backend back, so
	// applying the memory change anyway would desynchronize the two (a later open
	// would then re-acquire an inode the backend still considers acquired); the
	// client retransmits its reserved list periodically, so a skipped batch just
	// retries on the next message.
	std::set<inode_t> &openFiles = eptr->sessionData->openFilesSet;
	const bool transactional = fsOpContext.hasReadWriteTransaction();

	std::vector<inode_t> toRelease;
	for (inode_t openFileIno : openFiles) {
		if (inodes_to_reserve.contains(openFileIno)) {
			// no need to remind this file as reserved, as it is already open
			inodes_to_reserve.erase(openFileIno);
		} else {
			// release files not belonging to the reserve inodes list provided
			toRelease.push_back(openFileIno);
		}
	}

	size_t operationCount = 0;
	std::vector<inode_t> releasedBatch;
	std::vector<inode_t> acquiredBatch;

	// Applies (on commit success) and drops the current batch's pending in-memory
	// changes. A failed batch is dropped entirely: its backend writes rolled back
	// with the transaction, so its memory changes must not be applied later.
	auto applyBatches = [&openFiles, &releasedBatch, &acquiredBatch](bool committed) {
		if (committed) {
			for (inode_t released : releasedBatch) { openFiles.erase(released); }
			for (inode_t acquired : acquiredBatch) { openFiles.insert(acquired); }
		}
		releasedBatch.clear();
		acquiredBatch.clear();
	};

	for (inode_t openFileIno : toRelease) {
		gFSOperations->release(context, fsOpContext, openFileIno, eptr->sessionData->sessionId);
		if (!transactional) {
			openFiles.erase(openFileIno);
			continue;
		}
		releasedBatch.push_back(openFileIno);
		operationCount++;

		if (operationCount >= kTransactionBatchSize) {
			const bool committed = commitTransactionBatch(fsOpContext, operationCount);
			if (!committed) {
				safs::log_err("{}: failed to commit release batch for reserving inodes", __func__);
			}
			applyBatches(committed);
		}
	}

	for (const auto &inode_to_reserve : inodes_to_reserve) {
		if (gFSOperations->acquire(context, fsOpContext, inode_to_reserve,
		                           eptr->sessionData->sessionId) != SAUNAFS_STATUS_OK) {
			continue;
		}
		if (!transactional) {
			openFiles.insert(inode_to_reserve);
			continue;
		}
		acquiredBatch.push_back(inode_to_reserve);
		operationCount++;

		if (operationCount >= kTransactionBatchSize) {
			const bool committed = commitTransactionBatch(fsOpContext, operationCount);
			if (!committed) {
				safs::log_err("{}: failed to commit acquire batch for reserving inodes", __func__);
			}
			applyBatches(committed);
		}
	}

	// Commit the final batch for KV backends
	if (transactional && operationCount > 0) {
		const bool committed = fsOpContext.getReadWriteTransaction()->commit();
		if (!committed) {
			safs::log_err("{}: failed to commit final transaction for reserving inodes", __func__);
		}
		applyBatches(committed);
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
	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);

	gFSOperations->statfs(context, fsOpContext, &totalspace, &availspace, &trashspace,
	                      &reservedspace, &inodes);

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
		status = gFSOperations->access(context, inode, modemask);
	}

	constexpr uint8_t kAnswerSize = sizeof(msgid) + sizeof(status);
	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_ACCESS, kAnswerSize);
	put32bit(&ptr, msgid);
	put8bit(&ptr, status);
}

void matoclserv_sau_whole_path_lookup(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t msgid;
	inode_t parentInode;
	inode_t found_inode;
	std::string path;
	uint32_t uid, gid;
	Attributes attr;
	uint8_t status = SAUNAFS_STATUS_OK;

	cltoma::wholePathLookup::deserialize(data, length, msgid, parentInode, path, uid, gid);

	status = matoclserv_check_group_cache(eptr, gid);
	if (status == SAUNAFS_STATUS_OK) {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		status = gFSOperations->wholePathLookup(context, parentInode, path, &found_inode, attr);
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
		status = gFSOperations->fullPathByInode(context, inode, fullPath);
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
		status = gFSOperations->quotaGet(context, owners, results);

		if (inode == context.rootinode() && !foundContextRootInodeResult(inode)) {
			auto fsOpContext = gFSOperations->createFilesystemOperationContext(
			    FilesystemOperationContext::TransactionType::kReadOnly);
			auto *ino = gFSOperations->nodeOperations()->idToNode(fsOpContext, inode);
			StatsRecord rootInodeStatRec;
			gFSOperations->nodeOperations()->getStats(fsOpContext, ino, &rootInodeStatRec);
			results.emplace_back(QuotaEntry{QuotaEntryKey{QuotaOwner{QuotaOwnerType::kInode, inode},
			                                              QuotaRigor::kUsed, QuotaResource::kSize},
			                                rootInodeStatRec.size});
		}
	}

	MessageBuffer reply;
	if (status == SAUNAFS_STATUS_OK) {
		status = gFSOperations->quotaGetInfo(matoclserv_get_context(eptr), results, info);
	}
	if (status == SAUNAFS_STATUS_OK) {
		matocl::fuseGetSelfQuota::serialize(reply, messageId, results);
	} else {
		matocl::fuseGetSelfQuota::serialize(reply, messageId, status);
	}
	matoclserv_createpacket(eptr, std::move(reply));
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
		auto fsOpContext = gFSOperations->createFilesystemOperationContext(
		    FilesystemOperationContext::TransactionType::kReadOnly);
		status = gFSOperations->getAttr(context, fsOpContext, inode, attr);
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
	uint32_t msgid;
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

	// Out-state shared between the replayable body and the deferred reply
	// continuation; each body run refreshes it.
	auto replyAttr = std::make_shared<Attributes>();

	// Replayable body: setattr mutates the inode node, so it can conflict with a
	// concurrent writer/op on the same inode under async commit.
	OpReplay runSetAttr = [eptr, uid, gid, inode, setmask, attrmode, attruid, attrgid, attratime,
	                       attrmtime, sugidclearmode,
	                       replyAttr](FilesystemOperationContext &ctx) -> uint8_t {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		return gFSOperations->setAttr(context, ctx, inode, setmask, attrmode, attruid, attrgid,
		                              attratime, attrmtime, sugidclearmode, *replyAttr);
	};

	if (eptr->sessionData) {
		eptr->sessionData->currHourOperationsStats[2]++;
	}

	auto sendSetAttrReply = [eptr, msgid, replyAttr](uint8_t replyStatus) {
		const uint32_t kFailedAnswerSize = sizeof(msgid) + sizeof(replyStatus);
		const uint32_t kSuccessAnswerSize = sizeof(msgid) + replyAttr->size();
		uint8_t answerSize =
		    (replyStatus != SAUNAFS_STATUS_OK) ? kFailedAnswerSize : kSuccessAnswerSize;
		uint8_t *ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_SETATTR, answerSize);
		put32bit(&ptr, msgid);
		if (replyStatus != SAUNAFS_STATUS_OK) {
			put8bit(&ptr, replyStatus);
		} else {
			memcpy(ptr, replyAttr->data(), replyAttr->size());
		}
	};

	if (status != SAUNAFS_STATUS_OK) {  // group-cache error: reply without running the op
		sendSetAttrReply(status);
		return;
	}

	// Group commit: the continuation replies once the op's batch is durable (or
	// with the body's own error status).
	matoclserv_submit_op(eptr, std::move(runSetAttr), [sendSetAttrReply](uint8_t commitStatus) {
		sendSetAttrReply(commitStatus);
	});
}

void matoclserv_fuse_truncate(matoclserventry *eptr, PacketHeader header, const uint8_t *data) {
	sassert(header.type == SAU_CLTOMA_FUSE_TRUNCATE || header.type == SAU_CLTOMA_FUSE_TRUNCATE_END);

	// Deserialize the request (immutable inputs for the replayable body below)
	std::vector<uint8_t> request(data, data + header.length);
	uint8_t status = SAUNAFS_STATUS_OK;
	uint32_t messageId, uid, gid;
	inode_t inode;
	uint32_t reqLockId = 0;
	bool reqOpened = false;
	uint64_t reqLength;

	const PacketSerializer *serializer =
	    PacketSerializer::getSerializer(header.type, eptr->version);
	const bool isEnd = (header.type == SAU_CLTOMA_FUSE_TRUNCATE_END);

	if (isEnd) {
		cltoma::fuseTruncateEnd::deserialize(request, messageId, inode, uid, gid, reqLength,
		                                     reqLockId);
	} else {
		serializer->deserializeFuseTruncate(request, messageId, inode, reqOpened, uid, gid,
		                                    reqLength);
	}
	status = matoclserv_check_group_cache(eptr, gid);

	// Truncate is a small state machine; the body runs the WHOLE pre-commit decision tree
	// so a retryable commit conflict (concurrent writers/truncates on the same inode, the
	// EC/XOR truncate-atomicity workload) replays it on a fresh transaction instead of
	// replying EIO. The body records which reply path the run took (phase) and refreshes
	// every mutable output on each run. Commit-worthy outcomes (final OK, the
	// writeChunk-no-duplication reply, and DELAYED) return OK so the commit proceeds;
	// other statuses return as-is without committing.
	//
	// When a run initiates a chunkserver operation (phase kDelayed) it enqueues the
	// DelayedChunkOperation entry ITSELF, marked commitPending, because the deferred
	// commit no longer blocks the event loop: the chunkserver's status could arrive
	// before the commit lands, and matoclserv_chunk_status stashes it on the pending
	// entry until the commit continuation resolves it. A replay drops the previous
	// attempt's entry before re-running.
	enum class TruncPhase : uint8_t { kError, kReplyWriteChunk, kDelayed, kDone };
	struct TruncState {
		TruncPhase phase = TruncPhase::kError;
		uint32_t type = 0;
		uint64_t chunkId = 0;
		uint32_t lockId = 0;
		uint64_t length = 0;
		uint64_t fileLength = 0;
		Attributes attr{};
		DelayedChunkOperation *queued = nullptr;  ///< commit-pending entry of the latest run
		bool tookDelayedPath = false;  ///< onCommit took the delayed path; reply comes via chunk_status
	};
	auto truncState = std::make_shared<TruncState>();
	truncState->type = isEnd ? FUSE_TRUNCATE_END : FUSE_TRUNCATE;
	// lockId must survive body replays (mirror write_chunk): a batch-conflict replay of the
	// non-END duplicate path passes the previously allocated lockId back into writeChunk so it
	// re-enters its own surviving in-memory lock instead of bouncing off it as LOCKED. For END it
	// stays the client-supplied reqLockId. So it is initialized ONCE here, not reset per run.
	truncState->lockId = reqLockId;

	OpReplay runTruncate = [eptr, serializer, isEnd, reqOpened, reqLength, inode, uid, gid,
	                        messageId, truncState](FilesystemOperationContext &ctx) -> uint8_t {
		// Replay: drop the delayed entry a previous attempt enqueued, then reset the
		// per-run state mutated by previous attempts (NOT lockId; see its init above).
		matoclserv_drop_queued_delayed_op(eptr, truncState->queued);
		truncState->queued = nullptr;
		truncState->phase = TruncPhase::kError;
		truncState->type = isEnd ? FUSE_TRUNCATE_END : FUSE_TRUNCATE;
		truncState->length = reqLength;
		bool opened = isEnd ? true : reqOpened;  // END: permissions checked on SAU_CLTOMA_TRUNCATE
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		uint8_t st = SAUNAFS_STATUS_OK;

		if (isEnd) {
			// We have to verify lockid in this request
			if (truncState->lockId == 0) {
				// unlocking with lockid == 0 means "force unlock", this is not allowed
				return SAUNAFS_ERROR_WRONGLOCKID;
			}
			// let's check if chunk is still locked by us
			st = gFSOperations->getChunkId(context, ctx, inode, truncState->length / SFSCHUNKSIZE,
			                               &truncState->chunkId);
			if (st == SAUNAFS_STATUS_OK) {
				st = gChunkOperations->canUnlock(truncState->chunkId, truncState->lockId);
			}
			if (st != SAUNAFS_STATUS_OK) { return st; }
			gFSOperations->endSetLength(ctx, truncState->chunkId);
		}

		st =
		    gFSOperations->trySetLength(context, ctx, inode, opened, truncState->length,
		                                (truncState->type != FUSE_TRUNCATE_END), truncState->lockId,
		                                truncState->attr, &truncState->chunkId);

		// In case of SAUNAFS_ERROR_NOTPOSSIBLE we have to tell the client to write the
		// chunk before truncating (new client truncating an xor/ec chunk does it itself).
		if (st == SAUNAFS_ERROR_NOTPOSSIBLE && !isEnd) {
			uint8_t chunkOperationPending;
			st = gFSOperations->writeChunk(context, ctx, inode, truncState->length / SFSCHUNKSIZE,
			                               &truncState->lockId, &truncState->chunkId,
			                               &chunkOperationPending, &truncState->fileLength);
			if (st != SAUNAFS_STATUS_OK) { return st; }
			if (chunkOperationPending) {
				// But first we have to duplicate chunk :)
				truncState->type = FUSE_TRUNCATE_BEGIN;
				truncState->length = truncState->fileLength;
				truncState->phase = TruncPhase::kDelayed;
			} else {
				// No duplication is needed: commit writeChunk's metadata, reply
				// length+lockId.
				truncState->phase = TruncPhase::kReplyWriteChunk;
				return SAUNAFS_STATUS_OK;
			}
		} else if (st == SAUNAFS_ERROR_DELAYED) {
			// Truncate request has been sent to chunkservers; commit, then the reply
			// comes via matoclserv_chunk_status.
			truncState->phase = TruncPhase::kDelayed;
		} else if (st != SAUNAFS_STATUS_OK) {
			return st;
		} else {
			st = gFSOperations->doSetLength(context, ctx, inode, truncState->length,
			                                truncState->attr);
			if (st != SAUNAFS_STATUS_OK) { return st; }
			truncState->phase = TruncPhase::kDone;
			return SAUNAFS_STATUS_OK;
		}

		// phase == kDelayed: a chunkserver operation is in flight; enqueue its entry
		// now (commit-pending) so its status is never dropped while our commit lands.
		auto operation = std::make_unique<DelayedChunkOperation>();
		operation->chunkId = truncState->chunkId;
		operation->messageId = messageId;
		operation->inode = inode;
		operation->uid = context.uid();
		operation->gid = context.gid();
		operation->auid = context.auid();
		operation->agid = context.agid();
		operation->fileLength = truncState->length;
		operation->lockId = truncState->lockId;
		operation->type = truncState->type;
		operation->serializer = serializer;
		operation->commitPending = true;
		truncState->queued = operation.get();
		eptr->delayedChunkOperations.push_back(std::move(operation));
		return SAUNAFS_STATUS_OK;
	};

	if (eptr->sessionData) { eptr->sessionData->currHourOperationsStats[2]++; }

	auto sendTruncateReply = [eptr, serializer, messageId, inode, truncState](uint8_t replyStatus) {
		std::vector<uint8_t> reply;
		if (replyStatus == SAUNAFS_STATUS_OK && truncState->phase == TruncPhase::kReplyWriteChunk) {
			// New client must write the chunk itself before truncating.
			matocl::fuseTruncate::serialize(reply, messageId, truncState->fileLength,
			                                truncState->lockId);
		} else if (replyStatus == SAUNAFS_STATUS_OK) {
			dcm_modify(inode, eptr->sessionData->sessionId);
			serializer->serializeFuseTruncate(reply, truncState->type, messageId, truncState->attr);
		} else if (truncState->type == FUSE_TRUNCATE_BEGIN) {
			// For BEGIN operations, use the status packet format
			matocl::fuseTruncate::serialize(reply, messageId, replyStatus);
		} else {
			safs::log_debug("matoclserv_fuse_truncate: Failed to truncate: {} (code {})",
			                saunafs_error_string(replyStatus), replyStatus);
			serializer->serializeFuseTruncate(reply, truncState->type, messageId, replyStatus);
		}
		matoclserv_createpacket(eptr, reply);
	};

	if (status != SAUNAFS_STATUS_OK) {  // group-cache error: reply without running the op
		sendTruncateReply(status);
		return;
	}

	// Group commit; the continuation reconciles whichever phase the FINAL body run
	// took (a replay can flip it, e.g. kDone to kDelayed when a concurrent chunk
	// operation appears) and handles the body's own error statuses.
	matoclserv_submit_op(
	    eptr, std::move(runTruncate),
	    [truncState, sendTruncateReply](uint8_t commitStatus) {
		    // Reply only (kill-gated). On the delayed path the reply is emitted by
		    // process_chunk_status from the onCommit hook, so it is skipped here.
		    if (commitStatus != SAUNAFS_STATUS_OK || !truncState->tookDelayedPath) {
			    sendTruncateReply(commitStatus);
		    }
	    },
	    [eptr, truncState](uint8_t commitStatus) {
		    // Durable delayed-op transition; runs with the final status even for a killed
		    // client so a stashed chunkserver status is processed and the lock released.
		    if (commitStatus != SAUNAFS_STATUS_OK) {
			    matoclserv_drop_queued_delayed_op(eptr, truncState->queued);
			    truncState->queued = nullptr;
		    } else if (truncState->queued != nullptr) {
			    truncState->tookDelayedPath = true;
			    DelayedChunkOperation *queued = truncState->queued;
			    truncState->queued = nullptr;
			    matoclserv_resolve_pending_delayed_op(eptr, queued);
		    }
	    });
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

	// ReadWrite mode is needed to update atime of the symlink
	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	status = gFSOperations->readlink(context, fsOpContext, inode, path);

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.getReadWriteTransaction()->commit()) {
			// Best-effort: atime update only, do not fail the readlink.
			safs::log_err("{}: transaction failed to commit: inode {}", __func__, inode);
		}
	}

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
	uint32_t msgid;
	uint8_t status;

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
	status = matoclserv_check_group_cache(eptr, gid);

	// Reply data (new symlink inode + its attributes) shared between the body and the
	// reply continuation; replyData->first defaults to 0 (request a fresh inode id).
	// The name and target path are copied out of the packet buffer, which dies with
	// this handler frame.
	auto replyData = std::make_shared<std::pair<inode_t, Attributes>>();
	HString hname(reinterpret_cast<const char *>(name), nleng);
	std::string targetPath(reinterpret_cast<const char *>(path), pleng);
	OpReplay runSymlink = [eptr, uid, gid, inode, hname, targetPath,
	                       replyData](FilesystemOperationContext &ctx) -> uint8_t {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		// Reset the shared out-param: a batch-conflict replay re-runs this body, and
		// symlink() asserts the new-inode out-param starts at 0 (mknod resets internally).
		replyData->first = 0;
		return gFSOperations->symlink(context, ctx, inode, hname, targetPath, &replyData->first,
		                              &replyData->second);
	};

	if (eptr->sessionData) {
		eptr->sessionData->currHourOperationsStats[6]++;
	}

	auto sendSymlinkReply = [eptr, msgid, replyData](uint8_t replyStatus) {
		const uint32_t kFailedAnswerSize = sizeof(msgid) + sizeof(replyStatus);
		const uint32_t kSuccessAnswerSize =
		    sizeof(msgid) + sizeof(replyData->first) + replyData->second.size();
		uint32_t answerSize =
		    (replyStatus != SAUNAFS_STATUS_OK) ? kFailedAnswerSize : kSuccessAnswerSize;
		uint8_t *ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_SYMLINK, answerSize);
		put32bit(&ptr, msgid);
		if (replyStatus != SAUNAFS_STATUS_OK) {
			put8bit(&ptr, replyStatus);
		} else {
			putINode(&ptr, replyData->first);
			memcpy(ptr, replyData->second.data(), replyData->second.size());
		}
	};

	if (status != SAUNAFS_STATUS_OK) {  // group-cache error: reply without running the op
		sendSymlinkReply(status);
		return;
	}

	// Group commit: the continuation replies once the op's batch is durable (or with
	// the body's own error status).
	matoclserv_submit_op(eptr, std::move(runSymlink),
	                     [sendSymlinkReply](uint8_t commitStatus) { sendSymlinkReply(commitStatus); });
}

void matoclserv_fuse_mknod(matoclserventry *eptr, PacketHeader header, const uint8_t *data) {
	uint32_t messageId, uid, gid, rdev;
	inode_t parentInode;
	LegacyString<uint8_t> name;
	uint8_t type;
	uint16_t mode;
	uint16_t umask;

	if (header.type == SAU_CLTOMA_FUSE_MKNOD) {
		cltoma::fuseMknod::deserialize(data, header.length, messageId, parentInode, name, type,
		                               mode, umask, uid, gid, rdev);
	} else {
		throw IncorrectDeserializationException(
				"Unknown packet type for matoclserv_fuse_mknod: " + std::to_string(header.type));
	}

	// Reply data shared between the op body and the reply continuation. The op body
	// is replayable (see runMknod), so a retry refreshes these via the same struct.
	auto replyData = std::make_shared<std::pair<inode_t, Attributes>>();
	uint8_t status = matoclserv_check_group_cache(eptr, gid);

	// The op body as a replayable closure: re-runnable on a fresh transaction if the
	// commit hits a retryable conflict. It mutates only backend state (plus ephemeral
	// per-op in-memory node state) and refreshes replyData; it carries no in-process
	// persistent side effect, so replay never double-applies anything.
	OpReplay runMknod = [eptr, uid, gid, parentInode, name, type, mode, umask, rdev,
	                     replyData](FilesystemOperationContext &ctx) -> uint8_t {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		return gFSOperations->mknod(context, ctx, parentInode, HString(name),
		                            static_cast<FSNodeType>(type), mode, umask, rdev,
		                            &replyData->first, replyData->second);
	};

	if (eptr->sessionData) {
		eptr->sessionData->currHourOperationsStats[8]++;
	}

	auto sendMknodReply = [eptr, messageId, replyData](uint8_t replyStatus) {
		MessageBuffer reply;
		if (replyStatus == SAUNAFS_STATUS_OK) {
			matocl::fuseMknod::serialize(reply, messageId, replyData->first, replyData->second);
		} else {
			matocl::fuseMknod::serialize(reply, messageId, replyStatus);
		}
		matoclserv_createpacket(eptr, std::move(reply));
	};

	if (status != SAUNAFS_STATUS_OK) {  // group-cache error: reply without running the op
		sendMknodReply(status);
		return;
	}

	// Group commit: the continuation replies once the op's batch is durable (or
	// with the body's own error status).
	matoclserv_submit_op(eptr, std::move(runMknod), [sendMknodReply](uint8_t commitStatus) {
		sendMknodReply(commitStatus);
	});
}

// Fused create+open: mknod a file and acquire it for the calling session in a single
// transaction, so an empty-file create costs one durable commit instead of two (the
// mknod + open round-trip pair). Version-gated on the client; old clients still send
// the separate FUSE_MKNOD and FUSE_OPEN.
void matoclserv_fuse_create(matoclserventry *eptr, PacketHeader header, const uint8_t *data) {
	uint32_t messageId, uid, gid;
	inode_t parentInode;
	LegacyString<uint8_t> name;
	uint16_t mode, umask;
	uint8_t flags;

	if (header.type == SAU_CLTOMA_FUSE_CREATE) {
		cltoma::fuseCreate::deserialize(data, header.length, messageId, parentInode, name, mode,
		                                umask, uid, gid, flags);
	} else {
		throw IncorrectDeserializationException(
				"Unknown packet type for matoclserv_fuse_create: " + std::to_string(header.type));
	}

	// Reply data shared between the op body and the reply continuation; a retry
	// refreshes it through the same struct.
	auto replyData = std::make_shared<std::pair<inode_t, Attributes>>();
	uint8_t status = matoclserv_check_group_cache(eptr, gid);

	// Replayable op body: mknod + acquire the open file + openCheck, all in one
	// transaction. On the KV backend the in-memory open-file record (Session::openFilesSet)
	// is deliberately NOT done here; it is applied once on commit success (below) so a
	// retry replays the persistent acquire on a fresh transaction without the already-open
	// early-return skipping it. On the in-memory Master there is no transaction to roll
	// back, so acquire+record are coupled here (matoclserv_insert_open_file): a later
	// openCheck failure must not orphan an untracked acquire that disconnect cleanup would
	// never release (mirrors matoclserv_fuse_open).
	OpReplay runCreate = [eptr, uid, gid, parentInode, name, mode, umask, flags,
	                      replyData](FilesystemOperationContext &ctx) -> uint8_t {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		uint8_t opStatus =
		    gFSOperations->mknod(context, ctx, parentInode, HString(name), FSNodeType::kFile, mode,
		                         umask, 0, &replyData->first, replyData->second);
		if (opStatus == SAUNAFS_STATUS_OK) {
			opStatus = ctx.getReadWriteTransaction() == nullptr
			               ? matoclserv_insert_open_file(ctx, eptr->sessionData, replyData->first)
			               : matoclserv_acquire_open_file_persist(ctx, eptr->sessionData,
			                                                      replyData->first);
		}
		if (opStatus == SAUNAFS_STATUS_OK) {
			opStatus = gFSOperations->openCheck(context, ctx, replyData->first,
			                                    flags | AFTER_CREATE, replyData->second);
		}
		return opStatus;
	};

	if (eptr->sessionData) {
		eptr->sessionData->currHourOperationsStats[8]++;
	}

	// Builds the reply (inode + attr) applying the same data-cache adjustment the
	// FUSE_OPEN reply uses, since this op opens the file as well as creating it.
	auto sendCreateReply = [eptr, messageId](uint8_t replyStatus, inode_t ino,
	                                         Attributes replyAttr) {
		MessageBuffer reply;
		if (replyStatus == SAUNAFS_STATUS_OK) {
			// When the data cache manager cannot keep this open coherent (dcm_open == 0), clear
			// the ALLOWDATACACHE attribute bit so the client does not cache this file's data.
			if (dcm_open(ino, eptr->sessionData->sessionId) == 0) {
				replyAttr[1] &= (0xFF ^ (MATTR_ALLOWDATACACHE << 4));
			}
			matocl::fuseCreate::serialize(reply, messageId, ino, replyAttr);
		} else {
			matocl::fuseCreate::serialize(reply, messageId, replyStatus);
		}
		matoclserv_createpacket(eptr, std::move(reply));
	};

	if (status != SAUNAFS_STATUS_OK) {  // group-cache error: reply without running the op
		sendCreateReply(status, replyData->first, replyData->second);
		return;
	}

	// Reply continuation (dropped for a killed client) plus an onCommit hook that records the
	// open file on commit success and runs even for a killed client, so the persisted acquire
	// is tracked in openFilesSet for disconnect cleanup. On the in-memory Master the op body
	// already recorded it (matoclserv_insert_open_file); the set insert there is an idempotent
	// no-op. Mirrors matoclserv_fuse_open.
	matoclserv_submit_op(
	    eptr, std::move(runCreate),
	    [sendCreateReply, replyData](uint8_t commitStatus) {
		    sendCreateReply(commitStatus, replyData->first, replyData->second);
	    },
	    [eptr, replyData](uint8_t commitStatus) {
		    if (commitStatus == SAUNAFS_STATUS_OK) {
			    matoclserv_record_open_file(eptr->sessionData, replyData->first);
		    }
	    });
}

void matoclserv_fuse_mkdir(matoclserventry *eptr, PacketHeader header, const uint8_t *data) {
	uint32_t messageId, uid, gid;
	inode_t inode;
	LegacyString<uint8_t> name;
	bool copysgid;
	uint16_t mode, umask;

	if (header.type == SAU_CLTOMA_FUSE_MKDIR) {
		cltoma::fuseMkdir::deserialize(data, header.length, messageId,
				inode, name, mode, umask, uid, gid, copysgid);
	} else {
		throw IncorrectDeserializationException(
				"Unknown packet type for matoclserv_fuse_mkdir: " + std::to_string(header.type));
	}

	// Reply data shared between the replayable op body and the reply continuation.
	auto replyData = std::make_shared<std::pair<inode_t, Attributes>>();
	uint8_t status = matoclserv_check_group_cache(eptr, gid);

	// Replayable body: mutates only backend state and refreshes replyData; no in-process
	// persistent side effect, so a whole-batch replay never double-applies anything.
	OpReplay runMkdir = [eptr, uid, gid, inode, name, mode, umask, copysgid,
	                     replyData](FilesystemOperationContext &ctx) -> uint8_t {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		return gFSOperations->mkdir(context, ctx, inode, HString(name), mode, umask, copysgid,
		                            &replyData->first, replyData->second);
	};

	if (eptr->sessionData) {
		eptr->sessionData->currHourOperationsStats[4]++;
	}

	auto sendMkdirReply = [eptr, messageId, replyData](uint8_t replyStatus) {
		MessageBuffer reply;
		if (replyStatus == SAUNAFS_STATUS_OK) {
			matocl::fuseMkdir::serialize(reply, messageId, replyData->first, replyData->second);
		} else {
			matocl::fuseMkdir::serialize(reply, messageId, replyStatus);
		}
		matoclserv_createpacket(eptr, std::move(reply));
	};

	if (status != SAUNAFS_STATUS_OK) {  // group-cache error: reply without running the op
		sendMkdirReply(status);
		return;
	}

	// Group commit: the continuation replies once the op's batch is durable (or with
	// the body's own error status).
	matoclserv_submit_op(eptr, std::move(runMkdir),
	                     [sendMkdirReply](uint8_t commitStatus) { sendMkdirReply(commitStatus); });
}

void matoclserv_fuse_unlink(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	uint32_t uid,gid;
	uint8_t nleng;
	const uint8_t *name;
	uint32_t msgid;
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

	// Replayable body: unlink reads the target inode, so a concurrent writer on it
	// (unlink-while-open is exactly this) makes the commit conflict under async. The
	// name is copied out of the packet buffer, which dies with this handler frame.
	HString hname(reinterpret_cast<const char *>(name), nleng);
	OpReplay runUnlink = [eptr, uid, gid, inode,
	                      hname](FilesystemOperationContext &ctx) -> uint8_t {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		return gFSOperations->unlink(context, ctx, inode, hname);
	};

	if (eptr->sessionData) {
		eptr->sessionData->currHourOperationsStats[9]++;
	}

	auto sendUnlinkReply = [eptr, msgid](uint8_t replyStatus) {
		uint8_t *ptr =
		    matoclserv_createpacket(eptr, MATOCL_FUSE_UNLINK, sizeof(msgid) + sizeof(replyStatus));
		put32bit(&ptr, msgid);
		put8bit(&ptr, replyStatus);
	};

	if (status != SAUNAFS_STATUS_OK) {  // group-cache error: reply without running the op
		sendUnlinkReply(status);
		return;
	}

	// Group commit: the continuation replies once the op's batch is durable (or
	// with the body's own error status).
	matoclserv_submit_op(eptr, std::move(runUnlink),
	                     [sendUnlinkReply](uint8_t commitStatus) { sendUnlinkReply(commitStatus); });
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

		status = gFSOperations->recursiveRemove(
		    context, parent_inode, HString(name),
		    std::bind(matoclserv_fuse_recursive_remove_wake_up, eptr->sessionData->sessionId, msgid,
		              std::placeholders::_1),
		    job_id);
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

	// Replayable body: rmdir reads the parent dir and the target, so a concurrent
	// writer outside the batch can make the commit conflict. The name is copied out
	// of the packet buffer, which dies with this handler frame.
	HString hname(reinterpret_cast<const char *>(name), nleng);
	OpReplay runRmdir = [eptr, uid, gid, inode, hname](FilesystemOperationContext &ctx) -> uint8_t {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		return gFSOperations->rmdir(context, ctx, inode, hname);
	};

	if (eptr->sessionData) {
		eptr->sessionData->currHourOperationsStats[5]++;
	}

	auto sendRmdirReply = [eptr, msgid](uint8_t replyStatus) {
		uint8_t *ptr =
		    matoclserv_createpacket(eptr, MATOCL_FUSE_RMDIR, sizeof(msgid) + sizeof(replyStatus));
		put32bit(&ptr, msgid);
		put8bit(&ptr, replyStatus);
	};

	if (status != SAUNAFS_STATUS_OK) {  // group-cache error: reply without running the op
		sendRmdirReply(status);
		return;
	}

	// Group commit: the continuation replies once the op's batch is durable (or with
	// the body's own error status).
	matoclserv_submit_op(eptr, std::move(runRmdir),
	                     [sendRmdirReply](uint8_t commitStatus) { sendRmdirReply(commitStatus); });
}

void matoclserv_fuse_rename(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode_src;
	inode_t inode_dst;
	uint8_t nleng_src,nleng_dst;
	const uint8_t *name_src,*name_dst;
	uint32_t uid,gid;
	uint32_t msgid;
	uint8_t status;

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

	// Reply data (renamed inode + its attributes) shared between the body and the
	// reply continuation. Both names are copied out of the packet buffer, which dies
	// with this handler frame.
	auto replyData = std::make_shared<std::pair<inode_t, Attributes>>();
	HString hnameSrc(reinterpret_cast<const char *>(name_src), nleng_src);
	HString hnameDst(reinterpret_cast<const char *>(name_dst), nleng_dst);
	OpReplay runRename = [eptr, uid, gid, inode_src, hnameSrc, inode_dst, hnameDst,
	                      replyData](FilesystemOperationContext &ctx) -> uint8_t {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		return gFSOperations->rename(context, ctx, inode_src, hnameSrc, inode_dst, hnameDst,
		                             &replyData->first, &replyData->second);
	};

	if (eptr->sessionData) {
		eptr->sessionData->currHourOperationsStats[10]++;
	}

	auto sendRenameReply = [eptr, msgid, replyData](uint8_t replyStatus) {
		uint8_t *ptr;
		if (replyStatus == SAUNAFS_STATUS_OK) {
			const uint32_t kSuccessAnswerSize =
			    sizeof(msgid) + sizeof(replyData->first) + replyData->second.size();
			ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_RENAME, kSuccessAnswerSize);
			put32bit(&ptr, msgid);
			putINode(&ptr, replyData->first);
			memcpy(ptr, replyData->second.data(), replyData->second.size());
		} else {
			ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_RENAME,
			                              sizeof(msgid) + sizeof(replyStatus));
			put32bit(&ptr, msgid);
			put8bit(&ptr, replyStatus);
		}
	};

	if (status != SAUNAFS_STATUS_OK) {  // group-cache error: reply without running the op
		sendRenameReply(status);
		return;
	}

	// Group commit: the continuation replies once the op's batch is durable (or with
	// the body's own error status).
	matoclserv_submit_op(eptr, std::move(runRename),
	                     [sendRenameReply](uint8_t commitStatus) { sendRenameReply(commitStatus); });
}

void matoclserv_fuse_link(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	inode_t inode_dst;
	uint8_t nleng_dst;
	const uint8_t *name_dst;
	uint32_t uid,gid;
	uint32_t msgid;
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

	// Reply data (new link inode + its attributes) shared between the body and the
	// reply continuation. The name is copied out of the packet buffer, which dies
	// with this handler frame.
	auto replyData = std::make_shared<std::pair<inode_t, Attributes>>();
	HString hnameDst(reinterpret_cast<const char *>(name_dst), nleng_dst);
	OpReplay runLink = [eptr, uid, gid, inode, inode_dst, hnameDst,
	                    replyData](FilesystemOperationContext &ctx) -> uint8_t {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		return gFSOperations->link(context, ctx, inode, inode_dst, hnameDst, &replyData->first,
		                           &replyData->second);
	};

	if (eptr->sessionData) {
		eptr->sessionData->currHourOperationsStats[11]++;
	}

	auto sendLinkReply = [eptr, msgid, replyData](uint8_t replyStatus) {
		const uint32_t kFailedAnswerSize = sizeof(msgid) + sizeof(replyStatus);
		const uint32_t kSuccessAnswerSize =
		    sizeof(msgid) + sizeof(replyData->first) + replyData->second.size();
		uint32_t answerSize =
		    (replyStatus != SAUNAFS_STATUS_OK) ? kFailedAnswerSize : kSuccessAnswerSize;
		uint8_t *ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_LINK, answerSize);
		put32bit(&ptr, msgid);
		if (replyStatus != SAUNAFS_STATUS_OK) {
			put8bit(&ptr, replyStatus);
		} else {
			putINode(&ptr, replyData->first);
			memcpy(ptr, replyData->second.data(), replyData->second.size());
		}
	};

	if (status != SAUNAFS_STATUS_OK) {  // group-cache error: reply without running the op
		sendLinkReply(status);
		return;
	}

	// Group commit: the continuation replies once the op's batch is durable (or with
	// the body's own error status).
	matoclserv_submit_op(eptr, std::move(runLink),
	                     [sendLinkReply](uint8_t commitStatus) { sendLinkReply(commitStatus); });
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
			auto fsOpContext = gFSOperations->createFilesystemOperationContext(
			    FilesystemOperationContext::TransactionType::kReadWrite);

			status = gFSOperations->readdir(context, fsOpContext, inode, first_entry,
			                                number_of_entries, dir_entries);

			if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
				if (!fsOpContext.getReadWriteTransaction()->commit()) {
					// Best-effort: atime update only, do not fail the directory listing.
					safs::log_err("{}: Failed to commit atime update for inode {}", __func__,
					              inode);
				}
			}

			if (status != SAUNAFS_STATUS_OK) {
				matocl::fuseGetDir::serialize(buffer, message_id, status);
			} else {
				matocl::fuseGetDir::serialize(buffer, message_id, first_entry, dir_entries);
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
	status = gFSOperations->readdirSize(context, inode, flags, &custom, &dleng);

	constexpr uint32_t kFailedAnswerSize = sizeof(msgid) + sizeof(status);
	const uint32_t kSuccessAnswerSize = sizeof(msgid) + dleng;  // Can't be constexpr
	uint32_t answerSize = (status != SAUNAFS_STATUS_OK) ? kFailedAnswerSize : kSuccessAnswerSize;

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_GETDIR, answerSize);

	put32bit(&ptr, msgid);

	if (status != SAUNAFS_STATUS_OK) {
		put8bit(&ptr, status);
	} else {
		auto fsOpContext = gFSOperations->createFilesystemOperationContext(
		    FilesystemOperationContext::TransactionType::kReadWrite);

		gFSOperations->readdirData(context, fsOpContext, flags, custom, ptr);

		// Best effort to update atime
		if (fsOpContext.hasReadWriteTransaction()) {
			if (!fsOpContext.getReadWriteTransaction()->commit()) {
				safs::log_err("{}: Failed to commit atime update for inode {}", __func__, inode);
			}
		}
	}

	eptr->sessionData->currHourOperationsStats[12]++;
}

void matoclserv_fuse_open(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	uint32_t uid,gid;
	uint8_t flags;
	uint32_t msgid;
	uint8_t status;

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

	// Builds and queues the FUSE_OPEN reply. Shared by the synchronous and the
	// deferred-commit paths so the (raw-pointer) packet layout lives in one place.
	auto sendOpenReply = [eptr, msgid](uint8_t st, Attributes replyAttr, inode_t ino) {
		uint8_t *ptr;
		if (st == SAUNAFS_STATUS_OK) {
			if (dcm_open(ino, eptr->sessionData->sessionId) == 0) {
				replyAttr[1] &= (0xFF ^ (MATTR_ALLOWDATACACHE << 4));
			}
			ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_OPEN, sizeof(msgid) + replyAttr.size());
		} else {
			ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_OPEN, sizeof(msgid) + sizeof(st));
		}
		put32bit(&ptr, msgid);
		if (st == SAUNAFS_STATUS_OK) {
			memcpy(ptr, replyAttr.data(), replyAttr.size());
		} else {
			put8bit(&ptr, st);
		}
	};

	status = matoclserv_check_group_cache(eptr, gid);

	// Reply attributes shared with the op body so a commit retry can refresh them.
	auto sharedAttr = std::make_shared<Attributes>();

	// Replayable op body: acquire the open file + openCheck. On the KV backend the acquire
	// is backend-persist-only and the in-memory openFilesSet record is deferred to the
	// onCommit hook below, so a retry replays the persistent acquire on a fresh transaction
	// without the already-open early-return skipping it. On the in-memory Master there is no
	// transaction to roll back, so acquire+record are coupled here (as the legacy
	// matoclserv_insert_open_file did): a later openCheck failure must not orphan an
	// untracked acquire that disconnect cleanup would never release.
	OpReplay runOpen = [eptr, uid, gid, inode, flags,
	                    sharedAttr](FilesystemOperationContext &ctx) -> uint8_t {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		uint8_t st = ctx.getReadWriteTransaction() == nullptr
		                 ? matoclserv_insert_open_file(ctx, eptr->sessionData, inode)
		                 : matoclserv_acquire_open_file_persist(ctx, eptr->sessionData, inode);
		if (st == SAUNAFS_STATUS_OK) {
			st = gFSOperations->openCheck(context, ctx, inode, flags, *sharedAttr);
		}
		return st;
	};

	if (status != SAUNAFS_STATUS_OK) {  // group-cache error: reply without running the op
		sendOpenReply(status, *sharedAttr, inode);
		return;
	}

	// Reply continuation (dropped for a killed client) plus an onCommit hook that records
	// the open file on commit success and runs even for a killed client, so the persisted
	// acquire is tracked in openFilesSet for disconnect cleanup. On the in-memory Master
	// runOpen already recorded it; the set insert there is an idempotent no-op.
	matoclserv_submit_op(
	    eptr, std::move(runOpen),
	    [sendOpenReply, inode, sharedAttr](uint8_t commitStatus) {
		    sendOpenReply(commitStatus, *sharedAttr, inode);
	    },
	    [eptr, inode](uint8_t commitStatus) {
		    if (eptr->sessionData) { eptr->sessionData->currHourOperationsStats[13]++; }
		    if (commitStatus == SAUNAFS_STATUS_OK) {
			    matoclserv_record_open_file(eptr->sessionData, inode);
		    }
	    });
}

void matoclserv_fuse_read_chunk(matoclserventry *eptr, PacketHeader header, const uint8_t *data) {
	sassert(header.type == SAU_CLTOMA_FUSE_READ_CHUNK);
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

	// ReadWrite transaction is needed to update atime inside readChunk
	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	status = gFSOperations->readChunk(fsOpContext, inode, index, &chunkid, &fleng);

	std::vector<ChunkTypeWithAddress> allChunkCopies;
	if (status == SAUNAFS_STATUS_OK) {
		if (chunkid > 0) {
			status = gChunkOperations->getVersionAndLocations(
			    chunkid, eptr->peerIpAddress, version, kMaxNumberOfChunkCopies, allChunkCopies);
			remove_unsupported_ec_parts(eptr->version, allChunkCopies);
		} else {
			version = 0;
		}
	}

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.getReadWriteTransaction()->commit()) {
			// Best-effort: atime update only, do not fail the chunk read.
			safs::log_err("{}: transaction failed to commit: inode {}, chunk index {}", __func__,
			              inode, index);
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
		status = gFSOperations->getChunksInfo(context, eptr->peerIpAddress, inode, chunk_index,
		                                      chunk_count, chunks);
	}

	if (status != SAUNAFS_STATUS_OK) {
		matoclserv_createpacket(eptr, matocl::chunksInfo::build(message_id, status));
		return;
	}

	matoclserv_createpacket(eptr, matocl::chunksInfo::build(message_id, chunks));
}

void matoclserv_fuse_write_chunk(matoclserventry *eptr, PacketHeader header, const uint8_t *data) {
	sassert(header.type == SAU_CLTOMA_FUSE_WRITE_CHUNK);
	inode_t inode;
	uint32_t chunkIndex;
	uint32_t lockId;
	uint32_t messageId;

	const PacketSerializer* serializer = PacketSerializer::getSerializer(header.type, eptr->version);

	std::vector<uint8_t> receivedData(data, data + header.length);
	serializer->deserializeFuseWriteChunk(receivedData, messageId, inode, chunkIndex, lockId);

	uint32_t min_server_version = kFirstXorVersion;

	// Out-state shared between the replayable body and the deferred continuation; each
	// body run refreshes it. lockId is deliberately NOT reset between runs: a replay
	// passes the previous attempt's allocated lockId back in, so writeChunk re-enters
	// its own lock instead of bouncing off it as LOCKED (the failed commit rolled back
	// the backend lock record, but the chunk module's in-memory lock survives). `queued`
	// tracks the DelayedChunkOperation the latest run enqueued while its commit is in
	// flight.
	struct WriteChunkState {
		uint64_t chunkId = 0;
		uint64_t fileLength = 0;
		uint32_t lockId = 0;
		uint8_t opflag = 0;
		DelayedChunkOperation *queued = nullptr;
		bool tookDelayedPath = false;  ///< onCommit took the delayed path; reply via chunk_status
	};
	auto wc = std::make_shared<WriteChunkState>();
	wc->lockId = lockId;

	// Replayable body: re-runnable on a fresh txn if the commit conflicts (concurrent
	// writers to the same inode are exactly the EC/overlapping-write workload). When
	// writeChunk initiates a chunkserver operation (opflag), the body enqueues the
	// delayed entry ITSELF, marked commitPending, because the deferred commit no longer
	// blocks the event loop: the chunkserver's status could arrive before the commit
	// lands, and matoclserv_chunk_status stashes it on the pending entry until the
	// commit continuation resolves it.
	OpReplay runWriteChunk = [eptr, inode, chunkIndex, min_server_version, serializer, messageId,
	                          wc](FilesystemOperationContext &ctx) -> uint8_t {
		matoclserv_drop_queued_delayed_op(eptr, wc->queued);
		wc->queued = nullptr;
		uint8_t st = gFSOperations->writeChunk(matoclserv_get_context(eptr), ctx, inode,
		                                       chunkIndex, &wc->lockId, &wc->chunkId, &wc->opflag,
		                                       &wc->fileLength, min_server_version);
		if (st == SAUNAFS_STATUS_OK && wc->opflag) {
			auto operation = std::make_unique<DelayedChunkOperation>();
			operation->inode = inode;
			operation->chunkId = wc->chunkId;
			operation->messageId = messageId;
			operation->fileLength = wc->fileLength;
			operation->lockId = wc->lockId;
			operation->type = FUSE_WRITE;
			operation->serializer = serializer;
			operation->commitPending = true;
			wc->queued = operation.get();
			eptr->delayedChunkOperations.push_back(std::move(operation));
		}
		return st;
	};

	auto sendWriteChunkError = [eptr, serializer, messageId, inode, chunkIndex,
	                            wc](uint8_t errorStatus) {
		if (errorStatus == SAUNAFS_ERROR_LOCKED) {
			// The chunk is locked, so we need to add this client to the wait-for-unlock
			// list. The chunkId must have been set by writeChunk above.
			matoclserv_add_to_wait_for_unlock_list(eptr, wc->chunkId, inode, chunkIndex);
		}
		std::vector<uint8_t> outMessage;
		serializer->serializeFuseWriteChunk(outMessage, messageId, errorStatus);
		matoclserv_createpacket(eptr, outMessage);
	};

	if (eptr->sessionData) {
		eptr->sessionData->currHourOperationsStats[15]++;
	}

	// Group commit; the continuation reconciles whichever path the FINAL body run
	// took (a replay can flip opflag) and handles the body's own error statuses
	// (LOCKED keeps the wait-for-unlock path).
	matoclserv_submit_op(
	    eptr, std::move(runWriteChunk),
	    [eptr, serializer, messageId, inode, chunkIndex, wc,
	     sendWriteChunkError](uint8_t commitStatus) {
		    // Reply only (kill-gated). The delayed-op queue transition runs in onCommit below.
		    if (commitStatus != SAUNAFS_STATUS_OK) {
			    sendWriteChunkError(commitStatus);
			    return;
		    }
		    if (wc->tookDelayedPath) {
			    // Delayed path: the reply comes via matoclserv_chunk_status (from onCommit).
			    return;
		    }
		    // Immediate path: reply with the chunk's version and locations.
		    dcm_modify(inode, eptr->sessionData->sessionId);
		    uint8_t respondStatus = matoclserv_fuse_write_chunk_respond(
		        eptr, serializer, wc->chunkId, messageId, wc->fileLength, wc->lockId);
		    if (respondStatus != SAUNAFS_STATUS_OK) {
			    // The write transaction is already durable; release the chunk lock with
			    // its own small op (conflict-retried, since other commits are in flight).
			    const uint64_t chunkId = wc->chunkId;
			    OpReplay runWriteEndCleanup =
			        [chunkId](FilesystemOperationContext &ctx) -> uint8_t {
				    gFSOperations->writeEnd(ctx, 0, 0, chunkId, 0);  // ignore status, just do it
				    return SAUNAFS_STATUS_OK;
			    };
			    if (matoclserv_commit_op_with_retry(runWriteEndCleanup) != SAUNAFS_STATUS_OK) {
				    safs::log_err(
				        "matoclserv_fuse_write_chunk: transaction failed to commit during write "
				        "end: inode {}, chunk index {}",
				        inode, chunkIndex);
			    }
		    }
	    },
	    [eptr, wc](uint8_t commitStatus) {
		    // Durable delayed-op transition; runs with the final status even for a killed
		    // client so a stashed chunkserver status is processed and the lock released.
		    // A chunkserver op from a failed attempt may still complete; its status is then
		    // warn-dropped.
		    if (commitStatus != SAUNAFS_STATUS_OK) {
			    matoclserv_drop_queued_delayed_op(eptr, wc->queued);
			    wc->queued = nullptr;
		    } else if (wc->queued != nullptr) {
			    wc->tookDelayedPath = true;
			    DelayedChunkOperation *queued = wc->queued;
			    wc->queued = nullptr;
			    matoclserv_resolve_pending_delayed_op(eptr, queued);
		    }
	    });
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
	uint8_t status = SAUNAFS_STATUS_OK;

	std::vector<uint8_t> request(data, data + header.length);
	const PacketSerializer* serializer = PacketSerializer::getSerializer(header.type, eptr->version);
	serializer->deserializeFuseWriteChunkEnd(request, messageId, chunkId, lockId, inode, fileLength);

	if (lockId == 0) {
		// this lock id passed to chunk_unlock would force chunk unlock
		status = SAUNAFS_ERROR_WRONGLOCKID;
	} else if (eptr->sessionData->flags & SESFLAG_READONLY) {
		status = SAUNAFS_ERROR_EROFS;
	}

	// Replayable body: writeEnd updates the inode length and unlocks the chunk; with
	// group commit a concurrent writer on the same inode can make this commit conflict,
	// so it must replay on a fresh txn rather than reply EIO.
	OpReplay runWriteEnd = [inode, fileLength, chunkId,
	                        lockId](FilesystemOperationContext &ctx) -> uint8_t {
		return gFSOperations->writeEnd(ctx, inode, fileLength, chunkId, lockId);
	};

	auto sendWriteEndReply = [eptr, serializer, messageId, inode](uint8_t replyStatus) {
		dcm_modify(inode, eptr->sessionData->sessionId);
		std::vector<uint8_t> outMessage;
		serializer->serializeFuseWriteChunkEnd(outMessage, messageId, replyStatus);
		matoclserv_createpacket(eptr, outMessage);
	};

	if (status != SAUNAFS_STATUS_OK) {  // pre-check error: reply without running the op
		sendWriteEndReply(status);
		return;
	}

	// Group commit: the continuation replies once the op's batch is durable (or
	// with the body's own error status).
	matoclserv_submit_op(eptr, std::move(runWriteEnd), [sendWriteEndReply](uint8_t commitStatus) {
		sendWriteEndReply(commitStatus);
	});
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
		status = gFSOperations->repair(context, inode, correct_only, &chunksnotchanged,
		                               &chunkserased, &chunksrepaired);
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
	ChunkCountArray chunkCount;
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

	status = gFSOperations->checkFile(matoclserv_get_context(eptr), inode, chunkCount);

	if (status != SAUNAFS_STATUS_OK) {
		ptr = matoclserv_createpacket(eptr,MATOCL_FUSE_CHECK, sizeof(msgid) + sizeof(status));
		put32bit(&ptr,msgid);
		put8bit(&ptr,status);
	} else {
		ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_CHECK,
		                              sizeof(msgid) + CHUNK_MATRIX_SIZE * sizeof(uint32_t));
		put32bit(&ptr, msgid);
		for (uint32_t i = 0; i < CHUNK_MATRIX_SIZE; i++) { put32bit(&ptr, chunkCount[i]); }
	}
}

void matoclserv_fuse_request_task_id(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t msgid, taskid;
	cltoma::requestTaskId::deserialize(data, length, msgid);
	taskid = gFSOperations->reserveJobId();
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

	status = gFSOperations->getTrashTimePrepare(matoclserv_get_context(eptr), inode, gmode,
	                                            fileTrashtimes, dirTrashtimes);
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
		gFSOperations->getTrashTimeStore(fileTrashtimes, dirTrashtimes, ptr);
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
		status = gFSOperations->setTrashTime(
		    matoclserv_get_context(eptr, uid, 0), inode, trashtime, smode, settrashtime_stats,
		    std::bind(matoclserv_fuse_settrashtime_wake_up, eptr->sessionData->sessionId, msgid,
		              settrashtime_stats, std::placeholders::_1));
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

	if (header.type == SAU_CLTOMA_FUSE_GETGOAL) {
		cltoma::fuseGetGoal::deserialize(data, header.length, msgid, inode, gmode);
	} else {
		throw IncorrectDeserializationException(
				"Unknown packet type for matoclserv_fuse_getgoal: " + std::to_string(header.type));
	}

	// getGoal could attempt to change the stored goal if invalid.
	// So we need a read-write transaction.
	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	GoalStatistics fgtab{{0}}, dgtab{{0}}; // explicit value initialization to clear variables
	uint8_t status =
	    gFSOperations->getGoal(matoclserv_get_context(eptr), fsOpContext, inode, gmode, fgtab, dgtab);

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.getReadWriteTransaction()->commit()) {
			safs::log_err("{}: transaction failed to commit: inode {}, gmode {}", __func__, inode,
			              static_cast<uint32_t>(gmode));
			status = SAUNAFS_ERROR_IO;
		}
	}

	MessageBuffer reply;

	if (status == SAUNAFS_STATUS_OK) {
		const std::map<int, Goal> &goalDefinitions = gFSOperations->getAllGoalDefinitions();
		std::vector<FuseGetGoalStats> sauReply;
		for (const auto &goal : goalDefinitions) {
			if (fgtab[goal.first] || dgtab[goal.first]) {
				sauReply.emplace_back(goal.second.getName(), fgtab[goal.first], dgtab[goal.first]);
			}
		}
		matocl::fuseGetGoal::serialize(reply, msgid, sauReply);
	} else {
		matocl::fuseGetGoal::serialize(reply, msgid, status);
	}

	matoclserv_createpacket(eptr, std::move(reply));
}

void matoclserv_fuse_setgoal_wake_up(uint32_t session_id, uint32_t msgid, uint32_t type,
				     std::shared_ptr<SetGoalTask::StatsArray> setgoal_stats,
				     uint32_t status) {
	sassert(type == SAU_CLTOMA_FUSE_SETGOAL);
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

		matocl::fuseSetGoal::serialize(reply, msgid, changed, notchanged, notpermitted);
	} else {
		matocl::fuseSetGoal::serialize(reply, msgid, status);
	}
	matoclserv_createpacket(eptr, std::move(reply));
}

void matoclserv_fuse_setgoal(matoclserventry *eptr, PacketHeader header, const uint8_t *data) {
	inode_t inode;
	uint32_t uid;
	uint32_t msgid;
	uint8_t goalId = 0, smode;
	uint8_t status = SAUNAFS_STATUS_OK;

	if (header.type == SAU_CLTOMA_FUSE_SETGOAL) {
		std::string goalName;
		cltoma::fuseSetGoal::deserialize(data, header.length,
				msgid, inode, uid, goalName, smode);
		// find a proper goalId,
		const std::map<int, Goal> &goalDefinitions = gFSOperations->getAllGoalDefinitions();
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
		status = gFSOperations->setGoal(
		    context, inode, goalId, smode, setgoal_stats,
		    std::bind(matoclserv_fuse_setgoal_wake_up, eptr->sessionData->sessionId, msgid,
		              header.type, setgoal_stats, std::placeholders::_1));
	}

	if (status != SAUNAFS_ERROR_WAITING) {
		matoclserv_fuse_setgoal_wake_up(eptr->sessionData->sessionId, msgid, header.type,
						setgoal_stats, status);
	}
}

void matoclserv_fuse_geteattr(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	uint32_t msgid;
	ExtraAttributesArray fileEAttrTab;
	ExtraAttributesArray dirEAttrTab;
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

	status = gFSOperations->getExtraAttr(matoclserv_get_context(eptr), inode, gmode, fileEAttrTab,
	                                     dirEAttrTab);
	fn = 0;
	dn = 0;

	if (status == SAUNAFS_STATUS_OK) {
		for (i = 0; i < kMaxExtraAttributes; i++) {
			if (fileEAttrTab[i]) { fn++; }
			if (dirEAttrTab[i]) { dn++; }
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
		for (i = 0; i < kMaxExtraAttributes; i++) {
			if (fileEAttrTab[i]) {
				put8bit(&ptr, i);
				put32bit(&ptr, fileEAttrTab[i]);
			}
		}
		for (i = 0; i < kMaxExtraAttributes; i++) {
			if (dirEAttrTab[i]) {
				put8bit(&ptr, i);
				put32bit(&ptr, dirEAttrTab[i]);
			}
		}
	}
}

void matoclserv_fuse_seteattr(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	uint32_t uid;
	uint32_t msgid;
	uint8_t eattr,smode;

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

	// Reply data (changed / notchanged / notpermitted counts) shared with the
	// continuation; refreshed each time the replayable body runs.
	auto replyData = std::make_shared<std::array<inode_t, 3>>();
	OpReplay runSetEattr = [eptr, uid, inode, eattr, smode,
	                        replyData](FilesystemOperationContext &ctx) -> uint8_t {
		return gFSOperations->setExtraAttr(matoclserv_get_context(eptr, uid, 0), ctx, inode, eattr,
		                                   smode, &(*replyData)[0], &(*replyData)[1],
		                                   &(*replyData)[2]);
	};

	auto sendSetEattrReply = [eptr, msgid, replyData](uint8_t replyStatus) {
		const uint32_t kFailedSize = sizeof(msgid) + sizeof(replyStatus);
		const uint32_t kSuccessSize = sizeof(msgid) + 3 * sizeof(inode_t);
		uint8_t *ptr = matoclserv_createpacket(
		    eptr, MATOCL_FUSE_SETEATTR,
		    (replyStatus != SAUNAFS_STATUS_OK) ? kFailedSize : kSuccessSize);
		put32bit(&ptr, msgid);
		if (replyStatus != SAUNAFS_STATUS_OK) {
			put8bit(&ptr, replyStatus);
		} else {
			putINode(&ptr, (*replyData)[0]);
			putINode(&ptr, (*replyData)[1]);
			putINode(&ptr, (*replyData)[2]);
		}
	};

	matoclserv_submit_op(eptr, std::move(runSetEattr), sendSetEattrReply);
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
	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);

	mode = get8bit(&data);

	if (mode != XATTR_GMODE_GET_DATA && mode != XATTR_GMODE_LENGTH_ONLY) {
		ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_GETXATTR, kFailedSize);
		put32bit(&ptr, msgid);
		put8bit(&ptr, SAUNAFS_ERROR_EINVAL);
	} else if (anleng == 0) {
		XAttrListResult listResult;
		uint32_t xasize;
		status = gFSOperations->listXAttr(context, fsOpContext, inode, opened, listResult, &xasize);
		const uint32_t kSuccessSize =
		    sizeof(msgid) + sizeof(xasize) + ((mode == XATTR_GMODE_GET_DATA) ? xasize : 0);
		ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_GETXATTR,
		                              (status != SAUNAFS_STATUS_OK) ? kFailedSize : kSuccessSize);

		put32bit(&ptr,msgid);

		if (status != SAUNAFS_STATUS_OK) {
			put8bit(&ptr, status);
		} else {
			put32bit(&ptr, xasize);
			if (mode == XATTR_GMODE_GET_DATA && xasize > 0) {
				memcpy(ptr, kAclXattrs, sizeof(kAclXattrs));
				if (!listResult.data.empty()) {
					memcpy(ptr + sizeof(kAclXattrs), listResult.data.data(),
					       listResult.data.size());
				}
			}
		}
	} else {
		XAttrGetResult getResult;
		status = gFSOperations->getXAttr(context, fsOpContext, inode, opened, anleng, attrname,
		                                 getResult);
		uint32_t avleng = static_cast<uint32_t>(getResult.value.size());
		const uint32_t kSuccessSize =
		    sizeof(msgid) + sizeof(avleng) + ((mode == XATTR_GMODE_GET_DATA) ? avleng : 0);
		ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_GETXATTR,
		                              (status != SAUNAFS_STATUS_OK) ? kFailedSize : kSuccessSize);

		put32bit(&ptr, msgid);

		if (status != SAUNAFS_STATUS_OK) {
			put8bit(&ptr, status);
		} else {
			put32bit(&ptr, avleng);
			if (mode == XATTR_GMODE_GET_DATA && avleng > 0) {
				memcpy(ptr, getResult.value.data(), avleng);
			}
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

	// Copy the name and value out of the packet buffer (it dies with this frame); the
	// body may run more than once on a batch replay.
	std::vector<uint8_t> nameBytes(attrname, attrname + anleng);
	std::vector<uint8_t> valueBytes(attrvalue, attrvalue + avleng);
	OpReplay runSetXattr = [eptr, uid, gid, inode, opened, anleng, avleng, mode, nameBytes,
	                        valueBytes](FilesystemOperationContext &ctx) -> uint8_t {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		return gFSOperations->setXAttr(context, ctx, inode, opened, anleng, nameBytes.data(),
		                               avleng, valueBytes.data(), mode);
	};

	auto sendSetXattrReply = [eptr, msgid](uint8_t replyStatus) {
		uint8_t *ptr =
		    matoclserv_createpacket(eptr, MATOCL_FUSE_SETXATTR, sizeof(msgid) + sizeof(replyStatus));
		put32bit(&ptr, msgid);
		put8bit(&ptr, replyStatus);
	};

	if (status != SAUNAFS_STATUS_OK) {  // group-cache error: reply without running the op
		sendSetXattrReply(status);
		return;
	}

	matoclserv_submit_op(eptr, std::move(runSetXattr), sendSetXattrReply);
}

void matoclserv_fuse_append(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	inode_t inode_src;
	uint32_t uid;
	uint32_t gid;
	uint32_t msgid;
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

	// Replayable body: append copies the source file's chunk references onto the target
	// (addFile bumps each chunk's in-memory refcount + persists the chunk record), so it
	// shares write_chunk's accepted property -- a whole-batch replay re-runs addFile and
	// the failed attempt's in-memory refcount bump lingers (backend rolls back). That is a
	// conservative leak (refcount too high => extra COW, never too low), not corruption.
	OpReplay runAppend = [eptr, uid, gid, inode,
	                      inode_src](FilesystemOperationContext &ctx) -> uint8_t {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		return gFSOperations->append(context, ctx, inode, inode_src);
	};

	auto sendAppendReply = [eptr, msgid](uint8_t replyStatus) {
		uint8_t *ptr =
		    matoclserv_createpacket(eptr, MATOCL_FUSE_APPEND, sizeof(msgid) + sizeof(replyStatus));
		put32bit(&ptr, msgid);
		put8bit(&ptr, replyStatus);
	};

	if (status != SAUNAFS_STATUS_OK) {  // group-cache error: reply without running the op
		sendAppendReply(status);
		return;
	}

	matoclserv_submit_op(eptr, std::move(runAppend),
	                     [sendAppendReply](uint8_t commitStatus) { sendAppendReply(commitStatus); });
}

void matoclserv_fuse_snapshot_wake_up(uint32_t type, uint32_t session_id, uint32_t msgid, int status) {
	matoclserventry *eptr = matoclserv_find_connection(session_id);
	if (!eptr) {
		return;
	}

	MessageBuffer buffer;
	sassert(type == SAU_CLTOMA_FUSE_SNAPSHOT);
	matocl::snapshot::serialize(buffer, msgid, status);
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

	if (header.type == SAU_CLTOMA_FUSE_SNAPSHOT) {
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

	status = gFSOperations->getDirStats(matoclserv_get_context(eptr), inode, &inodes, &dirs, &files,
	                                    &links, &chunks, &leng, &size, &rsize);

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

	status = gFSOperations->getDirStats(matoclserv_get_context(eptr), inode, &inodes, &dirs, &files,
	                                    &links, &chunks, &leng, &size, &rsize);

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

	status = gFSOperations->readTrashSize(eptr->sessionData->rootInode, eptr->sessionData->flags,
	                                      &dleng);

	ptr = matoclserv_createpacket(
	    eptr, MATOCL_FUSE_GETTRASH,
	    (status != SAUNAFS_STATUS_OK) ? sizeof(msgid) + sizeof(status) : (sizeof(msgid) + dleng));

	put32bit(&ptr, msgid);

	if (status != SAUNAFS_STATUS_OK) {
		put8bit(&ptr, status);
	} else {
		gFSOperations->readTrashData(eptr->sessionData->rootInode, eptr->sessionData->flags, ptr);
	}
}

void matoclserv_fuse_gettrash(matoclserventry *eptr, const PacketHeader &header,
                              const uint8_t *data) {
	uint32_t maxEntries, msgId;
	PacketVersion version;
	deserializePacketVersionNoHeader(data, header.length, version);

	if (version == cltoma::fuseGetTrash::kClientPositionOffset) {
		uint32_t off;
		cltoma::fuseGetTrash::deserialize(data, header.length, msgId, off, maxEntries);
		std::vector<NamedInodeEntry> entries;
		gFSOperations->readTrash(
		    off, std::min<uint32_t>(maxEntries, matocl::fuseGetDir::kMaxNumberOfDirectoryEntries),
		    entries);
		matoclserv_createpacket(eptr, matocl::fuseGetTrash::build(msgId, entries));
	} else if (version == cltoma::fuseGetTrash::kClientHandleOffset) {
		uint64_t off;
		cltoma::fuseGetTrash::deserialize(data, header.length, msgId, off, maxEntries);
		std::vector<HandleInodeEntry> entries;
		auto fsOpContext = gFSOperations->createFilesystemOperationContext(
		    FilesystemOperationContext::TransactionType::kReadOnly);
		gFSOperations->readTrash(
		    fsOpContext, off,
		    std::min<uint32_t>(maxEntries, matocl::fuseGetDir::kMaxNumberOfDirectoryEntries),
		    entries);
		matoclserv_createpacket(eptr, matocl::fuseGetTrash::build(msgId, entries));
	} else {
		throw IncorrectDeserializationException(
		    "Unknown packet version for matoclserv_fuse_gettrash: " + std::to_string(version));
	}
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

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);
	status = gFSOperations->getDetachedAttr(fsOpContext, eptr->sessionData->rootInode,
	                                        eptr->sessionData->flags, inode, attr, dtype);

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

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);
	status = gFSOperations->getTrashPath(fsOpContext, eptr->sessionData->rootInode,
	                                     eptr->sessionData->flags, inode, path);

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

	status = gFSOperations->setTrashPath(matoclserv_get_context(eptr), inode,
	                                     std::string(reinterpret_cast<const char *>(path), pleng));

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

	status = gFSOperations->undel(matoclserv_get_context(eptr), inode);

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_UNDEL, sizeof(msgid) + sizeof(status));

	put32bit(&ptr, msgid);
	put8bit(&ptr, status);
}

void matoclserv_fuse_purge(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	inode_t inode;
	uint32_t msgid;

	constexpr uint32_t kExpectedLength = sizeof(msgid) + sizeof(inode);

	if (length != kExpectedLength) {
		safs_pretty_syslog(LOG_NOTICE, "CLTOMA_FUSE_PURGE - wrong size (%" PRIu32 "/%" PRIu32 ")",
		                   length, kExpectedLength);
		eptr->mode = ClientConnectionMode::KILL;
		return;
	}

	get32bit(&data, msgid);
	getINode(&data, inode);

	OpReplay runPurge = [eptr, inode](FilesystemOperationContext &ctx) -> uint8_t {
		return gFSOperations->purge(matoclserv_get_context(eptr), ctx, inode);
	};

	auto sendPurgeReply = [eptr, msgid](uint8_t replyStatus) {
		uint8_t *ptr =
		    matoclserv_createpacket(eptr, MATOCL_FUSE_PURGE, sizeof(msgid) + sizeof(replyStatus));
		put32bit(&ptr, msgid);
		put8bit(&ptr, replyStatus);
	};

	matoclserv_submit_op(eptr, std::move(runPurge),
	                     [sendPurgeReply](uint8_t commitStatus) { sendPurgeReply(commitStatus); });
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

	status = gFSOperations->readReservedSize(eptr->sessionData->rootInode, eptr->sessionData->flags,
	                                         &dleng);

	constexpr uint32_t kFailedSize = sizeof(msgid) + sizeof(status);
	const uint32_t kSuccessSize = sizeof(msgid) + dleng;
	const uint32_t answerSize = (status!=SAUNAFS_STATUS_OK) ? kFailedSize : kSuccessSize;

	ptr = matoclserv_createpacket(eptr, MATOCL_FUSE_GETRESERVED, answerSize);

	put32bit(&ptr, msgid);

	if (status!=SAUNAFS_STATUS_OK) {
		put8bit(&ptr,status);
	} else {
		gFSOperations->readReservedData(eptr->sessionData->rootInode, eptr->sessionData->flags,
		                                ptr);
	}
}

void matoclserv_fuse_getreserved(matoclserventry *eptr, const PacketHeader &header,
                                 const uint8_t *data) {
	uint32_t maxEntries, msgId;
	PacketVersion version;
	deserializePacketVersionNoHeader(data, header.length, version);

	if (version == cltoma::fuseGetReserved::kClientPositionOffset) {
		uint32_t off;
		cltoma::fuseGetReserved::deserialize(data, header.length, msgId, off, maxEntries);
		std::vector<NamedInodeEntry> entries;
		gFSOperations->readReserved(
		    off, std::min<uint32_t>(maxEntries, matocl::fuseGetDir::kMaxNumberOfDirectoryEntries),
		    entries);
		matoclserv_createpacket(eptr, matocl::fuseGetReserved::build(msgId, entries));
	} else if (version == cltoma::fuseGetReserved::kClientHandleOffset) {
		uint64_t off;
		cltoma::fuseGetReserved::deserialize(data, header.length, msgId, off, maxEntries);
		std::vector<HandleInodeEntry> entries;
		auto fsOpContext = gFSOperations->createFilesystemOperationContext(
		    FilesystemOperationContext::TransactionType::kReadOnly);
		gFSOperations->readReserved(
		    fsOpContext, off,
		    std::min<uint32_t>(maxEntries, matocl::fuseGetDir::kMaxNumberOfDirectoryEntries),
		    entries);
		matoclserv_createpacket(eptr, matocl::fuseGetReserved::build(msgId, entries));
	} else {
		throw IncorrectDeserializationException(
		    "Unknown packet version for matoclserv_fuse_gettreserved: " + std::to_string(version));
	}
}

void matoclserv_fuse_deleteacl(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t messageId, uid, gid;
	inode_t inode;
	AclType type;
	cltoma::fuseDeleteAcl::deserialize(data, length, messageId, inode, uid, gid, type);

	uint8_t status = matoclserv_check_group_cache(eptr, gid);

	OpReplay runDeleteAcl = [eptr, uid, gid, inode,
	                         type](FilesystemOperationContext &ctx) -> uint8_t {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		return gFSOperations->deleteAcl(context, ctx, inode, type);
	};

	auto sendDeleteAclReply = [eptr, messageId](uint8_t replyStatus) {
		matoclserv_createpacket(eptr, matocl::fuseDeleteAcl::build(messageId, replyStatus));
	};

	if (status != SAUNAFS_STATUS_OK) {  // group-cache error: reply without running the op
		sendDeleteAclReply(status);
		return;
	}

	matoclserv_submit_op(eptr, std::move(runDeleteAcl), sendDeleteAclReply);
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
		auto fsOpContext = gFSOperations->createFilesystemOperationContext(
		    FilesystemOperationContext::TransactionType::kReadOnly);

		status = gFSOperations->getAcl(context, fsOpContext, inode, acl);

		if (status == SAUNAFS_STATUS_OK) {
			if (eptr->version >= kRichACLVersion) {
				FSNode *node = gFSOperations->nodeOperations()->idToNode(fsOpContext, inode);
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
	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	status = gFSOperations->flockOperation(context, fsOpContext, inode, owner,
	                                       eptr->sessionData->sessionId, requestId, messageId, op,
	                                       nonblocking, applied);

	if ((status == SAUNAFS_STATUS_OK || status == SAUNAFS_ERROR_WAITING) &&
	    fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.getReadWriteTransaction()->commit()) {
			safs::log_err("{}: transaction failed to commit: inode {}", __func__, inode);
			status = SAUNAFS_ERROR_IO;
			applied.clear();
		}
	}

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

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);

	status = gFSOperations->posixLockProbe(context, fsOpContext, inode, lock_info.l_start, lock_end,
	                                       owner, eptr->sessionData->sessionId, 0, message_id,
	                                       lock_info.l_type, lock_info);

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
	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	status = gFSOperations->posixLockOperation(context, fsOpContext, inode, lock_info.l_start,
	                                           lock_end, owner, eptr->sessionData->sessionId,
	                                           request_id, message_id, op, nonblocking, applied);

	if ((status == SAUNAFS_STATUS_OK || status == SAUNAFS_ERROR_WAITING) &&
	    fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.getReadWriteTransaction()->commit()) {
			safs::log_err("{}: transaction failed to commit: inode {}", __func__, inode);
			status = SAUNAFS_ERROR_IO;
			applied.clear();
		}
	}

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

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);

	if (version == cltoma::manageLocksList::kAll) {
		cltoma::manageLocksList::deserialize(data, length, type, pending, start, max);
		max = std::min(max, SAU_CLTOMA_MANAGE_LOCKS_LIST_LIMIT);
		status = gFSOperations->locksListAll(context, fsOpContext, (uint8_t)type, pending, start,
		                                     max, locks);
	} else if (version == cltoma::manageLocksList::kInode) {
		cltoma::manageLocksList::deserialize(data, length, inode, type, pending, start, max);
		max = std::min(max, SAU_CLTOMA_MANAGE_LOCKS_LIST_LIMIT);
		status = gFSOperations->locksListInode(context, fsOpContext, (uint8_t)type, pending, inode,
		                                       start, max, locks);
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

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	if (version == cltoma::manageLocksUnlock::kSingle) {
		cltoma::manageLocksUnlock::deserialize(data, length, type, inode, sessionid, owner, start,
		                                       end);
		// Passing a 0 as lock's end is equivalent to passing 'till EOF'
		if (end == 0) {
			end = std::numeric_limits<decltype(end)>::max();
		}
		if (type == safs_locks::Type::kAll || type == safs_locks::Type::kFlock) {
			status = gFSOperations->flockOperation(context, fsOpContext, inode, owner, sessionid, 0,
			                                       0, safs_locks::kUnlock, true, flocks_applied);
		}
		if (status == SAUNAFS_STATUS_OK &&
		    (type == safs_locks::Type::kAll || type == safs_locks::Type::kPosix)) {
			status = gFSOperations->posixLockOperation(context, fsOpContext, inode, start, end,
			                                           owner, sessionid, 0, 0, safs_locks::kUnlock,
			                                           true, posix_applied);
		}
	} else if (version == cltoma::manageLocksUnlock::kInode) {
		cltoma::manageLocksUnlock::deserialize(data, length, type, inode);
		if (type == safs_locks::Type::kAll || type == safs_locks::Type::kFlock) {
			status = gFSOperations->locksUnlockInode(
			    context, fsOpContext, (uint8_t)safs_locks::Type::kFlock, inode, flocks_applied);
		}
		if (status == SAUNAFS_STATUS_OK &&
		    (type == safs_locks::Type::kAll || type == safs_locks::Type::kPosix)) {
			status = gFSOperations->locksUnlockInode(
			    context, fsOpContext, (uint8_t)safs_locks::Type::kPosix, inode, posix_applied);
		}
	} else {
		throw IncorrectDeserializationException("Unknown SAU_CLTOMA_MANAGE_LOCKS_UNLOCK version: " +
		                                        std::to_string(version));
	}

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.getReadWriteTransaction()->commit()) {
			safs::log_err("{}: transaction failed to commit: inode {}", __func__, inode);
			status = SAUNAFS_ERROR_IO;
			flocks_applied.clear();
			posix_applied.clear();
		}
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
	std::vector<JobInfo> jobs_info = gFSOperations->getCurrentTasksInfo();
	matoclserv_createpacket(eptr, matocl::listTasks::build(jobs_info));
}

void matoclserv_stop_task(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t job_id, msgid;
	uint8_t status;
	cltoma::stopTask::deserialize(data, length, msgid, job_id);
	status = gFSOperations->cancelJob(job_id);
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

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	// we do not reply, so there is not need for checking status of this fs_operation
	gFSOperations->locksRemovePending(context, fsOpContext, type, interruptData.owner,
	                                  eptr->sessionData->sessionId, interruptData.ino,
	                                  interruptData.reqid);

	if (fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.getReadWriteTransaction()->commit()) {
			safs::log_err("{}: transaction failed to commit: inode {}", __func__,
			              interruptData.ino);
		}
	}
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

	OpReplay runSetAcl = [eptr, uid, gid, inode, type, use_posix, posix_acl,
	                      rich_acl](FilesystemOperationContext &ctx) -> uint8_t {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		if (use_posix) {
			return gFSOperations->setAcl(context, ctx, inode, type, posix_acl);
		}
		return gFSOperations->setAcl(context, ctx, inode, rich_acl);
	};

	auto sendSetAclReply = [eptr, messageId](uint8_t replyStatus) {
		matoclserv_createpacket(eptr, matocl::fuseSetAcl::build(messageId, replyStatus));
	};

	if (status != SAUNAFS_STATUS_OK) {  // group-cache error: reply without running the op
		sendSetAclReply(status);
		return;
	}

	matoclserv_submit_op(eptr, std::move(runSetAcl),
	                     [sendSetAclReply](uint8_t commitStatus) { sendSetAclReply(commitStatus); });
}

void matoclserv_fuse_setquota(matoclserventry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t messageId, uid, gid;
	std::vector<QuotaEntry> entries;
	cltoma::fuseSetQuota::deserialize(data, length, messageId, uid, gid, entries);

	uint8_t status = matoclserv_check_group_cache(eptr, gid);

	OpReplay runSetQuota = [eptr, uid, gid, entries](FilesystemOperationContext &ctx) -> uint8_t {
		FsContext context = matoclserv_get_context(eptr, uid, gid);
		return gFSOperations->quotaSet(context, ctx, entries);
	};

	auto sendSetQuotaReply = [eptr, messageId](uint8_t replyStatus) {
		MessageBuffer reply;
		matocl::fuseSetQuota::serialize(reply, messageId, replyStatus);
		matoclserv_createpacket(eptr, std::move(reply));
	};

	if (status != SAUNAFS_STATUS_OK) {  // group-cache error: reply without running the op
		sendSetQuotaReply(status);
		return;
	}

	matoclserv_submit_op(eptr, std::move(runSetQuota), sendSetQuotaReply);
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
			status = gFSOperations->quotaGetAll(context, results);
		}
	} else if (version == cltoma::fuseGetQuota::kSelectedLimits) {
		std::vector<QuotaOwner> owners;
		cltoma::fuseGetQuota::deserialize(data, length, messageId, uid, gid, owners);
		status = matoclserv_check_group_cache(eptr, gid);
		if (status == SAUNAFS_STATUS_OK) {
			FsContext context = matoclserv_get_context(eptr, uid, gid);
			status = gFSOperations->quotaGet(context, owners, results);
		}
	} else {
		throw IncorrectDeserializationException(
				"Unknown SAU_CLTOMA_FUSE_GET_QUOTA version: " + std::to_string(version));
	}

	MessageBuffer reply;
	if (status == SAUNAFS_STATUS_OK) {
		status = gFSOperations->quotaGetInfo(matoclserv_get_context(eptr), results, info);
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

	matoclserv_for_each_session([&client_configs](const Session &session) {
		if (session.config.empty()) { return; }
		NetworkAddress addr(session.peerIpAddress, session.peerPort);
		client_configs[addr.toString()] = session.config;
	});

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
		uint8_t status = gFSOperations->startChecksumRecalculation();

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

void matocl_locks_release(const FsContext &context, const FilesystemOperationContext &fsOpContext,
                          inode_t inode, uint32_t sessionId) {
	std::vector<FileLocks::Owner> applied;

	gFSOperations->locksClearSession(context, fsOpContext, (uint8_t)safs_locks::Type::kFlock, inode,
	                                 sessionId, applied);

	for (auto candidate : applied) {
		matoclserv_lock_wake_up(candidate.sessionid, candidate.msgid, safs_locks::Type::kFlock);
	}

	applied.clear();
	gFSOperations->locksClearSession(context, fsOpContext, (uint8_t)safs_locks::Type::kPosix, inode,
	                                 sessionId, applied);

	for (auto candidate : applied) {
		matoclserv_lock_wake_up(candidate.sessionid, candidate.msgid, safs_locks::Type::kPosix);
	}
}

bool matocl_close_files(Session *currentSession) {
	FsContext context = FsContext::getForMaster(eventloop_time());

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);
	const bool contextHasTransaction = fsOpContext.hasReadWriteTransaction();

	// Teardown must preserve backend correctness across partial failures.
	//
	// File-backed backends apply release() synchronously, so every processed inode can be erased
	// from openFilesSet immediately.
	//
	// Transactional KV backends may batch many release() calls into one commit. If a batch commit
	// fails, the backend state for that batch is rolled back, so those inodes must remain in
	// openFilesSet. Callers that keep the session managed can retry them later. Only inodes from
	// successfully committed batches are erased here.
	//
	// We intentionally do not perform immediate retries for failed batches in
	// this function: release() may emit side effects outside the backend
	// transaction, so retrying here could duplicate those effects without a
	// matching backend state change. Failed inodes stay in openFilesSet so later
	// session-management or timeout-driven cleanup can retry them.
	std::vector<inode_t> pendingBatch;
	std::vector<inode_t> committed;
	if (contextHasTransaction) { pendingBatch.reserve(kTransactionBatchSize); }

	// Commits one pending batch for transactional backends.
	auto flushBatch = [&](bool isFinal) -> bool {
		if (!fsOpContext.getReadWriteTransaction()->commit()) { return false; }

		if (!isFinal) {
			committed.insert(committed.end(), pendingBatch.begin(), pendingBatch.end());
		}

		pendingBatch.clear();

		if (!isFinal) {
			fsOpContext = gFSOperations->createFilesystemOperationContext(
			    FilesystemOperationContext::TransactionType::kReadWrite);
		}

		return true;
	};

	bool anyFailure = false;
	size_t pendingOperations = currentSession->openFilesSet.size();

	for (inode_t openFileInode : currentSession->openFilesSet) {
		gFSOperations->release(context, fsOpContext, openFileInode, currentSession->sessionId);
		matocl_locks_release(context, fsOpContext, openFileInode, currentSession->sessionId);

		if (contextHasTransaction) {
			pendingOperations--;
			pendingBatch.push_back(openFileInode);

			const bool shouldFlush =
			    pendingBatch.size() >= kTransactionBatchSize || pendingOperations == 0;

			if (shouldFlush && !flushBatch(/*isFinal=*/pendingOperations == 0)) {
				safs::log_err(
				    "{}: batch commit failed while closing files for session {}; "
				    "leaving {} inode(s) from this batch plus {} un-started "
				    "inodes in openFilesSet for callers that can retry later",
				    __func__, currentSession->sessionId, pendingBatch.size(), pendingOperations);
				anyFailure = true;
				break;
			}
		}
	}

	if (!anyFailure) {
		currentSession->openFilesSet.clear();
	} else {
		for (inode_t inode : committed) { currentSession->openFilesSet.erase(inode); }
	}

	return !anyFailure;
}

void matoclserv_session_files(matoclserventry *eptr, [[maybe_unused]] const uint8_t *data,
                              [[maybe_unused]] uint32_t length) {
	std::vector<SessionFiles> sessions;
	MessageBuffer reply;

	// File-backed sessions report the local live-connection view. KV-backed managers may provide a
	// backend-derived catalog instead, such as a broader session view that is not limited to
	// currently connected local clients.
	if (matoclserv_uses_backend_session_list()) {
		sessions = matoclserv_list_sessions();
	} else {
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
	}

	matocl::listSessions::serialize(reply, sessions);
	matoclserv_createpacket(eptr, std::move(reply));
}

bool matocl_session_timedout(Session *currentSession) { return matocl_close_files(currentSession); }

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
	matoclserv_remove_timed_out_sessions(
	    [](Session *currentSession) { return matocl_session_timedout(currentSession); });
}

void matocl_before_disconnect(matoclserventry *eptr) {
	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	// Release locks held by delayed chunk ops the client never finished: a killed client sends
	// no end-of-operation request, so the lock must be released here or it strands and blocks
	// future writers. For a write op whose status arrived after the client was killed, a failed
	// create also drops the speculative chunk so it is not left dangling in the file.
	for (const auto &operation : eptr->delayedChunkOperations) {
		if (operation->type == FUSE_TRUNCATE) {
			gFSOperations->endSetLength(fsOpContext, operation->chunkId);
		} else if (operation->type == FUSE_WRITE) {
			if (operation->statusArrived && operation->pendingIsFailedCreate && eptr->sessionData) {
				FsContext context = FsContext::getForMasterWithSession(
				    eventloop_time(), eptr->sessionData->rootInode, eptr->sessionData->flags,
				    operation->uid, operation->gid, operation->auid, operation->agid);
				gFSOperations->removeChunkFromFile(context, fsOpContext, operation->inode,
				                                   operation->chunkId);
			}
			gFSOperations->writeEnd(fsOpContext, 0, 0, operation->chunkId, 0);
		}
	}

	// Commit the transaction for KV backends
	if (fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.getReadWriteTransaction()->commit()) {
			safs::log_critical(
			    "{}: transaction failed to commit while unlocking chunks for session {}", __func__,
			    eptr->sessionData ? eptr->sessionData->sessionId : 0);
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

		matoclserv_session_disconnected(eptr->sessionData);
	}

	matoclserv_remove_entry_from_unlock_list(eptr);
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

	if (type == SAU_CLTOMA_STARTTLS) {
		matoclserv_starttls(eptr);
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
				case SAU_CLTOMA_INOTIFIER_LIST:
					matoclserv_inotifier_list(eptr, data, length);
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
				case SAU_CLTOMA_FUSE_MKNOD:
					matoclserv_fuse_mknod(eptr, PacketHeader(type, length), data);
					break;
				case SAU_CLTOMA_FUSE_CREATE:
					matoclserv_fuse_create(eptr, PacketHeader(type, length), data);
					break;
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
					matoclserv_fuse_read_chunk(eptr, PacketHeader(type, length), data);
					break;
				case SAU_CLTOMA_CHUNKS_INFO:
					matoclserv_chunks_info(eptr, data, length);
					break;
				case SAU_CLTOMA_FUSE_WRITE_CHUNK:
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
				case SAU_CLTOMA_FUSE_GETGOAL:
					matoclserv_fuse_getgoal(eptr, PacketHeader(type, length), data);
					break;
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
					matoclserv_fuse_truncate(eptr, PacketHeader(type, length), data);
					break;
				case CLTOMA_FUSE_REPAIR:
				    matoclserv_fuse_repair(eptr, data, length);
				    break;
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
				case SAU_CLTOMA_INOTIFIER_LIST:
				    matoclserv_inotifier_list(eptr, data, length);
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
				case SAU_CLTOMA_ENDTLS:
					eptr->tlsSession.reset();
					break;
				default:
				    safs::log_info(
				        "main master server module: got unknown message from sfsmount (type:{})",
				        type);
				    eptr->mode=ClientConnectionMode::KILL;
			}
		} else if (eptr->registered == ClientState::kOldTools) {        // old sfstools
			safs::log_err("registered old tools connection !!!");
			eptr->mode=ClientConnectionMode::KILL;
			return;
		}
	} catch (const kv::RetryableTransactionError &e) {
		// A retryable backend read error (e.g. a transaction timeout under load) reached
		// the dispatch boundary from a read-only or not-yet-wrapped op. Do NOT abort the
		// single-threaded master: drop this client connection so it reconnects and retries
		// the request. Write ops route their bodies through the group-commit batch, which
		// retries retryable read errors in replay before ever reaching here. Non-retryable
		// failures (corruption, genuinely-absent required keys) still propagate and
		// fail-stop the master.
		safs::log_warn(
				"main master server module: retryable backend read error handling message "
				"(type:{}, length:{}, err:{}): {}; dropping client connection",
				type, length, e.errorCode(), e.what());
		eptr->mode = ClientConnectionMode::KILL;
	} catch (IncorrectDeserializationException& e) {
		safs::log_info(
				"main master server module: got inconsistent message from mount "
				"(type:{}, length:{}), {}", type, length, e.what());
		eptr->mode = ClientConnectionMode::KILL;
	}
}

void matoclserv_term() {
	safs::log_info("main master server module: closing {}:{}", gListenHost, gListenPort);
	tcpclose(masterSocket);

	// Drop any in-flight batch commits before tearing down client entries so their
	// reply continuations (which capture eptr) cannot run against freed memory.
	gInFlightBatch.reset();
	gHeldOps.clear();
	gOpenBatch.members.clear();
	gOpenBatch.hasContext = false;
	gOpenBatch.ctx = FilesystemOperationContext();

	// Invalidate before closing so a commit-wakeup callback racing in from the backend
	// network thread reads -1 (and skips the write) instead of touching a closed fd.
	int wakeupFd = gCommitWakeupFd.exchange(-1, std::memory_order_acq_rel);
	if (wakeupFd >= 0) {
		close(wakeupFd);
	}

	for (const auto &eptr : matoclservList) {
		eptr->delayedChunkOperations.clear();
	}

	matoclservList.clear();
	matoclserv_session_unload();
}

void matoclserv_read(matoclserventry *eptr) {
	SignalLoopWatchdog watchdog;
	int32_t bytesRead;

	watchdog.start();
	while (eptr->mode != ClientConnectionMode::KILL) {
		if (eptr->tlsSession != nullptr && eptr->mode != ClientConnectionMode::HANDSHAKE) {
			bytesRead =
			    SSL_read(eptr->tlsSession->session(), eptr->inputPacket.pointerToBeReadInto(),
			             eptr->inputPacket.bytesToBeRead());
		} else {
			bytesRead = read(eptr->socket, eptr->inputPacket.pointerToBeReadInto(),
			                 eptr->inputPacket.bytesToBeRead());
		}

		if (bytesRead == 0) {
			if (eptr->registered == ClientState::kRegistered) {
				safs::log_info("connection with client (ip:{}) has been closed by peer",
				               ipToString(eptr->peerIpAddress));
			}

			eptr->mode = ClientConnectionMode::KILL;
			return;
		}

		if (bytesRead < 0) {
			if (eptr->tlsSession != nullptr && eptr->mode != ClientConnectionMode::HANDSHAKE) {
				int err = SSL_get_error(eptr->tlsSession->session(), bytesRead);
				if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
					eptr->mode = ClientConnectionMode::KILL;
				}
				return;
			} else {
				if (errno != EAGAIN) {
#ifdef ECONNRESET
					if (errno != ECONNRESET) {
#endif
						safs_silent_errlog(LOG_NOTICE,
						                   "main master server module: (ip:%s) read error",
						                   ipToString(eptr->peerIpAddress).c_str());
#ifdef ECONNRESET
					}
#endif
					eptr->mode = ClientConnectionMode::KILL;
				}
				return;
			}
		}

		try {
			eptr->inputPacket.increaseBytesRead(bytesRead);
		} catch (const InputPacketTooLongException &ex) {
			safs::log_warn(
			    "main master server module: packet received from peer {}:{} is too long: {}",
			    ipToString(eptr->peerIpAddress), eptr->peerPort, ex.what());
			eptr->mode = ClientConnectionMode::KILL;
			return;
		}

		metrics::Counter::increment(metrics::Counter::Master::CLIENT_RX_BYTES, bytesRead);
		statsBytesReceived += bytesRead;

		if (eptr->inputPacket.hasData()) {
			auto header = eptr->inputPacket.getHeader();
			const auto data = eptr->inputPacket.getData();
			matoclserv_gotpacket(eptr, header.type, data.data(), data.size());

			statsPacketsReceived++;
			metrics::Counter::increment(metrics::Counter::Master::CLIENT_RX_PACKETS);

			eptr->inputPacket.reset();
		}

		if (watchdog.expired()) {
			break;
		}
	}
}

void matoclserv_write(matoclserventry *eptr) {
	SignalLoopWatchdog watchdog;
	int32_t bytesWritten;

	watchdog.start();
	while (!eptr->outputPackets.empty()) {
		OutputPacket &outputPacket = eptr->outputPackets.front();

		if (eptr->tlsSession != nullptr) {
			bytesWritten = SSL_write(eptr->tlsSession->session(),
			                         outputPacket.packet.data() + outputPacket.bytesSent,
			                         outputPacket.packet.size() - outputPacket.bytesSent);
			if (bytesWritten < 0) {
				int err = SSL_get_error(eptr->tlsSession->session(), bytesWritten);
				if (err != SSL_ERROR_WANT_WRITE && err != SSL_ERROR_WANT_READ) {
					eptr->mode = ClientConnectionMode::KILL;
				}
				return;
			}
		} else {
			bytesWritten = write(eptr->socket, outputPacket.packet.data() + outputPacket.bytesSent,
			                     outputPacket.packet.size() - outputPacket.bytesSent);
			if (bytesWritten < 0) {
				if (errno != EAGAIN) {
					safs_silent_errlog(LOG_NOTICE, "main master server module: (ip:%s) write error",
					                   ipToString(eptr->peerIpAddress).c_str());
					eptr->mode = ClientConnectionMode::KILL;
				}
				return;
			}
		}

		outputPacket.bytesSent += bytesWritten;
		metrics::Counter::increment(metrics::Counter::Master::CLIENT_TX_BYTES, bytesWritten);
		statsBytesSent += bytesWritten;

		if (outputPacket.bytesSent >= outputPacket.packet.size()) {
			statsPacketsSent++;
			metrics::Counter::increment(metrics::Counter::Master::CLIENT_TX_PACKETS);
			eptr->outputPackets.pop_front();
		} else {
			return;
		}

		if (watchdog.expired()) {
			break;
		}
	}
}

void matoclserv_wantexit() {
	exiting = 1;
}

bool matoclserv_client_async_operations_finished() {
	for (const auto &eptr : matoclservList) {
		if (!eptr->delayedChunkOperations.empty()) {
			return false;
		}
	}
	return true;
}

int matoclserv_canexit() {
	matoclserventry *adminTerminator = nullptr;
	static bool terminatorPacketSent = false;

	if (!matoclserv_client_async_operations_finished()) {
		return 0;
	}

	// Wait for the group-commit pipeline to drain: an in-flight batch's continuations
	// still reference client entries, and held ops have unsent replies.
	if (gInFlightBatch.has_value() || !gHeldOps.empty() || !gOpenBatch.members.empty()) {
		return 0;
	}

	for (const auto &eptr : matoclservList) {
		if (!eptr->outputPackets.empty()) {
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

		if (eptr->mode == ClientConnectionMode::HANDSHAKE) {
			int lastHandshakeError = eptr->lastHandshakeError;
			if (lastHandshakeError == SSL_ERROR_WANT_READ) {
				pdesc.back().events |= POLLIN;
			} else if (lastHandshakeError == SSL_ERROR_WANT_WRITE) {
				pdesc.back().events |= POLLOUT;
			} else {
				// Default: allow both if unknown
				pdesc.back().events |= POLLIN | POLLOUT;
			}
		} else {
			if (exiting == 0) { pdesc.back().events |= POLLIN; }

			if (!eptr->outputPackets.empty()) { pdesc.back().events |= POLLOUT; }
		}
	}
}

void matoclserv_serve(const std::vector<pollfd> &pdesc) {
	uint32_t now = eventloop_time();

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
			eptr->adminTask = AdminTask::kNone;
			eptr->tlsSession = nullptr;

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
				if (eptr->mode == ClientConnectionMode::HANDSHAKE) {
					matoclserv_tlshandshake(eptr.get());
				} else {
					matoclserv_read(eptr.get());
				}
			}
		}
	}

// write
	for (const auto &eptr : matoclservList) {
		if (eptr->lastWriteTimestamp + 2 < now && eptr->registered != ClientState::kOldTools &&
		    eptr->outputPackets.empty()) {
			// 4 byte length because of 'msgid'
			uint8_t *ptr = matoclserv_createpacket(eptr.get(), ANTOAN_NOP, 4);
			*((uint32_t *)ptr) = 0;
		}

		if (eptr->pDescPos >= 0) {
			if ((((pdesc[eptr->pDescPos].events & POLLOUT) == 0 && !eptr->outputPackets.empty()) ||
			     (pdesc[eptr->pDescPos].revents & POLLOUT)) &&
			    eptr->mode != ClientConnectionMode::KILL) {
				eptr->lastWriteTimestamp = now;
				if (eptr->mode == ClientConnectionMode::HANDSHAKE) {
					matoclserv_tlshandshake(eptr.get());
				} else {
					matoclserv_write(eptr.get());
				}
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
			// Defer teardown while batch commits still reference this entry; their
			// replies are dropped in matoclserv_finish_member() and the entry is
			// freed on a later iteration once pendingCommits hits zero.
			if (eptr->pendingCommits > 0) {
				++eptrIt;
				continue;
			}
			matocl_before_disconnect(eptr);
			eptr->tlsSession.reset();
			tcpclose(eptr->socket);
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
		if (gChunkOperations->getMissingCount() < 100) {
			starting = 0;
		} else {
			starting--;
		}
	}
}

int matoclserv_iolimits_reload() {
	std::string configFile = cfg_getstring("GLOBALIOLIMITS_FILENAME", "");
	const std::string defaultConfigFile = ETC_PATH "/leil-globaliolimits.cfg";
	const std::string legacyConfigFile = ETC_PATH "/sfsglobaliolimits.cfg";
	if (configFile == defaultConfigFile && access(defaultConfigFile.c_str(), F_OK) != 0 &&
	    access(legacyConfigFile.c_str(), F_OK) == 0) {
		safs::log_warn(
		    "using legacy global I/O limits configuration file {} because configured default file {} was not found",
		    legacyConfigFile, defaultConfigFile);
		configFile = legacyConfigFile;
	}
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
	eventloop_timeregister(TIMEMODE_RUN_LATE, 3600, 0, matocl_clean_unlock_chunks_list);
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

	matoclserv_configure_session_sustain_time();

	matoclserv_iolimits_reload();

	gMaxCommitRetries = cfg_getuint32("MATOCL_MAX_COMMIT_RETRIES", 5U);
	gDebugInjectConflicts = cfg_getuint32("MATOCL_DEBUG_INJECT_COMMIT_CONFLICTS", 0U);
	gDebugInjectReadConflicts = cfg_getuint32("MATOCL_DEBUG_INJECT_READ_CONFLICTS", 0U);

	std::string oldListenHost = gListenHost;
	std::string oldListenPort = gListenPort;

	gListenHost = cfg_getstring("MATOCL_LISTEN_HOST","*");
	gListenPort = cfg_getstring("MATOCL_LISTEN_PORT","9421");

	if (oldListenHost == gListenHost && oldListenPort == gListenPort) {
		safs::log_info("main master server module: socket address hasn't changed ({}:{})",
		               gListenHost, gListenPort);
		return;
	}

	int newlsock = tcpsocket();
	if (newlsock < 0) {
		safs::log_warn(
		    "main master server module: socket address has changed, but can't create new socket");
		gListenHost = oldListenHost;
		gListenPort = oldListenPort;
		return;
	}

	tcpnonblock(newlsock);
	tcpnodelay(newlsock);
	tcpreuseaddr(newlsock);

	if (tcpsetacceptfilter(newlsock) < 0 && errno != ENOTSUP) {
		safs_silent_errlog(LOG_NOTICE, "main master server module: can't set accept filter");
	}

	if (tcpstrlisten(newlsock, gListenHost.c_str(), gListenPort.c_str(), 100) < 0) {
		safs::log_err(
		    "main master server module: socket address has changed, but can't listen on socket ({}:{})",
		    gListenHost, gListenPort);
		gListenHost = oldListenHost;
		gListenPort = oldListenPort;
		tcpclose(newlsock);
		return;
	}

	safs::log_info("main master server module: socket address has changed, now listen on {}:{}",
	               gListenHost, gListenPort);
	tcpclose(masterSocket);
	masterSocket = newlsock;
}

int matoclserv_network_init() {
	gListenHost = cfg_getstring("MATOCL_LISTEN_HOST", "*");
	gListenPort = cfg_getstring("MATOCL_LISTEN_PORT", "9421");

	gMaxCommitRetries = cfg_getuint32("MATOCL_MAX_COMMIT_RETRIES", 5U);
	gDebugInjectConflicts = cfg_getuint32("MATOCL_DEBUG_INJECT_COMMIT_CONFLICTS", 0U);
	gDebugInjectReadConflicts = cfg_getuint32("MATOCL_DEBUG_INJECT_READ_CONFLICTS", 0U);

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

	if (tcpstrlisten(masterSocket, gListenHost.c_str(), gListenPort.c_str(), 100) < 0) {
		safs::log_err("main master server module: can't listen on {}:{}", gListenHost, gListenPort);
		return -1;
	}

	safs::log_info("main master server module: listen on {}:{}", gListenHost, gListenPort);

	matoclservList.clear();

	if (metadataserver::isMaster()) {
		matoclserv_become_master();
	}

	// Wakeup channel for deferred commits: poll() watches this eventfd so a commit
	// completing on the backend network thread wakes the loop immediately. Non-fatal if
	// it fails (EFD missing): the deferred path then runs at poll-timeout cadence.
	int wakeupFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	gCommitWakeupFd.store(wakeupFd, std::memory_order_release);
	if (wakeupFd < 0) {
		safs::log_warn("main master server module: eventfd for async commit wakeup failed: {}",
		               strerr(errno));
	}

	eventloop_reloadregister(matoclserv_reload);
	metadataserver::registerFunctionCalledOnPromotion(matoclserv_become_master);
	eventloop_destructregister(matoclserv_term);
	eventloop_pollregister(matoclserv_desc,matoclserv_serve);
	eventloop_pollregister(matoclserv_commit_wakeup_desc, matoclserv_commit_wakeup_serve);
	eventloop_eachloopregister(matoclserv_poll_batch);
	eventloop_wantexitregister(matoclserv_wantexit);
	eventloop_canexitregister(matoclserv_canexit);
	return 0;
}
