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

#include <string>
#include <vector>

#include "common/exception.h"
#include "admin/options.h"

SAUNAFS_CREATE_EXCEPTION_CLASS(WrongUsageException, Exception);

class SaunaFsAdminCommand {
public:
	typedef std::vector<std::pair<std::string, std::string>> SupportedOptions;

	static const std::string kPorcelainMode;
	static const std::string kPorcelainModeDescription;
	static const std::string kVerboseMode;
	static const std::string kTlsMode;
	static const std::string kTlsModeDescription;

	virtual ~SaunaFsAdminCommand() {}
	virtual std::string name() const = 0;
	virtual void usage() const = 0;
	virtual SupportedOptions supportedOptions() const { return {}; }
	virtual void run(const Options& options) const = 0;
};
