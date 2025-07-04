/*
   Copyright 2023 Leil Storage OÜ

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

#include "changelog_filename.h"
#include "common/str_utils.h" // For split_string, and potentially others
#include "common/exceptions.h"  // For ConfigurationException or custom
#include <iomanip>
#include <sstream>
#include <regex> // For parsing

namespace sauna_fs {

// Define constants declared in header
const std::string kChangelogBaseName = "changelog.sfs";
const std::string kChangelogMlBaseName = "changelog_ml.sfs";
const std::string kChangelogKeywordUndef = "UNDEF";
const std::string kChangelogKeywordLive = "LIVE";
const std::string kChangelogSuffixMds = ".mds";
const std::string kChangelogSuffixMls = ".mls";

namespace changelog_filename {

std::string id_to_hex_string(uint64_t id) {
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(16) << std::hex << id;
    std::string hex_str = oss.str();
    // Convert to uppercase as per example format
    for (char &c : hex_str) {
        c = static_cast<char>(toupper(c));
    }
    return hex_str;
}

uint64_t hex_string_to_id(const std::string& hex_str) {
    if (hex_str == kChangelogKeywordUndef || hex_str.length() != 16) {
        return 0; // Or throw error, but 0 can signify UNDEF in this context
    }
    try {
        return std::stoull(hex_str, nullptr, 16);
    } catch (const std::invalid_argument& ia) {
        // Consider logging here
        return 0;
    } catch (const std::out_of_range& oor) {
        // Consider logging here
        return 0;
    }
}

std::string generate_filename_from_components(const ChangelogNameComponents& components) {
    std::ostringstream oss;
    oss << components.base_prefix << "."
        << components.cluster_id << "."
        << components.first_id_hex << "."
        << (components.last_id_hex.empty() ? kChangelogKeywordUndef : components.last_id_hex) << "."
        << components.begin_epoch_utc;

    if (components.live_status == ChangelogLiveStatus::LIVE) {
        oss << "." << kChangelogKeywordLive;
    } else if (components.live_status == ChangelogLiveStatus::FINALIZED && !components.end_epoch_utc.empty()) {
        oss << "." << components.end_epoch_utc;
    }
    // If UNDEF_LAST_ID, end_epoch_utc is not appended.

    // Add role-based suffix (.mds or .mls)
    oss << (components.is_metalogger ? kChangelogSuffixMls : kChangelogSuffixMds);

    if (!components.suffix_hostname.empty()) {
        oss << "." << components.suffix_hostname;
    }

    return oss.str();
}

ChangelogNameComponents parse_filename(const std::string& filename) {
    // New format:
    // <base_prefix>.<CLUSTER_ID>.<FIRST_ID>.<LAST_ID>.<BEGIN_EPOCH_UTC>Z.[<END_EPOCH_UTC>Z | LIVE].<ROLE_SUFFIX>.[<HOSTNAME>]
    // Example: changelog.sfs.alpha01.00000000683DB60E.0000000068F844BB.2025-06-05T1423Z.2025-06-22T1555Z.mds.mds01
    // Example: changelog_ml.sfs.alpha01.00000000683DB60E.UNDEF.2025-06-05T1423Z.LIVE.mls.metalogger1

    ChangelogNameComponents components;
    std::vector<std::string> parts = str_utils::split_string(filename, '.');

    // Minimum parts: base(2) + cluster + first_id + last_id + begin_utc + state + role_suffix = 8
    // Maximum parts: base(2) + cluster + first_id + last_id + begin_utc + state + role_suffix + hostname = 9
    if (parts.size() < 8 || parts.size() > 9) {
        throw std::runtime_error("Invalid changelog filename format: incorrect number of parts (" + std::to_string(parts.size()) + ") for '" + filename + "'");
    }

    components.base_prefix = parts[0] + "." + parts[1];
    bool base_is_ml = (components.base_prefix == kChangelogMlBaseName);
    bool base_is_std = (components.base_prefix == kChangelogBaseName);

    if (!base_is_ml && !base_is_std) {
        throw std::runtime_error("Invalid changelog filename format: unknown base prefix '" + components.base_prefix + "' in '" + filename + "'");
    }

    components.cluster_id = parts[2];
    components.first_id_hex = parts[3];
    components.last_id_hex = parts[4];
    components.begin_epoch_utc = parts[5];

    // Validate FIRST_ID
    components.first_id_val = hex_string_to_id(components.first_id_hex);
    if (components.first_id_hex == kChangelogKeywordUndef || (components.first_id_val == 0 && components.first_id_hex != "0000000000000000")) {
        throw std::runtime_error("Invalid changelog filename: FIRST_ID cannot be UNDEF or invalid hex '" + components.first_id_hex + "' in '" + filename + "'");
    }

    // Validate LAST_ID
    if (components.last_id_hex != kChangelogKeywordUndef) {
        components.last_id_val = hex_string_to_id(components.last_id_hex);
        if (components.last_id_val == 0 && components.last_id_hex != "0000000000000000") {
            throw std::runtime_error("Invalid changelog filename: LAST_ID is not UNDEF but invalid hex '" + components.last_id_hex + "' in '" + filename + "'");
        }
    } else {
        components.last_id_val = 0; // Represents UNDEF
    }

    // State part (LIVE or END_EPOCH_UTC)
    const std::string& state_part = parts[6];
    if (state_part == kChangelogKeywordLive) {
        components.live_status = ChangelogLiveStatus::LIVE;
        if (components.last_id_hex != kChangelogKeywordUndef) {
            throw std::runtime_error("Invalid changelog filename: .LIVE file must have LAST_ID as UNDEF in '" + filename + "'");
        }
    } else if (std::regex_match(state_part, std::regex("\\d{4}-\\d{2}-\\d{2}T\\d{4}Z"))) {
        components.end_epoch_utc = state_part;
        components.live_status = ChangelogLiveStatus::FINALIZED;
        if (components.last_id_hex == kChangelogKeywordUndef) {
             throw std::runtime_error("Invalid changelog filename: Finalized file with END_EPOCH must not have LAST_ID as UNDEF in '" + filename + "'");
        }
    } else {
        // Old style UNDEF without LIVE keyword and without END_EPOCH (e.g. ...UNDEF.YYYY-MM-DDTHHMMZ.mds.host)
        // In this case, parts[6] is actually the role_suffix.
        // This check is tricky. The new format *always* has either LIVE or an END_EPOCH as parts[6].
        // So, if it's not LIVE and not a timestamp, it's a format violation unless we allow parsing of a transitional state.
        // For strictness with the new format, this is an error.
        // However, the original parser had a notion of `UNDEF_LAST_ID` if the state part was missing
        // and `last_id_hex` was "UNDEF".
        // Given the new structure where role suffix is mandatory, this path should ideally not be hit if
        // the filename conforms to the new structure. If parts.size() is 8 or 9, part[6] must be state.
         throw std::runtime_error("Invalid changelog filename: Malformed state part. Expected 'LIVE' or 'YYYY-MM-DDTHHMMZ', got '" + state_part + "' in '" + filename + "'");
    }

    // Role Suffix part
    const std::string& role_suffix_part = parts[7];
    if (role_suffix_part == kChangelogSuffixMls.substr(1)) { // ".mls" -> "mls"
        components.is_metalogger = true;
        components.suffix_metalogger_tag = kChangelogSuffixMls; // Store with dot
        if (!base_is_ml) {
            throw std::runtime_error("Invalid changelog filename: Role suffix 'mls' found but base prefix is not for metalogger in '" + filename + "'");
        }
    } else if (role_suffix_part == kChangelogSuffixMds.substr(1)) { // ".mds" -> "mds"
        components.is_metalogger = false;
        components.suffix_metalogger_tag = kChangelogSuffixMds; // Store with dot
        if (!base_is_std) {
            throw std::runtime_error("Invalid changelog filename: Role suffix 'mds' found but base prefix is not for standard changelog in '" + filename + "'");
        }
    } else {
        throw std::runtime_error("Invalid changelog filename: Invalid role suffix. Expected 'mds' or 'mls', got '" + role_suffix_part + "' in '" + filename + "'");
    }

    // Hostname (optional)
    if (parts.size() == 9) {
        components.suffix_hostname = parts[8];
    } else {
        components.suffix_hostname = ""; // No hostname
    }

    return components;
}


} // namespace changelog_filename
} // namespace sauna_fs
