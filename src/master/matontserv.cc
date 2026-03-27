/*
   Copyright 2023      Leil Storage OÜ


   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include <deque>
#include <queue>
#include <string>

#include "common/datapack.h"
#include "common/event_loop.h"
#include "common/human_readable_format.h"
#include "common/input_packet.h"
#include "common/loop_watchdog.h"
#include "common/output_packet.h"
#include "common/serialization.h"
#include "common/sockets.h"
#include "common/tls_session.h"
#include "config/cfg.h"
#include "master/filesystem_node.h"
#include "master/filesystem_operations.h"
#include "master/matontserv.h"
#include "master/personality.h"
#include "protocol/SFSCommunication.h"
#include "slogger/slogger.h"

constexpr uint32_t kMaxPacketSize = 1500000;
constexpr size_t kPacketHeaderSize = sizeof(uint32_t) * 2;

enum class NotifierConnectionMode : uint8_t {
	KILL,
	HEADER,
	DATA,
	HANDSHAKE,
};

struct MatontservEntry {
	NotifierConnectionMode mode = NotifierConnectionMode::HEADER;
	int sock;
	int32_t pollFdIndex{-1};
	uint32_t lastRead{0};
	uint32_t lastWrite{0};
	std::array<uint8_t, kPacketHeaderSize> headerBuff{0};
	InputPacket inputPacket{kMaxPacketSize};
	std::queue<OutputPacket> outputPackets;
	uint16_t timeout{10};
	std::string serviceStrIp;
	uint32_t version{};
	uint32_t serviceIp{};
	std::string config;

	/// Context of the TLS channel used for communication with a
	/// metadata notifier application.
	/// If no TLS is used, this is `nullptr`.
	std::unique_ptr<TlsSession> tlsSession{nullptr};
	int lastHandshakeError{0};

	MatontservEntry(int sock_, uint32_t now) : sock(sock_), lastRead(now), lastWrite(now) {}
};

static std::deque<MatontservEntry> matontservList;
static int listenSocket;
static int32_t listenSocketPollFdIndex;
static bool gExiting = false;

// from config
static std::string ListenHost;
static std::string ListenPort;

/// Starts/continues a TLS handshake (non-blocking)
/// @param eptr Pointer to the notifier connection in the master
void matontserv_tlshandshake(MatontservEntry *eptr) {
	sassert(eptr->mode == NotifierConnectionMode::HANDSHAKE);

	int ret = SSL_accept(eptr->tlsSession->session());

	if (ret == 1) {
		safs::log_info("TLS handshake completed with notifier from {}:{}",
		               ipToString(eptr->serviceIp), eptr->serviceStrIp);
		eptr->mode = NotifierConnectionMode::HEADER;
		return;
	}

	int err = SSL_get_error(eptr->tlsSession->session(), ret);
	eptr->lastHandshakeError = err;

	if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
		safs::log_info("TLS handshake in progress with notifier from {}:{}: {}",
		               ipToString(eptr->serviceIp), eptr->serviceStrIp, opensslErrorString(err));
		return;  // retry later
	}

	eptr->mode = NotifierConnectionMode::KILL;
	safs::log_err("TLS handshake failed: {}", opensslErrorString(err));
}

/// Initiate a TLS connection with the notifier.
/// @param eptr Pointer to the notifier connection in the master
void matontserv_starttls(MatontservEntry *eptr) {
	// Initialize a TLS session for the peer.
	std::string keyFile = cfg_getstring("TLS_KEY_FILE", std::string(TlsSession::kNoFile));
	std::string certFile = cfg_getstring("TLS_CERT_FILE", std::string(TlsSession::kNoFile));
	std::string trustFile = cfg_getstring("TLS_CA_CERT_FILE", std::string(TlsSession::kNoFile));

	try {
		eptr->tlsSession =
		    std::make_unique<TlsSession>(eptr->sock, true, keyFile, certFile, trustFile);
		safs::log_info("Starting TLS session with notifier from {}:{}", ipToString(eptr->serviceIp),
		               eptr->serviceStrIp);
		eptr->mode = NotifierConnectionMode::HANDSHAKE;
		matontserv_tlshandshake(eptr);
	} catch (const std::exception &e) {
		eptr->mode = NotifierConnectionMode::KILL;
		safs::log_err("Failed to start TLS session with notifier from {}:{}: {}",
		              ipToString(eptr->serviceIp), eptr->serviceStrIp, e.what());
	}
}

void matontserv_endtls(MatontservEntry *eptr) {
	if (eptr->tlsSession != nullptr) {
		int ret = SSL_shutdown(eptr->tlsSession->session());
		if (ret < 0) {
			safs::log_warn("TLS shutdown failed: {}",
			               opensslErrorString(SSL_get_error(eptr->tlsSession->session(), ret)));
		}
		eptr->tlsSession.reset();
	}
}

uint8_t *matontserv_addpacket(MatontservEntry *eptr, uint32_t type, uint32_t size) {
	eptr->outputPackets.emplace(PacketHeader(type, size));
	// Return pointer to where payload should be written (after header)
	return eptr->outputPackets.back().packet.data() + kPacketHeaderSize;
}

void matontserv_broadcast_message(uint64_t version, std::string_view message) {
	uint8_t *data;

	for (auto &entry : matontservList) {
		if (entry.version == 0) { continue; }
		data = matontserv_addpacket(&entry, MATONT_METACHANGES_LOG,
		                            1 + sizeof(version) + message.size());
		put8bit(&data, 0xFF);
		put64bit(&data, version);
		memcpy(data, message.data(), message.size());
	}
}

void matontserv_wantexit(void) {
	gExiting = true;
	for (auto &entry : matontservList) { matontserv_addpacket(&entry, MATONT_END_SESSION, 0); }
	// Now we won't create any new packets, but we will wait for all existing packets to be
	// transmitted to notifiers
}

int matontserv_canexit(void) {
	// Exit when all connections are closed
	return matontservList.empty();
}

void matontserv_reload(void) {
	std::string oldListenHost, oldListenPort;
	int newlsock;

	oldListenHost = ListenHost;
	oldListenPort = ListenPort;
	ListenHost = cfg_getstr("MATONT_LISTEN_HOST", "*");
	ListenPort = cfg_getstr("MATONT_LISTEN_PORT", "0");

	if (oldListenHost == ListenHost && oldListenPort == ListenPort) {
		safs::log_info("master <-> notifiers module: socket address hasn't changed ({}:{})",
		               ListenHost, ListenPort);
		return;
	}

	if (ListenPort == "0") {
		safs::log_info(
		    "master <-> notifiers module: notifier service disabled, stopping listening on {}:{}",
		    oldListenHost, oldListenPort);
		tcpclose(listenSocket);
		listenSocket = -1;
		return;
	}

	newlsock = tcpsocket();
	if (newlsock < 0) {
		safs::log_warn(
		    "master <-> notifiers module: socket address has changed, but can't create new socket");
		ListenHost = oldListenHost;
		ListenPort = oldListenPort;
		return;
	}

	tcpnonblock(newlsock);
	tcpnodelay(newlsock);
	tcpreuseaddr(newlsock);

	if (tcpsetacceptfilter(newlsock) < 0 && errno != ENOTSUP) {
		safs::log_info_with_error_code(errno,
		                               "master <-> notifiers module: can't set accept filter");
	}

	if (tcpstrlisten(newlsock, ListenHost.c_str(), ListenPort.c_str(), 100) < 0) {
		safs::log_err(
		    "master <-> notifiers module: socket address has changed, but can't listen on socket ({}:{})",
		    ListenHost, ListenPort);
		ListenHost = oldListenHost;
		ListenPort = oldListenPort;
		tcpclose(newlsock);
		return;
	}

	safs::log_info("master <-> notifiers module: socket address has changed, now listen on {}:{}",
	               ListenHost, ListenPort);
	tcpclose(listenSocket);
	listenSocket = newlsock;
}

void matontserv_term(void) {
	safs::log_info("master <-> notifiers module: closing {}:{}", ListenHost,
	               ListenPort);

	for (auto &entry : matontservList) {
		// End TLS session if any
		matontserv_endtls(&entry);
	}

	tcpclose(listenSocket);

	matontservList.clear();
}

void matontserv_desc(std::vector<pollfd> &pdesc) {
	if (!gExiting) {
		pdesc.push_back({listenSocket, POLLIN, 0});
		listenSocketPollFdIndex = pdesc.size() - 1;
	} else {
		listenSocketPollFdIndex = -1;
	}

	for (auto &eptr : matontservList) {
		pdesc.push_back({eptr.sock, POLLIN, 0});
		eptr.pollFdIndex = pdesc.size() - 1;
		if (eptr.mode == NotifierConnectionMode::HANDSHAKE) {
			short events = 0;
			switch (eptr.lastHandshakeError) {
			case SSL_ERROR_WANT_READ:
				events = POLLIN;
				break;
			case SSL_ERROR_WANT_WRITE:
				events = POLLOUT;
				break;
			default:
				events = POLLIN | POLLOUT;
				break;
			}
			pdesc.back().events = events;
		} else {
			if (!eptr.outputPackets.empty()) { pdesc.back().events |= POLLOUT; }
		}
	}
}

std::vector<INotifierListEntry> matontserv_inotifiers() {
	std::vector<INotifierListEntry> ret;
	for (auto &entry : matontservList) { ret.emplace_back(entry.serviceIp, entry.version); }
	return ret;
}

void matontserv_register(MatontservEntry *eptr, const uint8_t *data, uint32_t length) {
	if (eptr->version > 0) {
		safs::log_warn("got register message from registered notifier !!!");
		eptr->mode = NotifierConnectionMode::KILL;
		return;
	}
	if (length < 1) {
		safs::log_info("NTTOMA_REGISTER - wrong size ({})", length);
		eptr->mode = NotifierConnectionMode::KILL;
		return;
	} else {
		uint8_t rversion = get8bit(&data);
		if (rversion < 1) {
			safs::log_info("NTTOMA_REGISTER - wrong version ({})",
			                   rversion);
			return;
		}

		static const uint32_t expected_length[] = {0, 7};
		if (length != expected_length[rversion]) {
			safs::log_info("NTTOMA_REGISTER (ver {}) - wrong size ({}/{})", rversion, length,
			               expected_length[rversion]);
			eptr->mode = NotifierConnectionMode::KILL;
			return;
		}
		get32bit(&data, eptr->version);
		eptr->timeout = get16bit(&data);

		if (eptr->timeout < 10) {
			safs::log_info(
			    "NTTOMA_REGISTER communication timeout too small ({} seconds - should be at least 10 seconds)",
			    eptr->timeout);
			if (eptr->timeout < 3) { eptr->timeout = 3; }
		}
	}
}

void matontserv_get_path_type_inode(MatontservEntry *eptr, const uint8_t *data, uint32_t length) {
	if (eptr->version == 0) {
		safs::log_warn("got get_path_type_inode message from unregistered notifier !!!");
		eptr->mode = NotifierConnectionMode::KILL;
		return;
	}

	if (length < sizeof(uint64_t)) {
		safs::log_info("NTTOMA_GET_PATH_TYPE_INODE - wrong size ({})", length);
		return;
	}

	// Convert the received 64-bit inode value to the buildtime-configured inode_t type (may be
	// uint32_t or uint64_t)
	uint8_t *responseData;
	uint64_t responseInode = get64bit(&data);

	// Check if inode_t is uint32_t and responseInode does not fit
	bool invalidInode = (sizeof(inode_t) == sizeof(uint32_t)) &&
	                    (responseInode > std::numeric_limits<uint32_t>::max());

	inode_t inode = static_cast<inode_t>(responseInode);
	std::string pathByInode;
	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);
	FSNode *node =
	    invalidInode ? nullptr : gFSOperations->nodeOperations()->idToNode(fsOpContext, inode);
	if (node == nullptr) {
		safs::log_info("NTTOMA_GET_PATH_TYPE_INODE - inode {} not found", responseInode);
		// Send back an empty path
		responseData = matontserv_addpacket(eptr, MATONT_GET_PATH_TYPE_INODE,
		                                    sizeof(uint64_t) + sizeof(FSNodeType::kUnknown) + 1);
		put64bit(&responseData, responseInode);
		put8bit(&responseData, FSNodeType::kUnknown);
		memcpy(responseData, "", 1);
		return;
	}

	pathByInode = gFSOperations->fullPathByInode(fsOpContext, inode);
	responseData =
	    matontserv_addpacket(eptr, MATONT_GET_PATH_TYPE_INODE,
	                         sizeof(responseInode) + sizeof(node->type) + pathByInode.size() + 1);
	put64bit(&responseData, responseInode);
	put8bit(&responseData, static_cast<uint8_t>(node->type));
	memcpy(responseData, pathByInode.c_str(), pathByInode.size() + 1);
}

void matontserv_gotpacket(MatontservEntry *eptr, uint32_t type, const uint8_t *data,
                          uint32_t length) {
	try {
		switch (type) {
		case ANTOAN_NOP:
			break;
		case ANTOAN_UNKNOWN_COMMAND:  // for future use
			break;
		case ANTOAN_BAD_COMMAND_SIZE:  // for future use
			break;
		case NTTOMA_REGISTER:
			matontserv_register(eptr, data, length);
			break;
		case NTTOMA_GET_PATH_TYPE_INODE:
			matontserv_get_path_type_inode(eptr, data, length);
			break;
		case NTTOMA_STARTTLS:
			matontserv_starttls(eptr);
			break;
		default:
			safs::log_info("master <-> notifiers module: got unknown message (type:{})",
			               type);
			eptr->mode = NotifierConnectionMode::KILL;
		}
	} catch (IncorrectDeserializationException &ex) {
		safs::log_info("Packet 0x{} - can't deserialize: {}", type, ex.what());
		eptr->mode = NotifierConnectionMode::KILL;
	}
}

void matontserv_read(MatontservEntry *eptr) {
	SignalLoopWatchdog watchdog;
	int32_t i;

	watchdog.start();
	while (eptr->mode != NotifierConnectionMode::KILL) {
		uint32_t bytesToRead = eptr->inputPacket.bytesToBeRead();
		if (eptr->tlsSession != nullptr && eptr->mode != NotifierConnectionMode::HANDSHAKE) {
			i = SSL_read(eptr->tlsSession->session(), eptr->inputPacket.pointerToBeReadInto(),
			             bytesToRead);
		} else {
			i = read(eptr->sock, eptr->inputPacket.pointerToBeReadInto(), bytesToRead);
		}

		if (i == 0) {
			if (eptr->tlsSession != nullptr) {
				int err = SSL_get_error(eptr->tlsSession->session(), i);
				safs::log_info("TLS connection with NT({}) got error: {}", eptr->serviceStrIp,
				               opensslErrorString(err));
			} else {
				safs::log_info("connection with NT({}) has been closed by peer",
				               eptr->serviceStrIp);
			}
			eptr->mode = NotifierConnectionMode::KILL;
			return;
		}

		if (i < 0) {
			int err;
			if (eptr->tlsSession != nullptr && eptr->mode != NotifierConnectionMode::HANDSHAKE) {
				err = SSL_get_error(eptr->tlsSession->session(), i);
				if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
					safs::log_warn("read from NT({}) error: {}", eptr->serviceStrIp,
					               opensslErrorString(err));
					eptr->mode = NotifierConnectionMode::KILL;
				}
			} else {
				err = errno;
				if (errno != EAGAIN) {
					safs::log_err("read from NT({}) error: {}", eptr->serviceStrIp, strerror(err));
					eptr->mode = NotifierConnectionMode::KILL;
				}
			}
			return;
		}

		try {
			eptr->inputPacket.increaseBytesRead(i);
		} catch (InputPacketTooLongException &ex) {
			safs::log_warn("reading from NT({}): {}", eptr->serviceStrIp, ex.what());
			eptr->mode = NotifierConnectionMode::KILL;
			return;
		}

		if (eptr->inputPacket.hasData()) {
			auto header = eptr->inputPacket.getHeader();
			const auto data = eptr->inputPacket.getData();
			matontserv_gotpacket(eptr, header.type, data.data(), data.size());
			eptr->inputPacket.reset();
		}

		if (watchdog.expired()) { break; }
	}
}

void matontserv_write(MatontservEntry *eptr) {
	SignalLoopWatchdog watchdog;
	int32_t bytesWritten;

	watchdog.start();
	while (!eptr->outputPackets.empty()) {
		OutputPacket &outputPacket = eptr->outputPackets.front();
		if (eptr->tlsSession != nullptr) {
			bytesWritten = SSL_write(eptr->tlsSession->session(),
			                         outputPacket.packet.data() + outputPacket.bytesSent,
			                         outputPacket.packet.size() - outputPacket.bytesSent);
			if (bytesWritten < 0) {
				int err = SSL_get_error(eptr->tlsSession->session(), bytesWritten);
				if (err != SSL_ERROR_WANT_WRITE && err != SSL_ERROR_WANT_READ) {
					safs::log_warn("write to NT({}) error: {}", eptr->serviceStrIp,
					               opensslErrorString(err));
					eptr->mode = NotifierConnectionMode::KILL;
				}
				return;
			}
		} else {
			bytesWritten = write(eptr->sock, outputPacket.packet.data() + outputPacket.bytesSent,
			                     outputPacket.packet.size() - outputPacket.bytesSent);
			if (bytesWritten < 0) {
				int err = errno;
				if (err != EAGAIN) {
					safs::log_err("main master server module: (ip:{}) write error: {}",
					              eptr->serviceStrIp, strerror(err));
					eptr->mode = NotifierConnectionMode::KILL;
				}
				return;
			}
		}

		outputPacket.bytesSent += bytesWritten;

		if (outputPacket.bytesSent >= outputPacket.packet.size()) {
			eptr->outputPackets.pop();
		} else {
			return;
		}

		if (watchdog.expired()) { break; }
	}
}

void matontserv_serve(const std::vector<pollfd> &pdesc) {
	uint32_t now = eventloop_time();
	int ns;

	if (listenSocketPollFdIndex >= 0 && (pdesc[listenSocketPollFdIndex].revents & POLLIN)) {
		ns = tcpaccept(listenSocket);
		if (ns < 0) {
			safs::log_info("master <-> notifiers socket: accept error");
		} else if (metadataserver::isMaster()) {
			tcpnonblock(ns);
			tcpnodelay(ns);
			matontservList.emplace_front(ns, now);
			MatontservEntry &eptr = matontservList.front();
			tcpgetpeer(eptr.sock, &(eptr.serviceIp), nullptr);
			eptr.serviceStrIp = ipToString(eptr.serviceIp);
			eptr.config.clear();
		} else {
			tcpclose(ns);
		}
	}

	for (auto &entry : matontservList) {
		if (entry.pollFdIndex >= 0) {
			if (pdesc[entry.pollFdIndex].revents & (POLLERR | POLLHUP)) {
				entry.mode = NotifierConnectionMode::KILL;
			}

			if (entry.mode == NotifierConnectionMode::HANDSHAKE) {
				matontserv_tlshandshake(&entry);
			} else {
				if ((pdesc[entry.pollFdIndex].revents & POLLIN) &&
				    entry.mode != NotifierConnectionMode::KILL) {
					entry.lastRead = now;
					matontserv_read(&entry);
				}

				if ((pdesc[entry.pollFdIndex].revents & POLLOUT) &&
				    entry.mode != NotifierConnectionMode::KILL) {
					entry.lastWrite = now;
					matontserv_write(&entry);
				}
			}
		}
		if ((uint32_t)(entry.lastRead + entry.timeout) < (uint32_t)now) {
			entry.mode = NotifierConnectionMode::KILL;
		}
		if ((uint32_t)(entry.lastWrite + (entry.timeout / 3)) < (uint32_t)now &&
		    entry.outputPackets.empty() && !gExiting) {
			matontserv_addpacket(&entry, ANTOAN_NOP, 0);
		}
	}

	for (auto it = matontservList.begin(); it != matontservList.end();) {
		if (it->mode == NotifierConnectionMode::KILL) {
			matontserv_endtls(&(*it));
			tcpclose(it->sock);
			it = matontservList.erase(it);
		} else {
			++it;
		}
	}
}

void matontserv_register_eventloop_handlers() {
	eventloop_wantexitregister(matontserv_wantexit);
	eventloop_canexitregister(matontserv_canexit);
	eventloop_reloadregister(matontserv_reload);
	eventloop_destructregister(matontserv_term);
	eventloop_pollregister(matontserv_desc, matontserv_serve);
}

int matontserv_init(void) {
	ListenHost = cfg_getstr("MATONT_LISTEN_HOST", "*");
	ListenPort = cfg_getstr("MATONT_LISTEN_PORT", "0");

	if (ListenPort == "0") {
		safs::log_info(
		    "master <-> notifiers module: notifier service disabled, not listening for connections");
		return 0;
	}

	listenSocket = tcpsocket();
	if (listenSocket < 0) {
		safs::log_err("master <-> notifiers module: can't create socket");
		return -1;
	}
	tcpnonblock(listenSocket);
	tcpnodelay(listenSocket);
	tcpreuseaddr(listenSocket);
	if (tcpsetacceptfilter(listenSocket) < 0 && errno != ENOTSUP) {
		safs::log_err("master <-> notifiers module: can't set accept filter");
	}

	if (tcpstrlisten(listenSocket, ListenHost.c_str(), ListenPort.c_str(), 100) < 0) {
		safs::log_err("master <-> notifiers module: can't listen on {}:{}",
		              ListenHost, ListenPort);
		return -1;
	}
	safs::log_info("master <-> notifiers module: listen on {}:{}", ListenHost,
	               ListenPort);

	matontserv_register_eventloop_handlers();
	return 0;
}
