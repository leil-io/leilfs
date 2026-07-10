/*
   Copyright 2023      Leil Storage OÜ

   This file is part of SaunaFS.

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

#include "fdb/fdb.h"
#include "kv/itransaction.h"

namespace fdb {

/// Provides an implementation of a read-write transaction for FoundationDB, compatible with the
/// kv::IReadWriteTransaction interface, to be used with the kv::IKVEngine interface.
/// Wraps a fdb::Transaction to provide the necessary methods for key-value operations.
class FDBTransaction final : public kv::IReadWriteTransaction {
public:
	/// Constructs a FDBTransaction using the provided fdb::Transaction.
	/// @param tr The fdb::Transaction to wrap.
	FDBTransaction(fdb::Transaction &&_tr) : tr_(std::move(_tr)), error_(0) {}

	// Non-copyable, non-movable (base class is non-movable)
	FDBTransaction(const FDBTransaction &) = delete;
	FDBTransaction &operator=(const FDBTransaction &) = delete;
	FDBTransaction(FDBTransaction &&) = delete;
	FDBTransaction &operator=(FDBTransaction &&) = delete;

	/// Default destructor. The members are RAII or simple.
	~FDBTransaction() = default;

	/// Retrieves the value for a given key.
	/// @param key The key to retrieve the value for.
	std::optional<kv::Value> get(const kv::Key &key) override;

	/// Retrieves the value for a given key without adding it to the
	/// transaction's read conflict range (snapshot read).
	/// @warning Snapshot reads do not participate in conflict checking and
	///          must not be used for correctness-critical read-modify-write
	///          logic. They are intended for advisory reads (e.g. observing
	///          a hot counter) where occasional anomalies are acceptable.
	/// @param key The key to retrieve the value for.
	std::optional<kv::Value> getSnapshot(const kv::Key &key) override;

	/// Retrieves the value for a given key asynchronously.
	/// @param key The key to retrieve the value for.
	/// @return A future that will contain the value when ready.
	/// @note The transaction must remain alive until the future's get() method is called.
	std::unique_ptr<kv::IFuture> getAsync(const kv::Key &key) override;

	/// Retrieves a range of keys and values
	/// @param start The starting key for the range.
	/// @param end The ending key for the range.
	/// @param limit The maximum number of key-value pairs to retrieve.
	kv::GetRangeResult getRange(const kv::KeySelector &start, const kv::KeySelector &end,
	                            int limit = kv::kDefaultGetRangeLimit) override;

	/// Retrieves a range of keys and values asynchronously.
	/// @param start The starting key for the range.
	/// @param end The ending key for the range.
	/// @param limit The maximum number of key-value pairs to retrieve.
	/// @return A future that will contain the range when ready.
	/// @note The transaction must remain alive until the future's get() method is called.
	std::unique_ptr<kv::IRangeFuture> getRangeAsync(const kv::KeySelector &start,
	                                                const kv::KeySelector &end,
	                                                int limit = kv::kDefaultGetRangeLimit) override;

	/// Sets a value for a given key.
	/// @param key The key to set the value for.
	/// @param value The value to set for the key.
	void set(const kv::Key &key, const kv::Value &value) override;

	/// Atomically adds a delta value to the existing value for a given key.
	/// @param key The key to add the delta to.
	/// @param delta The delta value to add (must be little-endian).
	void atomicAdd(const kv::Key &key, const kv::Value &delta) override;

	/// Atomically sets the value to max(existing, value) as little-endian unsigned
	/// integers (FDB MAX, not lexicographic BYTE_MAX). Conflict-free.
	/// @see kv::IReadWriteTransaction::atomicMax.
	void atomicMax(const kv::Key &key, const kv::Value &value) override;

	/// Removes a key from the database.
	/// @param key The key to remove.
	void remove(const kv::Key &key) override;

	/// Removes a half-open key range [start, end) from the database.
	void removeRange(const kv::Key &start, const kv::Key &end) override;

	/// Commits the transaction, making all changes permanent.
	bool commit() override;

	/// Submits the commit asynchronously and returns a pollable future.
	std::unique_ptr<kv::ICommitFuture> commitAsync() override;

	/// Returns the committed version of the transaction, if available.
	std::optional<int64_t> getCommittedVersion() const override;

	/// Number of buffered mutations issued on this transaction so far.
	uint64_t mutationCount() const override { return mutationCount_; }

	/// Approximate byte size of the buffered writes so far (client-side estimate).
	std::optional<uint64_t> getApproximateSize() override;

	/// Returns the error code of the last operation.
	fdb_error_t error() const { return error_; }

private:
	fdb::Transaction tr_;
	fdb_error_t error_{1};
	uint64_t mutationCount_{0};
};

}  // namespace fdb
