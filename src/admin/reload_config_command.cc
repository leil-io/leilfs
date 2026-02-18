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
#include "admin/reload_config_command.h"

#include <unistd.h>
#include <iomanip>
#include <iostream>

#include "admin/registered_admin_connection.h"
#include "protocol/cltoma.h"
#include "protocol/matocl.h"

std::string ReloadConfigCommand::name() const {
	return "reload-config";
}

SaunaFsAdminCommand::SupportedOptions ReloadConfigCommand::supportedOptions() const {
	return {{kTlsMode, kTlsModeDescription}};
}

void ReloadConfigCommand::usage() const {
	std::cerr << name() << " <master ip> <master port>" << std::endl;
	std::cerr << "    Requests reloading configuration from the config file." << std::endl;
	std::cerr << "    This is synchronous (waits for reload to finish)." << std::endl;
	std::cerr << "    Authentication with the admin password is required." << std::endl;
}

void ReloadConfigCommand::run(const Options& options) const {
	if (options.arguments().size() != 2) {
		throw WrongUsageException(
				"Expected <metadataserver ip> and <metadataserver port> for " + name());
	}

	auto tlsCfg =
	    options.getValue<std::string>("--tlsconfigfile", std::string(TlsSession::kNoFile));

	auto connection =
	    RegisteredAdminConnection::create(options.argument(0), options.argument(1), tlsCfg);
	auto adminReloadResponse =
			connection->sendAndReceive(cltoma::adminReload::build(), SAU_MATOCL_ADMIN_RELOAD);
	uint8_t status;
	matocl::adminStopWithoutMetadataDump::deserialize(adminReloadResponse, status);
	std::cerr << saunafs_error_string(status) << std::endl;
	if (status != SAUNAFS_STATUS_OK) {
		exit(1);
	}
}
