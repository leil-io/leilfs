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
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "fdb/fdb.h"
#include "fdb/fdb_context.h"
#include "fdb/fdb_kv_engine.h"
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

		error = 0;
		auto secondRead = future->get(&error);
		ASSERT_NE(error, 0);
		ASSERT_TRUE(secondRead.getPairs().empty());
		ASSERT_FALSE(secondRead.hasMore());
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
