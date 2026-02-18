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
#include "common/server_connection.h"

#include <csignal>

#include "common/cwrap.h"
#include "common/exceptions.h"
#include "common/message_receive_buffer.h"
#include "protocol/cltoma.h"
#include "protocol/SFSCommunication.h"
#include "errors/sfserr.h"
#include "common/multi_buffer_writer.h"
#include "protocol/packet.h"
#include "common/sockets.h"
#include "common/time_utils.h"

const int ServerConnection::kDefaultTimeout;

constexpr size_t kMaxMessageSize = 4 * 1024 * 1024;

namespace {

/// Writes the \p request to the \p fd_ socket
void sendRequestGeneric(int fd, const MessageBuffer &request, const Timeout &timeout,
                        const TlsSession *tlsSession) {
	if (tlsSession) {
		size_t totalWritten = 0;
		while (totalWritten < request.size()) {
			int ret = SSL_write(tlsSession->session(), request.data() + totalWritten,
			                    static_cast<int>(request.size() - totalWritten));
			if (ret <= 0) {
				int err = SSL_get_error(tlsSession->session(), ret);
				if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
					int pollEvents = (err == SSL_ERROR_WANT_WRITE) ? POLLOUT : POLLIN;
					int status = tcptopoll(fd, pollEvents, timeout.remaining_ms());
					if (status == 0 || timeout.expired()) {
						throw ConnectionException("TLS write timeout");
					} else if (status < 0) {
						throw ConnectionException("TLS write poll failed: " +
						                          errorString(tcpgetlasterror()));
					}
					continue;
				}
				throw ConnectionException("TLS write failed: " + opensslErrorString(err));
			}
			totalWritten += ret;
		}
	} else {
		MultiBufferWriter writer;
		writer.addBufferToSend(request.data(), request.size());
		while (writer.hasDataToSend()) {
			int status = tcptopoll(fd, POLLOUT, timeout.remaining_ms());
			if (status == 0 || timeout.expired()) {
				throw ConnectionException("Can't write data to socket: timeout");
			} else if (status < 0) {
				throw ConnectionException("Can't write data to socket: " +
				                          errorString(tcpgetlasterror()));
			}
			ssize_t bytesWritten = writer.writeTo(fd);
			if (bytesWritten < 0) {
				throw ConnectionException("Can't write data to socket: " +
				                          errorString(tcpgetlasterror()));
			}
		}
	}
}

/// Reads a message from the \p socket
/// Throws if its type is different than \p expectedType.
/// Ignores NOP messages if \p receiveMode is ReceiveMode::kReceiveFirstNonNopMessage.
MessageBuffer receiveRequestGeneric(int fd, PacketHeader::Type expectedType,
                                    ServerConnection::ReceiveMode receiveMode,
                                    const Timeout &timeout, TlsSession *tlsSession) {
	if (tlsSession) {
		MessageBuffer buffer;
		while (true) {
			// Phase 1: Read the packet header
			buffer.resize(PacketHeader::kSize);  // Resize buffer to hold only the header
			size_t totalRead = 0;
			while (totalRead < PacketHeader::kSize) {
				int ret = SSL_read(tlsSession->session(), buffer.data() + totalRead,
				                   static_cast<int>(PacketHeader::kSize - totalRead));
				if (ret <= 0) {
					int err = SSL_get_error(tlsSession->session(), ret);
					if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
						int pollEvents = (err == SSL_ERROR_WANT_WRITE) ? POLLOUT : POLLIN;
						int status = tcptopoll(fd, pollEvents, timeout.remaining_ms());
						if (status == 0 || timeout.expired()) {
							throw ConnectionException("TLS read timeout");
						} else if (status < 0) {
							throw ConnectionException("TLS read poll failed: " +
							                          errorString(tcpgetlasterror()));
						}
						continue;
					}
					throw ConnectionException("TLS read failed: " + opensslErrorString(err));
				}
				totalRead += ret;
			}

			PacketHeader header;
			deserialize(buffer, header);  // Deserialize header from the small buffer
			// Validate payload length against a reasonable maximum to prevent excessive allocation
			if (header.length > kMaxMessageSize - PacketHeader::kSize) {
				throw Exception("Received message too large: " + std::to_string(header.length) +
				                " bytes, exceeding maximum allowed.");
			}

			// Phase 2: Resize buffer to hold the full message and read the remaining payload
			buffer.resize(PacketHeader::kSize + header.length);
			while (totalRead < PacketHeader::kSize + header.length) {
				int ret =
				    SSL_read(tlsSession->session(), buffer.data() + totalRead,
				             static_cast<int>(PacketHeader::kSize + header.length - totalRead));
				if (ret <= 0) {
					int err = SSL_get_error(tlsSession->session(), ret);
					if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
						int pollEvents = (err == SSL_ERROR_WANT_WRITE) ? POLLOUT : POLLIN;
						int status = tcptopoll(fd, pollEvents, timeout.remaining_ms());
						if (status == 0 || timeout.expired()) {
							throw ConnectionException("TLS read timeout");
						} else if (status < 0) {
							throw ConnectionException("TLS read poll failed: " +
							                          errorString(tcpgetlasterror()));
						}
						continue;
					}
					throw ConnectionException("TLS read failed: " + opensslErrorString(err));
				}
				totalRead += ret;
			}

			if (receiveMode == ServerConnection::ReceiveMode::kReceiveFirstNonNopMessage &&
			    header.type == ANTOAN_NOP) {
				// We have received a NOP message and were instructed to ignore it
				continue;
			}

			if (header.type != expectedType) {
				throw Exception("Received unexpected message #" + std::to_string(header.type));
			}

			return MessageBuffer(buffer.data() + PacketHeader::kSize,
			                     buffer.data() + PacketHeader::kSize + header.length);
		}
	} else {
		MessageReceiveBuffer reader(kMaxMessageSize);
		while (!reader.hasMessageData()) {
			int status = tcptopoll(fd, POLLIN, timeout.remaining_ms());
			if (status == 0 || timeout.expired()) {
				throw ConnectionException("Can't read data from socket: timeout");
			} else if (status < 0) {
				throw ConnectionException("Can't read data from socket: " +
				                          errorString(tcpgetlasterror()));
			}
			ssize_t bytesRead = reader.readFrom(fd);
			if (bytesRead == 0) {
				throw ConnectionException("Can't read data from socket: connection reset by peer");
			}
			if (bytesRead < 0) {
				throw ConnectionException("Can't read data from socket: " +
				                          errorString(tcpgetlasterror()));
			}
			if (reader.isMessageTooBig()) { throw Exception("Receive buffer overflow"); }
			while (reader.hasMessageData() &&
			       receiveMode == ServerConnection::ReceiveMode::kReceiveFirstNonNopMessage &&
			       reader.getMessageHeader().type == ANTOAN_NOP) {
				// We have received a NOP message and were instructed to ignore it
				reader.removeMessage();
			}
		}

		if (reader.getMessageHeader().type != expectedType) {
			throw Exception("Received unexpected message #" +
			                std::to_string(reader.getMessageHeader().type));
		}

		uint32_t length = reader.getMessageHeader().length;
		return MessageBuffer(reader.getMessageData(), reader.getMessageData() + length);
	}
}

} // anonymous namespace

ServerConnection::ServerConnection(const std::string &host, const std::string &port,
                                   const std::string &tlsConfigFile)
    : fd_(-1), timeout_(kDefaultTimeout), tlsConfigFile_(tlsConfigFile) {
	NetworkAddress server;
	tcpresolve(host.c_str(), port.c_str(), &server.ip, &server.port, false);
	connect(server);
}

ServerConnection::ServerConnection(const NetworkAddress &server, const std::string &tlsConfigFile)
    : fd_(-1), timeout_(kDefaultTimeout), tlsConfigFile_(tlsConfigFile) {
	connect(server);
}

ServerConnection::~ServerConnection() {
	if (tlsSession_ != nullptr) { tlsSession_.reset(); }

	if (fd_ != -1) {
		tcpclose(fd_);
	}
}

MessageBuffer ServerConnection::sendAndReceive(
		const MessageBuffer& request,
		PacketHeader::Type expectedType,
		ReceiveMode receiveMode) {
	return ServerConnection::sendAndReceive(fd_, request, expectedType, receiveMode, timeout_, tlsSession_.get());
}

MessageBuffer ServerConnection::sendAndReceive(
		int fd,
		const MessageBuffer& request,
		PacketHeader::Type expectedType,
		ReceiveMode receiveMode,
		int tm, TlsSession* tlsSession) {
	Timeout timeout{std::chrono::milliseconds(tm)};
	sendRequestGeneric(fd, request, timeout, tlsSession);
	return receiveRequestGeneric(fd, expectedType, receiveMode, timeout, tlsSession);
}

void ServerConnection::connect(const NetworkAddress &server) {
	fd_ = tcpsocket();
	int tcpLastError = 0;
	if (fd_ < 0) {
		tcpLastError = tcpgetlasterror();
		throw ConnectionException("Can't create socket: " + std::string(strerr(tcpLastError)));
	}
	tcpnonblock(fd_);
	if (tcpnumtoconnect(fd_, server.ip, server.port, timeout_) != 0) {
		tcpLastError = tcpgetlasterror();
		tcpclose(fd_);
		fd_ = -1;
		throw ConnectionException("Can't connect to " + server.toString() + ": " +
		                          strerr(tcpLastError));
	}

	if (tlsConfigFile_.empty()) {
		return;
	} else {
		startTlsSession(server);
	}
}

bool ServerConnection::performTlsHandshake() {
	sassert(tlsSession_ != nullptr);

#ifndef _WIN32
	struct sigaction old_action, ignore_action;
	ignore_action.sa_handler = SIG_IGN;
	sigemptyset(&ignore_action.sa_mask);
	ignore_action.sa_flags = 0;
	sigaction(SIGPIPE, &ignore_action, &old_action);
#endif

	SSL *ssl = tlsSession_->session();
	int ret;
	Timeout timeout{std::chrono::milliseconds(timeout_)};
	while (true) {
		ret = SSL_connect(ssl);
		if (ret == 1) {
#ifndef _WIN32
			sigaction(SIGPIPE, &old_action, nullptr);
#endif
			return true;
		}

		int err = SSL_get_error(ssl, ret);
		lastHandshakeError_ = err;

		if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
			int pollEvents = (err == SSL_ERROR_WANT_WRITE) ? POLLOUT : POLLIN;
			int status = tcptopoll(fd_, pollEvents, timeout.remaining_ms());
			if (status == 0 || timeout.expired()) {
#ifndef _WIN32
				sigaction(SIGPIPE, &old_action, nullptr);
#endif
				return false;  // handshake timeout
			} else if (status < 0) {
#ifndef _WIN32
				sigaction(SIGPIPE, &old_action, nullptr);
#endif
				return false;  // poll error
			}
			continue;
		} else {
			break;
		}
	}

#ifndef _WIN32
	sigaction(SIGPIPE, &old_action, nullptr);
#endif

	return false;
}

void ServerConnection::startTlsSession(const NetworkAddress &server) {
	if (fd_ < 0 || tlsConfigFile_.empty()) {
		throw ConnectionException("Can't start TLS session: no connection or TLS config file");
	}

	TlsSession::TlsConfig tlsCfg = TlsSession::TlsConfig::fromFile(tlsConfigFile_);

	if (tlsCfg.expectedHostname.empty()) { tlsCfg.expectedHostname = std::to_string(server.ip); }

	tlsSession_ = std::make_unique<TlsSession>(fd_, tlsCfg);

	auto startTlsRequest = cltoma::startTls::build();
	ssize_t ret = tcptowrite(fd_, startTlsRequest.data(), startTlsRequest.size(), timeout_);
	if (ret < 0) {
		throw ConnectionException("cannot transmit startTls request to metadata server: " +
		                          errorString(tcpgetlasterror()));
	} else if (ret != static_cast<int>(startTlsRequest.size())) {
		throw ConnectionException("cannot transmit startTls request to metadata server: sent " +
		                          std::to_string(ret) + " bytes instead of " +
		                          std::to_string(startTlsRequest.size()));
	}

	if (!performTlsHandshake()) {
		throw ConnectionException("TLS handshake failed with server: " + server.toString() +
		                          ", error: " + opensslErrorString(lastHandshakeError_));
	}
}

KeptAliveServerConnection::~KeptAliveServerConnection() {
	threadCanRun_ = false;
	cond_.notify_all();
	nopThread_.join();
}

MessageBuffer KeptAliveServerConnection::sendAndReceive(
		const MessageBuffer& request,
		PacketHeader::Type expectedResponseType,
		ReceiveMode receiveMode) {
	Timeout timeout{std::chrono::milliseconds(timeout_)};
	/* synchronized with nopThread_ */ {
		std::unique_lock<std::mutex> lock(mutex_);
		sendRequestGeneric(fd_, request, timeout, tlsSession_.get());
	}
	return receiveRequestGeneric(fd_, expectedResponseType, receiveMode, timeout,
	                             tlsSession_.get());
}

void KeptAliveServerConnection::startNopThread() {
	auto threadCode = [this]() {
		std::unique_lock<std::mutex> lock(mutex_);
		while (threadCanRun_) {
			cond_.wait_for(lock, std::chrono::seconds(1));
			if (threadCanRun_) {
				// We have the lock, we can send something
				Timeout timeout{std::chrono::milliseconds(timeout_)};
				sendRequestGeneric(fd_, buildLegacyPacket(ANTOAN_NOP), timeout, tlsSession_.get());
			}
		}
	};
	nopThread_ = std::thread(threadCode);
}
