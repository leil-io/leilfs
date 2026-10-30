/*
   Copyright 2026 Leil Storage OÜ

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

#include "common/platform.h"
#include "common/legacy_data_files.h"

#include <filesystem>
#include <optional>
#include <system_error>

#include "slogger/slogger.h"

namespace legacy_data_files {

namespace {

constexpr std::string_view kExtension = ".leil";
constexpr std::string_view kLegacyExtension = ".sfs";
/// The empty metadata file installed by the packages, which belong to them
constexpr std::string_view kPackagedSuffix = ".empty";

/// @p name with extension @p from of the data file @p family replaced by @p to
/// ("changelog.sfs.1" -> "changelog.leil.1"), or nothing if @p name is no such file.
std::optional<std::string> replaceExtension(std::string_view name, std::string_view family,
                                            std::string_view from, std::string_view to) {
	if (!name.starts_with(family)) { return std::nullopt; }
	std::string_view rest = name.substr(family.size());
	if (!rest.starts_with(from)) { return std::nullopt; }
	std::string_view suffix = rest.substr(from.size());
	if (!suffix.empty() && !suffix.starts_with('.')) { return std::nullopt; }
	return std::string(family).append(to).append(suffix);
}

}  // namespace

std::string legacyName(std::string_view name) {
	const std::string_view family = name.substr(0, name.find('.'));
	if (family.empty()) { return std::string(name); }
	return replaceExtension(name, family, kExtension, kLegacyExtension)
	    .value_or(std::string(name));
}

bool usesLegacyNames(const std::string &dir, std::initializer_list<std::string_view> families) {
	namespace fs = std::filesystem;

	bool legacy = false;
	std::error_code error;
	for (fs::directory_iterator entry(dir, error), end; !error && entry != end;
	     entry.increment(error)) {
		const std::string name = entry->path().filename().string();
		if (name.ends_with(kPackagedSuffix)) { continue; }
		for (std::string_view family : families) {
			// Any file under the new name: the names were switched, or it is a new installation
			if (replaceExtension(name, family, kExtension, kExtension)) { return false; }
			if (replaceExtension(name, family, kLegacyExtension, kLegacyExtension)) {
				legacy = true;
			}
		}
	}
	if (error) {
		// Unreadable here, so also for the daemon itself, which then fails as it would have
		safs::log_warn("can't list data directory {} for pre-rebrand data files: {}", dir,
		               error.message());
		return false;
	}
	if (legacy) {
		const fs::path absoluteDir = fs::absolute(dir, error);
		safs::log_info(
		    "data directory {} has pre-rebrand data files (*{}), using those names; to switch to "
		    "*{}, stop the daemon and rename its files",
		    error ? dir : absoluteDir.string(), kLegacyExtension, kExtension);
	}
	return legacy;
}

}  // namespace legacy_data_files
