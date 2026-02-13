/*
   SaunaFS is free software; you can redistribute it and/or modify
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
#include "admin/delete_sessions_command.h"

#include <iostream>

#include "admin/registered_admin_connection.h"
#include "protocol/cltoma.h"
#include "protocol/matocl.h"
#include "common/exception.h"

std::string DeleteSessionsCommand::name() const {
	return "delete-session";
}

void DeleteSessionsCommand::usage() const {
	std::cerr << name() << " <master ip> <master port> <session_id>" << std::endl;
	std::cerr << "    Deletes the specified session." << std::endl;
}

SaunaFsAdminCommand::SupportedOptions DeleteSessionsCommand::supportedOptions() const {
	return {
	    {kTlsMode, kTlsModeDescription},
	};
}

void DeleteSessionsCommand::run(const Options& options) const {
	if (options.arguments().size() != 3) {
		throw WrongUsageException("Expected <master ip>, <master port>, and <session_id> for " + name());
	}

	auto tlsCfg =
	    options.getValue<std::string>("--tlsconfigfile", std::string(TlsSession::kNoFile));

	ServerConnection connection(options.argument(0), options.argument(1), tlsCfg);
	uint64_t sessionId = std::stoull(options.argument(2));

	auto request = cltoma::deleteSession::build(sessionId);
	auto response = connection.sendAndReceive(request, SAU_MATOCL_DELETE_SESSION);

	uint8_t status;
	matocl::deleteSession::deserialize(response, status);
	if (status == SAUNAFS_STATUS_OK) {
		std::cout << "Session deleted successfully\n";
	} else {
		std::cerr << "Failed to delete session. Status: " << status << "\n";
	}
}
