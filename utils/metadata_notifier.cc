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

/*
   Experimental: This is a utility binary for testing notifier connections to master
   and its functionality for receiving changelog notifications.
*/

#include "common/platform.h"

#include <cstring>
#include <iostream>
#include <regex>
#include <string>
#include <vector>

#include <netdb.h>
#include <unistd.h>

#include "common/datapack.h"
#include "common/serialization.h"
#include "common/sockets.h"
#include "protocol/SFSCommunication.h"
#include "protocol/packet.h"

constexpr uint8_t kHeaderSize = 8;
constexpr uint16_t kPacketVersionMajor = SAUNAFS_PACKAGE_VERSION_MAJOR;
constexpr uint8_t kPacketVersionMinor = SAUNAFS_PACKAGE_VERSION_MINOR;
constexpr uint8_t kPacketVersionMicro = SAUNAFS_PACKAGE_VERSION_MICRO;
constexpr uint16_t kCfgDefaultMasterTimeout = 60U;
constexpr int kInvalidFD = -1;

/// Structure for the packet being sent or received.
struct PacketStruct {
	std::vector<uint8_t> buffer;
	size_t startOffset = 0;
	size_t bytesLeft = 0;

	PacketStruct(uint32_t type, uint32_t size)
	    : buffer(kHeaderSize + size), startOffset(0), bytesLeft(kHeaderSize + size) {
		uint8_t *ptr = buffer.data();
		put32bit(&ptr, type);
		put32bit(&ptr, size);
	}
};

PacketStruct createRegisterPacket() {
	uint8_t rversion = 1;
	constexpr uint32_t kRegisterPayloadSize =
	    sizeof(rversion) + sizeof(kPacketVersionMajor) + sizeof(kPacketVersionMinor) +
	    sizeof(kPacketVersionMicro) + sizeof(kCfgDefaultMasterTimeout);
	PacketStruct packet(NTTOMA_REGISTER, kRegisterPayloadSize);

	uint8_t *ptr = packet.buffer.data() + kHeaderSize;
	put8bit(&ptr, rversion);
	put16bit(&ptr, kPacketVersionMajor);
	put8bit(&ptr, kPacketVersionMinor);
	put8bit(&ptr, kPacketVersionMicro);
	put16bit(&ptr, kCfgDefaultMasterTimeout);

	return packet;
}

PacketStruct createGetPathTypeInodePacket(uint64_t inode) {
	PacketStruct packet(NTTOMA_GET_PATH_TYPE_INODE, sizeof(inode));
	uint8_t *ptr = packet.buffer.data() + kHeaderSize;
	put64bit(&ptr, inode);
	return packet;
}

void writeToSocket(int sock, PacketStruct &pack) {
	size_t offset = pack.startOffset;
	size_t bytesLeft = pack.bytesLeft;

	while (bytesLeft > 0) {
		ssize_t writtenBytes = write(sock, pack.buffer.data() + offset, bytesLeft);

		if (writtenBytes < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				fprintf(stderr, "write to Master error: %s\n", strerror(errno));
				return;
			}
			continue;
		}

		offset += writtenBytes;
		bytesLeft -= writtenBytes;
	}
}

void sendRegister(int sock) {
	PacketStruct packet = createRegisterPacket();
	writeToSocket(sock, packet);
}

void sendGetPathTypeInode(int sock, uint64_t inode) {
	PacketStruct packet = createGetPathTypeInodePacket(inode);
	writeToSocket(sock, packet);
}

int connectToServer(const char *host, int port) {
	struct addrinfo hints = {};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	struct addrinfo *res;
	char port_str[16];
	snprintf(port_str, sizeof(port_str), "%d", port);

	if (getaddrinfo(host, port_str, &hints, &res) != 0) {
		std::cerr << "getaddrinfo failed\n";
		return kInvalidFD;
	}

	int sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (sockfd < 0) {
		std::cerr << "socket failed\n";
		freeaddrinfo(res);
		return kInvalidFD;
	}

	if (connect(sockfd, res->ai_addr, res->ai_addrlen) < 0) {
		std::cerr << "connect failed\n";
		close(sockfd);
		freeaddrinfo(res);
		return kInvalidFD;
	}

	freeaddrinfo(res);
	return sockfd;
}

int main(int argc, char *argv[]) {
	if (argc != 3) {
		fprintf(stderr, "Usage: %s <host> <port>\n", argv[0]);
		return EXIT_FAILURE;
	}

	const char *host = argv[1];
	int port = atoi(argv[2]);

	int sockfd = connectToServer(host, port);
	if (sockfd < 0) { return EXIT_FAILURE; }

	fprintf(stderr, "Connected to %s:%d\n", host, port);

	sendRegister(sockfd);
	fprintf(stderr, "Register packet sent\n");

	std::vector<uint8_t> recvBuffer;
	recvBuffer.reserve(4096);
	std::regex access_regex(R"(ACCESS\((\d+)\))");

	while (true) {
		uint8_t temp[4096];
		ssize_t n = read(sockfd, temp, sizeof(temp));
		if (n <= 0) break;  // connection closed or error
		recvBuffer.insert(recvBuffer.end(), temp, temp + n);

		while (recvBuffer.size() >= kHeaderSize) {
			const uint8_t *ptr = recvBuffer.data();
			const uint8_t *parsePtr = ptr;
			uint32_t packetType = 0, dataLen = 0;
			get32bit(&parsePtr, packetType);
			get32bit(&parsePtr, dataLen);

			if (recvBuffer.size() < kHeaderSize + dataLen) break;  // wait for full packet

			parsePtr = ptr + kHeaderSize;

			if (packetType == MATONT_METACHANGES_LOG) {
				uint8_t rver = *parsePtr++;
				if (rver != 0xFF) {
					fprintf(stderr, "Invalid packet format\n");
					recvBuffer.erase(recvBuffer.begin(),
					                 recvBuffer.begin() + kHeaderSize + dataLen);
					continue;
				}
				uint64_t logVersion = 0;
				memcpy(&logVersion, parsePtr, sizeof(logVersion));
				parsePtr += sizeof(logVersion);

				std::string str(reinterpret_cast<const char *>(parsePtr), dataLen - 9);

				// Print log string
				fprintf(stderr, "%s\n", str.c_str());

				// If log string starts with ACCESS(<inode>), send request for path/type
				std::smatch match;
				if (std::regex_search(str, match, access_regex)) {
					uint64_t inode = std::stoull(match[1].str());
					sendGetPathTypeInode(sockfd, inode);
				}
			} else if (packetType == MATONT_GET_PATH_TYPE_INODE) {
				uint64_t inode = get64bit(&parsePtr);
				uint8_t nodetype = *parsePtr++;
				std::string path(reinterpret_cast<const char *>(parsePtr), dataLen - 9);
				// Print inode, type, and path
				fprintf(stderr, "inode %lu: type=%c path=%s\n", inode, static_cast<char>(nodetype),
				        path.c_str());
			} else {
				fprintf(stderr, "Unknown packet type: %u\n", packetType);
			}

			recvBuffer.erase(recvBuffer.begin(), recvBuffer.begin() + kHeaderSize + dataLen);
		}
	}

	close(sockfd);

	return EXIT_SUCCESS;
}
