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
#include "common/epoch_utils.h" // For generating test timestamps
#include "gtest/gtest.h"

namespace sauna_fs {
namespace changelog_filename {

TEST(ChangelogFilenameTest, IDToHexConversion) {
    EXPECT_EQ("0000000000000000", id_to_hex_string(0));
    EXPECT_EQ("000000000000000A", id_to_hex_string(10));
    EXPECT_EQ("00000000683DB60E", id_to_hex_string(1748985806)); // Example from task
    EXPECT_EQ("FFFFFFFFFFFFFFFF", id_to_hex_string(0xFFFFFFFFFFFFFFFFULL));
}

TEST(ChangelogFilenameTest, HexToIDConversion) {
    EXPECT_EQ(0ULL, hex_string_to_id("0000000000000000"));
    EXPECT_EQ(10ULL, hex_string_to_id("000000000000000A"));
    EXPECT_EQ(10ULL, hex_string_to_id("000000000000000a")); // Lowercase
    EXPECT_EQ(1748985806ULL, hex_string_to_id("00000000683DB60E"));
    EXPECT_EQ(0xFFFFFFFFFFFFFFFFULL, hex_string_to_id("FFFFFFFFFFFFFFFF"));
    EXPECT_EQ(0ULL, hex_string_to_id("UNDEF"));
    EXPECT_EQ(0ULL, hex_string_to_id("INVALIDHEX")); // Invalid
    EXPECT_EQ(0ULL, hex_string_to_id("123")); // Too short
}


TEST(ChangelogFilenameTest, GenerateLiveMetaloggerFilename) {
    ChangelogNameComponents components;
    components.base_prefix = kChangelogMlBaseName;
    components.cluster_id = "alpha01";
    components.first_id_hex = id_to_hex_string(1748985806ULL);
    components.last_id_hex = kChangelogKeywordUndef; // Use constant
    components.begin_epoch_utc = epoch_utils::epoch_to_utc_string(1748985806ULL); // "2025-06-05T1423Z"
    components.live_status = ChangelogLiveStatus::LIVE;
    components.suffix_hostname = "metalogger1";
    // components.suffix_metalogger_tag = kChangelogSuffixMls; // Set for completeness, though generator uses is_metalogger
    components.is_metalogger = true;

    std::string expected = "changelog_ml.sfs.alpha01.00000000683DB60E.UNDEF.2025-06-05T1423Z.LIVE.mls.metalogger1"; // Changed .ml to .mls
    EXPECT_EQ(expected, generate_filename_from_components(components));
}

TEST(ChangelogFilenameTest, GenerateFinalizedMetaloggerFilename) {
    ChangelogNameComponents components;
    components.base_prefix = kChangelogMlBaseName;
    components.cluster_id = "alpha01";
    components.first_id_hex = id_to_hex_string(1748985806ULL);
    components.last_id_hex = id_to_hex_string(1750468539ULL);
    components.begin_epoch_utc = epoch_utils::epoch_to_utc_string(1748985806ULL); // "2025-06-05T1423Z"
    components.end_epoch_utc = epoch_utils::epoch_to_utc_string(1750468539ULL);   // "2025-06-22T1555Z"
    components.live_status = ChangelogLiveStatus::FINALIZED;
    components.suffix_hostname = "metalogger1";
    // components.suffix_metalogger_tag = kChangelogSuffixMls; // Set for completeness
    components.is_metalogger = true;

    std::string expected = "changelog_ml.sfs.alpha01.00000000683DB60E.0000000068F844BB.2025-06-05T1423Z.2025-06-22T1555Z.mls.metalogger1"; // Changed .ml to .mls
    // Corrected LAST_ID hex for 1750468539ULL is 0000000068F844BB
    EXPECT_EQ(expected, generate_filename_from_components(components));
}

TEST(ChangelogFilenameTest, GenerateFinalizedMasterFilename) {
    ChangelogNameComponents components;
    components.base_prefix = kChangelogBaseName;
    components.cluster_id = "alpha01";
    components.first_id_hex = id_to_hex_string(1748985806ULL);
    components.last_id_hex = id_to_hex_string(1750468539ULL);
    components.begin_epoch_utc = epoch_utils::epoch_to_utc_string(1748985806ULL);
    components.end_epoch_utc = epoch_utils::epoch_to_utc_string(1750468539ULL);
    components.live_status = ChangelogLiveStatus::FINALIZED;
    components.suffix_hostname = "mds01";
    // components.suffix_metalogger_tag = kChangelogSuffixMds; // Set for completeness
    components.is_metalogger = false;

    std::string expected = "changelog.sfs.alpha01.00000000683DB60E.0000000068F844BB.2025-06-05T1423Z.2025-06-22T1555Z.mds.mds01"; // Added .mds
    EXPECT_EQ(expected, generate_filename_from_components(components));
}

TEST(ChangelogFilenameTest, GenerateFilenameNoHostname) {
    ChangelogNameComponents components;
    components.base_prefix = kChangelogBaseName;
    components.cluster_id = "beta02";
    components.first_id_hex = id_to_hex_string(1000ULL);
    components.last_id_hex = id_to_hex_string(2000ULL);
    components.begin_epoch_utc = epoch_utils::epoch_to_utc_string(1000ULL); // "1970-01-01T0016Z"
    components.end_epoch_utc = epoch_utils::epoch_to_utc_string(2000ULL);   // "1970-01-01T0033Z"
    components.live_status = ChangelogLiveStatus::FINALIZED;
    components.suffix_hostname = ""; // No hostname
    // components.suffix_metalogger_tag = kChangelogSuffixMds; // Set for completeness
    components.is_metalogger = false;

    std::string expected = "changelog.sfs.beta02.00000000000003E8.00000000000007D0.1970-01-01T0016Z.1970-01-01T0033Z.mds"; // Added .mds
    EXPECT_EQ(expected, generate_filename_from_components(components));
}

TEST(ChangelogFilenameTest, ParseLiveMetaloggerFilename) {
    std::string filename = "changelog_ml.sfs.alpha01.00000000683DB60E.UNDEF.2025-06-05T1423Z.LIVE.mls.metalogger1"; // .ml to .mls
    ChangelogNameComponents c = parse_filename(filename);

    EXPECT_EQ(kChangelogMlBaseName, c.base_prefix);
    EXPECT_TRUE(c.is_metalogger);
    EXPECT_EQ("alpha01", c.cluster_id);
    EXPECT_EQ("00000000683DB60E", c.first_id_hex);
    EXPECT_EQ(1748985806ULL, c.first_id_val);
    EXPECT_EQ(kChangelogKeywordUndef, c.last_id_hex); // use constant
    EXPECT_EQ(0ULL, c.last_id_val);
    EXPECT_EQ("2025-06-05T1423Z", c.begin_epoch_utc);
    EXPECT_EQ("", c.end_epoch_utc);
    EXPECT_EQ(ChangelogLiveStatus::LIVE, c.live_status);
    EXPECT_EQ(kChangelogSuffixMls, c.suffix_metalogger_tag); // check for .mls
    EXPECT_EQ("metalogger1", c.suffix_hostname);
}

TEST(ChangelogFilenameTest, ParseFinalizedMasterFilename) {
    std::string filename = "changelog.sfs.alpha01.00000000683DB60E.0000000068F844BB.2025-06-05T1423Z.2025-06-22T1555Z.mds.mds01"; // added .mds
    ChangelogNameComponents c = parse_filename(filename);

    EXPECT_EQ(kChangelogBaseName, c.base_prefix);
    EXPECT_FALSE(c.is_metalogger);
    EXPECT_EQ("alpha01", c.cluster_id);
    EXPECT_EQ("00000000683DB60E", c.first_id_hex);
    EXPECT_EQ(1748985806ULL, c.first_id_val);
    EXPECT_EQ("0000000068F844BB", c.last_id_hex);
    EXPECT_EQ(1750468539ULL, c.last_id_val);
    EXPECT_EQ("2025-06-05T1423Z", c.begin_epoch_utc);
    EXPECT_EQ("2025-06-22T1555Z", c.end_epoch_utc);
    EXPECT_EQ(ChangelogLiveStatus::FINALIZED, c.live_status);
    EXPECT_EQ(kChangelogSuffixMds, c.suffix_metalogger_tag); // check for .mds
    EXPECT_EQ("mds01", c.suffix_hostname);
}

TEST(ChangelogFilenameTest, ParseFilenameNoHostname) {
    std::string filename = "changelog.sfs.beta02.00000000000003E8.00000000000007D0.1970-01-01T0016Z.1970-01-01T0033Z.mds"; // Added .mds
    ChangelogNameComponents c = parse_filename(filename);

    EXPECT_EQ(kChangelogBaseName, c.base_prefix);
    EXPECT_FALSE(c.is_metalogger);
    EXPECT_EQ("beta02", c.cluster_id);
    EXPECT_EQ("00000000000003E8", c.first_id_hex);
    EXPECT_EQ(1000ULL, c.first_id_val);
    EXPECT_EQ("00000000000007D0", c.last_id_hex);
    EXPECT_EQ(2000ULL, c.last_id_val);
    EXPECT_EQ("1970-01-01T0016Z", c.begin_epoch_utc);
    EXPECT_EQ("1970-01-01T0033Z", c.end_epoch_utc);
    EXPECT_EQ(ChangelogLiveStatus::FINALIZED, c.live_status);
    EXPECT_EQ(kChangelogSuffixMds, c.suffix_metalogger_tag); // check for .mds
    EXPECT_EQ("", c.suffix_hostname);
}

TEST(ChangelogFilenameTest, ParseFilenameMetaloggerNoHostname) {
    std::string filename = "changelog_ml.sfs.gamma03.0000000000000001.0000000000000002.1970-01-01T0000Z.1970-01-01T0001Z.mls"; // .ml to .mls
     ChangelogNameComponents c = parse_filename(filename);
    EXPECT_EQ(kChangelogMlBaseName, c.base_prefix);
    EXPECT_TRUE(c.is_metalogger);
    EXPECT_EQ(kChangelogSuffixMls, c.suffix_metalogger_tag); // check for .mls
    EXPECT_EQ("", c.suffix_hostname); // No hostname part
}


TEST(ChangelogFilenameTest, ParseInvalidFilenames) {
    EXPECT_THROW(parse_filename("changelog.sfs.cluster.first.last.begin.state"), std::runtime_error); // Too few parts (missing role)
    EXPECT_THROW(parse_filename("wrong_prefix.sfs.alpha01.0000000000000001.0000000000000002.time1.time2.mds.host"), std::runtime_error);
    EXPECT_THROW(parse_filename("changelog.sfs.alpha01.NOTHEX.UNDEF.time1.LIVE.mds.host"), std::runtime_error);
    EXPECT_THROW(parse_filename("changelog.sfs.alpha01.0000000000000001.NOTHEX.time1.time2.mds.host"), std::runtime_error);
    EXPECT_THROW(parse_filename("changelog.sfs.alpha01.0000000000000001.UNDEF.time1.2025-01-01T1200Z.mds.host"), std::runtime_error); // Finalized (has end time) but UNDEF last_id
    EXPECT_THROW(parse_filename("changelog.sfs.alpha01.0000000000000001.0000000000000002.time1.LIVE.mds.host"), std::runtime_error); // LIVE but not UNDEF last_id

    // Mismatched base_prefix and role_suffix
    EXPECT_THROW(parse_filename("changelog.sfs.alpha01.0000000000000001.UNDEF.time1.LIVE.mls.host"), std::runtime_error); // .mls role with non-ml base
    EXPECT_THROW(parse_filename("changelog_ml.sfs.alpha01.0000000000000001.UNDEF.time1.LIVE.mds.host"), std::runtime_error); // .mds role with ml base

    // Missing hostname when role suffix implies it should be there (depends on strictness, current parser allows no hostname)
    // The parser now allows optional hostname, so "changelog_ml.sfs.alpha01.ID1.ID2.T1.T2.mls" is valid.
    // EXPECT_THROW(parse_filename("changelog_ml.sfs.alpha01.0000000000000001.0000000000000002.time1.time2.mls"), std::runtime_error); // Missing hostname

    EXPECT_THROW(parse_filename("changelog.sfs.id.first.last.begin.end.mds.host.extra"), std::runtime_error); // Too many parts
    EXPECT_THROW(parse_filename("changelog.sfs.id.first.last.begin.LIVE.badrole.host"), std::runtime_error); // Invalid role
}

TEST(ChangelogFilenameTest, ParseUndefNotLiveFilename) {
    // This test case is tricky with the new mandatory state part (LIVE or END_EPOCH_UTC).
    // An "UNDEF" last_id without a "LIVE" keyword and without an "END_EPOCH_UTC"
    // doesn't fit the strict new format if a role suffix follows directly.
    // Example: "changelog.sfs.delta04.ID1.UNDEF.BEGIN_UTC.mds.host1"
    // Here, "mds" would be misinterpreted as the state_part.
    // The parser will now reject this because state_part must be LIVE or a timestamp.
    // Thus, this specific test for a "special state" of UNDEF_LAST_ID without LIVE is no longer valid
    // for filenames that are supposed to include a role suffix immediately after the state.
    // If such files need to be parsed (e.g. from a transition period), the parser would need to be more lenient
    // or have a special mode. For now, enforcing the new structure.
    std::string filename_old_style_undef = "changelog.sfs.delta04.000000000000000A.UNDEF.1970-01-01T0100Z.host1"; // Missing role, will fail on part count
    EXPECT_THROW(parse_filename(filename_old_style_undef), std::runtime_error);

    // A valid LIVE file with UNDEF:
    std::string filename_live_undef = "changelog.sfs.delta04.000000000000000A.UNDEF.1970-01-01T0100Z.LIVE.mds.host1";
    ChangelogNameComponents c = parse_filename(filename_live_undef);
    EXPECT_EQ(kChangelogBaseName, c.base_prefix);
    EXPECT_FALSE(c.is_metalogger);
    EXPECT_EQ("delta04", c.cluster_id);
    EXPECT_EQ("000000000000000A", c.first_id_hex);
    EXPECT_EQ(10ULL, c.first_id_val);
    EXPECT_EQ(kChangelogKeywordUndef, c.last_id_hex);
    EXPECT_EQ(0ULL, c.last_id_val);
    EXPECT_EQ("1970-01-01T0100Z", c.begin_epoch_utc);
    EXPECT_EQ("", c.end_epoch_utc); // No end epoch for LIVE
    EXPECT_EQ(ChangelogLiveStatus::LIVE, c.live_status);
    EXPECT_EQ(kChangelogSuffixMds, c.suffix_metalogger_tag);
    EXPECT_EQ("host1", c.suffix_hostname);
}


} // namespace changelog_filename
} // namespace sauna_fs
