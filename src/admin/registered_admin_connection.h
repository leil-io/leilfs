/*
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

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

#pragma once

#include "common/platform.h"

#include <memory>

#include "common/server_connection.h"

/// An authenticated (using an admin password) version of cl<->ma connection.
class RegisteredAdminConnection : public KeptAliveServerConnection {
public:
	/// Creates a new registered admin connection.
	/// Asks for a password and authenticates using an challenge-response mechanism.
	/// Throws if something goes wrong.
	static std::unique_ptr<RegisteredAdminConnection> create(
			const std::string& host,
			const std::string& port,
			const std::string &tlsConfigFile = "",
			int timeout = kDefaultTimeout);

private:
	/// Private constructor for ::create.
	RegisteredAdminConnection(const std::string host, const std::string &port,
	                          const std::string &tlsConfigFile = "")
	    : KeptAliveServerConnection(host, port, tlsConfigFile) {}
};
