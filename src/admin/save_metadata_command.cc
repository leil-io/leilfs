/*
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"
#include "admin/save_metadata_command.h"

#include <iostream>
#include <limits>

#include "admin/registered_admin_connection.h"
#include "protocol/cltoma.h"
#include "protocol/matocl.h"

std::string SaveMetadataCommand::name() const {
	return "save-metadata";
}

void SaveMetadataCommand::usage() const {
	std::cerr << name() << " <metadataserver ip> <metadataserver port>\n"
			"    Requests saving the current state of metadata into the metadata.sfs file.\n"
			"    With --async fail if the process cannot be started, e.g. because the process\n"
			"    is already in progress. Without --async, fails if either the process cannot be\n"
			"    started or if it finishes with an error (i.e., no metadata file is created).\n"
			"    With --timeout <seconds> set how long to wait for the operation (default: 5).\n"
			"    Authentication with the admin password is required." << std::endl;
}

SaunaFsAdminCommand::SupportedOptions SaveMetadataCommand::supportedOptions() const {
	return {{"--async", "Don't wait for the task to finish."},
	        {"--timeout=", "Operation timeout"},
	        {kTlsMode, kTlsModeDescription}};
}

void SaveMetadataCommand::run(const Options& options) const {
	if (options.arguments().size() != 2) {
		throw WrongUsageException(
				"Expected <metadataserver ip> and <metadataserver port> for " + name());
	}

	auto tlsCfg =
	    options.getValue<std::string>("--tlsconfigfile", std::string(TlsSession::kNoFile));

	bool async = options.isSet("--async");
	int timeout = ServerConnection::kDefaultTimeout;
	if (options.isSet("--timeout")) {
		// getValue<int> throws on non-numeric or out-of-int-range input; treat any such
		// failure as out of range so it is rejected by the single check below.
		int seconds = -1;
		try {
			seconds = options.getValue<int>("--timeout");
		} catch (const std::exception &) {
			// fall through with seconds == -1
		}
		if (seconds <= 0 || seconds > std::numeric_limits<int>::max() / 1000) {
			throw WrongUsageException("--timeout must be between 1 and 2147483 seconds");
		}
		timeout = seconds * 1000;
	}
	auto connection =
	    RegisteredAdminConnection::create(options.argument(0), options.argument(1), tlsCfg, timeout);
	auto request = cltoma::adminSaveMetadata::build(async);
	auto response = connection->sendAndReceive(request, SAU_MATOCL_ADMIN_SAVE_METADATA);
	uint8_t status;
	matocl::adminSaveMetadata::deserialize(response, status);
	std::cerr << saunafs_error_string(status) << std::endl;
	if (status != SAUNAFS_STATUS_OK) {
		exit(1);
	}
}
