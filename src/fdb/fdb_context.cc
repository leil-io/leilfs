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

#include <stdexcept>

#include "slogger/slogger.h"

namespace fdb {

class FDBException : public std::runtime_error {
public:
	FDBException(int error_code, const std::string &message)
	    : std::runtime_error("FoundationDB error: " + message +
	                         " (code: " + std::to_string(error_code) + ")") {}
};

inline void check_fdb_err(int err, std::string msg) {
	if (err != 0) {
		std::string error_str = fmt::format("FoundationDB error: {}. {}", DB::errorMsg(err), msg);
		safs::log_err(error_str);
		throw FDBException(err, error_str);
	}
}

inline void check_fdb_op(auto &&cmd, std::string msg) {
	auto err = cmd;
	check_fdb_err(err, msg);
}

std::shared_ptr<FDBContext> FDBContext::create(FDBConfig &&config) {
	return std::shared_ptr<FDBContext>(new FDBContext(std::move(config)));
}

FDBContext::FDBContext(FDBConfig &&config) : config_(std::move(config)) {
	try {
		check_fdb_op(DB::selectAPIVersion(FDB_API_VERSION), "Failed to set API Version");

		// Configure external client settings if provided (needed on multi-version setups)
		if (const char* extLib = std::getenv("FDB_EXTERNAL_CLIENT_LIBRARY"); extLib && *extLib) {
			check_fdb_op(DB::setNetworkOption(FDB_NET_OPTION_EXTERNAL_CLIENT_LIBRARY, extLib),
			            "Failed to set FDB external client library");
		}
		if (const char* extDir = std::getenv("FDB_EXTERNAL_CLIENT_DIRECTORY"); extDir && *extDir) {
			check_fdb_op(DB::setNetworkOption(FDB_NET_OPTION_EXTERNAL_CLIENT_DIRECTORY, extDir),
			            "Failed to set FDB external client directory");
		}
		if (const char* clusterFile = std::getenv("FDB_CLUSTER_FILE"); clusterFile && *clusterFile) {
			// Allow setting cluster file path globally via network option
			check_fdb_op(DB::setNetworkOption(FDB_NET_OPTION_CLUSTER_FILE, clusterFile),
			            "Failed to set FDB cluster file via network option");
		}

		check_fdb_op(DB::setupNetwork(), "Failed to setup network thread");

		networkThread_ = std::thread([] {
			pthread_setname_np(pthread_self(), "fdb_network");
			try {
				DB::runNetwork();
			} catch (const std::exception &e) {
				safs::log_err("FoundationDB network thread error: {}", e.what());
			}
		});
	} catch (const FDBException &e) {
		safs::log_err("Failed to initialize FDBContext: {}", e.what());
		throw;
	}
}

FDBContext::~FDBContext() {
	if (networkThread_.joinable()) {
		try {
			check_fdb_op(DB::stopNetwork(), "Failed to stop network thread");
			networkThread_.join();
		} catch (const std::exception &e) {
			// Log but don't throw from destructor
			safs::log_err("Error while stopping FDB network thread: {}", e.what());
		}
	}
}

std::shared_ptr<DB> FDBContext::getDB() {
	if (db_) { return db_; }

	try {
		db_ = std::make_shared<DB>(config_.clusterFile);
		check_fdb_err(db_->error(), "Failed to get fdb::DB instance");
		return db_;
	} catch (const FDBException &e) {
		safs::log_err("Failed to get DB instance: {}", e.what());
		throw;
	}
}

}  // namespace fdb
