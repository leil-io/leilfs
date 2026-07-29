/*
   Copyright 2025      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include <cassert>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/type_defs.h"
#include "kv/itransaction.h"
#include "kv/kv_types.h"
#include "master/filesystem_node_arena.h"

namespace kv {
class IReadOnlyTransaction;
class IReadWriteTransaction;
}  // namespace kv

/// Final durability outcome delivered to backend-owned transaction effects.
enum class FilesystemTransactionOutcome : std::uint8_t {
	kCommitted,
	kAborted,
	kIndeterminate,
};

/// Backend-owned effects that must follow the durability outcome of one transaction.
///
/// The public filesystem layer deliberately does not know what an effect contains. A KV
/// backend can use this object to stage cache changes while its transaction is active, apply
/// them after a known commit, discard them after a definite abort, and reconcile them from
/// durable state after an indeterminate commit.
class FilesystemTransactionEffects {
public:
	FilesystemTransactionEffects() = default;
	FilesystemTransactionEffects(const FilesystemTransactionEffects &) = delete;
	FilesystemTransactionEffects &operator=(const FilesystemTransactionEffects &) = delete;
	virtual ~FilesystemTransactionEffects() = default;

	virtual void finish(FilesystemTransactionOutcome outcome) noexcept = 0;
};

/// Context for filesystem operations that may require database transactions.
///
/// This class provides an optional transaction context that can be propagated through the call
/// stack during filesystem operations. It enables grouping multiple database operations into a
/// single atomic unit, ensuring consistency and improving performance.
///
/// Usage patterns:
/// - **In-memory implementation** (Master, Shadows): Transactions are not used; the context
///   remains empty and is effectively ignored.
/// - **Distributed implementations** (e.g., FoundationDB-based MDS): The context carries an
///   active transaction, allowing efficient batching and atomic commits.
///
/// The class supports both read-only and read-write transactions, with move-only semantics
/// to ensure proper ownership of transaction resources.
class FilesystemOperationContext {
public:
	/// Type of transaction required for this operation
	enum class TransactionType : std::uint8_t {
		kNone,      ///< No transaction
		kReadOnly,  ///< Read-only transaction
		kReadWrite  ///< Read-write transaction
	};

	/// Create an empty context (no transaction)
	FilesystemOperationContext() = default;

	/// Create a context with a read-only transaction
	explicit FilesystemOperationContext(std::unique_ptr<kv::IReadOnlyTransaction> txn);

	/// Create a context with a read-write transaction
	explicit FilesystemOperationContext(std::unique_ptr<kv::IReadWriteTransaction> txn);

	/// Move-only semantics (transactions cannot be copied)
	FilesystemOperationContext(const FilesystemOperationContext &) = delete;
	FilesystemOperationContext &operator=(const FilesystemOperationContext &) = delete;
	FilesystemOperationContext(FilesystemOperationContext &&other) noexcept;
	FilesystemOperationContext &operator=(FilesystemOperationContext &&other) noexcept;

	/// Destructor.
	/// Destroying the context releases any owned transaction objects and destroys the nodes
	/// owned by the arena. It does not implicitly commit or rollback transactions; callers must
	/// ensure that any required commit or rollback is performed via the transaction interfaces
	/// before the context is destroyed.
	/// Debug builds catch a committed context whose deferred changelog was not drained,
	/// because dropping those entries would lose changelog, metalogger and notifier sinks.
	~FilesystemOperationContext();

	/// Returns true if this context has an active transaction
	bool hasTransaction() const;

	/// Returns true if this context has a read-write transaction
	bool hasReadWriteTransaction() const;

	/// Get the read-only transaction (null if none)
	kv::IReadOnlyTransaction *getReadOnlyTransaction() const;

	/// Get the read-write transaction (null if none or read-only)
	kv::IReadWriteTransaction *getReadWriteTransaction() const;

	/// Relinquishes ownership of the read-write transaction (nullptr if none). Used by
	/// the retry coordinator to retain the transaction across attempts (backend
	/// recovery keeps its accumulated backoff) while this context, with all its
	/// attempt-scoped state (arena, deferred changelog buffer, confirmation flag), is
	/// discarded. After this call the context has no transaction and should only be
	/// destroyed.
	std::unique_ptr<kv::IReadWriteTransaction> releaseReadWriteTransaction();

	/// Returns the backend-owned effects object, or nullptr when none is attached.
	FilesystemTransactionEffects *transactionEffects() const { return transactionEffects_.get(); }

	/// Attaches the effects object for this transaction.
	///
	/// A context has one backend owner, so replacing an existing object is a programming
	/// error. The object may be attached through a const operation seam because it is
	/// transaction-local physical state, like nodeArena().
	void setTransactionEffects(std::unique_ptr<FilesystemTransactionEffects> effects) const;

	/// Delivers the transaction's final durability outcome exactly once.
	///
	/// Destroying an unfinished context delivers kAborted automatically. Callers must
	/// explicitly deliver kIndeterminate because reconciliation can require backend reads.
	void finishTransactionEffects(FilesystemTransactionOutcome outcome) const noexcept;

	/// Commits the owned read-write transaction and finalizes backend effects from the
	/// reported outcome. Returns true without work when the context has no transaction.
	bool commitTransaction() const;

	/// Finalizes backend effects after an externally driven synchronous or asynchronous
	/// commit. The transaction's commitOutcomeUnknown() distinguishes a definite abort
	/// from an indeterminate result.
	void finishTransactionEffectsAfterCommit(bool committed) const noexcept;

	/// Get the transaction type hint
	TransactionType getTransactionType() const { return transactionType_; }

	/// Per-operation owner of KV-materialized nodes; stays empty on in-memory builds.
	/// Accessible from const contexts: pinning is physical cache state shared through the
	/// const resolution seam, not a logical filesystem mutation.
	/// @see FSNodeArena
	FSNodeArena &nodeArena() const { return nodeArena_; }

	/// Per-operation cache of decoded chunk-table bucket rows, keyed by (inode,
	/// bucket); an empty vector caches an absent row. Only KV backends populate
	/// it, mirroring their transaction's view so one operation reads each row at
	/// most once. Mutable like nodeArena_: physical cache state shared through
	/// the const resolution seam.
	std::map<std::pair<inode_t, uint32_t>, std::vector<uint64_t>> &bucketRowCache() const {
		return bucketRowCache_;
	}

	/// A changelog entry whose publication is held until the owning transaction commits.
	struct DeferredChangelogEntry {
		uint64_t version;
		std::string entry;
	};

	/// Buffer changelog sinks until this context's transaction commits.
	/// Needed for replayable KV commits: aborted attempts drop their buffers, and
	/// consumers never see uncommitted entries.
	void setDeferChangelog() { deferChangelog_ = true; }

	/// Returns true when changelog entries must be buffered instead of published inline.
	bool deferChangelog() const { return deferChangelog_; }

	/// Buffer one changelog entry for post-commit publication.
	void appendDeferredChangelogEntry(uint64_t version, std::string entry) const {
		deferredChangelogEntries_.emplace_back(version, std::move(entry));
	}

	/// Drains buffered entries for post-commit publication.
	/// Dropping an unconfirmed context without draining discards aborted/replayed entries.
	std::vector<DeferredChangelogEntry> takeDeferredChangelogEntries() const {
		return std::exchange(deferredChangelogEntries_, {});
	}

	/// Records that the transaction committed so the destructor catches an undrained buffer.
	void confirmCommitted() const { commitConfirmed_ = true; }

	/// Marks this context as the shared transaction of a client group-commit batch.
	/// Batch bodies stage in-memory-derived blind writes long before the batch's
	/// asynchronous commit is submitted; those writes must be conflict-checked (see
	/// kv::IReadWriteTransaction::addReadConflictKey) or a synchronous commit landing
	/// inside that window is silently overwritten by the stale staged value.
	void setGroupCommitBatch() { groupCommitBatch_ = true; }

	/// Returns true for the shared transaction of a client group-commit batch.
	bool isGroupCommitBatch() const { return groupCommitBatch_; }

private:
	/// The active read-write transaction (if any)
	std::unique_ptr<kv::IReadWriteTransaction> rwTransaction_ = nullptr;

	/// The active read-only transaction (if any and rwTransaction_ is null)
	std::unique_ptr<kv::IReadOnlyTransaction> roTransaction_ = nullptr;

	/// Hint about what transaction type to create if lazy initialization is needed
	TransactionType transactionType_ = TransactionType::kNone;

	/// Nodes materialized by a KV backend during this operation; freed on teardown.
	/// Mutable so the const seam can pin; see nodeArena().
	mutable FSNodeArena nodeArena_;

	/// See bucketRowCache(); mutable like nodeArena_.
	mutable std::map<std::pair<inode_t, uint32_t>, std::vector<uint64_t>> bucketRowCache_;

	/// See setDeferChangelog().
	bool deferChangelog_ = false;

	/// Changelog entries buffered for post-commit publication; mutable like nodeArena_.
	mutable std::vector<DeferredChangelogEntry> deferredChangelogEntries_;

	/// See setGroupCommitBatch().
	bool groupCommitBatch_ = false;

	/// See confirmCommitted(); mutable like the entry buffer it guards.
	mutable bool commitConfirmed_ = false;

	/// Type-erased backend effects whose lifetime is tied to the transaction.
	mutable std::unique_ptr<FilesystemTransactionEffects> transactionEffects_;

	/// Guards the exactly-once outcome contract.
	mutable bool transactionEffectsFinished_ = false;
};
