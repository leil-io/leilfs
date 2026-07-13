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

#include "kv/ifuture.h"

namespace fdb::detail {

/// Translates a failed FoundationDB READ into the typed error contract: retryable
/// errors throw kv::RetryableTransactionError so the master's op boundary replays the
/// op on a fresh transaction, and every other backend error throws the
/// kv::TransactionError base so a backend failure can never be mistaken for a missing
/// key (absent keys are the only thing reported as an empty result).
[[noreturn]] inline void throwTransactionError(fdb_error_t err) {
	// transaction_timed_out (1031) is deliberately absent from FDB's RETRYABLE
	// predicate because a timeout during COMMIT has an unknown result. This helper
	// guards READ paths only, and the op-boundary replay runs on a fresh
	// transaction with a fresh timeout budget, so a timed-out read is safe to
	// replay. The commit path (FDBCommitFuture::getResult) uses RETRYABLE_NOT_COMMITTED,
	// which excludes 1031 and the whole maybe-committed class, so a commit with an
	// unknown result is never blindly replayed.
	constexpr fdb_error_t kTransactionTimedOut = 1031;
	if (err == kTransactionTimedOut ||
	    fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE, err) != 0) {
		throw kv::RetryableTransactionError(static_cast<int>(err), fdb_get_error(err));
	}
	throw kv::TransactionError(static_cast<int>(err), fdb_get_error(err));
}

}  // namespace fdb::detail
