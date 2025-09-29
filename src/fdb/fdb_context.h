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

#include "fdb/fdb.h"

#include <memory>
#include <thread>

namespace fdb {

/// FDBConfig holds the configuration for the FoundationDB client.
/// It currently contains only the path to the cluster file, but will be extended to include
/// other configuration options in the future.
struct FDBConfig {
	std::string clusterFile;
};

/// FDBContext manages the FoundationDB client library context.
/// It initializes the network thread and provides access to the database instance.
/// On destruction, it stops the network thread and cleans up resources.
class FDBContext {
public:
	/// Creates a shared pointer to an FDBContext instance.
	__attribute__((no_sanitize("address")))
	static std::shared_ptr<FDBContext> create(FDBConfig &&config);

	/// Not needed constructors and assignment operators.
	/// Deleted to prevent copying or moving of the FDBContext object.
	FDBContext(const FDBContext &) = delete;
	FDBContext &operator=(const FDBContext &) = delete;
	FDBContext(FDBContext &&) = delete;
	FDBContext &operator=(FDBContext &&) = delete;

	/// Destructor that stops the network thread and cleans up resources.
	~FDBContext();

	/// Returns a shared pointer to the FoundationDB database instance.
	/// If the database instance is not yet created, it initializes it using the provided
	/// configuration.
	std::shared_ptr<DB> getDB();

private:
	/// Initializes the FoundationDB client library and starts the network thread.
	/// @param config Configuration for the FoundationDB client.
	__attribute__((no_sanitize("address")))
	explicit FDBContext(FDBConfig &&config);

	/// The thread that runs the FoundationDB network loop.
	/// It is created when the FDBContext is initialized and runs until the context is destroyed.
	/// This thread is responsible for handling network events and processing transactions.
	std::thread networkThread_;

	/// The configuration for the FoundationDB client.
	FDBConfig config_;

	/// The database instance managed by this context.
	/// It is created lazily when the getDB() method is called.
	std::shared_ptr<DB> db_;
};

}  // namespace fdb
