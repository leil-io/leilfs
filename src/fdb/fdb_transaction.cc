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

#include "fdb/fdb_transaction.h"
#include "kv/ifuture.h"

namespace fdb {

// The adapter adds no handle guards to data operations: an absent backend handle
// (moved-from wrapper) is a wrong-state call and the wrapped fdb::Transaction throws
// std::logic_error for it, per the kv error-domain rules. Swallowing it here would pose
// as a missing key, an empty range, or a silently dropped write. Mutations are counted
// only after the wrapper accepts them, so a throwing call leaves the count untouched.
// getApproximateSize() remains a diagnostic exception that returns std::nullopt when no
// estimate can be reported.

std::optional<kv::Value> FDBTransaction::get(const kv::Key &key) { return tr_.get(key); }

std::optional<kv::Value> FDBTransaction::getSnapshot(const kv::Key &key) {
	return tr_.get(key, /*snapshot=*/true);
}

std::unique_ptr<kv::IFuture> FDBTransaction::getAsync(const kv::Key &key) {
	return tr_.getAsync(key);
}

std::unique_ptr<kv::IFuture> FDBTransaction::getSnapshotAsync(const kv::Key &key) {
	return tr_.getAsync(key, /*snapshot=*/true);
}

kv::GetRangeResult FDBTransaction::getRange(const kv::KeySelector &start,
                                            const kv::KeySelector &end, int limit) {
	return tr_.getRange(start, end, limit);
}

std::unique_ptr<kv::IRangeFuture> FDBTransaction::getRangeAsync(const kv::KeySelector &start,
                                                                const kv::KeySelector &end,
                                                                int limit) {
	return tr_.getRangeAsync(start, end, limit);
}

void FDBTransaction::set(const kv::Key &key, const kv::Value &value) {
	tr_.set(key, value);
	mutationCount_++;
}

void FDBTransaction::atomicAdd(const kv::Key &key, const kv::Value &delta) {
	tr_.atomicAdd(key, delta);
	mutationCount_++;
}

void FDBTransaction::atomicMax(const kv::Key &key, const kv::Value &value) {
	tr_.atomicMax(key, value);
	mutationCount_++;
}

void FDBTransaction::remove(const kv::Key &key) {
	tr_.remove(key);
	mutationCount_++;
}

void FDBTransaction::removeRange(const kv::Key &start, const kv::Key &end) {
	tr_.removeRange(start, end);
	mutationCount_++;
}

void FDBTransaction::addReadConflictKey(const kv::Key &key) { tr_.addReadConflictKey(key); }

bool FDBTransaction::commit() { return tr_.commit(); }

std::unique_ptr<kv::ICommitFuture> FDBTransaction::commitAsync() { return tr_.commitAsync(); }

std::optional<int64_t> FDBTransaction::getCommittedVersion() const {
	return tr_.getCommittedVersion();
}

std::optional<uint64_t> FDBTransaction::getApproximateSize() const {
	if (!tr_) { return std::nullopt; }

	return tr_.getApproximateSize();
}

}  // namespace fdb
