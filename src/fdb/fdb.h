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

#include <cstdint>
#include <memory>
#include <string>

#include <fdb/fdb_api_version.h>  // Needs to be included before foundationdb/fdb_c.h
#include <foundationdb/fdb_c.h>
#include <foundationdb/fdb_c_types.h>

#include "kv/itransaction.h"

namespace fdb {

/// Process-wide FoundationDB operation counters for performance profiling.
/// Each field counts issued client calls; reads and commits incur a network
/// round-trip, buffered writes (set/atomicAdd/clear) do not until commit.
struct FdbOpCounters {
	uint64_t pointReads{0};       ///< fdb_transaction_get calls (sync get + getAsync).
	uint64_t rangeReads{0};       ///< fdb_transaction_get_range calls (sync + async).
	uint64_t sets{0};             ///< fdb_transaction_set calls (buffered).
	uint64_t atomicAdds{0};       ///< FDB_MUTATION_TYPE_ADD ops (buffered).
	uint64_t clears{0};           ///< fdb_transaction_clear calls (buffered).
	uint64_t clearRanges{0};      ///< fdb_transaction_clear_range calls (buffered).
	uint64_t commits{0};          ///< commit attempts (each one round-trip).
	uint64_t commitConflicts{0};  ///< commit attempts that failed with a retryable error.
	uint64_t commitFailures{0};   ///< commit attempts that failed with a non-retryable error.
};

/// Returns a snapshot of the process-wide FDB operation counters.
FdbOpCounters getOpCounters();

/// Enables or disables op-counter accounting (disabled by default). When
/// disabled, the per-call bump is a single relaxed bool load and getOpCounters()
/// keeps reporting whatever was counted while enabled. Intended to be toggled
/// once at startup from configuration.
void setOpCountersEnabled(bool enabled);

/// Returns whether op-counter accounting is currently enabled.
bool opCountersEnabled();

/// A class that wraps the FoundationDB database instance.
class DB {
public:
	/// Constructs a DB instance with the specified cluster file.
	explicit DB(const std::string &clusterFile) {
		const auto *clusterPath = clusterFile.empty() ? nullptr : clusterFile.c_str();
		FDBDatabase *auxDB{nullptr};
		error_ = fdb_create_database(clusterPath, &auxDB);

		if (error_ == 0) { db_.reset(auxDB); }
	}

	/// Not needed constructors and assignment operators.
	/// Deleted to prevent copying or moving of the DB object.
	DB(const DB &) = delete;
	DB &operator=(const DB &) = delete;
	DB(DB &&) = delete;
	DB &operator=(DB &&) = delete;

	/// Default destructor.
	/// Cleans up the FDBDatabase instance using the custom deleter.
	~DB() = default;

	static fdb_error_t selectAPIVersion(int version);
	static std::string_view errorMsg(fdb_error_t code);
	static bool evaluatePredicate(int predicate_test, fdb_error_t code);

	// network

	/// Sets a network option for the FoundationDB client.
	/// @param option The network option to set.
	/// @param value The value to set for the option. Default is an empty string.
	/// @return The error code of the operation.
	/// @see FDBNetworkOption for available options.
	static fdb_error_t setNetworkOption(FDBNetworkOption option, std::string_view value = {});

	/// Sets up the FoundationDB network thread.
	/// This must be called before any database operations.
	/// @return The error code of the operation.
	static fdb_error_t setupNetwork();

	/// Runs the FoundationDB network thread.
	/// This must be called after setting up the network.
	/// @return The error code of the operation.
	/// @note This function blocks until the network thread is stopped.
	/// It is typically called in a separate thread.
	static fdb_error_t runNetwork();

	/// Stops the FoundationDB network thread.
	/// This should be called when the application is shutting down.
	/// @return The error code of the operation.
	/// @note This function blocks until the network thread is stopped.
	static fdb_error_t stopNetwork();

	// database

	/// Returns the error code of the last operation.
	fdb_error_t error() const { return error_; }

	/// Allows to use DB as a boolean in conditions. e.g. if (db) { ... }
	explicit operator bool() const { return error() == 0; }

	/// Allows to use DB as a pointer to FDBDatabase. e.g. auto *dbPtr = db;
	/// This is useful for passing the database to functions that expect a pointer.
	operator FDBDatabase *() const { return db_.get(); }

	/// Returns a pointer to the underlying FDBDatabase.
	FDBDatabase *getDB() const {
		if (error_ != 0) { return nullptr; }
		return db_.get();
	}

	/// Wrapper to set an option for the database.
	fdb_error_t setOption(FDBDatabaseOption option, std::string_view value = {});

private:
	/// Custom deleter for FDBDatabase (C struct), to ensure proper cleanup.
	struct FDBDatabaseDeleter {
		constexpr FDBDatabaseDeleter() noexcept = default;
		void operator()(FDBDatabase *_db) const {
			(_db != nullptr) ? fdb_database_destroy(_db) : void();
		}
	};

	std::unique_ptr<FDBDatabase, FDBDatabaseDeleter> db_;  ///< The wrapped FDBDatabase pointer.
	fdb_error_t error_ = 1;  ///< The error code of the last operation.
};

/// Custom deleter for FDBFuture (C struct), to ensure proper cleanup.
struct FDBFutureDeleter {
	constexpr FDBFutureDeleter() noexcept = default;
	void operator()(FDBFuture *fut) const {
		if (fut != nullptr) { fdb_future_destroy(fut); }
	}
};

/// Owning handle for an FDBFuture; automatically destroyed on scope exit.
using UniqueFDBFuture = std::unique_ptr<FDBFuture, FDBFutureDeleter>;

/// A class that wraps a FoundationDB transaction.
/// Provides methods to perform read and write operations on the database.
class Transaction {
public:
	/// Constructs a Transaction object using the provided DB instance.
	/// @param db A pointer to the FoundationDB database instance.
	/// @note The transaction is created with default options.
	Transaction(DB *db) {
		FDBTransaction *auxTr{nullptr};
		error_ = fdb_database_create_transaction(db->getDB(), &auxTr);

		if (error_ == 0) { tr_.reset(auxTr); }
	}

	fdb_error_t error() const { return error_; }
	explicit operator bool() const { return error_ == 0 && tr_; }

	fdb_error_t setOption(FDBTransactionOption option, std::string_view value = {});

	/// Gets a value for a given key.
	/// @param key The key to retrieve the value for.
	std::optional<kv::Value> get(const kv::Key &key, bool snapshot = false);

	/// Gets a value for a given key asynchronously.
	/// @param key The key to retrieve the value for.
	/// @param snapshot Whether to use a snapshot for the transaction.
	/// @return A future that will contain the value when ready.
	/// @note The transaction must remain alive until the future's get() method is called.
	std::unique_ptr<kv::IFuture> getAsync(const kv::Key &key, bool snapshot = false);

	/// Gets a range of keys and values.
	/// @param begin The starting key for the range.
	/// @param end The ending key for the range.
	/// @param limit The maximum number of key-value pairs to retrieve.
	/// @param iteration The iteration number for streaming mode.
	/// @param snapshot Whether to use a snapshot for the transaction.
	/// @param reverse Whether to retrieve the range in reverse order.
	/// @param streamingMode The streaming mode to use for the range query.
	/// @return GetRangeResult with key-value pairs and whether more results are available.
	/// @note If the transaction is not valid, it returns an empty GetRangeResult
	kv::GetRangeResult getRange(const kv::KeySelector &begin, const kv::KeySelector &end, int limit,
	                            int iteration = 0, bool snapshot = false, bool reverse = false,
	                            FDBStreamingMode streamingMode = FDB_STREAMING_MODE_SERIAL);

	/// Gets a range of keys and values asynchronously.
	/// @param begin The starting key for the range.
	/// @param end The ending key for the range.
	/// @param limit The maximum number of key-value pairs to retrieve.
	/// @param iteration The iteration number for streaming mode.
	/// @param snapshot Whether to use a snapshot for the transaction.
	/// @param reverse Whether to retrieve the range in reverse order.
	/// @param streamingMode The streaming mode to use for the range query.
	/// @return A future that will contain the range when ready.
	/// @note The transaction must remain alive until the future's get() method is called.
	std::unique_ptr<kv::IRangeFuture> getRangeAsync(
	    const kv::KeySelector &begin, const kv::KeySelector &end, int limit, int iteration = 0,
	    bool snapshot = false, bool reverse = false,
	    FDBStreamingMode streamingMode = FDB_STREAMING_MODE_SERIAL);

	/// Sets a value for a given key.
	/// @param key The key to set the value for.
	/// @param value The value to set for the key.
	void set(const kv::Key &key, const kv::Value &value);

	/// Atomically adds a delta value to the existing value for a given key.
	/// @param key The key to add the delta to.
	/// @param delta The delta value to add.
	void atomicAdd(const kv::Key &key, const kv::Value &delta);

	/// Removes a key from the database.
	/// @param key The key to remove.
	void remove(const kv::Key &key);

	/// Removes a half-open key range [start, end) from the database.
	void removeRange(const kv::Key &start, const kv::Key &end);

	/// Commits the transaction.
	/// @return True if the commit was successful, false otherwise.
	bool commit();

	/// Gets the committed version of the transaction.
	/// @return The committed version, if available.
	std::optional<int64_t> getCommittedVersion() const { return committedVersion_; }

private:
	/// Custom deleter for FDBTransaction (C struct), to ensure proper cleanup.
	struct FDBTransactionDeleter {
		constexpr FDBTransactionDeleter() noexcept = default;
		void operator()(FDBTransaction *_tr) const {
			_tr != nullptr ? fdb_transaction_destroy(_tr) : void();
		}
	};

	/// The wrapped FDBTransaction pointer.
	std::unique_ptr<FDBTransaction, FDBTransactionDeleter> tr_;
	/// The error code of the last operation.
	fdb_error_t error_{1};

	/// The commit version of the transaction, if applicable.
	std::optional<int64_t> committedVersion_;
};

}  // namespace fdb
