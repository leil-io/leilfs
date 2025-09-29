// src/master/changelog_fdb.cc

// must be before foundationdb include
#ifndef FDB_API_VERSION
#define FDB_API_VERSION 730
#endif
#include <foundationdb/fdb_c.h>
#include <foundationdb/fdb_c_types.h>

#include <string>

#include "master/changelog_fdb.h"
#include "slogger/slogger.h"

namespace {

FDBDatabase *g_fdb = nullptr;

const uint8_t *to_byte_ptr(const char *ptr) {
	// FoundationDB API requires uint8_t*, but std::string provides char*.
	// This cast is safe as both types have identical representation.
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
	return reinterpret_cast<const uint8_t *>(ptr);
}

}  // namespace

// Simple one-time DB init (call from master init)
int changelog_fdb_init() {
	safs::log_info("Initializing FoundationDB changelog");
	fdb_error_t fdbError;
	fdbError = fdb_select_api_version(FDB_API_VERSION);
	if (fdbError != 0) {
		safs::log_err("FDB select API version failed: {}", fdb_get_error(fdbError));
		return -1;
	}
	// fix: FDB create database failed: Action not possible before the network is configured
	fdbError = fdb_setup_network();
	if (fdbError != 0) {
		safs::log_err("FDB setup network failed: {}", fdb_get_error(fdbError));
		return -1;
	}
	fdbError = fdb_run_network();
	if (fdbError != 0) {
		safs::log_err("FDB run network failed: {}", fdb_get_error(fdbError));
		return -1;
	}
	fdbError = fdb_create_database(nullptr, &g_fdb);
	if (fdbError != 0) {
		safs::log_err("FDB create database failed: {}", fdb_get_error(fdbError));
		return -1;
	}
	return 0;
}

void changelog_fdb_append(uint64_t version, const char *entry) {
	safs::log_info("Adding FoundationDB changelog entry: {} (version {})", entry, version);
	if (!g_fdb) {
		safs::log_err("FDB is not initialized. Call changelog_fdb_init() first.");
		return;
	}

	// Key: \x00changelog/{version}
	std::string key("\x00changelog");
	key += std::to_string(version);

	fdb_error_t fdbError;

	FDBTransaction *transaction;
	fdbError = fdb_database_create_transaction(g_fdb, &transaction);
	if (fdbError != 0) {
		safs::log_err("FDB create transaction failed: {}", fdb_get_error(fdbError));
		return;
	}

	std::string value(entry);
	fdb_transaction_set(transaction, to_byte_ptr(key.data()), key.size(), to_byte_ptr(value.data()),
	                    value.size());

	FDBFuture *future = fdb_transaction_commit(transaction);
	fdbError = fdb_future_block_until_ready(future);
	if (fdbError != 0) {
		// if we return here the transaction is not destroyed
		safs::log_err("FDB block until ready failed: {}", fdb_get_error(fdbError));
	}
	fdbError = fdb_future_get_error(future);
	if (fdbError != 0) {
		// if we return here the transaction is not destroyed
		safs::log_err("FDB commit failed: {}", fdb_get_error(fdbError));
	}
	fdb_future_destroy(future);
	fdb_transaction_destroy(transaction);
}
