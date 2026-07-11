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

#pragma once

#include "common/platform.h"

// Needs to be included before foundationdb/fdb_c.h
#include <fdb/fdb_api_version.h>

#include <foundationdb/fdb_c.h>

#include <stdexcept>
#include <string>

#include "kv/ifuture.h"

namespace fdb {

/// Initialization/lifecycle failure of the FDB client itself (runtime start, database
/// creation), as opposed to per-transaction errors (kv::TransactionError family).
class FDBException : public std::runtime_error {
public:
	FDBException(int errorCode, const std::string &message)
	    : std::runtime_error("FoundationDB error: " + message +
	                         " (code: " + std::to_string(errorCode) + ")"),
	      errorCode_(errorCode) {}

	/// The fdb_c error code that caused this failure.
	int errorCode() const noexcept { return errorCode_; }

private:
	int errorCode_;
};

/// Throws FDBException when a lifecycle-level fdb_c call failed.
inline void checkFdbError(fdb_error_t err, const std::string &message) {
	if (err != 0) {
		throw FDBException(static_cast<int>(err), message + ": " + std::string(fdb_get_error(err)));
	}
}

}  // namespace fdb

namespace fdb::detail {

/// Whether a failed READ may be replayed on a fresh transaction.
/// transaction_timed_out (1031) is deliberately absent from FDB's RETRYABLE
/// predicate because a timeout during COMMIT has an unknown result. This predicate
/// guards READ paths only, and the op-boundary replay runs on a fresh transaction
/// with a fresh timeout budget, so a timed-out read is safe to replay. The commit
/// path (FDBCommitFuture::getResult) uses RETRYABLE_NOT_COMMITTED, which excludes
/// 1031 and the whole maybe-committed class, so a commit with an unknown result is
/// never blindly replayed.
inline bool isRetryableReadError(fdb_error_t err) {
	constexpr fdb_error_t kTransactionTimedOut = 1031;
	return err == kTransactionTimedOut ||
	       fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE, err) != 0;
}

/// Translates a failed FoundationDB READ into the typed error contract: retryable
/// errors throw kv::RetryableTransactionError so the master's op boundary replays the
/// op on a fresh transaction, and every other backend error throws the
/// kv::TransactionError base so a backend failure can never be mistaken for a missing
/// key (absent keys are the only thing reported as an empty result).
[[noreturn]] inline void throwTransactionError(fdb_error_t err) {
	if (isRetryableReadError(err)) {
		throw kv::RetryableTransactionError(static_cast<int>(err), fdb_get_error(err));
	}
	throw kv::TransactionError(static_cast<int>(err), fdb_get_error(err));
}

}  // namespace fdb::detail
