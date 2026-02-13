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

#include "admin/mount_info_list_command.h"

#include "common/platform.h"
#include "common/sessions_file.h"
#include "common/serialization_macros.h"
#include "common/server_connection.h"

#include "protocol/matocl.h"
#include "protocol/cltoma.h"
#include "protocol/matocl.h"
#include <iostream>

std::string MountInfoListCommand::name() const {
	return "list-mount-info";
}

SaunaFsAdminCommand::SupportedOptions MountInfoListCommand::supportedOptions() const {
	return {{kTlsMode, kTlsModeDescription}};
}

void MountInfoListCommand::usage() const {
	std::cerr << name() << " <master ip> <master port>\n";
	std::cerr << "    Lists mountpoint information from all currently open sessions\n";
}

void MountInfoListCommand::run(const Options& options) const {
	if (options.arguments().size() != 2) {
		throw WrongUsageException("Expected <master ip> and <master port> for " + name());
	}

	auto tlsCfg =
	    options.getValue<std::string>("--tlsconfigfile", std::string(TlsSession::kNoFile));

	ServerConnection connection(options.argument(0), options.argument(1), tlsCfg);
	std::vector<MountInfoEntry> mountInfoList;

	auto request = cltoma::mountInfoList::build();
	auto response = connection.sendAndReceive(request, SAU_MATOCL_MOUNT_INFO_LIST);
	matocl::mountInfoList::deserialize(response, mountInfoList);

	for (const auto& mountInfo : mountInfoList) {	
		std::cout << "Session ID: " << mountInfo.sessionId << std::endl;
		std::cout << mountInfo.mountInfo << std::endl;
		std::cout << std::endl;
	}
}
