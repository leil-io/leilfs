/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "common/platform.h"

#include <unistd.h>

#include <string>

#include "common/legacy_data_files.h"

// The data file names. A daemon whose data directory only has their pre-rebrand names (*.sfs)
// switches to those at startup, before it uses any, see useLegacyDataFileNames().
inline const char *kMetadataFilename          = "metadata.leil";
inline const char *kMetadataTmpFilename       = "metadata.leil.tmp";
inline const char *kMetadataEmergencyFilename = "metadata.leil.emergency";
inline const char *kMetadataMlFilename        = "metadata_ml.leil";
inline const char *kMetadataMlTmpFilename     = "metadata_ml.leil.tmp";
inline const char *kChangelogFilename         = "changelog.leil";
inline const char *kChangelogTmpFilename      = "changelog.leil.tmp";
inline const char *kChangelogMlFilename       = "changelog_ml.leil";
inline const char *kChangelogMlTmpFilename    = "changelog_ml.leil.tmp";
inline const char *kSessionsFilename          = "sessions.leil";
inline const char *kSessionsTmpFilename       = "sessions.leil.tmp";
inline const char *kSessionsMlFilename        = "sessions_ml.leil";
inline const char *kSessionsMlTmpFilename     = "sessions_ml.leil.tmp";
inline const char *kStatsFilename             = "stats.leil";

/// The metadata file name before SaunaFS, still read when it is the only one
constexpr const char *kMetadataLegacyFilename = "metadata.mfs";

/// Switches the data file names above to their pre-rebrand ones (*.sfs), for a daemon whose data
/// directory only has those (legacy_data_files::usesLegacyNames): it keeps using the files it
/// used before the rebrand until they are renamed by hand, with the daemon stopped.
inline void useLegacyDataFileNames() {
	kMetadataFilename          = "metadata.sfs";
	kMetadataTmpFilename       = "metadata.sfs.tmp";
	kMetadataEmergencyFilename = "metadata.sfs.emergency";
	kMetadataMlFilename        = "metadata_ml.sfs";
	kMetadataMlTmpFilename     = "metadata_ml.sfs.tmp";
	kChangelogFilename         = "changelog.sfs";
	kChangelogTmpFilename      = "changelog.sfs.tmp";
	kChangelogMlFilename       = "changelog_ml.sfs";
	kChangelogMlTmpFilename    = "changelog_ml.sfs.tmp";
	kSessionsFilename          = "sessions.sfs";
	kSessionsTmpFilename       = "sessions.sfs.tmp";
	kSessionsMlFilename        = "sessions_ml.sfs";
	kSessionsMlTmpFilename     = "sessions_ml.sfs.tmp";
	kStatsFilename             = "stats.sfs";
}

/// True if `dir` looks like an initialized data directory rather than an
/// empty one freshly created by package install/upgrade (metadata.leil.empty
/// is always installed into the default data dir regardless of state, so
/// bare directory existence can't tell the two apart). Also recognizes the
/// ".1" rotated backup MasterConn::downloadNext() leaves behind (see
/// rotateFiles() in masterconn.cc), so a directory left with only a rotated
/// backup after a crash during a metadata dump is still recognized as
/// having data instead of reading as empty. Pre-rebrand names count too.
inline bool hasKnownMetadata(const std::string &dir) {
	// The current names as literals: this runs before a daemon picks its data file names
	static const std::string kKnownMetadataFilenames[] = {
	    "metadata.leil", kMetadataLegacyFilename, "metadata_ml.leil", "metadata.leil.1",
	    "metadata_ml.leil.1"};
	for (const auto &name : kKnownMetadataFilenames) {
		for (const auto &candidate : {name, legacy_data_files::legacyName(name)}) {
			if (access((dir + "/" + candidate).c_str(), F_OK) == 0) { return true; }
		}
	}
	return false;
}
