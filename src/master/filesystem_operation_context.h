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
	FilesystemOperationContext(FilesystemOperationContext &&) = default;
	FilesystemOperationContext &operator=(FilesystemOperationContext &&) = default;

	/// Destructor.
	/// Destroying the context releases any owned transaction objects and destroys the nodes
	/// owned by the arena. It does not implicitly commit or rollback transactions; callers must
	/// ensure that any required commit or rollback is performed via the transaction interfaces
	/// before the context is destroyed.
	/// Debug builds catch a committed context whose deferred changelog was not drained,
	/// because dropping those entries would lose changelog, metalogger and notifier sinks.
	~FilesystemOperationContext() {
		assert(deferredChangelogEntries_.empty() || !commitConfirmed_);
	}

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

	/// Get the transaction type hint
	TransactionType getTransactionType() const { return transactionType_; }

	/// Per-operation owner of KV-materialized nodes; stays empty on in-memory builds.
	/// Accessible from const contexts: pinning is physical cache state shared through the
	/// const resolution seam, not a logical filesystem mutation.
	/// @see FSNodeArena
	FSNodeArena &nodeArena() const { return nodeArena_; }

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

	/// See setDeferChangelog().
	bool deferChangelog_ = false;

	/// Changelog entries buffered for post-commit publication; mutable like nodeArena_.
	mutable std::vector<DeferredChangelogEntry> deferredChangelogEntries_;

	/// See confirmCommitted(); mutable like the entry buffer it guards.
	mutable bool commitConfirmed_ = false;
};
