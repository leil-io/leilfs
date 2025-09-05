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

#include "kv/itransaction.h"

#include "fdb/fdb.h"

namespace fdb {

/// Provides an implementation of a read-write transaction for FoundationDB, compatible with the
/// kv::IReadWriteTransaction interface, to be used with the kv::IKVEngine interface.
/// Wraps a fdb::Transaction to provide the necessary methods for key-value operations.
class FDBTransaction final : public kv::IReadWriteTransaction {
public:
	/// Constructs a FDBTransaction using the provided fdb::Transaction.
	/// @param tr The fdb::Transaction to wrap.
	FDBTransaction(fdb::Transaction &&_tr) : tr_(std::move(_tr)), error_(0) {}

	// Non-copyable, but movable (maybe to a queue of transactions).
	FDBTransaction(const FDBTransaction &) = delete;
	FDBTransaction &operator=(const FDBTransaction &) = delete;

	/// Move constructor/assignment operator.
	FDBTransaction(FDBTransaction &&) = default;
	FDBTransaction &operator=(FDBTransaction &&) = default;

	/// Default destructor. The members are RAII or simple.
	~FDBTransaction() = default;

	/// Retrieves the value for a given key.
	/// @param key The key to retrieve the value for.
	std::optional<kv::Value> get(const kv::Key &key) override;

	/// Retrieves a range of keys and values
	/// @param start The starting key for the range.
	/// @param end The ending key for the range.
	/// @param limit The maximum number of key-value pairs to retrieve.
	kv::GetRangeResult getRange(const kv::KeySelector &start, const kv::KeySelector &end,
	                            int limit = kv::kDefaultGetRangeLimit) override;

	/// Sets a value for a given key.
	/// @param key The key to set the value for.
	/// @param value The value to set for the key.
	void set(const kv::Key &key, const kv::Value &value) override;

	/// Removes a key from the database.
	/// @param key The key to remove.
	void remove(const kv::Key &key) override;

	/// Commits the transaction, making all changes permanent.
	bool commit() override;

	/// Returns the committed version of the transaction, if available.
	std::optional<int64_t> getCommittedVersion() const override;

	/// Returns the error code of the last operation.
	fdb_error_t error() const { return error_; }

private:
	fdb::Transaction tr_;
	fdb_error_t error_{1};
};

}  // namespace fdb
