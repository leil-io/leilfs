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

#pragma once

#include "common/platform.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "common/network_address.h"
#include "common/tls_session.h"
#include "protocol/packet.h"

class ServerConnection {
public:
	enum class ReceiveMode {
		kReceiveFirstNonNopMessage, ///< Ignores ANTOAN_NOP responses
		kReceiveFirstMessage        ///< Returns the first received response, even ANTOAN_NOP
	};

	static const int kDefaultTimeout = 5000;

	ServerConnection(const std::string &host, const std::string &port,
	                 const std::string &tlsConfigFile = "");
	ServerConnection(const NetworkAddress &server, const std::string &tlsConfigFile = "");
	virtual ~ServerConnection();

	/// Sends a request and receives a response.
	/// Throws if there are connection errors or the received response is not
	/// of type \p expectedResponseType. \p request may be empty.
	/// \returns Body of the response (a message without a header)
	virtual MessageBuffer sendAndReceive(
			const MessageBuffer& request,
			PacketHeader::Type expectedResponseType,
			ReceiveMode receiveMode = ReceiveMode::kReceiveFirstNonNopMessage);

	/// A static method for those, who own a socket.
	static MessageBuffer sendAndReceive(
	    int fd, const MessageBuffer &request, PacketHeader::Type expectedResponseType,
	    ReceiveMode receiveMode = ReceiveMode::kReceiveFirstNonNopMessage,
	    int timeout = kDefaultTimeout, TlsSession *tlsSession = nullptr);

	void setTimeout(int timeout) {
		timeout_ = timeout;
	}

protected:
	/// A socket (or -1 if closed)
	int fd_;
	int timeout_;

	/// Optional TLS session (present if tlsConfigFile_ was set and handshake succeeded)
	std::unique_ptr<TlsSession> tlsSession_;
	// Path to TLS client config file
	std::string tlsConfigFile_;
	int lastHandshakeError_{0};

private:
	/// Opens a connection and sets \p fd_
	void connect(const NetworkAddress& server);

	/// Starts a TLS connection request to target server and sets up TLS session if tlsConfigFile_
	/// is not empty.
	void startTlsSession(const NetworkAddress &server);

	/// Performs a TLS handshake on the current fd_ and initializes tlsSession_ if tlsConfigFile_ is
	/// not empty.
	bool performTlsHandshake();
};

/// A special version of \p ServerConnection which sends \p ANTOAN_NOP every second.
/// Uses a background thread for this additional task
class KeptAliveServerConnection : public ServerConnection {
public:
	/// Inherited constructor
	KeptAliveServerConnection(const std::string &host, const std::string &port,
	                          const std::string &tlsConfigFile = "")
	    : ServerConnection(host, port, tlsConfigFile), threadCanRun_(true) {
		startNopThread();
	}

	/// Inherited constructor
	KeptAliveServerConnection(const NetworkAddress &server, const std::string &tlsConfigFile = "")
	    : ServerConnection(server, tlsConfigFile), threadCanRun_(true) {
		startNopThread();
	}

	/// A destructor which joins the the background thread
	virtual ~KeptAliveServerConnection();

	/// Overridden to synchronize with the background thread
	virtual MessageBuffer sendAndReceive(
			const MessageBuffer& request,
			PacketHeader::Type expectedResponseType,
			ReceiveMode receiveMode = ReceiveMode::kReceiveFirstNonNopMessage) override;

private:
	/// Initializes the \p nopThread_;
	void startNopThread();

	/// Set to false to stop the thread
	std::atomic<bool> threadCanRun_;

	/// For synchronization when writing fd_ or using cond
	std::mutex mutex_;

	/// For waiting between NOPs
	std::condition_variable cond_;

	/// A thread which sends ANTOAN_NOP every second
	std::thread nopThread_;
};
