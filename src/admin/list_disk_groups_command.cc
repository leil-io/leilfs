/*
   Copyright 2023 Leil Storage OÜ

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

#include <yaml-cpp/yaml.h>
#include <iostream>

#include "admin/list_chunkservers_command.h"
#include "admin/list_disk_groups_command.h"
#include "common/saunafs_version.h"
#include "common/serialization.h"
#include "common/server_connection.h"

std::string ListDiskGroupsCommand::name() const { return "list-disk-groups"; }

SaunaFsAdminCommand::SupportedOptions ListDiskGroupsCommand::supportedOptions() const {
	return {
	    {kTlsMode, kTlsModeDescription},
	};
}

void ListDiskGroupsCommand::usage() const {
	std::cerr << name() << " <master ip> <master port>\n";
	std::cerr << "    Prints disk groups configuration in chunkservers.\n";
}

void ListDiskGroupsCommand::run(const Options &options) const {
	if (options.arguments().size() != 2) {
		throw WrongUsageException(
		    "Expected <master ip> and <master port> for " + name());
	}

	auto tlsCfg =
	    options.getValue<std::string>("--tlsconfigfile", std::string(TlsSession::kNoFile));

	auto chunkservers = ListChunkserversCommand::getChunkserversList(
	    options.argument(0), options.argument(1), tlsCfg);

	YAML::Emitter yaml;
	yaml << YAML::BeginMap;  // start root map

	yaml << YAML::Key << "chunkservers";
	yaml << YAML::Value << YAML::BeginSeq;  // start chunkservers list

	for (const auto &chunkserver : chunkservers) {
		if (chunkserver.version == kDisconnectedChunkserverVersion) {
			continue;  // skip disconnected chunkservers
		}

		std::vector<uint8_t> request;
		std::vector<uint8_t> response;
		serializeLegacyPacket(request, CLTOCS_ADMIN_LIST_DISK_GROUPS);

		auto csAddress =
		    NetworkAddress(chunkserver.servip, chunkserver.servport);
		ServerConnection connection(csAddress);
		response = connection.sendAndReceive(request,
		                                     CSTOCL_ADMIN_LIST_DISK_GROUPS);

		std::string info;

		try {
			deserializeAllLegacyPacketDataNoHeader(response, info);
		} catch (const IncorrectDeserializationException &e) {
			std::cerr << e.what() << "\n";
			continue;
		}

		yaml << YAML::BeginMap;  // start chunkserver map
		yaml << YAML::Key << "chunkserver";
		yaml << YAML::Value << csAddress.toString();

		static constexpr const char *kDiskGroupsKey = "disk_groups";
		yaml << YAML::Key << kDiskGroupsKey;

		YAML::Node diskGroups = YAML::Load(info);

		if (diskGroups.IsMap() && diskGroups[kDiskGroupsKey]) {
			yaml << YAML::Value << diskGroups[kDiskGroupsKey];
		} else {
			yaml << YAML::Value << info;
		}

		yaml << YAML::EndMap;  // end chunkserver map
	}

	yaml << YAML::EndSeq;  // end chunkservers list
	yaml << YAML::EndMap;  // end root map

	std::cout << yaml.c_str() << "\n";
}
