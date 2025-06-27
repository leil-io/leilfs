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

#include "kv/itransaction.h"

#include <memory>

namespace kv {

/// Interface for a key-value engine based on read-only and read-write transactions.
/// Provides an abstraction level to create concrete KV engines, like FoundationDB, RocksDB, or
/// others.
class IKVEngine {
public:
	/// Default constructor.
	IKVEngine() = default;

	/// Virtual destructor to allow proper cleanup of derived classes.
	virtual ~IKVEngine() = default;

	// Unneeded constructors and assignment operators.
	IKVEngine(const IKVEngine &) = delete;
	IKVEngine &operator=(const IKVEngine &) = delete;
	IKVEngine(IKVEngine &&) = delete;
	IKVEngine &operator=(IKVEngine &&) = delete;

	/// Creates a new transaction for reading.
	/// @return A pointer to the created transaction.
	virtual std::unique_ptr<IReadOnlyTransaction> createReadOnlyTransaction() = 0;

	/// Creates a new transaction for reading and writing.
	/// @return A pointer to the created transaction.
	virtual std::unique_ptr<IReadWriteTransaction> createReadWriteTransaction() = 0;
};

}  // namespace kv
