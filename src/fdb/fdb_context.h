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

#include "common/platform.h"

#include <memory>

#include "fdb/fdb.h"

namespace fdb {

/// FDBConfig holds the configuration for the FoundationDB client.
/// It currently contains only the path to the cluster file, but will be extended to include
/// other configuration options in the future.
struct FDBConfig {
	std::string clusterFile;
};

/// Handle to one FoundationDB database connection on the shared process-wide runtime
/// (fdb::Runtime). Creating a context starts the runtime if needed; destroying it never
/// stops the network (a stopped FDB client cannot be restarted in-process), so contexts
/// may be created and destroyed freely. The network stops only via an explicit
/// Runtime::shutdown(); otherwise the runtime is deliberately leaked at process exit (see
/// fdb::Runtime).
class FDBContext {
public:
	/// Creates a shared pointer to an FDBContext instance.
	/// @throws FDBException if the shared runtime cannot be started.
	/// @throws std::logic_error when the runtime was already shut down (it cannot be
	///   restarted in-process; see fdb::Runtime).
	static std::shared_ptr<FDBContext> create(FDBConfig &&config);

	FDBContext(const FDBContext &) = delete;
	FDBContext &operator=(const FDBContext &) = delete;
	FDBContext(FDBContext &&) = delete;
	FDBContext &operator=(FDBContext &&) = delete;

	~FDBContext() = default;

	/// Returns a shared pointer to the FoundationDB database instance, creating it on
	/// first use from the configured cluster file.
	/// @throws FDBException if the database cannot be created.
	std::shared_ptr<DB> getDB();

private:
	explicit FDBContext(FDBConfig &&config);

	/// The configuration for the FoundationDB client.
	FDBConfig config_;

	/// The database instance managed by this context, created lazily by getDB().
	std::shared_ptr<DB> db_;
};

}  // namespace fdb
