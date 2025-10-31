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
#include "kv/kv_utils.h"
#include "master/kv_common_keys.h"
#include "slogger/slogger.h"
#include <string>
#include "common/datapack.h"

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
	constexpr int kNumKeys = 10;

	// Set up a transaction to insert multiple keys
	auto setTr = kvEngine->createReadWriteTransaction();

	for (int i = 0; i < kNumKeys; ++i) {
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

	// From key_2 (inclusive) to key_9 (inclusive)
	constexpr size_t kExpectedCount = 8;

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
	// Initialize the logging system
	safs::setup_logs();

	// Defaults
	std::string clusterFile = "/etc/foundationdb/fdb.cluster";
	std::vector<std::string> args;
	for (int i = 1; i < argc; ++i) { args.emplace_back(argv[i]); }

	if (!args.empty()) { clusterFile = args[0]; args.erase(args.begin()); }

	if (clusterFile.empty()) { usage(argv[0]); }

	safs::log_info("Starting FoundationDB test application. Using cluster file: {}", clusterFile);

	auto fdbContext = fdb::FDBContext::create({clusterFile});
	if (!fdbContext) {
		safs::log_err("Failed to create FDBContext: cluster: {}", clusterFile);
		return 1;
	}
	std::shared_ptr<fdb::DB> fdbDB = fdbContext->getDB();
	if (!fdbDB) {
		safs::log_err("Failed to connect to FoundationDB cluster at {}", clusterFile);
		return 1;
	}
	auto kvEngine = std::make_unique<fdb::FDBKVEngine>(fdbDB);

	// If no subcommand provided, run basic connectivity tests (backward compatible)
	if (args.empty()) {
		testSetAndGet(kvEngine.get());
		testGetRange(kvEngine.get());
		return 0;
	}

	// Subcommands
	const std::string cmd = args[0];
	if (cmd == "get") {
		if (args.size() != 2) { std::cerr << "get requires <key>\n"; return 2; }
		kv::Key key(args[1].begin(), args[1].end());
		auto tr = kvEngine->createReadWriteTransaction();
		auto val = tr->get(key);
		if (!val) { return 3; }
		std::cout << std::string(val->begin(), val->end()) << std::endl;
		return 0;
	} else if (cmd == "set") {
		if (args.size() != 3) { std::cerr << "set requires <key> <value>\n"; return 2; }
		kv::Key key(args[1].begin(), args[1].end());
		kv::Value value(args[2].begin(), args[2].end());
		auto tr = kvEngine->createReadWriteTransaction();
		tr->set(key, value);
		return tr->commit() ? 0 : 4;
	} else if (cmd == "get-meta-version") {
		auto tr = kvEngine->createReadWriteTransaction();
		auto val = tr->get(kv::toBytes(std::string(kMetaVersionKey)));
		if (!val) { return 3; }
		// FoundationDB atomic-add values are defined little-endian; read strictly LE
		uint64_t v = kv::fromBytesLE<uint64_t>(*val);
		std::cout << v << std::endl;
		return 0;
	} else if (cmd == "get-next-session") {
		auto tr = kvEngine->createReadWriteTransaction();
		auto val = tr->get(kv::toBytes(std::string(kMetaNextSessionKey)));
		if (!val) { return 3; }
		// Read strictly LE for consistency
		uint32_t s = kv::fromBytesLE<uint32_t>(*val);
		std::cout << s << std::endl;
		return 0;
	} else if (cmd == "seed-changelog") {
		// Usage: seed-changelog <version> <payload> [<version> <payload> ...]
		if (args.size() < 3 || (args.size() - 1) % 2 != 0) {
			std::cerr << "seed-changelog requires pairs of <version> <payload>\n";
			return 2;
		}
		auto tr = kvEngine->createReadWriteTransaction();
		for (size_t i = 1; i + 1 < args.size(); i += 2) {
			uint64_t ver = std::stoull(args[i]);
			std::string payload = args[i + 1];
			kv::Key key = kv::encodeKeyBE(std::string(kChangelogPrefix), ver);
			kv::Value value = kv::toBytes(payload);
			tr->set(key, value);
		}
		return tr->commit() ? 0 : 4;
	} else if (cmd == "dump-meta-version-hex") {
		auto tr = kvEngine->createReadWriteTransaction();
		auto val = tr->get(kv::toBytes(std::string(kMetaVersionKey)));
		if (!val) { return 3; }
		static const char *kHex = "0123456789ABCDEF";
		std::string hex;
		hex.reserve(val->size() * 2);
		for (auto b : *val) { hex.push_back(kHex[(b >> 4) & 0xF]); hex.push_back(kHex[b & 0xF]); }
		std::cout << hex << std::endl;
		return 0;
	} else {
		std::cerr << "Unknown command: " << cmd << "\n";
		return 2;
	}
}
