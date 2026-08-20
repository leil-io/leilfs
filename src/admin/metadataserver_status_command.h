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

#include <cstdint>
#include <optional>

#include "admin/saunafs_admin_command.h"

struct MetadataserverStatus {
	std::string personality;
	std::string serverStatus;
	uint64_t metadataVersion;
	std::optional<uint32_t> mdsId;
};

class MetadataserverStatusCommand : public SaunaFsAdminCommand {
public:
	std::string name() const override;
	void usage() const override;
	SupportedOptions supportedOptions() const override;
	void run(const Options& options) const override;
	/// Queries host:port for its status. When the caller already knows the peer's software
	/// version (e.g. from a metadataservers list reply, which carries one per member),
	/// passing it picks the request shape directly; otherwise the identity-bearing request is
	/// tried first, with the legacy request retried on a fresh connection if the peer rejects it.
	static MetadataserverStatus getStatus(const std::string &host, const std::string &port,
	                                      const std::string &tlsConfigFile,
	                                      std::optional<uint32_t> knownPeerVersion = std::nullopt);
};
