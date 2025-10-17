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

packetstruct *createGetPathTypeInodePacket(uint64_t inode) {
	packetstruct *outpacket = (packetstruct *)malloc(sizeof(packetstruct));
	assert(outpacket);
	uint32_t psize = 8 + sizeof(uint64_t);
	outpacket->packet = (uint8_t *)malloc(psize);
	assert(outpacket->packet);
	outpacket->bytesleft = psize;
	uint8_t *ptr = outpacket->packet;
	put32bit(&ptr, NTTOMA_GET_PATH_TYPE_INODE);
	put32bit(&ptr, 8);
	put64bit(&ptr, inode);
	outpacket->startptr = outpacket->packet;
	outpacket->next = nullptr;
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
	packetstruct *packet = createRegisterPacket(NTTOMA_REGISTER, 1 + 4 + 2);
	writeToSocket(sock, packet);
}

void sendGetPathTypeInode(int sock, uint64_t inode) {
	packetstruct *packet = createGetPathTypeInodePacket(inode);
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

		while (recvBuffer.size() >= 8) {
			const uint8_t *ptr = recvBuffer.data();
			const uint8_t *parsePtr = ptr;
			uint32_t packetType = 0, dataLen = 0;
			get32bit(&parsePtr, packetType);
			get32bit(&parsePtr, dataLen);

			if (recvBuffer.size() < 8 + dataLen) break;  // wait for full packet

			parsePtr = ptr + 8;

			if (packetType == MATONT_METACHANGES_LOG) {
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

				// Print log string
				fprintf(stderr, "%s\n", str.c_str());

				// If log string starts with ACCESS(<inode>), send request for path/type
				std::smatch match;
				if (std::regex_search(str, match, access_regex)) {
					uint64_t inode = std::stoull(match[1].str());
					sendGetPathTypeInode(sockfd, inode);
				}
			} else if (packetType == MATONT_GET_PATH_TYPE_INODE) {
				// Response: inode:64 nodetype:8 path:STDSTRING
				uint64_t inode = get64bit(&parsePtr);
				uint8_t nodetype = *parsePtr++;
				std::string path(reinterpret_cast<const char *>(parsePtr), dataLen - 9);
				// Print inode, type, and path
				fprintf(stderr, "inode %lu: type=%c path=%s\n", inode, static_cast<char>(nodetype), path.c_str());
			} else {
				fprintf(stderr, "Unknown packet type: %u\n", packetType);
			}

			recvBuffer.erase(recvBuffer.begin(), recvBuffer.begin() + 8 + dataLen);
		}
	}

	close(sockfd);

	return EXIT_SUCCESS;
}
