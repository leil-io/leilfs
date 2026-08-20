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
#include "admin/list_metadataservers_command.h"

#include <iomanip>
#include <iostream>

#include "protocol/cltoma.h"
#include "common/human_readable_format.h"
#include "common/saunafs_version.h"
#include "protocol/matocl.h"
#include "common/server_connection.h"
#include "common/sockets.h"
#include "admin/metadataserver_status_command.h"

std::string ListMetadataserversCommand::name() const {
	return "list-metadataservers";
}

void ListMetadataserversCommand::usage() const {
	std::cerr << name() << " <master ip> <master port>" << std::endl;
	std::cerr << "    Prints status of active metadata servers" << std::endl;
}

SaunaFsAdminCommand::SupportedOptions ListMetadataserversCommand::supportedOptions() const {
	return {{kPorcelainMode, kPorcelainModeDescription}, {kTlsMode, kTlsModeDescription}};
}

template<class T>
void printInfo(bool porcelain, const std::string& name, T value) {
	if (porcelain) {
		std::cout << value << "\n";
	} else {
		std::cout << "\t" << name << ": " << value << "\n";
	}
}

template<class T1, class T2, class... Args>
void printInfo(bool porcelain,
		const std::string& name1, T1 value1,
		const std::string& name2, T2 value2,
		const Args&... args) {
	if (porcelain) {
		std::cout << value1 << " ";
	} else {
		std::cout << "\t" << name1 << ": " << value1 << "\n";
	}
	printInfo(porcelain, name2, value2, args...);
}

void ListMetadataserversCommand::run(const Options& options) const {
	if (options.arguments().size() != 2) {
		throw WrongUsageException("Expected <master ip> and <master port> for " + name());
	}

	uint32_t ip = 0;
	uint16_t port = 0;
	std::string ipString = options.argument(0);
	std::string portString = options.argument(1);
	auto tlsCfg =
	    options.getValue<std::string>("--tlsconfigfile", std::string(TlsSession::kNoFile));
	tcpresolve(ipString.c_str(), portString.c_str(), &ip, &port, false);
	ServerConnection connection(NetworkAddress(ip, port), tlsCfg);
	auto request = cltoma::metadataserversList::build();
	auto response = connection.sendAndReceive(request, SAU_MATOCL_METADATASERVERS_LIST);

	std::vector<MetadataserverListEntry> shadowsList;
	uint32_t masterVersion;
	matocl::metadataserversList::deserialize(response, masterVersion, shadowsList);

	// A small hack: place the master at the beginning of the metadataservers list
	shadowsList.emplace_back(ip, port, masterVersion);
	std::reverse(shadowsList.begin(), shadowsList.end());
	int server = 1;
	for (const auto& e : shadowsList) {
		MetadataserverStatus s{"unknown", "unknown", 0, std::nullopt};
		std::string hostname;
		// If information about MATOCL_SERV_PORT used by shadows isn't available we cannot query it
		// for its hostname, metaversion etc.
		if (e.port != 0) {
			bool statusKnown = false;
			try {
				ServerConnection shadowConnection(NetworkAddress(e.ip, e.port), tlsCfg);
				// The list reply already carries each member's software version, so the
				// status query can be built in the right shape without probing the member.
				s = MetadataserverStatusCommand::getStatus(ipToString(e.ip), std::to_string(e.port),
				                                           tlsCfg, e.version);
				statusKnown = true;

				auto request = cltoma::hostname::build();
				auto response = shadowConnection.sendAndReceive(request, SAU_MATOCL_HOSTNAME);
				matocl::hostname::deserialize(response, hostname);
			} catch (const std::exception &) {
				// One unreachable member must not hide the rest of the list, and a failed
				// hostname lookup alone must not discard a status already fetched.
				if (!statusKnown) {
					s = MetadataserverStatus{"unreachable", "unreachable", 0, std::nullopt};
				}
				hostname = "unreachable";
			}
		} else {
			hostname = "unknown";
		}

		if (!options.isSet(kPorcelainMode)) {
			std::cout << "Server " << server++ << ":" << std::endl;
		}
		printInfo(options.isSet(kPorcelainMode),
				"IP", ipToString(e.ip),
				"Port", e.port,
				"Hostname", hostname,
				"Personality", s.personality,
				"Status", s.serverStatus,
				"Metadata version", s.metadataVersion,
				"Version", saunafsVersionToString(e.version));
	}
}
