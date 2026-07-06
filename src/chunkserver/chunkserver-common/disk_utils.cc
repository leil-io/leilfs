/*
   Copyright 2023 Leil Storage

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

#include <algorithm>
#include <cctype>

#include "chunkserver-common/disk_utils.h"

namespace disk {

Configuration::Configuration(std::string hddCfgLine) {
	// Trim leading whitespace characters
	auto forwardIt =
	    std::find_if(hddCfgLine.begin(), hddCfgLine.end(), [](char symbol) {
		    return !std::isspace<char>(symbol, std::locale::classic());
	    });

	if (hddCfgLine.begin() != forwardIt) {
		hddCfgLine.erase(hddCfgLine.begin(), forwardIt);
	}

	if (hddCfgLine.empty()) {  // The complete line is empty
		isValid = false;
		isEmpty = true;
		return;
	}

	if (hddCfgLine.at(0) == '#') {  // Skip comments
		isValid = false;
		isComment = true;
		return;
	}

	// Trim trailing whitespace characters
	auto reverseIt =
	    std::find_if(hddCfgLine.rbegin(), hddCfgLine.rend(), [](char symbol) {
		    return !std::isspace<char>(symbol, std::locale::classic());
	    });
	hddCfgLine.erase(reverseIt.base(), hddCfgLine.end());

	if (hddCfgLine.at(0) == '*') {
		isMarkedForRemoval = true;
		hddCfgLine.erase(hddCfgLine.begin());
	}

	// A disk type prefix is an alphanumeric token (plus '_') followed by ':'
	// before the first path character, e.g. "zonefs:" or "mock:". The prefix
	// selects the plugin used to instantiate the disk; unprefixed lines are
	// handled by CmrDisk.
	static const std::string zonedPrefix = "zonefs";
	const auto colonPos = hddCfgLine.find(':');
	if (colonPos != std::string::npos && colonPos > 0) {
		const std::string token = hddCfgLine.substr(0, colonPos);
		const bool isPrefixToken = std::all_of(
		    token.begin(), token.end(),
		    [](unsigned char symbol) { return std::isalnum(symbol) != 0 || symbol == '_'; });
		if (isPrefixToken) {
			prefix = token;
			isZoned = (prefix == zonedPrefix);
			hddCfgLine.erase(0, colonPos + 1);
		}
	}

	static std::string const delimiter = " | ";
	auto delimiterPos = hddCfgLine.find(delimiter);

	if (isZoned && delimiterPos == std::string::npos) {
		safs_pretty_syslog(LOG_WARNING,
		                   "Parse hdd line: %s - zoned drives must contain two "
		                   "paths separated by ' | '.",
		                   hddCfgLine.c_str());
		isValid = false;
		return;
	}

	if (delimiterPos != std::string::npos) {
		metaPath = hddCfgLine.substr(0, delimiterPos);
		dataPath = hddCfgLine.substr(delimiterPos + delimiter.length());
	} else {
		metaPath = hddCfgLine;
		dataPath = hddCfgLine;
	}

	// Ensure / at the end for both paths
	if (metaPath.at(metaPath.size() - 1) != '/') {
		metaPath.append("/");
	}

	if (dataPath.at(dataPath.size() - 1) != '/') {
		dataPath.append("/");
	}

	isValid = true;
}

Configuration::Configuration(const std::string &_metaPath,
                             const std::string &_dataPath,
                             bool _isMarkedForRemoval, bool _isZonedDevice)
    : metaPath(_metaPath),
      dataPath(_dataPath),
      isMarkedForRemoval(_isMarkedForRemoval),
      isZoned(_isZonedDevice),
      isValid(true) {}

}  // namespace disk
