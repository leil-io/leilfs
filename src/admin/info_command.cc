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
#include "admin/info_command.h"

#include <iostream>

#include "common/human_readable_format.h"
#include "common/saunafs_statistics.h"
#include "common/saunafs_version.h"
#include "common/server_connection.h"

std::string InfoCommand::name() const {
	return "info";
}

SaunaFsAdminCommand::SupportedOptions InfoCommand::supportedOptions() const {
	return {
		{kPorcelainMode, kPorcelainModeDescription},
		{kTlsMode, kTlsModeDescription},
	};
}

void InfoCommand::usage() const {
	std::cerr << name() << " <master ip> <master port>\n";
	std::cerr << "    Prints statistics concerning the SaunaFS installation.\n";
}

void InfoCommand::run(const Options& options) const {
	if (options.arguments().size() != 2) {
		throw WrongUsageException("Expected <master ip> and <master port> for " + name());
	}

	const std::string host = options.argument(0);
	const std::string port = options.argument(1);
	auto tlsCfg =
	    options.getValue<std::string>("--tlsconfigfile", std::string(TlsSession::kNoFile));
	ServerConnection connection(host, port, tlsCfg);
	std::vector<uint8_t> request, response;
	serializeLegacyPacket(request, CLTOMA_INFO);
	response = connection.sendAndReceive(request, MATOCL_INFO);
	SaunaFsStatistics info;
	deserializeAllLegacyPacketDataNoHeader(response, info);
	if (options.isSet(kPorcelainMode)) {
		std::cout << saunafsVersionToString(info.version) << ' '
		          << info.memoryUsage << ' ' << info.totalSpace << ' '
		          << info.availableSpace << ' ' << info.trashSpace << ' '
		          << info.trashNodes << ' ' << info.reservedSpace << ' '
		          << info.reservedNodes << ' ' << info.allNodes << ' '
		          << info.dirNodes << ' ' << info.fileNodes << ' '
		          << info.symlinkNodes << ' ' << info.chunks << ' '
		          << info.chunkCopies << ' '
		          << info.chunkCopies  // deprecated 'regular' copies
		          << std::endl;
	} else {
		std::cout << "SaunaFS v" << saunafsVersionToString(info.version) << '\n'
		          << "Memory usage:\t" << convertToIec(info.memoryUsage)
		          << "B\n"
		          << "Total space:\t" << convertToIec(info.totalSpace) << "B\n"
		          << "Available space:\t" << convertToIec(info.availableSpace)
		          << "B\n"
		          << "Trash space:\t" << convertToIec(info.trashSpace) << "B\n"
		          << "Trash files:\t" << info.trashNodes << '\n'
		          << "Reserved space:\t" << convertToIec(info.reservedSpace)
		          << "B\n"
		          << "Reserved files:\t" << info.reservedNodes << '\n'
		          << "FS objects:\t" << info.allNodes << '\n'
		          << "Directories:\t" << info.dirNodes << '\n'
		          << "Files:\t" << info.fileNodes << '\n'
		          << "Symlinks:\t" << info.symlinkNodes << '\n'
		          << "Chunks:\t" << info.chunks << '\n'
		          << "Chunk copies:\t" << info.chunkCopies << '\n'
		          << "Regular copies (deprecated):\t" << info.chunkCopies
		          << std::endl;
	}
}
