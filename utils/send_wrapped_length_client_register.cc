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

#include "common/datapack.h"
#include "common/massert.h"
#include "common/md5.h"
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
	if (argc != 4 || (std::string(argv[3]) != "session" && std::string(argv[3]) != "metasession")) {
		std::cerr << "Usage: " << argv[0] << " <host> <port> <session|metasession>\n";
		return 1;
	}
	bool metaSession = std::string(argv[3]) == "metasession";

	uint32_t ip;
	uint16_t port;
	eassert(tcpresolve(argv[1], argv[2], &ip, &port, 0) == 0);

	std::vector<uint8_t> body;
	body.insert(body.end(), FUSE_REGISTER_BLOB_ACL, FUSE_REGISTER_BLOB_ACL + REGISTER_BLOB_SIZE);
	body.push_back(metaSession ? REGISTER_NEWMETASESSION : REGISTER_NEWSESSION);
	appendPut32(body, saunafsVersion(5, 0, 0));

	// kRegisterNewSessionMinSize/kRegisterNewMetaSessionMinSize (matoclserv.cc)
	// are 77/73 = blob(64) + rcode(1) + version(4) + infoLength(4)
	// [+ pathLength(4), session only]. The session check is a "<" comparison,
	// so any infoLength that wraps the RHS below length defeats it (here,
	// wrapping it to 0). The metasession check is instead an exact-match "!="
	// against two candidate sizes (with/without a trailing MD5 passcode), so
	// infoLength must be chosen to land exactly on one of those wrapped
	// values: base + kDefaultMd5DigestSize + infoLength == base (mod 2^32),
	// i.e. infoLength == -kDefaultMd5DigestSize.
	appendPut32(body, metaSession ? 0u - static_cast<uint32_t>(kDefaultMd5DigestSize)
	                              : 0xFFFFFFFFu - 76u);
	if (!metaSession) {
		// pathLength: the body must be exactly kRegisterNewSessionMinSize (77
		// bytes, with this field present but unread) so it clears the plain
		// "packet too short" check and actually reaches the wrapped-length
		// comparison above; no info/path bytes follow, since infoLength alone
		// claims more data than is present.
		appendPut32(body, 0);
	}
	// For metasession, the body is already exactly kRegisterNewMetaSessionMinSize
	// (73 bytes) with no further fields, for the same reason.

	std::vector<uint8_t> packet;
	appendPut32(packet, CLTOMA_FUSE_REGISTER);
	appendPut32(packet, static_cast<uint32_t>(body.size()));
	packet.insert(packet.end(), body.begin(), body.end());

	int fd = tcpsocket();
	eassert(fd >= 0);
	tcpnodelay(fd);
	eassert(tcpnumconnect(fd, ip, port) == 0);
	eassert(tcptowrite(fd, packet.data(), packet.size(), 10000) == (int32_t)packet.size());

	// A master that rejects the malformed packet kills this connection; block
	// until it actually closes instead of guessing a fixed delay. No data is
	// ever sent back on this connection, so a clean EOF (read() returning 0)
	// is the only non-error outcome.
	uint8_t unused;
	eassert(tcptoread(fd, &unused, 1, 10000) == 0);
	tcpclose(fd);

	return 0;
}
