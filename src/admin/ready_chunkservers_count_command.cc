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
#include "admin/ready_chunkservers_count_command.h"

#include <iostream>
#include <vector>

#include "admin/list_chunkservers_command.h"
#include "common/tls_session.h"

std::string ReadyChunkserversCountCommand::name() const {
	return "ready-chunkservers-count";
}

SaunaFsAdminCommand::SupportedOptions ReadyChunkserversCountCommand::supportedOptions() const {
	return {{kTlsMode, kTlsModeDescription}};
}

void ReadyChunkserversCountCommand::usage() const {
	std::cerr << name() << " <master ip> <master port>\n";
	std::cerr << "    Prints number of chunkservers ready to be written to.\n";
}

void ReadyChunkserversCountCommand::run(const Options& options) const {
	if (options.arguments().size() != 2) {
		throw WrongUsageException("Expected exactly two arguments for " + name());
	}
	uint32_t readyChunkservers = 0;

	auto tlsCfg =
	    options.getValue<std::string>("--tlsconfigfile", std::string(TlsSession::kNoFile));

	auto chunkservers = ListChunkserversCommand::getChunkserversList(options.argument(0),
	                                                                 options.argument(1), tlsCfg);
	for (const auto& cs : chunkservers) {
		if (cs.totalspace > 0) {
			++readyChunkservers;
		}
	}
	std::cout << readyChunkservers << std::endl;
}
