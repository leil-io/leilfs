/*
   Copyright 2026 Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include <iostream>
#include <string>
#include <vector>

#include "common/massert.h"
#include "common/saunafs_version.h"
#include "common/sockets.h"
#include "protocol/cstoma.h"
#include "protocol/packet.h"

int main(int argc, char **argv) {
	if (argc != 3) {
		std::cerr << "Usage: " << argv[0] << " <host> <port>\n";
		return 1;
	}

	uint32_t ip;
	uint16_t port;
	eassert(tcpresolve(argv[1], argv[2], &ip, &port, 0) == 0);

	uint32_t csIp = 0;
	uint16_t csPort = 9611;
	uint32_t timeout = 10000;
	uint32_t csVersion = saunafsVersion(5, 0, 0);
	std::string clusterId = "x";

	std::vector<uint8_t> packet =
	    cstoma::registerHost::build(csIp, csPort, timeout, csVersion, clusterId);

	// The clusterId string field is the last one serialized. Its 4-byte length
	// prefix sits right after the fixed-size fields; the real serializer always
	// writes size + 1 (never 0), so overwrite it in place to reproduce what only
	// a malicious peer can send.
	uint32_t clusterIdLengthOffset = PacketHeader::kSize + serializedSize(PacketVersion(0)) +
	                                 serializedSize(csIp) + serializedSize(csPort) +
	                                 serializedSize(timeout) + serializedSize(csVersion);
	packet[clusterIdLengthOffset] = 0;
	packet[clusterIdLengthOffset + 1] = 0;
	packet[clusterIdLengthOffset + 2] = 0;
	packet[clusterIdLengthOffset + 3] = 0;

	int fd = tcpsocket();
	eassert(fd >= 0);
	tcpnodelay(fd);
	eassert(tcpnumconnect(fd, ip, port) == 0);
	eassert(tcptowrite(fd, packet.data(), packet.size(), 10000) == (int32_t)packet.size());
	tcpclose(fd);

	return 0;
}
