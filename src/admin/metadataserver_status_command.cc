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
#include "admin/metadataserver_status_command.h"

#include <iomanip>
#include <iostream>

#include "common/saunafs_version.h"
#include "common/server_connection.h"
#include "protocol/cltoma.h"
#include "protocol/matocl.h"

std::string MetadataserverStatusCommand::name() const {
	return "metadataserver-status";
}

void MetadataserverStatusCommand::usage() const {
	std::cerr << name() << " <master ip> <master port>" << std::endl;
	std::cerr << "    Prints status of a master or shadow master server" << std::endl;
}

SaunaFsAdminCommand::SupportedOptions MetadataserverStatusCommand::supportedOptions() const {
	return {{kPorcelainMode, kPorcelainModeDescription}, {kTlsMode, kTlsModeDescription}};
}

void MetadataserverStatusCommand::run(const Options& options) const {
	if (options.arguments().size() != 2) {
		throw WrongUsageException("Expected <master ip> and <master port> for " + name());
	}

	auto tlsCfg =
	    options.getValue<std::string>("--tlsconfigfile", std::string(TlsSession::kNoFile));

	MetadataserverStatus s =
	    MetadataserverStatusCommand::getStatus(options.argument(0), options.argument(1), tlsCfg);

	if (options.isSet(kPorcelainMode)) {
		std::cout << s.personality << "\t" << s.serverStatus << "\t" << s.metadataVersion;
		// Only Master/Shadow's existing 3-field porcelain output must stay byte-for-byte
		// unchanged; the 4th field is appended only when there is an mdsId to report.
		if (s.mdsId) { std::cout << "\t" << *s.mdsId; }
		std::cout << std::endl;
	} else {
		std::cout << "     personality: " << s.personality << std::endl;
		std::cout << "   server status: " << s.serverStatus << std::endl;
		std::cout << "metadata version: " << s.metadataVersion << std::endl;
		if (s.mdsId) { std::cout << "          mds id: " << *s.mdsId << std::endl; }
	}
}

MetadataserverStatus MetadataserverStatusCommand::getStatus(
    const std::string &host, const std::string &port, const std::string &tlsConfigFile,
    std::optional<uint32_t> knownPeerVersion) {
	auto sendStatusRequest = [&](bool wantsIdentity) {
		MessageBuffer request;
		if (wantsIdentity) {
			request = cltoma::metadataserverStatus::build(1, uint8_t{1});
		} else {
			request = cltoma::metadataserverStatus::build(1);
		}
		ServerConnection connection(host, port, tlsConfigFile);
		return connection.sendAndReceive(request, SAU_MATOCL_METADATASERVER_STATUS);
	};

	MessageBuffer response;
	if (knownPeerVersion) {
		response = sendStatusRequest(*knownPeerVersion >= kFirstVersionWithMdsRegistry);
	} else {
		// Prefer the identity-bearing request when the peer version is unknown. Releases
		// older than 5.12 reject that packet shape and close the connection, so retry the
		// legacy request on a fresh connection. Response decoding stays outside this catch:
		// a malformed response is not evidence that the peer needs the legacy request.
		try {
			response = sendStatusRequest(true);
		} catch (const std::exception &) { response = sendStatusRequest(false); }
	}

	PacketVersion packetVersion;
	deserializePacketVersionNoHeader(response, packetVersion);

	uint32_t messageId;
	uint8_t status;
	uint64_t metadataVersion;
	std::optional<uint32_t> mdsId;
	if (packetVersion == matocl::metadataserverStatus::kWithIdentityResponse) {
		uint32_t reportedMdsId;
		matocl::metadataserverStatus::deserialize(response, messageId, status, metadataVersion,
		                                          reportedMdsId);
		mdsId = reportedMdsId;
	} else {
		matocl::metadataserverStatus::deserialize(response, messageId, status, metadataVersion);
	}

	std::string personality, serverStatus;
	switch (status) {
	case SAU_METADATASERVER_STATUS_MASTER:
		personality = "master";
		serverStatus = "running";
		break;
	case SAU_METADATASERVER_STATUS_SHADOW_CONNECTED:
		personality = "shadow";
		serverStatus = "connected";
		break;
	case SAU_METADATASERVER_STATUS_SHADOW_DISCONNECTED:
		personality = "shadow";
		serverStatus = "disconnected";
		break;
	default:
		personality = "<unknown>";
		serverStatus = "<unknown>";
	}
	return MetadataserverStatus{personality, serverStatus, metadataVersion, mdsId};
}
