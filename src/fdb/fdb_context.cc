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

#include "fdb/fdb_context.h"

#include "fdb/fdb_errors.h"
#include "fdb/fdb_runtime.h"
#include "slogger/slogger.h"

namespace fdb {

std::shared_ptr<FDBContext> FDBContext::create(FDBConfig &&config) {
	return std::shared_ptr<FDBContext>(new FDBContext(std::move(config)));
}

FDBContext::FDBContext(FDBConfig &&config) : config_(std::move(config)) {
	try {
		Runtime::ensureStarted();
	} catch (const FDBException &e) {
		safs::log_err("Failed to initialize FDBContext: {}", e.what());
		throw;
	}
}

std::shared_ptr<DB> FDBContext::getDB() {
	if (db_) { return db_; }

	try {
		db_ = std::make_shared<DB>(config_.clusterFile);
		checkFdbError(db_->error(), "Failed to get fdb::DB instance");
		return db_;
	} catch (const FDBException &e) {
		safs::log_err("Failed to get DB instance: {}", e.what());
		throw;
	}
}

}  // namespace fdb
