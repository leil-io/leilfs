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

#include "common/platform.h"

#include <vector>

#include "common/legacy_data_files.h"
#include "common/run_tab.h"
#include "master/masterconn.h"
#include "master/metadata_backend_common.h"
#include "master/metadata_backend_file.h"

inline int metadata_backend_init() {
	if (gMetadataBackend == nullptr) {
		gMetadataBackend = std::make_unique<MetadataBackendFile>();

		if (!gMetadataBackend) {
			safs::log_err("Failed to initialize metadata backend");
			throw Exception("Failed to initialize metadata backend");
		}
	}

	return 0;
}

/// Keeps the pre-rebrand data file names (*.sfs) where the data directory only has those
inline int data_file_names_init() {
	if (legacy_data_files::usesLegacyNames(".", {"metadata_ml", "changelog_ml", "sessions_ml"})) {
		useLegacyDataFileNames();
	}
	return 0;
}

/// Functions to call before normal startup
inline const std::vector<RunTab> earlyRunTabs = {};

/// Functions to call during normal startup
inline const std::vector<RunTab> runTabs = {
    // has to be before anything uses its data files
    RunTab{.function = data_file_names_init, .name = "data file names"},
    RunTab{.function = metadata_backend_init, .name = "metadata backend"},
    RunTab{.function = masterconn_init, .name = "connection with master"}};

/// Functions to call delayed after the initialization is correct
inline const std::vector<RunTab> lateRunTabs = {};
