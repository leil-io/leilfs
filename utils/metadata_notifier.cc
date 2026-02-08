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
#include "common/tls_session.h"
#include "protocol/SFSCommunication.h"
#include "protocol/packet.h"

constexpr uint8_t kHeaderSize = 8;
constexpr uint16_t kPacketVersionMajor = SAUNAFS_PACKAGE_VERSION_MAJOR;
constexpr uint8_t kPacketVersionMinor = SAUNAFS_PACKAGE_VERSION_MINOR;
constexpr uint8_t kPacketVersionMicro = SAUNAFS_PACKAGE_VERSION_MICRO;
constexpr uint16_t kCfgDefaultMasterTimeout = 60U;
constexpr int kInvalidFD = -1;

// TLS session related values
/// If no TLS is used, this is `nullptr`.
std::unique_ptr<TlsSession> tlsSession;
int lastHandshakeError = 0;

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

PacketStruct createStartTlsPacket() {
	constexpr uint32_t kStartTlsPayloadSize = 0;
	PacketStruct packet(NTTOMA_STARTTLS, kStartTlsPayloadSize);
	return packet;
}

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

inline ssize_t writeMaster(int sock, const uint8_t *buf, size_t len) {
	// STARTTLS must be sent over plain TCP. After handshake, use TLS.
	if (tlsSession) {
		int ret = SSL_write(tlsSession->session(), buf, static_cast<int>(len));
		if (ret > 0) { return ret; }
		int err = SSL_get_error(tlsSession->session(), ret);
		if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
			errno = EAGAIN;
			return -1;
		}
		// Map other TLS errors to EIO for retry handling
		errno = EIO;
		return -1;
	}
	return ::write(sock, buf, len);
}

inline ssize_t readMaster(int sock, uint8_t *buf, size_t len) {
	// After TLS handshake, use SSL_read; otherwise plain TCP.
	if (tlsSession) {
		int ret = SSL_read(tlsSession->session(), buf, static_cast<int>(len));
		if (ret > 0) { return ret; }
		int err = SSL_get_error(tlsSession->session(), ret);
		if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
			errno = EAGAIN;
			return -1;
		}
		// Clean shutdown
		if (err == SSL_ERROR_ZERO_RETURN) { return 0; }
		// Map other TLS errors to EIO for retry handling
		errno = EIO;
		return -1;
	}
	return ::read(sock, buf, len);
}

void writeToSocket(int sock, PacketStruct &pack) {
	size_t offset = pack.startOffset;
	size_t bytesLeft = pack.bytesLeft;

	while (bytesLeft > 0) {
		ssize_t writtenBytes = writeMaster(sock, pack.buffer.data() + offset, bytesLeft);

		if (writtenBytes < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				fprintf(stderr, "write to Master error: %s\n", strerror(errno));
				return;
			}
			continue;
		}

		if (writtenBytes == 0) {
			// peer closed
			fprintf(stderr, "write to Master returned 0 (closed)\n");
			return;
		}

		offset += static_cast<size_t>(writtenBytes);
		bytesLeft -= static_cast<size_t>(writtenBytes);
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

void sendStartTls(int sock) {
	// Always send STARTTLS over plain TCP
	std::unique_ptr<TlsSession> saved = std::move(tlsSession);  // temporarily disable TLS
	PacketStruct packet = createStartTlsPacket();
	writeToSocket(sock, packet);
	tlsSession = std::move(saved);
}

bool sendTlsHandshake() {
	sassert(tlsSession != nullptr);

	int ret = SSL_connect(tlsSession->session());

	if (ret == 1) {
		// Handshake completed successfully
		return true;
	}

	int err = SSL_get_error(tlsSession->session(), ret);

	lastHandshakeError = err;

	if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
		// Non-fatal, handshake can be retried later
		fprintf(stderr, "TLS handshake in progress (notifier): %s",
		        opensslErrorString(err).c_str());
		return false;
	}

	// Fatal error
	fprintf(stderr, "TLS handshake failed (notifier): %s", opensslErrorString(err).c_str());
	return false;
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
	if (argc != 3 && argc != 4) {
		fprintf(stderr, "Usage: %s <host> <port> [tlsconfigfile]\n", argv[0]);
		return EXIT_FAILURE;
	}

	const char *host = argv[1];
	int port = atoi(argv[2]);

	int sockfd = connectToServer(host, port);
	if (sockfd < 0) { return EXIT_FAILURE; }

	if (argc == 4) {
		const char *tlsConfigFile = argv[3];
		fprintf(stderr, "Using TLS configuration from file: %s\n", tlsConfigFile);
		try {
			TlsSession::TlsConfig cfg = TlsSession::TlsConfig::fromFile(tlsConfigFile);
			cfg.isServer = false;
			if (cfg.expectedHostname.empty()) {
				cfg.expectedHostname = host;
			}

			tlsSession = std::make_unique<TlsSession>(sockfd, cfg);
			fprintf(stderr, "Starting TLS session with master %s:%d\n", host, port);
			// Perform TLS handshake
			sendStartTls(sockfd);

			if (!sendTlsHandshake()) {
				close(sockfd);
				return EXIT_FAILURE;
			}

			fprintf(stderr, "TLS handshake completed\n");
		} catch (const std::exception &e) {
			fprintf(stderr, "TLS connection initialization failed: %s\n", e.what());
			close(sockfd);
			return EXIT_FAILURE;
		}
	}

	fprintf(stderr, "Connected to %s:%d\n", host, port);

	sendRegister(sockfd);
	fprintf(stderr, "Register packet sent\n");

	std::vector<uint8_t> recvBuffer;
	recvBuffer.reserve(4096);
	std::regex accessRegex(R"(ACCESS\((\d+)\))");
	std::regex unlinkRegex(R"(UNLINK\([^)]*\):(\d+))");
	std::regex purgeRegex(R"(PURGE\((\d+)\))");

	while (true) {
		uint8_t temp[4096];
		ssize_t n = readMaster(sockfd, temp, sizeof(temp));
		if (n <= 0) { break; }  // connection closed or error
		recvBuffer.insert(recvBuffer.end(), temp, temp + n);

		while (recvBuffer.size() >= kHeaderSize) {
			const uint8_t *ptr = recvBuffer.data();
			const uint8_t *parsePtr = ptr;
			uint32_t packetType = 0, dataLen = 0;
			get32bit(&parsePtr, packetType);
			get32bit(&parsePtr, dataLen);

			if (recvBuffer.size() < kHeaderSize + dataLen) { break; }  // wait for full packet

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

				// If log string starts with ACCESS(<inode>), PURGE(<inode>) or
				// UNLINK(<parent_inode>,<name>):<unlinked_inode>, send request for path/type of
				// that inode
				std::smatch match;
				if (std::regex_search(str, match, accessRegex) ||
				    std::regex_search(str, match, unlinkRegex) ||
				    std::regex_search(str, match, purgeRegex)) {
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
