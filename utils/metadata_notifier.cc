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

#include "common/datapack.h"
#include "common/serialization.h"
#include "common/sockets.h"
#include "protocol/SFSCommunication.h"
#include "protocol/packet.h"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <netdb.h>
#include <unistd.h>

constexpr uint8_t kHeaderSize = 8;
constexpr uint32_t kCfgDefaultMasterTimeout = 60U;
constexpr int kInvalidFD = -1;

/// Structure for the packet being sent or received.
struct packetstruct {
	struct packetstruct *next;
	uint8_t *startptr;
	uint32_t bytesleft;
	uint8_t *packet;
};

packetstruct *createRegisterPacket(uint32_t type, uint32_t size) {
	packetstruct *outpacket;
	uint8_t *ptr;
	uint32_t psize;

	outpacket = (packetstruct *)malloc(sizeof(packetstruct));
	assert(outpacket);
	psize = size + 8;
	outpacket->packet = (uint8_t *)malloc(psize);
	assert(outpacket->packet);
	outpacket->bytesleft = psize;
	ptr = outpacket->packet;
	put32bit(&ptr, type);
	put32bit(&ptr, size);
	outpacket->startptr = (uint8_t *)(outpacket->packet);
	outpacket->next = nullptr;
	put8bit(&ptr, 1);
	put16bit(&ptr, SAUNAFS_PACKAGE_VERSION_MAJOR);
	put8bit(&ptr, SAUNAFS_PACKAGE_VERSION_MINOR);
	put8bit(&ptr, SAUNAFS_PACKAGE_VERSION_MICRO);
	put16bit(&ptr, kCfgDefaultMasterTimeout);

	return outpacket;
}

void writeToSocket(int sock, packetstruct *pack) {
	if (pack == nullptr) { return; }

	while (pack->bytesleft > 0) {
		int32_t writtenBytes = write(sock, pack->startptr, pack->bytesleft);

		if (writtenBytes < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				fprintf(stderr, "write to Master error: %s\n", strerror(errno));
				free(pack->packet);
				free(pack);
				return;
			}
			continue;
		}

		pack->startptr += writtenBytes;
		pack->bytesleft -= writtenBytes;
	}

	free(pack->packet);
	free(pack);
}

void sendRegister(int sock) {
	packetstruct *packet =
	    createRegisterPacket(NTTOMA_REGISTER, 1 + 4 + 2);
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

	// Start listening for MATONT_METACHANGES_LOG packets
	std::vector<uint8_t> recvBuffer;
	recvBuffer.reserve(4096);
	while (true) {
		uint8_t temp[4096];
		ssize_t n = read(sockfd, temp, sizeof(temp));
		if (n <= 0) break;  // connection closed or error
		recvBuffer.insert(recvBuffer.end(), temp, temp + n);

		while (recvBuffer.size() >= 8) {
			const uint8_t *ptr = recvBuffer.data();
			const uint8_t *parsePtr = ptr;
			uint32_t packetType = 0, dataLen = 0;
			get32bit(&parsePtr, packetType);
			get32bit(&parsePtr, dataLen);

			if (packetType != MATONT_METACHANGES_LOG) {
				fprintf(stderr, "Unknown packet type: %u\n", packetType);
				// Remove header to avoid infinite loop
				recvBuffer.erase(recvBuffer.begin(), recvBuffer.begin() + 8);
				continue;
			}

			// Process the packet
			if (recvBuffer.size() < 8 + dataLen) break;  // wait for full packet

			// Deserialize packet body
			parsePtr = ptr + 8;
			uint8_t rver = *parsePtr++;
			if (rver != 0xFF) {
				fprintf(stderr, "Invalid packet format\n");
				recvBuffer.erase(recvBuffer.begin(), recvBuffer.begin() + 8 + dataLen);
				continue;
			}
			uint64_t logVersion = 0;
			memcpy(&logVersion, parsePtr, sizeof(logVersion));
			parsePtr += sizeof(logVersion);

			std::string str(reinterpret_cast<const char *>(parsePtr), dataLen - 9);

			// Process packet
			fprintf(stderr, "%s\n", str.c_str());

			// Remove processed packet
			recvBuffer.erase(recvBuffer.begin(), recvBuffer.begin() + 8 + dataLen);
		}
	}

	close(sockfd);

	return EXIT_SUCCESS;
}
