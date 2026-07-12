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

#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "fdb/fdb.h"
#include "fdb/fdb_context.h"
#include "fdb/fdb_kv_engine.h"
#include "fdb/fdb_transaction.h"
#include "kv/ifuture.h"
#include "kv/itransaction.h"
#include "kv/kv_utils.h"

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

TEST_F(FDBKVEngineTest, GetRangeWithOffsets) {
	// Set up a transaction to insert multiple keys
	auto setTr = kvEngine->createReadWriteTransaction();

	constexpr size_t kNumKeys = 20;  // Just an arbitrary number

	for (size_t i = 0; i < kNumKeys; ++i) {
		std::string key = "key_" + std::to_string(i);
		std::string value = "value_" + std::to_string(i);
		setTr->set(kv::toBytes(key), kv::toBytes(value));
	}

	ASSERT_TRUE(setTr->commit());

	// Retrieve and print arbitrary ranges of keys using different offsets
	constexpr size_t kPageSize = 5;

	std::vector<std::string> resultsWithoutOffsets;
	std::vector<std::string> resultsWithOffsets;

	{
		kv::KeySelector startSelector(kv::toBytes("key_"), true, 0);
		kv::KeySelector endSelector(kv::toBytes("key_" + std::string(1, '\xff')), true, 0);

		auto transaction = kvEngine->createReadWriteTransaction();
		auto rangeResult = transaction->getRange(startSelector, endSelector, kNumKeys);

		auto pairs = rangeResult.getPairs();

		for (const auto &[key, value] : pairs) {
			resultsWithoutOffsets.emplace_back(key.begin(), key.end());
		}
	}

	std::string startKey = "key_0";
	std::string endKey = "key_" + std::string(1, '\xff');

	for (size_t offset = 0; offset < kNumKeys; offset += kPageSize) {
		safs::log_info("Offset: {}", offset);
		kv::KeySelector startSelector(kv::toBytes(startKey), true, static_cast<int>(offset));
		kv::KeySelector endSelector(kv::toBytes(endKey), true, 0);

		auto transaction = kvEngine->createReadWriteTransaction();
		auto rangeResult = transaction->getRange(startSelector, endSelector, kPageSize);

		auto pairs = rangeResult.getPairs();

		for (const auto &[key, value] : pairs) {
			safs::log_info("  {} = {}", std::string(key.begin(), key.end()),
			               std::string(value.begin(), value.end()));
			resultsWithOffsets.emplace_back(key.begin(), key.end());
		}
	}

	ASSERT_EQ(resultsWithoutOffsets.size(), resultsWithOffsets.size());

	for (size_t i = 0; i < resultsWithoutOffsets.size(); ++i) {
		ASSERT_EQ(resultsWithoutOffsets[i], resultsWithOffsets[i]);
	}

	std::vector<std::string> elementsWithOffsets;
	safs::log_info("Elements in [key_0 + kPageSize, key_17] for kPageSize = {}:", kPageSize);

	{
		// There are lexicographically 10 elements from key_0 to key_17:
		// {key_0, key_1, key_10, key_11, key_12, key_13, key_14, key_15, key_16, key_17}
		// With an offset of kPageSize=5, the range should contain the elements [key_13..key_17].
		kv::KeySelector startSelector(kv::toBytes("key_0"), true, kPageSize);
		kv::KeySelector endSelector(kv::toBytes("key_17"), true, 0);

		auto transaction = kvEngine->createReadWriteTransaction();
		auto rangeResult = transaction->getRange(startSelector, endSelector);

		auto pairs = rangeResult.getPairs();

		for (const auto &[key, value] : pairs) {
			safs::log_info("  {} = {}", std::string(key.begin(), key.end()),
			               std::string(value.begin(), value.end()));
			elementsWithOffsets.emplace_back(key.begin(), key.end());
		}
	}

	constexpr size_t kExpectedNumberOfElements = 5;
	ASSERT_EQ(elementsWithOffsets.size(), kExpectedNumberOfElements);
	ASSERT_EQ(elementsWithOffsets.front(), "key_13");  // Lexicographical order
	ASSERT_EQ(elementsWithOffsets.back(), "key_17");
}

TEST_F(FDBKVEngineTest, GetRangeAsync) {
	const auto prefix = kv::toBytes("async_range_key_");
	const auto end = kv::prefixEnd(prefix);
	kv::KeySelector startSelector(prefix, true, 0);
	kv::KeySelector endSelector(end, true, 0);

	constexpr size_t kNumKeys = 6;

	{
		auto transaction = kvEngine->createReadWriteTransaction();
		transaction->removeRange(prefix, end);
		ASSERT_TRUE(transaction->commit()) << "Failed to clear async range test keys.";
	}

	{
		auto transaction = kvEngine->createReadWriteTransaction();
		for (size_t i = 0; i < kNumKeys; ++i) {
			std::string key = "async_range_key_" + std::to_string(i);
			std::string value = "async_range_value_" + std::to_string(i);
			transaction->set(kv::toBytes(key), kv::toBytes(value));
		}
		ASSERT_TRUE(transaction->commit()) << "Failed to seed async range test keys.";
	}

	{
		auto transaction = kvEngine->createReadWriteTransaction();
		auto syncResult = transaction->getRange(startSelector, endSelector, kNumKeys);
		auto future = transaction->getRangeAsync(startSelector, endSelector, kNumKeys);
		ASSERT_TRUE(future != nullptr) << "Failed to create async range future.";

		int error = -1;
		auto asyncResult = future->get(&error);
		ASSERT_EQ(error, 0);
		ASSERT_EQ(asyncResult.getPairs().size(), syncResult.getPairs().size());
		ASSERT_EQ(asyncResult.hasMore(), syncResult.hasMore());
		for (size_t i = 0; i < syncResult.getPairs().size(); ++i) {
			ASSERT_EQ(asyncResult.getPairs()[i].key, syncResult.getPairs()[i].key);
			ASSERT_EQ(asyncResult.getPairs()[i].value, syncResult.getPairs()[i].value);
		}
	}

	{
		auto transaction = kvEngine->createReadWriteTransaction();
		auto future = transaction->getRangeAsync(startSelector, endSelector, 2);
		ASSERT_TRUE(future != nullptr) << "Failed to create paginated async range future.";

		int error = -1;
		auto rangeResult = future->get(&error);
		ASSERT_EQ(error, 0);
		ASSERT_EQ(rangeResult.getPairs().size(), 2U);
		ASSERT_TRUE(rangeResult.hasMore());

		// The future is single-use: a second get() is a wrong-state call.
		EXPECT_THROW(future->get(), std::logic_error);
	}

	{
		auto transaction = kvEngine->createReadWriteTransaction();
		kv::KeySelector secondPageStart(kv::toBytes("async_range_key_2"), true, 0);

		auto firstFuture = transaction->getRangeAsync(startSelector, endSelector, 2);
		auto secondFuture = transaction->getRangeAsync(secondPageStart, endSelector, 2);
		ASSERT_TRUE(firstFuture != nullptr && secondFuture != nullptr)
		    << "Failed to create multiple async range futures.";

		int firstError = -1;
		int secondError = -1;
		auto firstResult = firstFuture->get(&firstError);
		auto secondResult = secondFuture->get(&secondError);

		ASSERT_EQ(firstError, 0);
		ASSERT_EQ(secondError, 0);
		ASSERT_EQ(firstResult.getPairs().size(), 2U);
		ASSERT_EQ(secondResult.getPairs().size(), 2U);
		ASSERT_EQ(firstResult.getPairs().front().key, kv::toBytes("async_range_key_0"));
		ASSERT_EQ(secondResult.getPairs().front().key, kv::toBytes("async_range_key_2"));
	}

	{
		auto transaction = kvEngine->createReadWriteTransaction();
		auto missingPrefix = kv::toBytes("async_range_missing_");
		auto future =
		    transaction->getRangeAsync(kv::KeySelector(missingPrefix, true, 0),
		                               kv::KeySelector(kv::prefixEnd(missingPrefix), true, 0));
		ASSERT_TRUE(future != nullptr) << "Failed to create empty async range future.";

		int error = -1;
		auto rangeResult = future->get(&error);
		ASSERT_EQ(error, 0);
		ASSERT_TRUE(rangeResult.getPairs().empty());
		ASSERT_FALSE(rangeResult.hasMore());
	}
}

TEST_F(FDBKVEngineTest, RemoveRange) {
	constexpr size_t kNumKeys = 10;

	{  // Initially insert keys key_0, key_1, ..., key_9 into the database
		auto transaction = kvEngine->createReadWriteTransaction();
		for (size_t i = 0; i < kNumKeys; ++i) {
			std::string key = "key_" + std::to_string(i);
			std::string value = "value_" + std::to_string(i);
			transaction->set(kv::toBytes(key), kv::toBytes(value));
		}
		ASSERT_TRUE(transaction->commit());
	}

	{  // Remove the range [key_3, key_7), i.e. keys key_3, key_4, key_5 and key_6.
		auto transaction = kvEngine->createReadWriteTransaction();
		transaction->removeRange(kv::toBytes("key_3"), kv::toBytes("key_7"));
		ASSERT_TRUE(transaction->commit());
	}

	{
		auto transaction = kvEngine->createReadWriteTransaction();
		for (size_t i = 0; i < kNumKeys; ++i) {
			std::string key = "key_" + std::to_string(i);
			auto value = transaction->get(kv::toBytes(key));
			// Range of keys [key_3, key_7) should be removed, but the others should still be
			// present.
			if (i >= 3 && i < 7) {
				ASSERT_FALSE(value.has_value()) << "Expected key missing: " << key;
			} else {
				ASSERT_TRUE(value.has_value()) << "Expected key present: " << key;
			}
		}
	}

	{  // Remove the range [key_0, key_2), i.e. keys key_0 and key_1.
		auto transaction = kvEngine->createReadWriteTransaction();
		transaction->removeRange(kv::toBytes("key_0"), kv::toBytes("key_2"));
		ASSERT_TRUE(transaction->commit());
	}

	{  // Verify that key_0 and key_1 are removed, but key_2 is still present
		auto transaction = kvEngine->createReadWriteTransaction();
		ASSERT_FALSE(transaction->get(kv::toBytes("key_0")).has_value());
		ASSERT_FALSE(transaction->get(kv::toBytes("key_1")).has_value());
		ASSERT_TRUE(transaction->get(kv::toBytes("key_2")).has_value());
	}

	{  // Remove range with a single key: start key and end key are the same, but the range is
	   // end-exclusive, so key_2 should not be removed.
		auto transaction = kvEngine->createReadWriteTransaction();
		transaction->removeRange(kv::toBytes("key_2"), kv::toBytes("key_2"));
		ASSERT_TRUE(transaction->commit());
	}

	{  // Verify that key_2 is still present after calling removeRange() with a single key
		auto transaction = kvEngine->createReadWriteTransaction();
		ASSERT_TRUE(transaction->get(kv::toBytes("key_2")).has_value());
	}

	{  // Remove key_2 by calling removeRange() with a range that includes key_2
		auto transaction = kvEngine->createReadWriteTransaction();
		transaction->removeRange(kv::toBytes("key_2"), kv::toBytes("key_3"));
		ASSERT_TRUE(transaction->commit());
	}

	{  // Verify that key_2 is removed
		auto transaction = kvEngine->createReadWriteTransaction();
		ASSERT_FALSE(transaction->get(kv::toBytes("key_2")).has_value());
	}
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

TEST_F(FDBKVEngineTest, GetSnapshot) {
	auto key = kv::toBytes("snapshot_key");
	constexpr uint64_t initialValue = 42;

	// Write the initial value.
	{
		auto transaction = kvEngine->createReadWriteTransaction();
		transaction->set(key, kv::toBytesLE(initialValue));
		ASSERT_TRUE(transaction->commit()) << "Failed to commit initial value for snapshot test.";
	}

	// getSnapshot returns the correct value when the key exists.
	{
		auto transaction = kvEngine->createReadWriteTransaction();
		auto result = transaction->getSnapshot(key);
		ASSERT_TRUE(result.has_value()) << "getSnapshot should return a value for an existing key.";
		ASSERT_EQ(kv::fromBytesLE<uint64_t>(*result), initialValue)
		    << "getSnapshot returned an unexpected value.";
	}

	// getSnapshot returns nullopt for a non-existent key.
	{
		auto transaction = kvEngine->createReadWriteTransaction();
		auto result = transaction->getSnapshot(kv::toBytes("snapshot_key_missing"));
		ASSERT_FALSE(result.has_value())
		    << "getSnapshot should return nullopt for a non-existent key.";
	}

	// getSnapshot does not add the key to the read conflict range:
	// a transaction that snapshot-reads a key can still commit even if
	// another transaction modifies that key concurrently.
	{
		constexpr uint64_t kSideValue = 123U;
		const auto sideKey = kv::toBytes("snapshot_side_key");

		// Txn A: snapshot-read key and write to a different key.
		auto txnA = kvEngine->createReadWriteTransaction();
		auto snapResult = txnA->getSnapshot(key);
		ASSERT_TRUE(snapResult.has_value())
		    << "Snapshot read should return a value for existing key.";
		txnA->set(sideKey, kv::toBytesLE(kSideValue));

		// Txn B: concurrently modify key and commit.
		auto txnB = kvEngine->createReadWriteTransaction();
		txnB->atomicAdd(key, kv::toBytesLE(uint64_t{1}));
		ASSERT_TRUE(txnB->commit()) << "Concurrent writer (Txn B) should commit successfully.";

		// Txn A should still commit: snapshot read on key does not register
		// a read conflict, so Txn B's write does not cause a conflict.
		ASSERT_TRUE(txnA->commit())
		    << "Transaction with only a snapshot read on key should commit "
		       "despite a concurrent write to that key.";

		// Verify sideKey was written by Txn A.
		auto verifyTxn = kvEngine->createReadWriteTransaction();
		auto sideVal = verifyTxn->get(sideKey);
		ASSERT_TRUE(sideVal.has_value()) << "Side key written by Txn A should exist.";
		ASSERT_EQ(kv::fromBytesLE<uint64_t>(*sideVal), kSideValue)
		    << "Side key should contain the value written by Txn A.";
	}

	// By contrast, a regular get() participates in conflict detection and
	// causes the transaction to fail if another transaction modifies the
	// read key first.
	{
		constexpr uint64_t kSideValue = 456U;
		const auto sideKey = kv::toBytes("snapshot_side_key_conflict");

		// Txn A: regular read of key and write to a different key.
		auto txnA = kvEngine->createReadWriteTransaction();
		auto readResult = txnA->get(key);
		ASSERT_TRUE(readResult.has_value())
		    << "Regular read should return a value for existing key.";
		txnA->set(sideKey, kv::toBytesLE(kSideValue));

		// Txn B: modify key and commit.
		auto txnB = kvEngine->createReadWriteTransaction();
		txnB->atomicAdd(key, kv::toBytesLE(uint64_t{1}));
		ASSERT_TRUE(txnB->commit()) << "Concurrent writer (Txn B) should commit successfully.";

		// Txn A should fail: the regular read added key to the read conflict
		// range, and Txn B's write causes a conflict.
		ASSERT_FALSE(txnA->commit())
		    << "Transaction with a regular read on key should fail to commit "
		       "when another transaction modifies that key first.";
	}

	// Cleanup.
	{
		auto transaction = kvEngine->createReadWriteTransaction();
		transaction->remove(key);
		transaction->remove(kv::toBytes("snapshot_side_key"));
		transaction->remove(kv::toBytes("snapshot_side_key_conflict"));
		ASSERT_TRUE(transaction->commit()) << "Failed to clean up snapshot test keys.";
	}
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

TEST_F(FDBKVEngineTest, GetSnapshotAsync) {
	auto key = kv::toBytes("snapshot_async_key");
	constexpr uint64_t initialValue = 42;

	// Write the initial value.
	{
		auto transaction = kvEngine->createReadWriteTransaction();
		transaction->set(key, kv::toBytesLE(initialValue));
		ASSERT_TRUE(transaction->commit())
		    << "Failed to commit initial value for snapshot async test.";
	}

	// getSnapshotAsync returns the correct value when the key exists.
	{
		auto transaction = kvEngine->createReadWriteTransaction();
		auto future = transaction->getSnapshotAsync(key);
		ASSERT_TRUE(future != nullptr) << "Failed to create snapshot async future.";

		int error = 0;
		auto result = future->get(&error);
		ASSERT_EQ(error, 0) << "Error occurred while getting snapshot async value.";
		ASSERT_TRUE(result.has_value())
		    << "getSnapshotAsync should return a value for an existing key.";
		ASSERT_EQ(kv::fromBytesLE<uint64_t>(*result), initialValue)
		    << "getSnapshotAsync returned an unexpected value.";
	}

	// getSnapshotAsync returns nullopt for a non-existent key.
	{
		auto transaction = kvEngine->createReadWriteTransaction();
		auto future = transaction->getSnapshotAsync(kv::toBytes("snapshot_async_key_missing"));
		ASSERT_TRUE(future != nullptr) << "Failed to create snapshot async future.";

		int error = 0;
		auto result = future->get(&error);
		ASSERT_EQ(error, 0) << "Error occurred while getting non-existent snapshot async value.";
		ASSERT_FALSE(result.has_value())
		    << "getSnapshotAsync should return nullopt for a non-existent key.";
	}

	// getSnapshotAsync does not add the key to the read conflict range:
	// a transaction that snapshot-reads a key can still commit even if
	// another transaction modifies that key concurrently.
	{
		constexpr uint64_t kSideValue = 123U;
		const auto sideKey = kv::toBytes("snapshot_async_side_key");

		// Txn A: snapshot-read key asynchronously and write to a different key.
		auto txnA = kvEngine->createReadWriteTransaction();
		auto future = txnA->getSnapshotAsync(key);
		ASSERT_TRUE(future != nullptr) << "Failed to create snapshot async future.";

		int error = 0;
		auto snapResult = future->get(&error);
		ASSERT_EQ(error, 0) << "Error occurred while getting snapshot async value.";
		ASSERT_TRUE(snapResult.has_value())
		    << "Snapshot async read should return a value for existing key.";
		txnA->set(sideKey, kv::toBytesLE(kSideValue));

		// Txn B: concurrently modify key and commit.
		auto txnB = kvEngine->createReadWriteTransaction();
		txnB->atomicAdd(key, kv::toBytesLE(uint64_t{1}));
		ASSERT_TRUE(txnB->commit()) << "Concurrent writer (Txn B) should commit successfully.";

		// Txn A should still commit: the snapshot read on key does not register
		// a read conflict, so Txn B's write does not cause a conflict.
		ASSERT_TRUE(txnA->commit())
		    << "Transaction with only a snapshot async read on key should commit "
		       "despite a concurrent write to that key.";

		// Verify sideKey was written by Txn A.
		auto verifyTxn = kvEngine->createReadWriteTransaction();
		auto sideVal = verifyTxn->get(sideKey);
		ASSERT_TRUE(sideVal.has_value()) << "Side key written by Txn A should exist.";
		ASSERT_EQ(kv::fromBytesLE<uint64_t>(*sideVal), kSideValue)
		    << "Side key should contain the value written by Txn A.";
	}

	// Cleanup.
	{
		auto transaction = kvEngine->createReadWriteTransaction();
		transaction->remove(key);
		transaction->remove(kv::toBytes("snapshot_async_side_key"));
		ASSERT_TRUE(transaction->commit()) << "Failed to clean up snapshot async test keys.";
	}
}

TEST_F(FDBKVEngineTest, AddReadConflictKey) {
	auto key = kv::toBytes("read_conflict_key");
	constexpr uint64_t initialValue = 42;

	// Write the initial value.
	{
		auto transaction = kvEngine->createReadWriteTransaction();
		transaction->set(key, kv::toBytesLE(initialValue));
		ASSERT_TRUE(transaction->commit())
		    << "Failed to commit initial value for read conflict test.";
	}

	// Control: a snapshot read of key followed by a blind write elsewhere commits
	// despite a concurrent write to key -- nothing conflicts on its own.
	{
		constexpr uint64_t kSideValue = 123U;
		const auto sideKey = kv::toBytes("read_conflict_control_side_key");

		auto txnA = kvEngine->createReadWriteTransaction();
		auto snapResult = txnA->getSnapshot(key);
		ASSERT_TRUE(snapResult.has_value())
		    << "Snapshot read should return a value for existing key.";
		txnA->set(sideKey, kv::toBytesLE(kSideValue));

		auto txnB = kvEngine->createReadWriteTransaction();
		txnB->atomicAdd(key, kv::toBytesLE(uint64_t{1}));
		ASSERT_TRUE(txnB->commit()) << "Concurrent writer (Txn B) should commit successfully.";

		ASSERT_TRUE(txnA->commit())
		    << "Control transaction without addReadConflictKey should commit.";
	}

	// addReadConflictKey turns the same shape into a conflict-checked one:
	// Txn A never reads key with conflict tracking, but declaring it as a read
	// conflict makes Txn B's concurrent write abort Txn A's commit.
	{
		constexpr uint64_t kSideValue = 456U;
		const auto sideKey = kv::toBytes("read_conflict_side_key");

		// Txn A: pin the read version with a snapshot read, declare the read
		// conflict on key without reading it, and write to a different key.
		auto txnA = kvEngine->createReadWriteTransaction();
		auto snapResult = txnA->getSnapshot(key);
		ASSERT_TRUE(snapResult.has_value())
		    << "Snapshot read should return a value for existing key.";
		txnA->addReadConflictKey(key);
		txnA->set(sideKey, kv::toBytesLE(kSideValue));

		// Txn B: concurrently modify key and commit.
		auto txnB = kvEngine->createReadWriteTransaction();
		txnB->atomicAdd(key, kv::toBytesLE(uint64_t{1}));
		ASSERT_TRUE(txnB->commit()) << "Concurrent writer (Txn B) should commit successfully.";

		// Txn A should fail: key is in its read conflict set and Txn B wrote it.
		ASSERT_FALSE(txnA->commit())
		    << "Transaction with addReadConflictKey on key should fail to commit "
		       "when another transaction modifies that key first.";

		// Verify sideKey was not written by the aborted Txn A.
		auto verifyTxn = kvEngine->createReadWriteTransaction();
		ASSERT_FALSE(verifyTxn->get(sideKey).has_value())
		    << "Side key from the aborted transaction should not exist.";
	}

	// Cleanup.
	{
		auto transaction = kvEngine->createReadWriteTransaction();
		transaction->remove(key);
		transaction->remove(kv::toBytes("read_conflict_control_side_key"));
		ASSERT_TRUE(transaction->commit()) << "Failed to clean up read conflict test keys.";
	}
}

/// Tests extractIntegralLEFromFuture with various integral types and scenarios.
TEST_F(FDBKVEngineTest, ExtractIntegralLEFromFuture) {
	// Test with int64_t
	{
		std::string_view keyStr = "integral_key_int64";
		auto key = kv::toBytes(keyStr);
		constexpr int64_t expectedValue = 42;

		auto txn = kvEngine->createReadWriteTransaction();
		txn->set(key, kv::toBytesLE(expectedValue));
		ASSERT_TRUE(txn->commit()) << "Failed to commit transaction for int64_t test.";

		auto txn2 = kvEngine->createReadOnlyTransaction();
		auto future = txn2->getAsync(key);
		ASSERT_TRUE(future != nullptr) << "Failed to create async future for int64_t test.";

		auto extractedValue = kv::extractIntegralLEFromFuture<int64_t>(future, keyStr);
		ASSERT_EQ(extractedValue, expectedValue) << "Extracted int64_t value does not match.";
	}

	// Test with uint64_t
	{
		std::string_view keyStr = "integral_key_uint64";
		auto key = kv::toBytes(keyStr);
		constexpr uint64_t expectedValue = 0xDEADBEEFCAFEBABE;

		auto txn = kvEngine->createReadWriteTransaction();
		txn->set(key, kv::toBytesLE(expectedValue));
		ASSERT_TRUE(txn->commit()) << "Failed to commit transaction for uint64_t test.";

		auto txn2 = kvEngine->createReadOnlyTransaction();
		auto future = txn2->getAsync(key);
		ASSERT_TRUE(future != nullptr) << "Failed to create async future for uint64_t test.";

		auto extractedValue = kv::extractIntegralLEFromFuture<uint64_t>(future, keyStr);
		ASSERT_EQ(extractedValue, expectedValue) << "Extracted uint64_t value does not match.";
	}

	// Test with int32_t
	{
		std::string_view keyStr = "integral_key_int32";
		auto key = kv::toBytes(keyStr);
		constexpr int32_t expectedValue = -12345;

		auto txn = kvEngine->createReadWriteTransaction();
		txn->set(key, kv::toBytesLE(expectedValue));
		ASSERT_TRUE(txn->commit()) << "Failed to commit transaction for int32_t test.";

		auto txn2 = kvEngine->createReadOnlyTransaction();
		auto future = txn2->getAsync(key);
		ASSERT_TRUE(future != nullptr) << "Failed to create async future for int32_t test.";

		auto extractedValue = kv::extractIntegralLEFromFuture<int32_t>(future, keyStr);
		ASSERT_EQ(extractedValue, expectedValue) << "Extracted int32_t value does not match.";
	}

	// Test parallel extraction of multiple values
	{
		constexpr int64_t value1 = 100;
		constexpr int64_t value2 = 200;
		constexpr int64_t value3 = 300;

		auto key1 = kv::toBytes("parallel_key_1");
		auto key2 = kv::toBytes("parallel_key_2");
		auto key3 = kv::toBytes("parallel_key_3");

		auto txn = kvEngine->createReadWriteTransaction();
		txn->set(key1, kv::toBytesLE(value1));
		txn->set(key2, kv::toBytesLE(value2));
		txn->set(key3, kv::toBytesLE(value3));
		ASSERT_TRUE(txn->commit()) << "Failed to commit transaction for parallel test.";

		// Issue all async reads in parallel
		auto txn2 = kvEngine->createReadOnlyTransaction();
		auto future1 = txn2->getAsync(key1);
		auto future2 = txn2->getAsync(key2);
		auto future3 = txn2->getAsync(key3);

		ASSERT_TRUE(future1 != nullptr && future2 != nullptr && future3 != nullptr)
		    << "Failed to create async futures for parallel test.";

		// Extract all values
		auto extracted1 = kv::extractIntegralLEFromFuture<int64_t>(future1, "parallel_key_1");
		auto extracted2 = kv::extractIntegralLEFromFuture<int64_t>(future2, "parallel_key_2");
		auto extracted3 = kv::extractIntegralLEFromFuture<int64_t>(future3, "parallel_key_3");

		ASSERT_EQ(extracted1, value1) << "Parallel extraction 1 failed.";
		ASSERT_EQ(extracted2, value2) << "Parallel extraction 2 failed.";
		ASSERT_EQ(extracted3, value3) << "Parallel extraction 3 failed.";
	}

	// Test error case: missing key should throw exception
	{
		std::string_view nonExistentKeyStr = "non_existent_integral_key";
		auto nonExistentKey = kv::toBytes(nonExistentKeyStr);

		auto txn = kvEngine->createReadOnlyTransaction();
		auto future = txn->getAsync(nonExistentKey);
		ASSERT_TRUE(future != nullptr) << "Failed to create async future for missing key test.";

		EXPECT_THROW(
		    { kv::extractIntegralLEFromFuture<int64_t>(future, nonExistentKeyStr); },
		    std::runtime_error)
		    << "Expected exception for missing key was not thrown.";
	}

	safs::log_info("All extractIntegralLEFromFuture tests passed successfully.");
}

// --- Op-counter profiling gating -------------------------------------------

namespace {
/// Restores the process-wide op-counter gate on scope exit, so a failed
/// assertion cannot leak the enabled state into other tests in the suite.
struct OpCounterGateGuard {
	bool previous;
	OpCounterGateGuard() : previous(fdb::opCountersEnabled()) {}
	~OpCounterGateGuard() { fdb::setOpCountersEnabled(previous); }
};
}  // namespace

// The gate flag is process-global and needs no FDB connection:
// setOpCountersEnabled() must be observable through opCountersEnabled().
TEST(FdbOpCountersGatingTest, FlagReflectsSetter) {
	const OpCounterGateGuard guard;

	fdb::setOpCountersEnabled(true);
	EXPECT_TRUE(fdb::opCountersEnabled());

	fdb::setOpCountersEnabled(false);
	EXPECT_FALSE(fdb::opCountersEnabled());
}

// Locks the error-predicate semantics the commit-retry decision depends on. The retry
// paths (matoclserv_commit_op_with_retry, the deferred-commit poller, the group-commit
// batch poller) replay an op ONLY when FDBCommitFuture::getResult reports the commit
// retryable. That flag MUST come from RETRYABLE_NOT_COMMITTED, never the broad RETRYABLE
// predicate: RETRYABLE also covers commit_unknown_result / the maybe-committed class,
// where the commit may already have applied, so a blind replay would double-apply the op
// (a second inode/chunk, a double atomicAdd). This guards against a regression back to the
// broad predicate. Runs under the fixture so the FDB API version is selected (the predicate
// is a local libfdb_c call and needs no cluster).
TEST_F(FDBKVEngineTest, CommitErrorRetryablePredicate) {
	// FDB error codes (fdbclient/error_definitions.h).
	constexpr fdb_error_t kNotCommitted = 1020;          // definitely did not commit
	constexpr fdb_error_t kTransactionTooOld = 1007;     // definitely did not commit
	constexpr fdb_error_t kCommitUnknownResult = 1021;   // MAY have committed
	constexpr fdb_error_t kTransactionTimedOut = 1031;   // unknown result during commit

	// The predicate the commit path uses: only definitely-not-committed conflicts are
	// safe to blindly replay.
	EXPECT_TRUE(fdb::DB::evaluatePredicate(FDB_ERROR_PREDICATE_RETRYABLE_NOT_COMMITTED,
	                                       kNotCommitted));
	EXPECT_TRUE(fdb::DB::evaluatePredicate(FDB_ERROR_PREDICATE_RETRYABLE_NOT_COMMITTED,
	                                       kTransactionTooOld));
	EXPECT_FALSE(fdb::DB::evaluatePredicate(FDB_ERROR_PREDICATE_RETRYABLE_NOT_COMMITTED,
	                                        kCommitUnknownResult));
	EXPECT_FALSE(fdb::DB::evaluatePredicate(FDB_ERROR_PREDICATE_RETRYABLE_NOT_COMMITTED,
	                                        kTransactionTimedOut));

	// Why the broad predicate is wrong here: it would flag commit_unknown_result retryable.
	EXPECT_TRUE(fdb::DB::evaluatePredicate(FDB_ERROR_PREDICATE_RETRYABLE, kCommitUnknownResult));
	EXPECT_TRUE(fdb::DB::evaluatePredicate(FDB_ERROR_PREDICATE_MAYBE_COMMITTED,
	                                       kCommitUnknownResult));
	// not_committed is never in the maybe-committed class.
	EXPECT_FALSE(fdb::DB::evaluatePredicate(FDB_ERROR_PREDICATE_MAYBE_COMMITTED, kNotCommitted));
}

// Counters must move only while accounting is enabled. Drives real FDB ops and
// compares getOpCounters() deltas with the gate off versus on.
TEST_F(FDBKVEngineTest, OpCountersGatedByFlag) {
	const OpCounterGateGuard guard;
	const auto key = kv::toBytes("opcount_gate_key");

	// Gate off: a full set+commit+get cycle must not move any counter.
	fdb::setOpCountersEnabled(false);
	const fdb::FdbOpCounters before = fdb::getOpCounters();
	{
		auto txn = kvEngine->createReadWriteTransaction();
		txn->set(key, kv::toBytesLE(int64_t{1}));
		ASSERT_TRUE(txn->commit());
	}
	{
		auto txn = kvEngine->createReadWriteTransaction();
		(void)txn->get(key);
	}
	const fdb::FdbOpCounters disabled = fdb::getOpCounters();
	EXPECT_EQ(disabled.sets, before.sets);
	EXPECT_EQ(disabled.commits, before.commits);
	EXPECT_EQ(disabled.pointReads, before.pointReads);

	// Gate on: the same shaped work must bump the matching counters.
	fdb::setOpCountersEnabled(true);
	const fdb::FdbOpCounters gateOn = fdb::getOpCounters();
	{
		auto txn = kvEngine->createReadWriteTransaction();
		txn->set(key, kv::toBytesLE(int64_t{2}));
		ASSERT_TRUE(txn->commit());
	}
	{
		auto txn = kvEngine->createReadWriteTransaction();
		ASSERT_TRUE(txn->get(key).has_value());
	}
	const fdb::FdbOpCounters after = fdb::getOpCounters();
	// set is buffered (one call, no retry) so its delta is exact; reads and
	// commits incur a round-trip that may retry, so require at least one.
	EXPECT_EQ(after.sets - gateOn.sets, 1U);
	EXPECT_GE(after.commits - gateOn.commits, 1U);
	EXPECT_GE(after.pointReads - gateOn.pointReads, 1U);

	// Clean up the key; the gate is restored by OpCounterGateGuard on scope exit.
	{
		auto txn = kvEngine->createReadWriteTransaction();
		txn->remove(key);
		ASSERT_TRUE(txn->commit());
	}
}

// atomicMax persists the numeric max of the stored and candidate values, treats an
// absent key as zero, and is conflict-free (concurrent writers do not abort each
// other). Values are fixed-width little-endian, matching the FDB MAX mutation.
TEST_F(FDBKVEngineTest, AtomicMax) {
	const auto key = kv::toBytes("atomic_max_key");
	const auto absentKey = kv::toBytes("atomic_max_absent_key");

	{  // Start from a clean slate.
		auto txn = kvEngine->createReadWriteTransaction();
		txn->remove(key);
		txn->remove(absentKey);
		ASSERT_TRUE(txn->commit());
	}

	{  // Seed an initial value.
		auto txn = kvEngine->createReadWriteTransaction();
		txn->set(key, kv::toBytesLE(uint64_t{100}));
		ASSERT_TRUE(txn->commit());
	}

	{  // A smaller candidate must not lower the stored value.
		auto txn = kvEngine->createReadWriteTransaction();
		txn->atomicMax(key, kv::toBytesLE(uint64_t{50}));
		ASSERT_TRUE(txn->commit());
	}
	{
		auto txn = kvEngine->createReadWriteTransaction();
		auto v = txn->get(key);
		ASSERT_TRUE(v.has_value());
		EXPECT_EQ(kv::fromBytesLE<uint64_t>(*v), uint64_t{100});
	}

	{  // A larger candidate must raise it.
		auto txn = kvEngine->createReadWriteTransaction();
		txn->atomicMax(key, kv::toBytesLE(uint64_t{200}));
		ASSERT_TRUE(txn->commit());
	}
	{
		auto txn = kvEngine->createReadWriteTransaction();
		auto v = txn->get(key);
		ASSERT_TRUE(v.has_value());
		EXPECT_EQ(kv::fromBytesLE<uint64_t>(*v), uint64_t{200});
	}

	// Numeric, not lexicographic: 256 (LE bytes 00 01 ..) must beat 255 (LE bytes
	// FF 00 ..) even though 255's first byte is larger. This is what separates FDB
	// MAX (little-endian integer) from BYTE_MAX (byte-string/lexicographic).
	{
		auto txn = kvEngine->createReadWriteTransaction();
		txn->set(key, kv::toBytesLE(uint64_t{255}));
		ASSERT_TRUE(txn->commit());
	}
	{
		auto txn = kvEngine->createReadWriteTransaction();
		txn->atomicMax(key, kv::toBytesLE(uint64_t{256}));
		ASSERT_TRUE(txn->commit());
	}
	{
		auto txn = kvEngine->createReadWriteTransaction();
		auto v = txn->get(key);
		ASSERT_TRUE(v.has_value());
		EXPECT_EQ(kv::fromBytesLE<uint64_t>(*v), uint64_t{256});
	}

	{  // An absent key counts as zero, so any positive candidate is stored.
		auto txn = kvEngine->createReadWriteTransaction();
		txn->atomicMax(absentKey, kv::toBytesLE(uint64_t{7}));
		ASSERT_TRUE(txn->commit());
	}
	{
		auto txn = kvEngine->createReadWriteTransaction();
		auto v = txn->get(absentKey);
		ASSERT_TRUE(v.has_value());
		EXPECT_EQ(kv::fromBytesLE<uint64_t>(*v), uint64_t{7});
	}

	// Conflict-free: two transactions that both atomicMax the same key both commit
	// (atomicMax adds no read conflict range), and the result is the max of the two.
	{
		auto txnA = kvEngine->createReadWriteTransaction();
		txnA->atomicMax(key, kv::toBytesLE(uint64_t{1000}));
		auto txnB = kvEngine->createReadWriteTransaction();
		txnB->atomicMax(key, kv::toBytesLE(uint64_t{2000}));
		ASSERT_TRUE(txnB->commit());
		ASSERT_TRUE(txnA->commit()) << "atomicMax must not abort a concurrent writer.";
	}
	{
		auto txn = kvEngine->createReadWriteTransaction();
		auto v = txn->get(key);
		ASSERT_TRUE(v.has_value());
		EXPECT_EQ(kv::fromBytesLE<uint64_t>(*v), uint64_t{2000});
	}

	{  // Cleanup.
		auto txn = kvEngine->createReadWriteTransaction();
		txn->remove(key);
		txn->remove(absentKey);
		ASSERT_TRUE(txn->commit());
	}
}

// mutationCount() counts each buffered write exactly once and ignores reads. Group
// commit relies on this to detect whether a failed op body already poisoned a shared
// transaction. The transaction is intentionally dropped without committing: the count
// is a property of the buffered mutations, not of the commit.
TEST_F(FDBKVEngineTest, MutationCount) {
	auto txn = kvEngine->createReadWriteTransaction();
	EXPECT_EQ(txn->mutationCount(), uint64_t{0}) << "A fresh transaction has buffered nothing.";

	txn->set(kv::toBytes("mc_key"), kv::toBytesLE(uint64_t{1}));
	EXPECT_EQ(txn->mutationCount(), uint64_t{1}) << "set() buffers one mutation.";

	txn->atomicAdd(kv::toBytes("mc_key"), kv::toBytesLE(uint64_t{1}));
	EXPECT_EQ(txn->mutationCount(), uint64_t{2}) << "atomicAdd() buffers one mutation.";

	txn->atomicMax(kv::toBytes("mc_key"), kv::toBytesLE(uint64_t{9}));
	EXPECT_EQ(txn->mutationCount(), uint64_t{3}) << "atomicMax() buffers one mutation.";

	(void)txn->get(kv::toBytes("mc_key"));
	EXPECT_EQ(txn->mutationCount(), uint64_t{3}) << "get() is a read, not a mutation.";

	txn->remove(kv::toBytes("mc_key"));
	EXPECT_EQ(txn->mutationCount(), uint64_t{4}) << "remove() buffers one mutation.";

	txn->removeRange(kv::toBytes("mc_a"), kv::toBytes("mc_b"));
	EXPECT_EQ(txn->mutationCount(), uint64_t{5}) << "removeRange() buffers one mutation.";
}

// recoverAsync() makes the SAME kv transaction reusable after a retryable commit
// conflict: the recovery future completes, the adapter's bookkeeping is reset, and the
// replayed mutations commit. The retry coordinator relies on this to keep the backend's
// accumulated backoff across attempts instead of recreating the transaction.
TEST_F(FDBKVEngineTest, RecoverAsyncReusesTransactionAfterConflict) {
	const auto key = kv::toBytes("recover_async_key");

	auto txn = kvEngine->createReadWriteTransaction();
	(void)txn->get(key);  // Put the key in this transaction's read conflict range.
	{
		auto external = kvEngine->createReadWriteTransaction();
		external->set(key, kv::toBytes("external"));
		ASSERT_TRUE(external->commit());
	}
	txn->set(key, kv::toBytes("mine"));

	int commitError = 0;
	bool retryable = false;
	ASSERT_FALSE(txn->commitAsync()->getResult(&commitError, &retryable))
	    << "The external write must conflict with this transaction's read.";
	ASSERT_TRUE(retryable);

	auto recovery = txn->recoverAsync(commitError);
	ASSERT_NE(recovery, nullptr);
	recovery->get();
	EXPECT_EQ(txn->mutationCount(), uint64_t{0})
	    << "Recovery resets the adapter's bookkeeping for the replay.";

	// Replay on the same transaction object; this attempt sees the external write.
	auto observed = txn->get(key);
	ASSERT_TRUE(observed.has_value());
	EXPECT_EQ(*observed, kv::toBytes("external"));
	txn->set(key, kv::toBytes("replayed"));
	EXPECT_EQ(txn->mutationCount(), uint64_t{1});
	ASSERT_TRUE(txn->commitAsync()->getResult(&commitError, &retryable));

	{  // Verify the replayed value, then clean up.
		auto check = kvEngine->createReadWriteTransaction();
		auto value = check->get(key);
		ASSERT_TRUE(value.has_value());
		EXPECT_EQ(*value, kv::toBytes("replayed"));
		check->remove(key);
		ASSERT_TRUE(check->commit());
	}
}

// recoverAsync() on an adapter whose wrapped transaction has no backend handle is a
// wrong-state call: it must throw std::logic_error, never return a nullptr future that
// poses as a started recovery (see the recoverAsync contract in kv/itransaction.h).
TEST_F(FDBKVEngineTest, RecoverAsyncWithoutBackendHandleThrows) {
	fdb::FDBTransaction handleless{nullptr};
	constexpr int kNotCommitted = 1020;  // A retryable code, to isolate the handle check.
	EXPECT_THROW((void)handleless.recoverAsync(kNotCommitted), std::logic_error);
}

// getApproximateSize() reports the client-side estimate of the transaction's buffered writes.
// It is supported by the FDB backend, grows as mutations are added, and reflects the payload
// size (a large value increases it substantially more than a small one). The writer relies on
// this to bound a flush transaction below FDB's per-transaction size limit. The transaction is
// dropped without committing: the size is a property of the buffered mutations, not of the
// commit.
TEST_F(FDBKVEngineTest, GetApproximateSize) {
	auto txn = kvEngine->createReadWriteTransaction();

	// A fresh transaction reports a value (baseline overhead); the FDB backend supports it.
	auto initial = txn->getApproximateSize();
	ASSERT_TRUE(initial.has_value())
	    << "getApproximateSize() must be supported by the FDB backend.";

	// A buffered set() grows the estimate by at least the value payload.
	const auto key = kv::toBytes("approx_size_key");
	const kv::Value smallValue(64, 'x');
	txn->set(key, smallValue);
	auto afterSmall = txn->getApproximateSize();
	ASSERT_TRUE(afterSmall.has_value());
	EXPECT_GT(afterSmall.value(), initial.value())
	    << "A buffered set() must increase the approximate size.";
	EXPECT_GE(afterSmall.value(), initial.value() + smallValue.size())
	    << "The growth must cover at least the written value bytes.";

	// A large value grows the estimate substantially more than a small one.
	const auto bigKey = kv::toBytes("approx_size_big_key");
	const kv::Value bigValue(64 * 1024, 'y');  // 64 KiB, under FDB's per-value limit
	txn->set(bigKey, bigValue);
	auto afterBig = txn->getApproximateSize();
	ASSERT_TRUE(afterBig.has_value());
	EXPECT_GE(afterBig.value(), afterSmall.value() + bigValue.size())
	    << "A large buffered value must be reflected in the approximate size.";

	safs::log_info("Approximate size: initial={}, after 64B set={}, after 64KiB set={}",
	               initial.value(), afterSmall.value(), afterBig.value());
	// No cleanup: the transaction is intentionally dropped without committing.
}

// Exercises the async-commit path end to end: poll isReady(), finalize via getResult(),
// confirm the committed version is captured, that getResult() is single-use, and that
// the write is durable.
TEST_F(FDBKVEngineTest, CommitAsync) {
	const auto key = kv::toBytes("commit_async_key");
	const auto value = kv::toBytes("commit_async_value");

	auto txn = kvEngine->createReadWriteTransaction();
	txn->set(key, value);

	auto future = txn->commitAsync();
	ASSERT_TRUE(future != nullptr) << "commitAsync() must return a future.";

	// Drive the future to readiness through isReady(), then finalize. Bounded by a
	// wall-clock deadline (not an iteration count) so it stays robust on slow or
	// loaded CI; the timeout is generous since a healthy commit resolves in millis.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!future->isReady() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ASSERT_TRUE(future->isReady()) << "Commit future did not become ready within the timeout.";

	int error = -1;
	bool retryable = true;
	ASSERT_TRUE(future->getResult(&error, &retryable)) << "Async commit should succeed.";
	EXPECT_EQ(error, 0);
	EXPECT_FALSE(retryable);

	// A successful commit captures its version on the (still-alive) transaction.
	EXPECT_TRUE(txn->getCommittedVersion().has_value())
	    << "A successful async commit must expose its committed version.";

	// Single-use: a second getResult() reports false without re-committing.
	EXPECT_FALSE(future->getResult()) << "getResult() is single-use.";

	{  // The write is durable.
		auto verify = kvEngine->createReadWriteTransaction();
		auto got = verify->get(key);
		ASSERT_TRUE(got.has_value()) << "Async-committed key must be present.";
		EXPECT_EQ(*got, value);
	}

	{  // Cleanup.
		auto cleanup = kvEngine->createReadWriteTransaction();
		cleanup->remove(key);
		ASSERT_TRUE(cleanup->commit());
	}
}

// ---------------------------------------------------------------------------
// Deterministic fault injection (fdb::FaultInjection)
// ---------------------------------------------------------------------------

namespace {

constexpr fdb_error_t kNotCommitted = 1020;         // retryable, definitely not committed
constexpr fdb_error_t kCommitUnknownResult = 1021;  // maybe committed, NOT retryable
constexpr fdb_error_t kTransactionCancelled = 1025;
constexpr fdb_error_t kTransactionTooLarge = 2101;  // non-retryable

}  // namespace

TEST_F(FDBKVEngineTest, FaultInjectionRetryableAsyncReadThrows) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	fault.readErrors = {kNotCommitted};
	transaction.setFaultInjection(&fault);

	auto future = transaction.getAsync(kv::toBytes("fi_read_retryable"));
	ASSERT_NE(future, nullptr);
	int error = 0;
	EXPECT_THROW(future->get(&error), kv::RetryableTransactionError);
	EXPECT_EQ(error, kNotCommitted);
	EXPECT_EQ(transaction.error(), 0);
}

TEST_F(FDBKVEngineTest, FaultInjectionRetryableIsCatchableAsBase) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	fault.readErrors = {kNotCommitted};
	transaction.setFaultInjection(&fault);

	auto future = transaction.getAsync(kv::toBytes("fi_read_base_catch"));
	ASSERT_NE(future, nullptr);
	bool caught = false;
	try {
		future->get();
	} catch (const kv::TransactionError &error) {
		caught = true;
		EXPECT_EQ(error.errorCode(), kNotCommitted);
		EXPECT_TRUE(error.retryable());
	}
	EXPECT_TRUE(caught);
}

TEST_F(FDBKVEngineTest, FaultInjectionNonRetryableAsyncReadThrowsBase) {
	// A non-retryable read error throws the TransactionError base (and specifically
	// NOT the retryable subclass): a failed read never looks like a missing key, and
	// the op boundary never replays it.
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	fault.readErrors = {kTransactionTooLarge};
	transaction.setFaultInjection(&fault);

	auto future = transaction.getAsync(kv::toBytes("fi_read_nonretryable"));
	ASSERT_NE(future, nullptr);
	bool caughtBase = false;
	try {
		future->get();
	} catch (const kv::RetryableTransactionError &) {
		FAIL() << "A non-retryable error must not be catchable as retryable.";
	} catch (const kv::TransactionError &error) {
		caughtBase = true;
		EXPECT_EQ(error.errorCode(), kTransactionTooLarge);
		EXPECT_FALSE(error.retryable());
	}
	EXPECT_TRUE(caughtBase);
}

TEST_F(FDBKVEngineTest, FaultInjectionRetryableRangeReadThrows) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	fault.readErrors = {kNotCommitted};
	transaction.setFaultInjection(&fault);

	kv::KeySelector begin(kv::toBytes("fi_range_a"), true, 0);
	kv::KeySelector end(kv::toBytes("fi_range_z"), true, 0);
	auto future = transaction.getRangeAsync(begin, end, 10);
	ASSERT_NE(future, nullptr);
	int error = 0;
	EXPECT_THROW(future->get(&error), kv::RetryableTransactionError);
	EXPECT_EQ(error, kNotCommitted);
	EXPECT_EQ(transaction.error(), 0);
}

TEST_F(FDBKVEngineTest, FaultInjectionSyncRangeReadThrowsWithoutUpdatingError) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	fault.readErrors = {kNotCommitted};
	transaction.setFaultInjection(&fault);

	kv::KeySelector begin(kv::toBytes("fi_sync_range_a"), true, 0);
	kv::KeySelector end(kv::toBytes("fi_sync_range_z"), true, 0);
	try {
		(void)transaction.getRange(begin, end, 10);
		FAIL() << "The injected range-read failure must throw.";
	} catch (const kv::TransactionError &error) { EXPECT_EQ(error.errorCode(), kNotCommitted); }
	EXPECT_EQ(transaction.error(), 0);
}

TEST_F(FDBKVEngineTest, FaultInjectionSyncReadThrowsLikeAsync) {
	// The sync read follows the same contract as the async path: a retryable error
	// throws instead of being swallowed into an empty result.
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	fault.readErrors = {kNotCommitted};
	transaction.setFaultInjection(&fault);

	EXPECT_THROW(transaction.get(kv::toBytes("fi_read_sync")), kv::RetryableTransactionError);
	EXPECT_EQ(transaction.error(), kNotCommitted);
}

TEST_F(FDBKVEngineTest, FaultInjectionZeroEntriesSkipInjection) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	fault.readErrors = {0, kTransactionTooLarge};
	transaction.setFaultInjection(&fault);

	// First read: zero entry, no injection; absent key reads as nullopt with no error.
	auto value = transaction.get(kv::toBytes("fi_read_zero_skip_absent"));
	EXPECT_FALSE(value.has_value());
	EXPECT_EQ(transaction.error(), 0);

	// Second read: scripted non-retryable failure throws the base type.
	EXPECT_THROW(transaction.get(kv::toBytes("fi_read_zero_skip_absent")), kv::TransactionError);
	EXPECT_EQ(transaction.error(), kTransactionTooLarge);
}

TEST_F(FDBKVEngineTest, FaultInjectionPreCommitFailsWithoutWriting) {
	const kv::Key key = kv::toBytes("fi_pre_commit");
	{  // Clean slate.
		auto cleanup = kvEngine->createReadWriteTransaction();
		cleanup->remove(key);
		ASSERT_TRUE(cleanup->commit());
	}

	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	fault.preCommitErrors = {kNotCommitted};
	transaction.setFaultInjection(&fault);

	transaction.set(key, kv::toBytes("value"));
	EXPECT_FALSE(transaction.commit());
	EXPECT_EQ(transaction.error(), kNotCommitted);

	// Nothing was submitted: the key must not exist.
	auto verify = kvEngine->createReadWriteTransaction();
	EXPECT_FALSE(verify->get(key).has_value());
}

TEST_F(FDBKVEngineTest, FaultInjectionPreCommitAsyncReportsRetryable) {
	const kv::Key key = kv::toBytes("fi_pre_commit_async");
	{  // Clean slate.
		auto cleanup = kvEngine->createReadWriteTransaction();
		cleanup->remove(key);
		ASSERT_TRUE(cleanup->commit());
	}

	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	fault.preCommitErrors = {kNotCommitted};
	transaction.setFaultInjection(&fault);

	transaction.set(key, kv::toBytes("value"));
	auto future = transaction.commitAsync();
	ASSERT_NE(future, nullptr);
	EXPECT_TRUE(future->isReady());

	int error = 0;
	bool retryable = false;
	EXPECT_FALSE(future->getResult(&error, &retryable));
	EXPECT_EQ(error, kNotCommitted);
	EXPECT_TRUE(retryable) << "not_committed is definitely-not-committed, hence retryable.";

	auto verify = kvEngine->createReadWriteTransaction();
	EXPECT_FALSE(verify->get(key).has_value());
}

TEST_F(FDBKVEngineTest, FaultInjectionPostCommitOverrideCommitAppliesExactlyOnce) {
	// The maybe-committed detector: the commit really happens, then the result is
	// overridden with commit_unknown_result. A non-idempotent atomicAdd is the oracle:
	// the value becomes exactly one, proving that the durable mutation applied once.
	const kv::Key key = kv::toBytes("fi_post_commit");
	const kv::Value increment = kv::toBytesLE(uint64_t{1});
	{  // Clean slate.
		auto cleanup = kvEngine->createReadWriteTransaction();
		cleanup->remove(key);
		ASSERT_TRUE(cleanup->commit());
	}

	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	fault.postCommitErrors = {kCommitUnknownResult};
	transaction.setFaultInjection(&fault);

	transaction.atomicAdd(key, increment);
	EXPECT_FALSE(transaction.commit());
	EXPECT_EQ(transaction.error(), kCommitUnknownResult);

	{  // The write is durable despite the reported failure.
		auto verify = kvEngine->createReadWriteTransaction();
		auto got = verify->get(key);
		ASSERT_TRUE(got.has_value());
		EXPECT_EQ(kv::fromBytesLE<uint64_t>(*got), uint64_t{1});
	}

	{  // Cleanup.
		auto cleanup = kvEngine->createReadWriteTransaction();
		cleanup->remove(key);
		ASSERT_TRUE(cleanup->commit());
	}
}

TEST_F(FDBKVEngineTest, FaultInjectionPostCommitOverrideAsyncAppliesOnceAndIsNotRetryable) {
	const kv::Key key = kv::toBytes("fi_post_commit_async");
	const kv::Value increment = kv::toBytesLE(uint64_t{1});
	{  // Clean slate.
		auto cleanup = kvEngine->createReadWriteTransaction();
		cleanup->remove(key);
		ASSERT_TRUE(cleanup->commit());
	}

	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	fault.postCommitErrors = {kCommitUnknownResult};
	transaction.setFaultInjection(&fault);

	transaction.atomicAdd(key, increment);
	auto future = transaction.commitAsync();
	ASSERT_NE(future, nullptr);

	int error = 0;
	bool retryable = true;
	EXPECT_FALSE(future->getResult(&error, &retryable));
	EXPECT_EQ(error, kCommitUnknownResult);
	EXPECT_FALSE(retryable) << "A maybe-committed outcome must never be reported retryable: "
	                           "a blind replay could double-apply.";

	{  // The write is durable despite the reported failure.
		auto verify = kvEngine->createReadWriteTransaction();
		auto got = verify->get(key);
		ASSERT_TRUE(got.has_value());
		EXPECT_EQ(kv::fromBytesLE<uint64_t>(*got), uint64_t{1});
	}

	{  // Cleanup.
		auto cleanup = kvEngine->createReadWriteTransaction();
		cleanup->remove(key);
		ASSERT_TRUE(cleanup->commit());
	}
}

TEST_F(FDBKVEngineTest, FaultInjectionCommittedVersionLookupFailure) {
	// A committed-version lookup failure AFTER a successful commit is reported as a
	// SUCCESSFUL commit without a version: the data is durable, and reporting a
	// failure would invite the caller to replay an already-applied operation.
	const kv::Key key = kv::toBytes("fi_version_lookup");
	const kv::Value value = kv::toBytes("durable");
	{  // Clean slate.
		auto cleanup = kvEngine->createReadWriteTransaction();
		cleanup->remove(key);
		ASSERT_TRUE(cleanup->commit());
	}

	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	fault.committedVersionErrors = {kTransactionCancelled};
	transaction.setFaultInjection(&fault);

	transaction.set(key, value);
	EXPECT_TRUE(transaction.commit()) << "The commit is durable; a failed version lookup "
	                                     "must not be reported as a failed commit.";
	EXPECT_EQ(transaction.error(), 0);
	EXPECT_FALSE(transaction.getCommittedVersion().has_value());

	{  // The write is durable and the commit reported success.
		auto verify = kvEngine->createReadWriteTransaction();
		auto got = verify->get(key);
		ASSERT_TRUE(got.has_value());
		EXPECT_EQ(*got, value);
	}

	{  // Cleanup.
		auto cleanup = kvEngine->createReadWriteTransaction();
		cleanup->remove(key);
		ASSERT_TRUE(cleanup->commit());
	}
}

TEST_F(FDBKVEngineTest, FaultInjectionCommittedVersionLookupFailureAsync) {
	// Async twin of the sync case: getResult() reports success without a version.
	const kv::Key key = kv::toBytes("fi_version_lookup_async");
	const kv::Value value = kv::toBytes("durable");
	{  // Clean slate.
		auto cleanup = kvEngine->createReadWriteTransaction();
		cleanup->remove(key);
		ASSERT_TRUE(cleanup->commit());
	}

	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	fault.committedVersionErrors = {kTransactionCancelled};
	transaction.setFaultInjection(&fault);

	transaction.set(key, value);
	auto future = transaction.commitAsync();
	ASSERT_NE(future, nullptr);

	int error = -1;
	bool retryable = true;
	EXPECT_TRUE(future->getResult(&error, &retryable))
	    << "The commit is durable; a failed version lookup must not fail the commit.";
	EXPECT_EQ(error, 0);
	EXPECT_FALSE(retryable);
	EXPECT_FALSE(transaction.getCommittedVersion().has_value());
	EXPECT_EQ(transaction.error(), 0);

	{  // Cleanup.
		auto cleanup = kvEngine->createReadWriteTransaction();
		cleanup->remove(key);
		ASSERT_TRUE(cleanup->commit());
	}
}

TEST_F(FDBKVEngineTest, FailedRecommitClearsCommittedVersion) {
	// A failed commit attempt on a previously committed transaction must not leave the
	// earlier attempt's version visible: every attempt clears the version on entry.
	const kv::Key key = kv::toBytes("fi_recommit_version");
	{  // Clean slate.
		auto cleanup = kvEngine->createReadWriteTransaction();
		cleanup->remove(key);
		ASSERT_TRUE(cleanup->commit());
	}

	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	transaction.set(key, kv::toBytes("value"));
	ASSERT_TRUE(transaction.commit());
	ASSERT_TRUE(transaction.getCommittedVersion().has_value());

	// Second attempt on the same transaction (the state machine requires reset()
	// to leave kCommitted), failed deterministically before submission: the
	// version from the first attempt must be gone.
	transaction.reset();
	fdb::FaultInjection fault;
	fault.preCommitErrors = {kNotCommitted};
	transaction.setFaultInjection(&fault);
	EXPECT_FALSE(transaction.commit());
	EXPECT_FALSE(transaction.getCommittedVersion().has_value());

	{  // Cleanup.
		auto cleanup = kvEngine->createReadWriteTransaction();
		cleanup->remove(key);
		ASSERT_TRUE(cleanup->commit());
	}
}

TEST_F(FDBKVEngineTest, FailedRecommitAsyncClearsCommittedVersion) {
	// Async twin: commitAsync() clears the version on entry as well.
	const kv::Key key = kv::toBytes("fi_recommit_version_async");
	{  // Clean slate.
		auto cleanup = kvEngine->createReadWriteTransaction();
		cleanup->remove(key);
		ASSERT_TRUE(cleanup->commit());
	}

	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	transaction.set(key, kv::toBytes("value"));
	ASSERT_TRUE(transaction.commit());
	ASSERT_TRUE(transaction.getCommittedVersion().has_value());

	// The state machine requires reset() to leave kCommitted before a new attempt.
	transaction.reset();
	fdb::FaultInjection fault;
	fault.preCommitErrors = {kNotCommitted};
	transaction.setFaultInjection(&fault);
	auto future = transaction.commitAsync();
	ASSERT_NE(future, nullptr);
	int error = 0;
	EXPECT_FALSE(future->getResult(&error));
	EXPECT_EQ(error, kNotCommitted);
	EXPECT_FALSE(transaction.getCommittedVersion().has_value());

	{  // Cleanup.
		auto cleanup = kvEngine->createReadWriteTransaction();
		cleanup->remove(key);
		ASSERT_TRUE(cleanup->commit());
	}
}

TEST_F(FDBKVEngineTest, TransactionFromNullDBIsInvalidNotUB) {
	// Constructing a transaction from a null DB pointer must produce an invalid
	// transaction, never pass nullptr into the C API. Using it is a wrong-state call
	// per the kv error-domain rules: reads, writes and commits throw std::logic_error
	// instead of posing as a missing key or silently dropping a mutation. (An errored
	// DB cannot be built through the public API to test the getDB()==nullptr branch
	// the same way: fdb_create_database is lazy and succeeds even for a bogus
	// cluster-file path, deferring the failure to the first operation.)
	fdb::Transaction fromNull(nullptr);
	EXPECT_FALSE(fromNull);
	const kv::Key key = kv::toBytes("any");
	const kv::Value value = kv::toBytesLE(uint64_t{1});
	kv::KeySelector begin(kv::toBytes("a"), true, 0);
	kv::KeySelector end(kv::toBytes("z"), true, 0);

	EXPECT_THROW(fromNull.setOption(FDB_TR_OPTION_READ_YOUR_WRITES_DISABLE), std::logic_error);
	EXPECT_THROW(fromNull.get(key), std::logic_error);
	EXPECT_THROW(fromNull.getAsync(key), std::logic_error);
	EXPECT_THROW(fromNull.getRange(begin, end, 1), std::logic_error);
	EXPECT_THROW(fromNull.getRangeAsync(begin, end, 1), std::logic_error);
	EXPECT_THROW(fromNull.set(key, value), std::logic_error);
	EXPECT_THROW(fromNull.atomicAdd(key, value), std::logic_error);
	EXPECT_THROW(fromNull.atomicMax(key, value), std::logic_error);
	EXPECT_THROW(fromNull.remove(key), std::logic_error);
	EXPECT_THROW(fromNull.removeRange(begin.getKey(), end.getKey()), std::logic_error);
	EXPECT_THROW(fromNull.addReadConflictKey(key), std::logic_error);
	EXPECT_THROW(fromNull.commit(), std::logic_error);
	EXPECT_THROW(fromNull.commitAsync(), std::logic_error);
	EXPECT_FALSE(fromNull.getCommittedVersion().has_value());
	EXPECT_FALSE(fromNull.getApproximateSize().has_value());
}

TEST_F(FDBKVEngineTest, ConsumedReadFutureThrowsLogicError) {
	// Double-get on a single-use read future is a wrong-state call: it must throw
	// std::logic_error, never pose as a missing key or an empty range.
	auto transaction = kvEngine->createReadWriteTransaction();

	auto valueFuture = transaction->getAsync(kv::toBytes("consumed_future_key"));
	ASSERT_TRUE(valueFuture != nullptr);
	(void)valueFuture->get();
	EXPECT_THROW(valueFuture->get(), std::logic_error);

	auto rangeFuture =
	    transaction->getRangeAsync(kv::KeySelector(kv::toBytes("consumed_range_a"), true, 0),
	                               kv::KeySelector(kv::toBytes("consumed_range_b"), false, 0));
	ASSERT_TRUE(rangeFuture != nullptr);
	(void)rangeFuture->get();
	EXPECT_THROW(rangeFuture->get(), std::logic_error);
}

// ---------------------------------------------------------------------------
// Transaction state machine (fdb::TransactionState), onErrorAsync, options
// ---------------------------------------------------------------------------

using fdb::TransactionState;

TEST_F(FDBKVEngineTest, StateCommitAndResetRoundTrip) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);
	EXPECT_EQ(transaction.state(), TransactionState::kActive);

	const kv::Key key = kv::toBytes("state_commit_reset");
	transaction.set(key, kv::toBytes("v1"));
	ASSERT_TRUE(transaction.commit());
	EXPECT_EQ(transaction.state(), TransactionState::kCommitted);
	EXPECT_TRUE(transaction.getCommittedVersion().has_value());

	// Committed forbids further work but allows reset for the next attempt.
	EXPECT_THROW(transaction.set(key, kv::toBytes("v2")), std::logic_error);
	transaction.reset();
	EXPECT_EQ(transaction.state(), TransactionState::kActive);
	EXPECT_FALSE(transaction.getCommittedVersion().has_value());

	transaction.remove(key);
	ASSERT_TRUE(transaction.commit());
}

TEST_F(FDBKVEngineTest, StateRetryableReadErrorEntersRetryPending) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	fault.readErrors = {kNotCommitted};
	transaction.setFaultInjection(&fault);

	EXPECT_THROW(transaction.get(kv::toBytes("state_rp")), kv::RetryableTransactionError);
	EXPECT_EQ(transaction.state(), TransactionState::kRetryPending);

	// A poisoned transaction refuses further work loudly instead of no-opping.
	EXPECT_THROW(transaction.set(kv::toBytes("k"), kv::toBytes("v")), std::logic_error);
	EXPECT_THROW(transaction.commit(), std::logic_error);
	EXPECT_THROW((void)transaction.getAsync(kv::toBytes("k")), std::logic_error);
	EXPECT_THROW(transaction.addReadConflictKey(kv::toBytes("k")), std::logic_error);
}

TEST_F(FDBKVEngineTest, StateNonRetryableReadErrorEntersFailedAndResetRevives) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	fault.readErrors = {kTransactionTooLarge};
	transaction.setFaultInjection(&fault);

	auto future = transaction.getAsync(kv::toBytes("state_failed"));
	ASSERT_NE(future, nullptr);
	EXPECT_THROW(future->get(), kv::TransactionError);
	EXPECT_EQ(transaction.state(), TransactionState::kFailed);

	transaction.reset();
	EXPECT_EQ(transaction.state(), TransactionState::kActive);
	// The same handle is usable again after reset (live read).
	EXPECT_NO_THROW((void)transaction.get(kv::toBytes("state_failed_absent")));
}

// A read future issued in one attempt, held unconsumed while reset() starts a FRESH
// attempt, must not record its late failure into the new attempt: the attempt-generation
// guard keeps the superseded future inert so it cannot regress kActive.
TEST_F(FDBKVEngineTest, StaleReadFailureAfterResetLeavesFreshAttemptActive) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	transaction.setFaultInjection(&fault);

	// Issue a read in the first attempt; hold the future unconsumed.
	auto stale = transaction.getAsync(kv::toBytes("stale_after_reset"));
	ASSERT_NE(stale, nullptr);

	// Start a fresh attempt.
	transaction.reset();
	ASSERT_EQ(transaction.state(), TransactionState::kActive);

	// The stale future now fails: it must throw to ITS caller but leave the fresh attempt
	// Active. Without the generation guard it would stamp kRetryPending.
	fault.readErrors = {kNotCommitted};
	EXPECT_THROW(stale->get(), kv::RetryableTransactionError);
	EXPECT_EQ(transaction.state(), TransactionState::kActive);
}

// The same guard across a successful onErrorAsync() recovery, which re-activates the SAME
// handle for a fresh attempt: a read future from the recovered-past attempt must not drive
// the new attempt back out of kActive.
TEST_F(FDBKVEngineTest, StaleReadFailureAfterRecoveryLeavesFreshAttemptActive) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	transaction.setFaultInjection(&fault);

	// Issue a read in the first attempt; hold the future unconsumed.
	auto stale = transaction.getAsync(kv::toBytes("stale_after_recovery"));
	ASSERT_NE(stale, nullptr);

	// Drive the transaction to a retryable failure, then recover it back to Active.
	fault.readErrors = {kNotCommitted};
	EXPECT_THROW((void)transaction.get(kv::toBytes("stale_after_recovery_trigger")),
	             kv::RetryableTransactionError);
	ASSERT_EQ(transaction.state(), TransactionState::kRetryPending);
	auto recovery = transaction.onErrorAsync(kNotCommitted);
	ASSERT_NE(recovery, nullptr);
	recovery->get();
	ASSERT_EQ(transaction.state(), TransactionState::kActive);

	// The stale future (issued before the failure) resolves as a failure now; it must not
	// regress the recovered attempt.
	fault.readErrors = {kNotCommitted};
	EXPECT_THROW(stale->get(), kv::RetryableTransactionError);
	EXPECT_EQ(transaction.state(), TransactionState::kActive);
}

TEST_F(FDBKVEngineTest, StatePreCommitConflictEntersRetryPending) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	fault.preCommitErrors = {kNotCommitted};
	transaction.setFaultInjection(&fault);

	transaction.set(kv::toBytes("state_precommit"), kv::toBytes("v"));
	EXPECT_FALSE(transaction.commit());
	EXPECT_EQ(transaction.state(), TransactionState::kRetryPending);
}

TEST_F(FDBKVEngineTest, StateMaybeCommittedEntersIndeterminate) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	fault.postCommitErrors = {kCommitUnknownResult};
	transaction.setFaultInjection(&fault);

	const kv::Key key = kv::toBytes("state_indeterminate");
	transaction.set(key, kv::toBytes("v"));
	EXPECT_FALSE(transaction.commit());
	EXPECT_EQ(transaction.state(), TransactionState::kIndeterminate);

	// A maybe-committed transaction is destroy-only: no reset, no cancel, no reuse.
	EXPECT_THROW(transaction.reset(), std::logic_error);
	EXPECT_THROW(transaction.cancel(), std::logic_error);
	EXPECT_THROW(transaction.commit(), std::logic_error);

	// The injected override happened after a REAL commit; clean up the key.
	fdb::Transaction cleanup(fdbDB.get());
	cleanup.remove(key);
	ASSERT_TRUE(cleanup.commit());
}

TEST_F(FDBKVEngineTest, CommitWaitFailureEntersIndeterminate) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	// A wait-layer failure after the commit was submitted: the commit is durable but its
	// outcome is unknown to us. Injecting not_committed (which classifies as retryable)
	// proves the wait path forces kIndeterminate regardless of the code, never a resettable
	// or retryable state that would double-apply.
	fdb::FaultInjection fault;
	fault.commitWaitErrors = {kNotCommitted};
	transaction.setFaultInjection(&fault);

	const kv::Key key = kv::toBytes("state_commit_wait");
	transaction.set(key, kv::toBytes("v"));
	EXPECT_FALSE(transaction.commit());
	EXPECT_EQ(transaction.state(), TransactionState::kIndeterminate);

	EXPECT_THROW(transaction.reset(), std::logic_error);
	EXPECT_THROW(transaction.cancel(), std::logic_error);
	EXPECT_THROW(transaction.commit(), std::logic_error);

	// The wait failed after a REAL commit; clean up the key.
	fdb::Transaction cleanup(fdbDB.get());
	cleanup.remove(key);
	ASSERT_TRUE(cleanup.commit());
}

TEST_F(FDBKVEngineTest, CommitAsyncWaitFailureEntersIndeterminate) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	fault.commitWaitErrors = {kNotCommitted};
	transaction.setFaultInjection(&fault);

	const kv::Key key = kv::toBytes("state_commit_async_wait");
	transaction.set(key, kv::toBytes("v"));
	auto future = transaction.commitAsync();
	ASSERT_NE(future, nullptr);

	int error = 0;
	bool retryable = true;
	EXPECT_FALSE(future->getResult(&error, &retryable));
	// Never retryable: a maybe-committed op must not be replayed.
	EXPECT_FALSE(retryable);
	EXPECT_EQ(transaction.state(), TransactionState::kIndeterminate);
	EXPECT_THROW(transaction.reset(), std::logic_error);

	fdb::Transaction cleanup(fdbDB.get());
	cleanup.remove(key);
	ASSERT_TRUE(cleanup.commit());
}

TEST_F(FDBKVEngineTest, StateCommitAsyncTransitions) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	const kv::Key key = kv::toBytes("state_commit_async");
	transaction.set(key, kv::toBytes("v"));
	auto future = transaction.commitAsync();
	ASSERT_NE(future, nullptr);
	EXPECT_EQ(transaction.state(), TransactionState::kCommitInFlight);
	// Only the commit future may act while the commit is in flight.
	EXPECT_THROW(transaction.commit(), std::logic_error);
	EXPECT_THROW(transaction.reset(), std::logic_error);

	EXPECT_TRUE(future->getResult());
	EXPECT_EQ(transaction.state(), TransactionState::kCommitted);

	transaction.reset();
	transaction.remove(key);
	ASSERT_TRUE(transaction.commit());
}

TEST_F(FDBKVEngineTest, StateAbandonedCommitFutureEntersIndeterminate) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	const kv::Key key = kv::toBytes("state_abandoned_commit");
	transaction.set(key, kv::toBytes("v"));
	{
		auto future = transaction.commitAsync();
		ASSERT_NE(future, nullptr);
		// Destroyed unconsumed: the commit may have applied, so the transaction
		// must land in the destroy-only Indeterminate state.
	}
	EXPECT_EQ(transaction.state(), TransactionState::kIndeterminate);
	EXPECT_THROW(transaction.reset(), std::logic_error);

	// The abandoned commit may have been durable; remove the key either way.
	fdb::Transaction cleanup(fdbDB.get());
	cleanup.remove(key);
	ASSERT_TRUE(cleanup.commit());
}

TEST_F(FDBKVEngineTest, OnErrorAsyncRecoversTheSameTransaction) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	fault.readErrors = {kNotCommitted};
	transaction.setFaultInjection(&fault);

	EXPECT_THROW(transaction.get(kv::toBytes("state_recover")), kv::RetryableTransactionError);
	ASSERT_EQ(transaction.state(), TransactionState::kRetryPending);

	auto backoff = transaction.onErrorAsync(kNotCommitted);
	ASSERT_NE(backoff, nullptr);
	EXPECT_EQ(transaction.state(), TransactionState::kBackoffInFlight);
	EXPECT_NO_THROW(backoff->get());
	EXPECT_EQ(transaction.state(), TransactionState::kActive);

	// The recovered handle must complete a real replay end to end.
	const kv::Key key = kv::toBytes("state_recover");
	transaction.set(key, kv::toBytes("v"));
	ASSERT_TRUE(transaction.commit());

	fdb::Transaction verify(fdbDB.get());
	auto got = verify.get(key);
	ASSERT_TRUE(got.has_value());
	EXPECT_EQ(*got, kv::toBytes("v"));
	verify.reset();
	verify.remove(key);
	ASSERT_TRUE(verify.commit());
}

TEST_F(FDBKVEngineTest, OnErrorAsyncAbandonedFutureIsDestroyOnly) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	fault.readErrors = {kNotCommitted};
	transaction.setFaultInjection(&fault);

	EXPECT_THROW(transaction.get(kv::toBytes("state_abandoned_backoff")),
	             kv::RetryableTransactionError);
	{
		auto backoff = transaction.onErrorAsync(kNotCommitted);
		ASSERT_NE(backoff, nullptr);
		// Destroyed unconsumed: the recovery outcome is unknown, reuse is unsafe
		// and reset-during-on_error is undefined in FDB.
	}
	EXPECT_EQ(transaction.state(), TransactionState::kFailed);
	EXPECT_THROW(transaction.reset(), std::logic_error);
	EXPECT_THROW(transaction.cancel(), std::logic_error);
}

TEST_F(FDBKVEngineTest, OnErrorAsyncArgumentAndStateChecks) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	EXPECT_THROW((void)transaction.onErrorAsync(0), std::invalid_argument);
	// Only a surfaced retryable failure (kRetryPending) permits backend recovery.
	EXPECT_THROW((void)transaction.onErrorAsync(kNotCommitted), std::logic_error);
}

TEST_F(FDBKVEngineTest, OnErrorAsyncRejectsForeignErrorCode) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	fault.readErrors = {kNotCommitted};
	transaction.setFaultInjection(&fault);
	EXPECT_THROW(transaction.get(kv::toBytes("foreign_code")), kv::RetryableTransactionError);
	ASSERT_EQ(transaction.state(), TransactionState::kRetryPending);

	// A code this transaction never surfaced would drive FDB's retry classification
	// and backoff with someone else's failure; only the recorded code is accepted.
	constexpr fdb_error_t kTransactionTooOld = 1007;
	EXPECT_THROW((void)transaction.onErrorAsync(kTransactionTooOld), std::invalid_argument);
	EXPECT_EQ(transaction.state(), TransactionState::kRetryPending);

	auto backoff = transaction.onErrorAsync(kNotCommitted);
	ASSERT_NE(backoff, nullptr);
	EXPECT_NO_THROW(backoff->get());
	EXPECT_EQ(transaction.state(), TransactionState::kActive);
}

TEST_F(FDBKVEngineTest, LateReadFailureDoesNotRegressCommitted) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	transaction.setFaultInjection(&fault);

	const kv::Key key = kv::toBytes("late_read_committed");
	auto pending = transaction.getAsync(kv::toBytes("late_read_committed_other"));
	ASSERT_NE(pending, nullptr);
	transaction.set(key, kv::toBytes("v"));
	ASSERT_TRUE(transaction.commit());
	ASSERT_EQ(transaction.state(), TransactionState::kCommitted);

	// The pre-commit read resolves late and fails: it must throw, but it must not
	// regress the commit outcome into a replayable state (a replay would double-apply).
	fault.readErrors = {kNotCommitted};
	EXPECT_THROW((void)pending->get(), kv::RetryableTransactionError);
	EXPECT_EQ(transaction.state(), TransactionState::kCommitted);

	transaction.reset();
	transaction.remove(key);
	ASSERT_TRUE(transaction.commit());
}

TEST_F(FDBKVEngineTest, LateReadFailureDoesNotRegressIndeterminate) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	fault.postCommitErrors = {kCommitUnknownResult};
	transaction.setFaultInjection(&fault);

	const kv::Key key = kv::toBytes("late_read_indeterminate");
	auto pending = transaction.getAsync(kv::toBytes("late_read_indeterminate_other"));
	ASSERT_NE(pending, nullptr);
	transaction.set(key, kv::toBytes("v"));
	EXPECT_FALSE(transaction.commit());
	ASSERT_EQ(transaction.state(), TransactionState::kIndeterminate);

	// A late failed read must not turn a maybe-committed transaction replayable.
	fault.readErrors = {kNotCommitted};
	EXPECT_THROW((void)pending->get(), kv::RetryableTransactionError);
	EXPECT_EQ(transaction.state(), TransactionState::kIndeterminate);
	EXPECT_THROW(transaction.reset(), std::logic_error);

	// The injected override happened after a REAL commit; clean up the key.
	fdb::Transaction cleanup(fdbDB.get());
	cleanup.remove(key);
	ASSERT_TRUE(cleanup.commit());
}

TEST_F(FDBKVEngineTest, GetApproximateSizeReportsOnlyWhileActive) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	const kv::Key key = kv::toBytes("approx_size_states");
	transaction.set(key, kv::toBytes("v"));
	EXPECT_TRUE(transaction.getApproximateSize().has_value());

	auto future = transaction.commitAsync();
	ASSERT_NE(future, nullptr);
	// In-flight and post-commit states are owned by their futures; the buffered-mutation
	// estimate is meaningless there, so the diagnostic reports "no estimate".
	EXPECT_EQ(transaction.state(), TransactionState::kCommitInFlight);
	EXPECT_EQ(transaction.getApproximateSize(), std::nullopt);
	ASSERT_TRUE(future->getResult());
	EXPECT_EQ(transaction.state(), TransactionState::kCommitted);
	EXPECT_EQ(transaction.getApproximateSize(), std::nullopt);

	transaction.reset();
	EXPECT_TRUE(transaction.getApproximateSize().has_value());
	transaction.remove(key);
	ASSERT_TRUE(transaction.commit());
}

TEST_F(FDBKVEngineTest, VoidFutureIsSingleUse) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	fdb::FaultInjection fault;
	fault.readErrors = {kNotCommitted};
	transaction.setFaultInjection(&fault);

	EXPECT_THROW(transaction.get(kv::toBytes("state_single_use")), kv::RetryableTransactionError);
	auto backoff = transaction.onErrorAsync(kNotCommitted);
	ASSERT_NE(backoff, nullptr);
	EXPECT_NO_THROW(backoff->get());
	EXPECT_THROW(backoff->get(), std::logic_error);
}

TEST_F(FDBKVEngineTest, CancelEntersCancelledAndResetRevives) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	transaction.cancel();
	EXPECT_EQ(transaction.state(), TransactionState::kCancelled);
	EXPECT_EQ(transaction.error(), kTransactionCancelled);
	EXPECT_THROW((void)transaction.get(kv::toBytes("state_cancel")), std::logic_error);
	EXPECT_THROW(transaction.cancel(), std::logic_error);

	transaction.reset();
	EXPECT_EQ(transaction.state(), TransactionState::kActive);
	EXPECT_NO_THROW((void)transaction.get(kv::toBytes("state_cancel_absent")));
}

TEST_F(FDBKVEngineTest, DestroyingArmedUnreadyFutureFiresCallback) {
	// Pins the fdb_c lifetime semantics armReadyCallback (fdb.cc) relies on: destroying
	// a not-yet-ready future cancels it, cancellation makes it ready, and the armed
	// callback fires exactly once, releasing the heap node. A watch on a key nobody
	// writes is the one future that stays unready indefinitely, so it isolates the
	// destroy-before-ready path deterministically.
	FDBTransaction *rawTr = nullptr;
	ASSERT_EQ(fdb_database_create_transaction(fdbDB->getDB(), &rawTr), 0);
	const kv::Key key = kv::toBytes("armed_callback_never_written");
	FDBFuture *watch = fdb_transaction_watch(rawTr, key.data(), static_cast<int>(key.size()));
	ASSERT_NE(watch, nullptr);
	// A watch only becomes durable once its transaction commits.
	FDBFuture *commitFuture = fdb_transaction_commit(rawTr);
	ASSERT_EQ(fdb_future_block_until_ready(commitFuture), 0);
	ASSERT_EQ(fdb_future_get_error(commitFuture), 0);
	fdb_future_destroy(commitFuture);
	ASSERT_EQ(fdb_future_is_ready(watch), 0) << "watch resolved early; experiment invalid";

	static std::atomic<int> fired;
	fired = 0;
	ASSERT_EQ(
	    fdb_future_set_callback(
	        watch, [](FDBFuture * /*future*/, void * /*param*/) { fired.fetch_add(1); }, nullptr),
	    0);
	fdb_future_destroy(watch);

	// The callback runs on the FDB network thread; wait bounded.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (fired.load() == 0 && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	EXPECT_EQ(fired.load(), 1);
	fdb_transaction_destroy(rawTr);
}

TEST(FdbIntOptionEncodingTest, EncodesLittleEndian) {
	const auto bytes = fdb::detail::encodeInt64LE(0x0102030405060708);
	const std::array<uint8_t, 8> expected{0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
	EXPECT_EQ(bytes, expected);
	const auto minusOne = fdb::detail::encodeInt64LE(-1);
	for (uint8_t byte : minusOne) { EXPECT_EQ(byte, 0xFF); }
}

TEST_F(FDBKVEngineTest, SetMaxRetryDelayValidatesAndApplies) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	EXPECT_THROW(transaction.setMaxRetryDelayMs(-1), std::invalid_argument);
	// The FDB option range is [0, INT_MAX]; a larger value is the caller's bug and
	// must not reach the backend (where it would surface as a TransactionError).
	EXPECT_THROW(
	    transaction.setMaxRetryDelayMs(static_cast<int64_t>(std::numeric_limits<int>::max()) + 1),
	    std::invalid_argument);
	EXPECT_EQ(transaction.state(), TransactionState::kActive);
	EXPECT_NO_THROW(transaction.setMaxRetryDelayMs(1000));
	// Sticky: survives reset (re-applied internally; a live backend rejection would
	// throw here).
	EXPECT_NO_THROW(transaction.reset());
	EXPECT_EQ(transaction.state(), TransactionState::kActive);
}

TEST_F(FDBKVEngineTest, SetTimeoutValidatesAndApplies) {
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	EXPECT_THROW(transaction.setTimeoutMs(-1), std::invalid_argument);
	// The valid range is (0, INT_MAX]. 0 is rejected (FDB would read it as "disable the
	// timeout", the opposite of bounding), and a larger value is the caller's bug; neither
	// may reach the backend (where it would surface as a TransactionError).
	EXPECT_THROW(transaction.setTimeoutMs(0), std::invalid_argument);
	EXPECT_THROW(
	    transaction.setTimeoutMs(static_cast<int64_t>(std::numeric_limits<int>::max()) + 1),
	    std::invalid_argument);
	EXPECT_EQ(transaction.state(), TransactionState::kActive);
	EXPECT_NO_THROW(transaction.setTimeoutMs(5000));
	// Sticky like max_retry_delay: survives reset (re-applied internally; a live backend
	// rejection would throw here).
	EXPECT_NO_THROW(transaction.reset());
	EXPECT_EQ(transaction.state(), TransactionState::kActive);
}

TEST_F(FDBKVEngineTest, OptionsPersistAcrossOnErrorAndRevertOnReset) {
	// Live contract test for the FDB option lifetime the retry design relies on:
	// options persist across on_error but revert on reset. Observable via
	// retry_limit=1: the first backend recovery succeeds, the second fails ONLY if
	// the option survived the first on_error; after reset the limit is gone and
	// recovery succeeds again.
	fdb::Transaction transaction(fdbDB.get());
	ASSERT_TRUE(transaction);

	const auto limit = fdb::detail::encodeInt64LE(1);
	const std::string_view limitValue(reinterpret_cast<const char *>(limit.data()), limit.size());
	ASSERT_EQ(transaction.setOption(FDB_TR_OPTION_RETRY_LIMIT, limitValue), 0);

	fdb::FaultInjection fault;
	fault.readErrors = {kNotCommitted, kNotCommitted, kNotCommitted};
	transaction.setFaultInjection(&fault);

	EXPECT_THROW(transaction.get(kv::toBytes("opt_persist")), kv::RetryableTransactionError);
	auto first = transaction.onErrorAsync(kNotCommitted);
	ASSERT_NE(first, nullptr);
	EXPECT_NO_THROW(first->get());

	EXPECT_THROW(transaction.get(kv::toBytes("opt_persist")), kv::RetryableTransactionError);
	auto second = transaction.onErrorAsync(kNotCommitted);
	ASSERT_NE(second, nullptr);
	// retry_limit=1 persisted across the first recovery, so the second one fails
	// and the transaction is terminal.
	EXPECT_THROW(second->get(), kv::TransactionError);
	EXPECT_EQ(transaction.state(), TransactionState::kFailed);
	// A failed recovery is destroy-only: the transaction cannot be reset and reused.
	EXPECT_THROW(transaction.reset(), std::logic_error);

	// Revert on reset, shown on a fresh transaction: a reset from a legal (non-terminal)
	// state clears retry_limit back to its default, so a later recovery is no longer bounded
	// and succeeds where the already-consumed budget would otherwise fail.
	fdb::Transaction reused(fdbDB.get());
	ASSERT_TRUE(reused);
	ASSERT_EQ(reused.setOption(FDB_TR_OPTION_RETRY_LIMIT, limitValue), 0);
	fdb::FaultInjection revertFault;
	revertFault.readErrors = {kNotCommitted, kNotCommitted};
	reused.setFaultInjection(&revertFault);

	EXPECT_THROW(reused.get(kv::toBytes("opt_revert")), kv::RetryableTransactionError);
	auto beforeReset = reused.onErrorAsync(kNotCommitted);
	ASSERT_NE(beforeReset, nullptr);
	EXPECT_NO_THROW(beforeReset->get());  // consumes the single retry the limit allows

	reused.reset();  // legal from kActive; reverts retry_limit to the default
	EXPECT_THROW(reused.get(kv::toBytes("opt_revert")), kv::RetryableTransactionError);
	auto afterReset = reused.onErrorAsync(kNotCommitted);
	ASSERT_NE(afterReset, nullptr);
	EXPECT_NO_THROW(afterReset->get());  // succeeds only because the limit reverted
	EXPECT_EQ(reused.state(), TransactionState::kActive);
}
