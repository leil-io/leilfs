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

#pragma once

#include <memory>

#include "fdb/fdb.h"
#include "fdb/fdb_transaction.h"
#include "kv/ikv_engine.h"
#include "slogger/slogger.h"

namespace fdb {

class FDBKVEngine final : public kv::IKVEngine {
public:
	/// Constructs a new FDBKVEngine with the provided database instance.
	/// @param db A shared pointer to the FoundationDB database instance.
	explicit FDBKVEngine(std::shared_ptr<fdb::DB> _db) : db_(_db) {}

	/// Default destructor.
	~FDBKVEngine() = default;

	// Not needed constructors and assignment operators.
	FDBKVEngine(const FDBKVEngine &) = delete;
	FDBKVEngine &operator=(const FDBKVEngine &) = delete;
	FDBKVEngine(FDBKVEngine &&) = delete;
	FDBKVEngine &operator=(FDBKVEngine &&) = delete;

	/// Creates a new transaction for reading.
	/// @return A pointer to the created transaction.
	std::unique_ptr<kv::IReadOnlyTransaction> createReadOnlyTransaction() override {
		return createReadWriteTransaction();
	}

	/// Creates a new transaction for reading and writing.
	/// @return A pointer to the created transaction.
	std::unique_ptr<kv::IReadWriteTransaction> createReadWriteTransaction() override {
		auto transaction = std::make_unique<fdb::FDBTransaction>(db_.get());

		if (!*transaction) {
			safs::log_err("FDBKVEngine::createReadWriteTransaction: Failed to create transaction");
			return nullptr;
		}

		return transaction;
	}

private:
	std::shared_ptr<fdb::DB> db_;
};

}  // namespace fdb
