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

#include "common/platform.h"

#include "fdb/fdb.h"
#include "fdb/fdb_context.h"
#include "fdb/fdb_kv_engine.h"
#include "kv/ifuture.h"
#include "kv/itransaction.h"

#include <gtest/gtest.h>

/// Fixture for testing the FDBKVEngine.
/// Initializes the FoundationDB context and database connection before running tests.
/// Each test will use a fresh instance of FDBKVEngine.
class FDBKVEngineTest : public ::testing::Test {
protected:
	static void SetUpTestSuite() {
		try {
			fdbContext = fdb::FDBContext::create({"/etc/foundationdb/fdb.cluster"});
			ASSERT_TRUE(fdbContext != nullptr);

			fdbDB = fdbContext->getDB();
			ASSERT_TRUE(fdbDB != nullptr);
		} catch (const std::exception &e) {
			safs::log_err("Failed to set up FDB client: {}", e.what());
			FAIL() << "FDB client setup failed";
		}
	}

	static void TearDownTestSuite() {
		fdbDB.reset();
		fdbContext.reset();
	}

	void SetUp() override { kvEngine = std::make_unique<fdb::FDBKVEngine>(fdbDB); }

	// Static members to avoid re-initializing the context and database for each test
	static std::shared_ptr<fdb::FDBContext> fdbContext;
	static std::shared_ptr<fdb::DB> fdbDB;

	std::unique_ptr<fdb::FDBKVEngine> kvEngine;
};

// Static member definitions
std::shared_ptr<fdb::FDBContext> FDBKVEngineTest::fdbContext = nullptr;
std::shared_ptr<fdb::DB> FDBKVEngineTest::fdbDB = nullptr;

TEST_F(FDBKVEngineTest, SetGetAndClear) {
	std::vector<uint8_t> key{'f', 'o', 'o'};
	std::vector<uint8_t> value{'b', 'a', 'r'};

	{
		auto transaction = kvEngine->createReadWriteTransaction();
		transaction->set(key, value);
		ASSERT_TRUE(transaction->commit()) << "Failed to commit transaction for key '"
		                                   << std::string(key.begin(), key.end()) << "'.";
	}

	auto transaction = kvEngine->createReadWriteTransaction();
	auto retrievedValue = transaction->get(key);

	ASSERT_TRUE(retrievedValue.has_value())
	    << "Value for key '" << std::string(key.begin(), key.end()) << "' not found.";
	ASSERT_EQ(retrievedValue.value(), value) << "Retrieved value does not match expected value.";

	safs::log_info("Value retrieved successfully: {} = {}", std::string(key.begin(), key.end()),
	               std::string(retrievedValue.value().begin(), retrievedValue.value().end()));

	// Clear the key from the database
	auto clearTransaction = kvEngine->createReadWriteTransaction();
	clearTransaction->remove(key);
	ASSERT_TRUE(clearTransaction->commit())
	    << "Failed to clear key '" << std::string(key.begin(), key.end()) << "'.";
	safs::log_info("Key '{}' cleared successfully.", std::string(key.begin(), key.end()));
}

namespace {
void testGetRange(fdb::FDBKVEngine *kv, int start, int end, bool isStartInclusive,
                  bool isEndInclusive, size_t totalKeysInDB) {
	std::string startKey = "key_" + std::to_string(start);
	std::string endKey = "key_" + std::to_string(end);
	kv::KeySelector startSelector(kv::Key(startKey.begin(), startKey.end()), isStartInclusive, 0);
	kv::KeySelector endSelector(kv::Key(endKey.begin(), endKey.end()), isEndInclusive, 0);

	auto transaction = kv->createReadWriteTransaction();
	auto rangeResult = transaction->getRange(startSelector, endSelector, totalKeysInDB);

	size_t kExpectedCount = end - start + (isEndInclusive ? 1 : 0) - (isStartInclusive ? 0 : 1);
	ASSERT_EQ(rangeResult.getPairs().size(), kExpectedCount);

	safs::log_info("Keys in range {}{} - {}{}:", isStartInclusive ? "[" : "(", startKey, endKey,
	               isEndInclusive ? "]" : ")");

	for (const auto &pair : rangeResult.getPairs()) {
		safs::log_info("  {} = {}", std::string(pair.key.begin(), pair.key.end()),
		               std::string(pair.value.begin(), pair.value.end()));
	}
}
}  // namespace

TEST_F(FDBKVEngineTest, GetRange) {
	// Set up a transaction to insert multiple keys
	auto setTr = kvEngine->createReadWriteTransaction();

	constexpr size_t kNumKeys = 13;  // Just an arbitrary number

	for (size_t i = 0; i < kNumKeys; ++i) {
		std::string key = "key_" + std::to_string(i);
		std::string value = "value_" + std::to_string(i);
		setTr->set(kv::Key(key.begin(), key.end()), kv::Value(value.begin(), value.end()));
	}

	ASSERT_TRUE(setTr->commit());

	// Retrieve and print an arbitrary ranges of keys using different combinations of inclusivity
	// and exclusivity.

	constexpr int kStart = 2;
	constexpr int kEnd = 9;

	testGetRange(kvEngine.get(), kStart, kEnd, true, true, kNumKeys);
	testGetRange(kvEngine.get(), kStart, kEnd, true, false, kNumKeys);
	testGetRange(kvEngine.get(), kStart, kEnd, false, true, kNumKeys);
	testGetRange(kvEngine.get(), kStart, kEnd, false, false, kNumKeys);
}

TEST_F(FDBKVEngineTest, AtomicAdd) {
	kv::Key key{'c', 'o', 'u', 'n', 't'};
	constexpr int64_t initialValue = 10;
	constexpr int64_t delta = 5;

	{
		auto transaction = kvEngine->createReadWriteTransaction();
		kv::Value value = kv::toBytesLE(initialValue);
		transaction->set(key, value);
		ASSERT_TRUE(transaction->commit());
	}

	{
		auto transaction = kvEngine->createReadWriteTransaction();
		kv::Value value = kv::toBytesLE(delta);
		transaction->atomicAdd(key, value);
		ASSERT_TRUE(transaction->commit());
	}

	auto transaction = kvEngine->createReadWriteTransaction();
	auto result = transaction->get(key);

	ASSERT_TRUE(result.has_value()) << "Value for key 'count' not found.";

	ASSERT_TRUE(result.value().size() == sizeof(initialValue))
	    << "Value size mismatch for key 'count'.";

	auto finalValue = kv::fromBytesLE<int64_t>(result.value());
	ASSERT_EQ(finalValue, initialValue + delta)
	    << "Final value does not match expected value after atomicAdd.";

	safs::log_info("Atomic add successful: final value for 'count' is {}", finalValue);
}

TEST_F(FDBKVEngineTest, GetAsync) {
	std::string_view keyStr = "async_key";
	std::string_view valueStr = "async_value";
	auto key = kv::toBytes(keyStr);
	auto value = kv::toBytes(valueStr);

	// First, set the value using a synchronous transaction
	{
		auto transaction = kvEngine->createReadWriteTransaction();
		transaction->set(key, value);
		ASSERT_TRUE(transaction->commit()) << "Failed to commit transaction for async test.";
	}

	// Now retrieve it asynchronously
	auto transaction = kvEngine->createReadWriteTransaction();
	auto future = transaction->getAsync(key);

	ASSERT_TRUE(future != nullptr) << "Failed to create async future.";

	// Retrieve the value from the future
	int error = 0;
	auto retrievedValue = future->get(&error);

	ASSERT_EQ(error, 0) << "Error occurred while getting async value: " << error;
	ASSERT_TRUE(retrievedValue.has_value()) << "Value for key '" << keyStr << "' not found.";
	ASSERT_EQ(retrievedValue.value(), value)
	    << "Retrieved async value does not match expected value.";

	safs::log_info("Async value retrieved successfully: {} = {}", keyStr, valueStr);

	// Test retrieving a non-existing key
	std::string_view nonExistentKeyStr = "non_existent_key";
	auto nonExistentKey = kv::toBytes(nonExistentKeyStr);

	auto transaction2 = kvEngine->createReadWriteTransaction();
	auto futureMissing = transaction2->getAsync(nonExistentKey);

	ASSERT_TRUE(futureMissing != nullptr) << "Failed to create async future for non-existent key.";

	int error2 = 0;
	auto retrievedMissing = futureMissing->get(&error2);

	ASSERT_EQ(error2, 0) << "Error occurred while getting non-existent key: " << error2;
	ASSERT_FALSE(retrievedMissing.has_value()) << "Non-existent key should not have a value.";

	safs::log_info("Non-existent key test passed: key '{}' correctly returned no value",
	               nonExistentKeyStr);
}
