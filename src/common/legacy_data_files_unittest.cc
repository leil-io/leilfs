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
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "unittests/TemporaryDirectory.h"

namespace {

void writeFile(const std::string &path, const std::string &content) {
	std::ofstream(path) << content;
}

}  // namespace

TEST(LegacyDataFilesTests, LegacyName) {
	using legacy_data_files::legacyName;
	EXPECT_EQ(legacyName("metadata.leil"), "metadata.sfs");
	EXPECT_EQ(legacyName("metadata.leil.1"), "metadata.sfs.1");
	EXPECT_EQ(legacyName("metadata_ml.leil.back"), "metadata_ml.sfs.back");
	EXPECT_EQ(legacyName("changelog.leil.12"), "changelog.sfs.12");
	EXPECT_EQ(legacyName("csstats.leil"), "csstats.sfs");
	// Not a ".leil" data file
	EXPECT_EQ(legacyName("metadata.mfs"), "metadata.mfs");
	EXPECT_EQ(legacyName("metadata.leilx"), "metadata.leilx");
	EXPECT_EQ(legacyName(".leil"), ".leil");
}

TEST(LegacyDataFilesTests, UsesNewNamesOnlyWhereThoseExist) {
	TemporaryDirectory dir("/tmp", "legacy-data-files");
	const std::string &d = dir.name();
	using legacy_data_files::usesLegacyNames;

	// An empty directory, e.g. of a new shadow master: its files are created under the old names
	EXPECT_TRUE(usesLegacyNames(d, {"metadata", "changelog"}));

	// A pre-rebrand installation, rotated files included
	writeFile(d + "/changelog.sfs.3", "");
	writeFile(d + "/metadata.sfs", "");
	EXPECT_TRUE(usesLegacyNames(d, {"metadata", "changelog"}));

	// The package's empty metadata file and another daemon's files don't count
	writeFile(d + "/metadata.leil.empty", "");
	writeFile(d + "/metadata_ml.leil", "");
	writeFile(d + "/changelogx.leil", "");
	EXPECT_TRUE(usesLegacyNames(d, {"metadata", "changelog"}));
	EXPECT_FALSE(usesLegacyNames(d, {"metadata_ml"}));

	// Renamed by hand, or copied from the empty one: any file under the new name switches the
	// daemon to the new names
	std::filesystem::rename(d + "/changelog.sfs.3", d + "/changelog.leil.3");
	EXPECT_FALSE(usesLegacyNames(d, {"metadata", "changelog"}));
}

TEST(LegacyDataFilesTests, UsesOldNamesForAnUnreadableDirectory) {
	EXPECT_TRUE(legacy_data_files::usesLegacyNames("/nonexistent/leil-data-dir", {"metadata"}));
}
