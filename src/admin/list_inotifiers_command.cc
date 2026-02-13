/*
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
#include "admin/list_inotifiers_command.h"

#include <iostream>

#include "common/human_readable_format.h"
#include "common/saunafs_version.h"
#include "common/server_connection.h"
#include "common/sockets.h"
#include "protocol/cltoma.h"
#include "protocol/matocl.h"

std::string ListInotifiersCommand::name() const {
	return "list-inotifiers";
}

void ListInotifiersCommand::usage() const {
	std::cerr << name() << " <master ip> <master port>" << std::endl;
	std::cerr << "    EXPERIMENTAL: Prints status of active inotify notifiers" << std::endl;
}

SaunaFsAdminCommand::SupportedOptions ListInotifiersCommand::supportedOptions() const {
	return {{kPorcelainMode, kPorcelainModeDescription}, {kTlsMode, kTlsModeDescription}};
}

template <class T>
void printInfo(bool porcelain, const std::string &name, T value) {
	if (porcelain) {
		std::cout << value << "\n";
	} else {
		std::cout << "\t" << name << ": " << value << "\n";
	}
}

template <class T1, class T2, class... Args>
void printInfo(bool porcelain, const std::string &name1, T1 value1, const std::string &name2,
               T2 value2, const Args &...args) {
	if (porcelain) {
		std::cout << value1 << " ";
	} else {
		std::cout << "\t" << name1 << ": " << value1 << "\n";
	}
	printInfo(porcelain, name2, value2, args...);
}

void ListInotifiersCommand::run(const Options &options) const {
	constexpr uint8_t kMaxArgs = 2;
	constexpr uint8_t kIpArgOffset = 0;
	constexpr uint8_t kPortArgOffset = 1;
	
	if (options.arguments().size() != kMaxArgs) {
		throw WrongUsageException("Wrong number of arguments");
	}

	uint32_t ip = 0;
	uint16_t port = 0;
	const std::string &masterIp = options.argument(kIpArgOffset);
	const std::string &masterPort = options.argument(kPortArgOffset);
	auto tlsCfg =
	    options.getValue<std::string>("--tlsconfigfile", std::string(TlsSession::kNoFile));

	tcpresolve(masterIp.c_str(), masterPort.c_str(), &ip, &port, false);
	ServerConnection conn(NetworkAddress(ip, port), tlsCfg);
	auto request = cltoma::inotifierList::build();
	auto response = conn.sendAndReceive(request, SAU_MATOCL_INOTIFIER_LIST);

	std::vector<INotifierListEntry> inotifierList;
	matocl::inotifierList::deserialize(response, inotifierList);
	std::ranges::reverse(inotifierList);

	int server = 1;
	for (const auto &i : inotifierList) {
		if (!options.isSet(kPorcelainMode)) {
			std::cout << "Server " << server++ << ":" << std::endl;
		}
		printInfo(options.isSet(kPorcelainMode), "INotifier version",
		          saunafsVersionToString(i.version), "IP", ipToString(i.ip));
	}
}
