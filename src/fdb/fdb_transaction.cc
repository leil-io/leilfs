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

std::optional<kv::Value> FDBTransaction::get(const kv::Key &key) {
	if (!tr_) { return std::nullopt; }

	return tr_.get(key);
}

std::optional<kv::Value> FDBTransaction::getSnapshot(const kv::Key &key) {
	if (!tr_) { return std::nullopt; }

	return tr_.get(key, /*snapshot=*/true);
}

std::unique_ptr<kv::IFuture> FDBTransaction::getAsync(const kv::Key &key) {
	if (!tr_) { return nullptr; }

	return tr_.getAsync(key);
}

kv::GetRangeResult FDBTransaction::getRange(const kv::KeySelector &start,
                                            const kv::KeySelector &end, int limit) {
	if (!tr_) { return {{}, false}; }

	return tr_.getRange(start, end, limit);
}

std::unique_ptr<kv::IRangeFuture> FDBTransaction::getRangeAsync(const kv::KeySelector &start,
                                                                const kv::KeySelector &end,
                                                                int limit) {
	if (!tr_) { return nullptr; }

	return tr_.getRangeAsync(start, end, limit);
}

void FDBTransaction::set(const kv::Key &key, const kv::Value &value) {
	if (!tr_) { return; }

	mutationCount_++;
	tr_.set(key, value);
}

void FDBTransaction::atomicAdd(const kv::Key &key, const kv::Value &delta) {
	if (!tr_) { return; }

	mutationCount_++;
	tr_.atomicAdd(key, delta);
}

void FDBTransaction::atomicMax(const kv::Key &key, const kv::Value &value) {
	if (!tr_) { return; }

	mutationCount_++;
	tr_.atomicMax(key, value);
}

void FDBTransaction::remove(const kv::Key &key) {
	if (!tr_) { return; }

	mutationCount_++;
	tr_.remove(key);
}

void FDBTransaction::removeRange(const kv::Key &start, const kv::Key &end) {
	if (!tr_) { return; }

	mutationCount_++;
	tr_.removeRange(start, end);
}

bool FDBTransaction::commit() {
	if (!tr_) { return false; }

	return tr_.commit();
}

std::unique_ptr<kv::ICommitFuture> FDBTransaction::commitAsync() {
	if (!tr_) { return std::make_unique<kv::ImmediateCommitFuture>(false); }

	return tr_.commitAsync();
}

std::optional<int64_t> FDBTransaction::getCommittedVersion() const {
	return tr_.getCommittedVersion();
}

std::optional<uint64_t> FDBTransaction::getApproximateSize() const {
	if (!tr_) { return std::nullopt; }

	return tr_.getApproximateSize();
}

}  // namespace fdb
