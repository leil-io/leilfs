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
#include "admin/list_chunkservers_command.h"

#include <iostream>
#include <vector>

#include "protocol/cltoma.h"
#include "common/human_readable_format.h"
#include "common/saunafs_version.h"
#include "protocol/matocl.h"
#include "common/server_connection.h"

std::string ListChunkserversCommand::name() const {
	return "list-chunkservers";
}

SaunaFsAdminCommand::SupportedOptions ListChunkserversCommand::supportedOptions() const {
	return {
		{kPorcelainMode, kPorcelainModeDescription},
		{kTlsMode, kTlsModeDescription},
	};
}

void ListChunkserversCommand::usage() const {
	std::cerr << name() << " <master ip> <master port>\n";
	std::cerr << "    Prints information about all connected chunkservers.\n";
}

void ListChunkserversCommand::run(const Options& options) const {
	if (options.arguments().size() != 2) {
		throw WrongUsageException("Expected <master ip> and <master port> for " + name());
	}

	auto tlsCfg =
	    options.getValue<std::string>("--tlsconfigfile", std::string(TlsSession::kNoFile));

	for (const auto& cs : getChunkserversList(options.argument(0), options.argument(1), tlsCfg)) {
		auto address = NetworkAddress(cs.servip, cs.servport);
		if (cs.version == kDisconnectedChunkserverVersion) {
			if (options.isSet(kPorcelainMode)) {
				std::cout << address.toString() << " - 0 0 0 0 0 0 0 -" << std::endl;
			} else {
				std::cout << "Server " << address.toString() << ": disconnected" << std::endl;
			}
		} else {
			if (options.isSet(kPorcelainMode)) {
				std::cout << address.toString()
						<< ' ' << saunafsVersionToString(cs.version)
						<< ' ' << cs.chunkscount
						<< ' ' << cs.usedspace
						<< ' ' << cs.totalspace
						<< ' ' << cs.todelchunkscount
						<< ' ' << cs.todelusedspace
						<< ' ' << cs.todeltotalspace
						<< ' ' << cs.errorcounter
						<< ' ' << cs.label
						<< std::endl;
			} else {
				std::cout << "Server " << address.toString() << ":"
						<< "\n\tversion: " << saunafsVersionToString(cs.version)
						<< "\n\tlabel: " << cs.label
						<< "\n\tchunks: " << convertToSi(cs.chunkscount)
						<< "\n\tused space: " << convertToIec(cs.usedspace) << "B"
						<< " / " << convertToIec(cs.totalspace) << "B"
						<< "\n\tchunks marked for removal: " << convertToSi(cs.todelchunkscount)
						<< "\n\tused space marked for removal: "
						<< convertToIec(cs.todelusedspace) << "B"
						<< " / " << convertToIec(cs.todeltotalspace) << "B"
						<< "\n\terrors: " << convertToSi(cs.errorcounter)
						<< std::endl;
			}
		}
	}
}

std::vector<ChunkserverListEntry> ListChunkserversCommand::getChunkserversList (
		const std::string& masterHost, const std::string& masterPort, const std::string& tlsCfg) {
	ServerConnection connection(masterHost, masterPort, tlsCfg);
	auto request = cltoma::cservList::build(true);
	auto response = connection.sendAndReceive(request, SAU_MATOCL_CSERV_LIST);
	std::vector<ChunkserverListEntry> result;
	matocl::cservList::deserialize(response, result);
	return result;
}
