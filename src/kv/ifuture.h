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

#include <optional>
#include <stdexcept>
#include <string>

#include "kv/kv_types.h"

namespace kv {

/// Base type for backend transaction failures, carrying the backend error code and
/// whether the failed operation may be replayed.
///
/// Error-domain rules for the kv/fdb layers:
///  - Backend failures (the operation reached the backend and failed there) are reported
///    as a TransactionError; retryable() says whether the caller may replay the op.
///  - Invalid argument values (e.g. a bad versionstamp offset) are std::invalid_argument.
///  - Calls in the wrong transaction state are std::logic_error.
/// The local domains carry no backend error code, so they can never be mistaken for (or
/// fed into) backend retry handling.
///
/// Read contract: an empty result (nullopt) means only that the key does not exist.
/// Every backend read failure throws this family, the retryable subclass for transient
/// errors the op boundary may replay, this base type for everything else, so a backend
/// failure can never be mistaken for a missing key.
class TransactionError : public std::runtime_error {
public:
	TransactionError(int errorCode, const std::string &message)
	    : std::runtime_error(message), errorCode_(errorCode) {}

	int errorCode() const noexcept { return errorCode_; }

	/// Whether the caller may replay the failed operation (transient conflict/timeout
	/// class, as opposed to a permanent backend failure).
	virtual bool retryable() const noexcept { return false; }

private:
	int errorCode_;
};

/// Thrown by a read future's get() when the underlying transaction failed with a
/// RETRYABLE backend error (e.g. FoundationDB transaction_timed_out / not_committed /
/// transaction_too_old). The single-threaded master event loop catches this at the op
/// boundary and replays the op on a fresh transaction (bounded) instead of letting a
/// transient read failure abort the whole MDS. Non-retryable backend failures throw the
/// TransactionError base instead.
class RetryableTransactionError : public TransactionError {
public:
	RetryableTransactionError(int errorCode, const std::string &message)
	    : TransactionError(errorCode, message) {}

	bool retryable() const noexcept override { return true; }
};

/// Interface for asynchronous future results from key-value operations.
/// Provides methods to check readiness and retrieve values from pending operations.
///
/// @note This future is single-use: get() can only be called once.
/// @note The transaction that created this future must remain alive until get() is called.
class IFuture {
public:
	virtual ~IFuture() = default;

	// Non-copyable, non-movable
	IFuture(const IFuture &) = delete;
	IFuture &operator=(const IFuture &) = delete;
	IFuture(IFuture &&) = delete;
	IFuture &operator=(IFuture &&) = delete;

	/// Checks if the future result is ready without blocking.
	/// @return True if the result is ready, false otherwise.
	virtual bool isReady() = 0;

	/// Blocks until the result is ready and retrieves the value.
	/// @param error Optional out-param for the backend error code: 0 on success; on a
	///   backend failure the code is stored before the throw. Exceptions are the failure
	///   signal, the code is diagnostic only.
	/// @return The value, or std::nullopt only when the key does not exist.
	/// @throws RetryableTransactionError if the backend read failed with a retryable
	///   error (callers catch it at the op boundary and replay), TransactionError for
	///   any other backend failure. A failed read never looks like a missing key.
	/// @note This method can only be called once. Subsequent calls are wrong-state calls
	///   and throw std::logic_error.
	/// @note The caller must keep the transaction alive until this method returns.
	virtual std::optional<Value> get(int *error = nullptr) = 0;

protected:
	IFuture() = default;
};

/// Interface for asynchronous future results from key-value range operations.
///
/// @note This future is single-use: get() can only be called once.
/// @note The transaction that created this future must remain alive until get() is called.
class IRangeFuture {
public:
	virtual ~IRangeFuture() = default;

	// Non-copyable, non-movable
	IRangeFuture(const IRangeFuture &) = delete;
	IRangeFuture &operator=(const IRangeFuture &) = delete;
	IRangeFuture(IRangeFuture &&) = delete;
	IRangeFuture &operator=(IRangeFuture &&) = delete;

	/// Checks if the future result is ready without blocking.
	/// @return True if the result is ready, false otherwise.
	virtual bool isReady() = 0;

	/// Blocks until the result is ready and retrieves the range.
	/// @param error Optional out-param for the backend error code: 0 on success; on a
	///   backend failure the code is stored before the throw. Exceptions are the failure
	///   signal, the code is diagnostic only.
	/// @return The range result; empty only when no keys fall in the range.
	/// @throws RetryableTransactionError if the backend read failed with a retryable
	///   error (callers catch it at the op boundary and replay), TransactionError for
	///   any other backend failure. A failed read never looks like an empty range.
	/// @note This method can only be called once. Subsequent calls are wrong-state calls
	///   and throw std::logic_error.
	/// @note The caller must keep the transaction alive until this method returns.
	virtual GetRangeResult get(int *error = nullptr) = 0;

protected:
	IRangeFuture() = default;
};

/// Interface for an in-flight asynchronous commit.
///
/// Lets a single-threaded event loop submit a commit and keep doing other work,
/// polling isReady() each iteration and finalizing with getResult() once ready,
/// instead of blocking on a synchronous commit().
///
/// @note Single-use: getResult() may be called only once.
/// @note The transaction that produced this future must stay alive until getResult() returns.
class ICommitFuture {
public:
	virtual ~ICommitFuture() = default;

	// Non-copyable, non-movable
	ICommitFuture(const ICommitFuture &) = delete;
	ICommitFuture &operator=(const ICommitFuture &) = delete;
	ICommitFuture(ICommitFuture &&) = delete;
	ICommitFuture &operator=(ICommitFuture &&) = delete;

	/// Checks whether the commit result is available without blocking.
	virtual bool isReady() = 0;

	/// Finalizes the commit, blocking if not yet ready.
	/// @param error Optional out-param for the backend error code (0 on success).
	/// @param retryable Optional out-param: on failure, true if the error is a
	///   transient conflict the caller may retry by replaying the op on a fresh
	///   transaction (e.g. FDB not_committed / transaction_too_old); false for a
	///   permanent failure. Set to false on success. Keeps the retry decision
	///   backend-agnostic so callers need not interpret raw backend error codes.
	/// @return True if the commit succeeded and is durable, false otherwise.
	/// @note Single-use; subsequent calls return false.
	virtual bool getResult(int *error = nullptr, bool *retryable = nullptr) = 0;

	/// Registers a one-shot wakeup fired when the commit result becomes ready, so a
	/// blocked poll() can be woken to call getResult() promptly instead of waiting
	/// out the poll timeout. The callback may run on another thread (the FDB network
	/// thread) and may fire immediately and inline if the result is already ready,
	/// so it must be cheap, thread-safe, and must not touch this future or its
	/// transaction (e.g. just write an eventfd). Best-effort: a missed wakeup only
	/// delays finalization to the next poll, it never loses the result.
	virtual void setReadyCallback(void (*callback)(void *), void *arg) = 0;

protected:
	ICommitFuture() = default;
};

/// Trivial already-completed commit future, for backends that commit synchronously
/// (in-memory Master, test mocks). Keeps the deferred-commit code path uniform.
class ImmediateCommitFuture final : public ICommitFuture {
public:
	/// @param success Whether the (already-completed) commit succeeded.
	/// @param retryable On failure, whether the caller may retry. Normally false for
	///   a synchronous backend; set true to synthesize a transient conflict (used by
	///   the commit-retry fault-injection hook).
	explicit ImmediateCommitFuture(bool success, bool retryable = false)
	    : success_(success), retryable_(retryable) {}

	bool isReady() override { return true; }

	// Defaults are declared on the ICommitFuture base only; redefining them on the
	// override would bind statically and risk surprising callers.
	bool getResult(int *error, bool *retryable) override {
		if (error != nullptr) { *error = success_ ? 0 : 1; }
		if (retryable != nullptr) { *retryable = success_ ? false : retryable_; }
		// Single-use, matching FDBCommitFuture: report the outcome once, then false.
		if (consumed_) { return false; }
		consumed_ = true;
		return success_;
	}

	void setReadyCallback(void (*callback)(void *), void *arg) override {
		if (callback != nullptr) { callback(arg); }
	}

private:
	bool success_;
	bool retryable_;
	bool consumed_ = false;
};

}  // namespace kv
