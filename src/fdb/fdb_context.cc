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

#include "fdb/fdb_context.h"

#include "slogger/slogger.h"

namespace fdb {

inline void check_fdb_err(int err, std::string msg) {
	if (err != 0) {
		safs::log_err("FoundationDB error: {}. {}", DB::errorMsg(err), msg);
		std::terminate();
	}
}

inline void check_fdb_op(auto &&cmd, std::string msg) {
	auto err = cmd;
	check_fdb_err(err, msg);
}

std::shared_ptr<FDBContext> FDBContext::create(FDBConfig &&config) {
	auto context = std::shared_ptr<FDBContext>(new FDBContext(std::move(config)));
	return context;
}

FDBContext::FDBContext(FDBConfig &&config) : config_(std::move(config)) {
	check_fdb_op(DB::selectAPIVersion(FDB_API_VERSION), "Failed to set API Version");
	check_fdb_op(DB::setupNetwork(), "Failed to setup network thread");

	networkThread_ = std::thread([] {
		pthread_setname_np(pthread_self(), "fdb_network");
		DB::runNetwork();
	});
}

FDBContext::~FDBContext() {
	if (networkThread_.joinable()) {
		check_fdb_op(DB::stopNetwork(), "Failed to stop network thread");
		networkThread_.join();
	}
}

std::shared_ptr<DB> FDBContext::getDB() {
	if (db_) { return db_; }

	db_ = std::make_shared<DB>(config_.clusterFile);
	check_fdb_err(db_->error(), "Failed to get fdb::DB instance");
	return db_;
}

}  // namespace fdb
