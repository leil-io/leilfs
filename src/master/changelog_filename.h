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

#pragma once

#include <string>
#include <cstdint> // For uint64_t

namespace sauna_fs {

// Base prefix for master/shadow changelogs
extern const std::string kChangelogBaseName; // = "changelog.sfs";
// Base prefix for metalogger changelogs
extern const std::string kChangelogMlBaseName; // = "changelog_ml.sfs";

// Filename component constants
extern const std::string kChangelogKeywordUndef; // = "UNDEF";
extern const std::string kChangelogKeywordLive;  // = "LIVE";
extern const std::string kChangelogSuffixMds;    // = ".mds"; // Metadata Server
extern const std::string kChangelogSuffixMls;    // = ".mls"; // Metalogger Server


enum class ChangelogLiveStatus {
    LIVE,
    FINALIZED,
    UNDEF_LAST_ID // For when last_id is "UNDEF" but not explicitly .LIVE
};

struct ChangelogNameComponents {
    std::string base_prefix; // e.g., "changelog_ml.sfs"
    std::string cluster_id;
    std::string first_id_hex; // Parsed or to be generated (00000000683DB60E)
    std::string last_id_hex;  // Parsed or to be generated, or "UNDEF"
    std::string begin_epoch_utc; // YYYY-MM-DDTHHMMZ
    std::string end_epoch_utc;   // YYYY-MM-DDTHHMMZ, empty if live
    ChangelogLiveStatus live_status = ChangelogLiveStatus::FINALIZED;
    std::string suffix_hostname; // e.g., "mds01"
    std::string suffix_metalogger_tag; // ".ml" if metalogger, empty otherwise

    bool is_metalogger = false; // Derived or set

    // Raw values, useful for parsing/validation
    uint64_t first_id_val = 0;
    uint64_t last_id_val = 0; // 0 if UNDEF or not applicable
};

namespace changelog_filename {

/// Generates a changelog filename string based on the provided components.
/// The components struct should be populated appropriately.
/// FIRST_ID and LAST_ID are expected to be hex strings or "UNDEF".
/// BEGIN_EPOCH_UTC and END_EPOCH_UTC are expected in "YYYY-MM-DDTHHMMZ" format.
std::string generate_filename_from_components(const ChangelogNameComponents& components);

/// Parses a changelog filename string into its components.
/// Returns a ChangelogNameComponents struct.
/// Throws std::runtime_error if parsing fails or format is invalid.
ChangelogNameComponents parse_filename(const std::string& filename);

/// Helper to convert uint64_t ID to its 16-character zero-padded hex string.
std::string id_to_hex_string(uint64_t id);

/// Helper to convert 16-character zero-padded hex string to uint64_t ID.
/// Returns 0 on error or if input is "UNDEF".
uint64_t hex_string_to_id(const std::string& hex_str);

} // namespace changelog_filename
} // namespace sauna_fs
