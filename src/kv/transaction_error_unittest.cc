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
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include <gtest/gtest.h>

#include "kv/ifuture.h"

namespace {

// FoundationDB error codes used as representative examples.
constexpr int kNotCommitted = 1020;
constexpr int kTransactionTimedOut = 1031;
constexpr int kTransactionTooLarge = 2101;

}  // namespace

TEST(KVTransactionErrorTest, BaseIsNotRetryable) {
	kv::TransactionError error(kTransactionTooLarge, "transaction too large");
	EXPECT_EQ(error.errorCode(), kTransactionTooLarge);
	EXPECT_FALSE(error.retryable());
	EXPECT_STREQ(error.what(), "transaction too large");
}

TEST(KVTransactionErrorTest, RetryableSubclassIsRetryable) {
	kv::RetryableTransactionError error(kNotCommitted, "not committed");
	EXPECT_EQ(error.errorCode(), kNotCommitted);
	EXPECT_TRUE(error.retryable());
	EXPECT_STREQ(error.what(), "not committed");
}

TEST(KVTransactionErrorTest, CatchingBaseCatchesRetryable) {
	bool caught = false;
	try {
		throw kv::RetryableTransactionError(kTransactionTimedOut, "transaction timed out");
	} catch (const kv::TransactionError &error) {
		caught = true;
		EXPECT_EQ(error.errorCode(), kTransactionTimedOut);
		EXPECT_TRUE(error.retryable());
	}
	EXPECT_TRUE(caught);
}

TEST(KVTransactionErrorTest, CatchingRetryableSkipsBase) {
	// A non-retryable base error must NOT be caught by a retryable-only handler;
	// the op boundary relies on this to keep replay decisions strict.
	bool caughtRetryable = false;
	bool caughtBase = false;
	try {
		throw kv::TransactionError(kTransactionTooLarge, "transaction too large");
	} catch (const kv::RetryableTransactionError &) {
		caughtRetryable = true;
	} catch (const kv::TransactionError &) { caughtBase = true; }
	EXPECT_FALSE(caughtRetryable);
	EXPECT_TRUE(caughtBase);
}
