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

#include "fdb/fdb_transaction.h"

namespace fdb {

std::optional<kv::Value> FDBTransaction::get(const kv::Key &key) {
	if (!tr_) { return std::nullopt; }

	return tr_.get(key);
}

kv::GetRangeResult FDBTransaction::getRange(const kv::KeySelector &start,
                                            const kv::KeySelector &end, int limit) {
	if (!tr_) { return {{}, false}; }

	return tr_.getRange(start, end, limit);
}

void FDBTransaction::set(const kv::Key &key, const kv::Value &value) {
	if (!tr_) { return; }

	tr_.set(key, value);
}

void FDBTransaction::remove(const kv::Key &key) {
	if (!tr_) { return; }

	tr_.remove(key);
}

bool FDBTransaction::commit() {
	if (!tr_) { return false; }

	return tr_.commit();
}

}  // namespace fdb
