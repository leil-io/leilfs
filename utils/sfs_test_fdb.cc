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

// This app connects to the specified FoundationDB cluster file and performs basic operations.
// It is a simple test to check if the FoundationDB client library is communicating correctly with
// the cluster.

#include <unistd.h>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "fdb/fdb_context.h"
#include "fdb/fdb_kv_engine.h"
#include "kv/ikv_engine.h"
#include "slogger/slogger.h"

namespace {

void usage(char *progName) {
	std::cerr << "Usage: " << progName << " <cluster_file>\n";
	std::cerr << "Default path: /etc/foundationdb/fdb.cluster\n";
	exit(1);
}

void testSetAndGet(kv::IKVEngine *kvEngine) {
	std::vector<uint8_t> key{'f', 'o', 'o'};
	std::vector<uint8_t> value{'b', 'a', 'r'};

	{
		auto transaction = kvEngine->createReadWriteTransaction();
		transaction->set(key, value);
		if (!transaction->commit()) {
			safs::log_err("Error: Failed to commit transaction for key '{}'.",
			              std::string(key.begin(), key.end()));
			return;
		}
	}

	auto transaction = kvEngine->createReadWriteTransaction();
	auto retrievedValue = transaction->get(key);

	if (!retrievedValue.has_value()) {
		safs::log_err("Error: Value for key '{}' not found.", std::string(key.begin(), key.end()));
		return;
	}

	if (retrievedValue.value() != value) {
		safs::log_err("Error: Retrieved value does not match expected value.");
		return;
	}

	safs::log_info("Value retrieved successfully: {} = {}", std::string(key.begin(), key.end()),
	               std::string(retrievedValue.value().begin(), retrievedValue.value().end()));

	// Clear the key from the database
	auto clearTransaction = kvEngine->createReadWriteTransaction();
	clearTransaction->remove(key);
	if (clearTransaction->commit()) {
		safs::log_info("Key '{}' cleared successfully.", std::string(key.begin(), key.end()));
	} else {
		safs::log_err("Error: Failed to clear key '{}'.", std::string(key.begin(), key.end()));
	}
}

void testGetRange(kv::IKVEngine *kvEngine) {
	constexpr size_t kNumKeys = 10;

	// Set up a transaction to insert multiple keys
	auto setTr = kvEngine->createReadWriteTransaction();

	for (size_t i = 0; i < kNumKeys; ++i) {
		std::string key = "key_" + std::to_string(i);
		std::string value = "value_" + std::to_string(i);
		setTr->set(kv::Key(key.begin(), key.end()), kv::Value(value.begin(), value.end()));
	}

	if (!setTr->commit()) {
		safs::log_err("Error: Failed to commit transaction for setting keys.");
		return;
	}

	// Retrieve and print the range of keys

	auto transaction = kvEngine->createReadWriteTransaction();
	std::string startKey = "key_2";
	std::string endKey = "key_9";
	kv::KeySelector startSelector(kv::Key(startKey.begin(), startKey.end()), true, 0);
	kv::KeySelector endSelector(kv::Key(endKey.begin(), endKey.end()), true, 0);

	auto rangeResult = transaction->getRange(startSelector, endSelector, kNumKeys);

	if (rangeResult.getPairs().empty()) {
		safs::log_err("Error: No keys found in range {} - {}.", startKey, endKey);
		return;
	}

	// From key_2 (inclusive) to key_9 (always exclusive in FDB)
	constexpr size_t kExpectedCount = 7;

	if (rangeResult.getPairs().size() != kExpectedCount) {
		safs::log_err("Error: Expected {} keys in range, but found {}.", kExpectedCount,
		              rangeResult.getPairs().size());
		return;
	}

	safs::log_info("Keys in range {} - {}:", startKey, endKey);
	for (const auto &pair : rangeResult.getPairs()) {
		safs::log_info("  {} = {}", std::string(pair.key.begin(), pair.key.end()),
		               std::string(pair.value.begin(), pair.value.end()));
	}
}

}  // namespace

int main(int argc, char **argv) {
	if (argc > 2) { usage(argv[0]); }

	// Initialize the logging system
	safs::setup_logs();

	// Default path
	std::string clusterFile = "/etc/foundationdb/fdb.cluster";

	if (argc == 2) { clusterFile = std::string(argv[1]); }

	safs::log_info("Starting FoundationDB test application. Using cluster file: {}", clusterFile);

	// Create the FoundationDB context, which selects the API version, initializes the network
	// thread and connects to the cluster specified by the cluster file.
	// The object must survive until the end of the program.
	auto fdbContext = fdb::FDBContext::create({clusterFile});

	if (!fdbContext) {
		safs::log_err("Failed to create FDBContext: cluster: {}", clusterFile);
		return 1;
	}

	// Obtain the connection to the FoundationDB database.
	std::shared_ptr<fdb::DB> fdbDB = fdbContext->getDB();

	if (!fdbDB) {
		safs::log_err("Failed to connect to FoundationDB cluster at {}", clusterFile);
		return 1;
	}

	// Create the key-value engine using the FoundationDB database instance.
	// This engine will be used to perform read and write operations through transactions.
	auto kvEngine = std::make_unique<fdb::FDBKVEngine>(fdbDB);

	testSetAndGet(kvEngine.get());
	testGetRange(kvEngine.get());

	return 0;
}
