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

#include "list_sessions_command.h"

#include "common/human_readable_format.h"
#include "common/legacy_string.h"
#include "common/legacy_vector.h"
#include "common/platform.h"
#include "common/sessions_file.h"
#include "common/serialization_macros.h"
#include "common/server_connection.h"

#include "protocol/matocl.h"
#include "protocol/cltoma.h"
#include "protocol/matocl.h"
#include <iostream>

typedef std::array<uint32_t, 16> OperationStats;

std::string ListSessionsCommand::name() const {
	return "list-sessions";
}

void ListSessionsCommand::usage() const {
	std::cerr << name() << " <master ip> <master port>\n";
	std::cerr << "    Lists all currently open sessions\n";
}

SaunaFsAdminCommand::SupportedOptions ListSessionsCommand::supportedOptions() const {
	return {{kTlsMode, kTlsModeDescription}};
}

void ListSessionsCommand::run(const Options& options) const {
	if (options.arguments().size() != 2) {
		throw WrongUsageException("Expected <master ip> and <master port> for " + name());
	}

	auto tlsCfg =
	    options.getValue<std::string>("--tlsconfigfile", std::string(TlsSession::kNoFile));

	ServerConnection connection(options.argument(0), options.argument(1), tlsCfg);
	LegacyVector<SessionFiles> sessions;

	auto request = cltoma::listSessions::build();
	auto response = connection.sendAndReceive(request, SAU_MATOCL_SESSION_FILES);
	matocl::listSessions::deserialize(response, sessions);

	for (const auto& session : sessions) {
		std::cout << "Session ID: " << session.sessionId << std::endl;
		std::cout << "IP: " << ipToString(session.peerIp) << std::endl;
		std::cout << "Open files: " << session.filesNumber << std::endl;
		std::cout << std::endl;
	}
}
