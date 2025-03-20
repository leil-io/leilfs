/*
   Copyright 2023 Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

// This app connects to the specified FoundationDB cluster file, inserts
// a key-value pair, retrieves it and checks if the value is present.
// It is a simple test to check if the FoundationDB client library is
// communicating correctly with the cluster.

#include <unistd.h>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#define FDB_API_VERSION 730
#include <foundationdb/fdb_c.h>
#include <foundationdb/fdb_c_types.h>

constexpr fdb_error_t kNoError = 0;

namespace {
void usage(char *progName) {
	std::cerr << "Usage: " << progName << " <cluster_file>\n";
	std::cerr << "Default path: /etc/foundationdb/fdb.cluster\n";
	exit(1);
}

void checkError(fdb_error_t errorNumber) {
	if (errorNumber != kNoError) {
		std::cerr << "FDB error: " << fdb_get_error(errorNumber) << "\n";
		exit(errorNumber);
	}
}

void runNetworkThread() { checkError(fdb_run_network()); }

void setValue(FDBDatabase *database, size_t keySize, const uint8_t *key, size_t valueSize,
              const uint8_t *value) {
	FDBTransaction *transaction{};
	checkError(fdb_database_create_transaction(database, &transaction));
	fdb_transaction_set(transaction, key, static_cast<int>(keySize), value,
	                    static_cast<int>(valueSize));
	FDBFuture *commitFuture = fdb_transaction_commit(transaction);
	checkError(fdb_future_block_until_ready(commitFuture));
	fdb_future_destroy(commitFuture);
	fdb_transaction_destroy(transaction);
}

std::vector<uint8_t> getValue(FDBDatabase *database, size_t keySize, const uint8_t *key) {
	FDBTransaction *transaction = nullptr;
	checkError(fdb_database_create_transaction(database, &transaction));
	static constexpr fdb_bool_t kUseSnapshot{0};
	FDBFuture *getValueFuture =
	    fdb_transaction_get(transaction, key, static_cast<int>(keySize), kUseSnapshot);
	checkError(fdb_future_block_until_ready(getValueFuture));

	fdb_bool_t valuePresent{};
	const uint8_t *valueRead{};
	int valueLength{};
	checkError(fdb_future_get_value(getValueFuture, &valuePresent, &valueRead, &valueLength));

	std::vector<uint8_t> result;

	if (valuePresent != 0) { result.assign(valueRead, valueRead + valueLength); }

	fdb_future_destroy(getValueFuture);
	fdb_transaction_destroy(transaction);

	return result;
}

void cleanupFDB(FDBDatabase *database, std::jthread &networkThread) {
	if (database != nullptr) {
		fdb_database_destroy(database);
	}

	if (networkThread.joinable()) {
		checkError(fdb_stop_network());
	}
}
}  // namespace

int main(int argc, char **argv) {
	if (argc > 2) { usage(argv[0]); }

	// Default path
	std::string clusterFile = "/etc/foundationdb/fdb.cluster";

	if (argc == 2) { clusterFile = std::string(argv[1]); }

	// Set up network
	checkError(fdb_select_api_version(FDB_API_VERSION));
	checkError(fdb_setup_network());

	// Run network thread
	std::jthread networkThread([] { runNetworkThread(); });

	FDBDatabase *database = nullptr;
	checkError(fdb_create_database(clusterFile.c_str(), &database));

	std::cout << "Connected to FoundationDB cluster: " << clusterFile << "\n";

	std::vector<uint8_t> key{'f', 'o', 'o'};
	std::vector<uint8_t> value{'b', 'a', 'r'};

	// Create a transaction to insert a key-value pair (foo, bar)
	setValue(database, key.size(), key.data(), value.size(), value.data());
	// Create a transaction to retrieve the value for key "foo"
	auto retrievedValue = getValue(database, key.size(), key.data());

	if (retrievedValue != value) {
		std::cerr << "Error: Retrieved value does not match expected value.\n";
		cleanupFDB(database, networkThread);
		return 1;
	}

	std::cout << "Value retrieved successfully: foo = "
	          << std::string(retrievedValue.begin(), retrievedValue.end()) << "\n";

	cleanupFDB(database, networkThread);

	return 0;
}
