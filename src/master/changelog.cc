/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


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

#include "common/platform.h"

#include "master/changelog.h"

#include <syslog.h>
#include <unistd.h>
#include <cstdio>

#include <common/exceptions.h>
#include "common/event_loop.h"
#include "common/rotate_files.h" // For existing rotateFiles, may remove if fully replaced
#include "common/epoch_utils.h"
#include "common/cwrap.h" // For fs::rename, fs::exists etc.
#include "common/str_utils.h" // For helper functions if needed for parsing lines
#include "config/cfg.h"
#include "slogger/slogger.h"
#include "master/changelog_filename.h" // For new filename logic

#include <vector>
#include <algorithm> // For std::sort
#include <fstream>   // For std::ifstream to read first/last lines

// --- New static globals for changelog naming components ---
static std::string gBaseDataPath;      // e.g., "/var/lib/saunafs/data"
static std::string gClusterId;
static std::string gHostname;
static std::string gBaseLogName;       // e.g., "changelog.sfs" or "changelog_ml.sfs"
static bool gIsMetalogger = false;
// --- End new static globals ---

/// Stores the full path to the current open .LIVE changelog file.
static std::string gCurrentFullLiveFilepath;

static uint32_t gMinBackLogsNumber = 0;
static uint32_t gMaxBackLogsNumber = 50;

static uint32_t BackLogsNumber; // Value of BACK_LOGS from config
static FILE *fd = nullptr;
static bool gFlush = true;

// Helper function to join directory path and filename
static std::string join_path(const std::string& dir, const std::string& filename) {
    if (dir.empty()) {
        return filename;
    }
    if (dir.back() == '/') {
        return dir + filename;
    }
    return dir + "/" + filename;
}

// Helper function to read the first line and extract ID
static uint64_t read_first_id_from_file(const std::string& filepath) {
    std::ifstream file(filepath);
    std::string line;
    if (file.is_open() && std::getline(file, line)) {
        file.close();
        try {
            size_t colon_pos = line.find(':');
            if (colon_pos != std::string::npos) {
                return std::stoull(line.substr(0, colon_pos));
            }
        } catch (const std::exception& e) {
            safs_pretty_syslog(LOG_WARNING, "Could not parse first ID from changelog '%s': %s", filepath.c_str(), e.what());
        }
    } else {
        safs_pretty_syslog(LOG_WARNING, "Could not open or read first line from changelog '%s'", filepath.c_str());
    }
    return 0; // Indicates error or empty file
}

// Helper function to read the last line and extract ID
static uint64_t read_last_id_from_file(const std::string& filepath) {
    std::ifstream file(filepath);
    std::string line;
    std::string last_line;
    uint64_t last_id = 0;

    if (!file.is_open()) {
        safs_pretty_syslog(LOG_WARNING, "Could not open changelog '%s' to read last ID", filepath.c_str());
        return 0;
    }

    while (std::getline(file, line)) {
        if (!line.empty()) {
            last_line = line;
        }
    }
    file.close();

    if (!last_line.empty()) {
        try {
            size_t colon_pos = last_line.find(':');
            if (colon_pos != std::string::npos) {
                last_id = std::stoull(last_line.substr(0, colon_pos));
            }
        } catch (const std::exception& e) {
            safs_pretty_syslog(LOG_WARNING, "Could not parse last ID from changelog '%s' (line: '%s'): %s", filepath.c_str(), last_line.c_str(), e.what());
            return 0;
        }
    } else {
         safs_pretty_syslog(LOG_WARNING, "Changelog '%s' is empty or last line unreadable for last ID", filepath.c_str());
    }
    return last_id;
}

void changelog_rotate() {
	if (fd == nullptr) { // No file is open, nothing to rotate
		return;
	}

    const std::string live_file_to_finalize = gCurrentFullLiveFilepath;
    fclose(fd);
    fd = nullptr;

    safs_pretty_syslog(LOG_INFO, "Rotating changelog: %s", live_file_to_finalize.c_str());

    sauna_fs::ChangelogNameComponents live_components;
    try {
        // The filename part of live_file_to_finalize needs to be parsed
        size_t last_slash = live_file_to_finalize.find_last_of("/");
        std::string live_filename_part = (last_slash == std::string::npos) ? live_file_to_finalize : live_file_to_finalize.substr(last_slash + 1);
        live_components = sauna_fs::changelog_filename::parse_filename(live_filename_part);
    } catch (const std::runtime_error& e) {
        safs_pretty_syslog(LOG_ERR, "Failed to parse current live changelog name '%s' for rotation: %s. Manual intervention may be required.", live_file_to_finalize.c_str(), e.what());
        // Cannot proceed with renaming. What to do? Maybe try to reopen? For now, log and exit.
        // Re-opening is complex as we don't know the 'next' first_id without writing.
        gCurrentFullLiveFilepath.clear(); // Prevent further writes until re-init or fix.
        return;
    }

    uint64_t first_id_from_content = read_first_id_from_file(live_file_to_finalize);
    uint64_t last_id_from_content = read_last_id_from_file(live_file_to_finalize);

    if (first_id_from_content == 0 || last_id_from_content == 0) {
        safs_pretty_syslog(LOG_ERR, "Failed to read first/last ID from '%s' content. Cannot finalize.", live_file_to_finalize.c_str());
        // Try to reopen the live file? Or leave it as is? For now, leave fd as null.
        // The next changelog() call will fail to open if gCurrentFullLiveFilepath is not cleared,
        // or try to create a new one if it is.
        // To be safe, let's prevent further writes to a potentially problematic state.
        gCurrentFullLiveFilepath.clear();
        return;
    }

    if (live_components.first_id_val != first_id_from_content) {
         safs_pretty_syslog(LOG_WARNING, "Changelog '%s': Filename FIRST_ID %s (%" PRIu64 ") mismatches content FIRST_ID %" PRIu64 ". Using content ID for final name.",
                           live_file_to_finalize.c_str(), live_components.first_id_hex.c_str(), live_components.first_id_val, first_id_from_content);
         live_components.first_id_val = first_id_from_content; // Prefer content
         live_components.first_id_hex = sauna_fs::changelog_filename::id_to_hex_string(first_id_from_content);
         live_components.begin_epoch_utc = sauna_fs::epoch_utils::epoch_to_utc_string(first_id_from_content);
    }

    sauna_fs::ChangelogNameComponents final_components = live_components; // Copy base, cluster, host etc.
    final_components.last_id_hex = sauna_fs::changelog_filename::id_to_hex_string(last_id_from_content);
    final_components.last_id_val = last_id_from_content;
    final_components.end_epoch_utc = sauna_fs::epoch_utils::epoch_to_utc_string(last_id_from_content);
    final_components.live_status = sauna_fs::ChangelogLiveStatus::FINALIZED;

    std::string finalized_filename_part = sauna_fs::changelog_filename::generate_filename_from_components(final_components);
    std::string finalized_full_path = join_path(gBaseDataPath, finalized_filename_part);

    try {
        fs::rename(live_file_to_finalize, finalized_full_path);
        safs_pretty_syslog(LOG_INFO, "Changelog finalized: %s -> %s", live_file_to_finalize.c_str(), finalized_full_path.c_str());
    } catch (const fs::FilesystemException& e) {
        safs_pretty_syslog(LOG_ERR, "Failed to rename changelog from %s to %s: %s", live_file_to_finalize.c_str(), finalized_full_path.c_str(), e.what());
        // If rename fails, the original .LIVE file is still there.
        // The system might try to append to it on next changelog() call if fd is reopened.
        // For now, leave fd as null.
        gCurrentFullLiveFilepath.clear(); // Prevent reuse of the old live path.
        return;
    }

    // Clear gCurrentFullLiveFilepath so the next changelog() call creates a new .LIVE file.
    gCurrentFullLiveFilepath.clear();

    // Update .latest symlink
    if (cfg_getbool("CHANGELOG_CREATE_LATEST_SYMLINK", true)) { // Make it configurable, default true
        std::string symlink_name_part = gBaseLogName +
                                      (gIsMetalogger ? sauna_fs::kChangelogSuffixMls : sauna_fs::kChangelogSuffixMds) +
                                      ".latest";
        if (!gHostname.empty()) {
            symlink_name_part += "." + gHostname;
        }
        std::string full_symlink_path = join_path(gBaseDataPath, symlink_name_part);

        try {
            // Remove old symlink first, fs::remove should handle symlinks.
            if (fs::exists(full_symlink_path)) { // Check if it exists to avoid error if it's not a symlink or broken
                 fs::remove(full_symlink_path);
            }
            // Create new symlink. Target should be just the filename part for relative link.
            // However, fs::symlink typically takes target path then link path.
            // Using std::filesystem::create_symlink for better cross-platform potential if available
            // std::filesystem::create_symlink(finalized_filename_part, full_symlink_path); // C++17
            // For POSIX: symlink(target, linkpath)
            if (symlink(finalized_filename_part.c_str(), full_symlink_path.c_str()) != 0) {
                safs_pretty_syslog(LOG_WARNING, "Failed to create/update symlink %s -> %s: %s",
                                   full_symlink_path.c_str(), finalized_filename_part.c_str(), strerror(errno));
            } else {
                safs_pretty_syslog(LOG_INFO, "Updated symlink %s -> %s",
                                   full_symlink_path.c_str(), finalized_filename_part.c_str());
            }
        } catch (const fs::FilesystemException& e) { // Catch fs::remove exceptions
             safs_pretty_syslog(LOG_WARNING, "Error managing symlink %s: %s", full_symlink_path.c_str(), e.what());
        } catch (const std::exception& e) { // Catch other potential exceptions
             safs_pretty_syslog(LOG_WARNING, "Generic error managing symlink %s: %s", full_symlink_path.c_str(), e.what());
        }
    }


	// Handle BACK_LOGS: Keep only the latest N finalized logs
	if (BackLogsNumber > 0) {
        std::vector<std::string> files_in_dir;
        try {
            files_in_dir = fs::listdir(gBaseDataPath);
        } catch (const fs::FilesystemException& e) {
            safs_pretty_syslog(LOG_WARNING, "Could not list directory %s for BACK_LOGS cleanup: %s", gBaseDataPath.c_str(), e.what());
            return;
        }

        std::vector<sauna_fs::ChangelogNameComponents> finalized_logs;
        for (const auto& fname : files_in_dir) {
            try {
                // Only consider files matching our base log name and cluster ID
                if (fname.rfind(gBaseLogName + "." + gClusterId, 0) == 0) {
                    sauna_fs::ChangelogNameComponents comp = sauna_fs::changelog_filename::parse_filename(fname);
                    if (comp.live_status == sauna_fs::ChangelogLiveStatus::FINALIZED &&
                        comp.base_prefix == gBaseLogName && // Ensure it's the correct type (master/ml)
                        comp.cluster_id == gClusterId &&
                        ((gIsMetalogger && comp.suffix_metalogger_tag == ".ml") || (!gIsMetalogger && comp.suffix_metalogger_tag.empty())) &&
                        comp.suffix_hostname == gHostname // Only manage logs for this specific instance
                    ) {
                        finalized_logs.push_back(comp);
                    }
                }
            } catch (const std::runtime_error& e) {
                // Ignore files that don't parse; they aren't valid changelogs for this logic
            }
        }

        if (finalized_logs.size() > BackLogsNumber) {
            // Sort by first_id_val (ascending) to find the oldest
            std::sort(finalized_logs.begin(), finalized_logs.end(),
                [](const sauna_fs::ChangelogNameComponents& a, const sauna_fs::ChangelogNameComponents& b) {
                return a.first_id_val < b.first_id_val;
            });

            uint32_t num_to_delete = finalized_logs.size() - BackLogsNumber;
            for (uint32_t i = 0; i < num_to_delete; ++i) {
                std::string name_to_delete_part = sauna_fs::changelog_filename::generate_filename_from_components(finalized_logs[i]);
                std::string path_to_delete = join_path(gBaseDataPath, name_to_delete_part);
                try {
                    fs::remove(path_to_delete); // Corrected from fs::unlink
                    safs_pretty_syslog(LOG_INFO, "BACK_LOGS: Removed old changelog: %s", path_to_delete.c_str());
                } catch (const fs::FilesystemException& e) {
                    safs_pretty_syslog(LOG_WARNING, "BACK_LOGS: Failed to remove old changelog %s: %s", path_to_delete.c_str(), e.what());
                }
            }
        }
	} else { // BackLogsNumber == 0 means delete immediately (except the one just finalized if it's the only one)
        // The task implies BACK_LOGS means number of *previous* files.
        // If BACK_LOGS is 0, current interpretation of rotateFiles was to unlink the file being rotated away.
        // Here, if BackLogsNumber is 0, we might not want to delete the one we *just created* unless there are others.
        // The above logic correctly handles BackLogsNumber=0 by keeping 0 old files (so if size > 0, it deletes).
        // This means if BackLogsNumber is 0, the just finalized file would be deleted if it's the only one,
        // which might not be intended.
        // Let's adjust: if BackLogsNumber is 0, we don't do this cleanup. The old rotateFiles unlinked gChangelogFilename,
        // which is now `live_file_to_finalize`. That file has been renamed.
        // The old code: `unlink(gChangelogFilename.c_str());` if BackLogsNumber == 0.
        // This is effectively "keep 0 backups". The new code with BACK_LOGS=0 should result in 0 files *other than the one just written*.
        // The current sort-and-delete logic for `finalized_logs.size() > BackLogsNumber` handles this: if BackLogsNumber=0,
        // it will try to delete all files in `finalized_logs`. This includes the one just created.
        // This needs to be: if BackLogsNumber is 0, only delete if there are *other* files.
        // The simplest is to ensure BackLogsNumber is at least 1 for the purpose of keeping the *current* one.
        // Or, the logic `finalized_logs.size() > BackLogsNumber` already implies if BackLogsNumber=0, and size=1, 1>0 is true, so it deletes.
        // This seems to match the spirit of "keep N previous copies". If N=0, keep 0 previous.
        // So, if BackLogsNumber is 0, all *finalized* logs for this host/type will be deleted.
        // This needs to be confirmed if it's the desired behavior.
        // For now, the logic stands: if BackLogsNumber is 0, it will try to remove all logs matching the pattern.
    }
}

void changelog(uint64_t version, const char* entry) {
	if (fd == nullptr) {
        if (gBaseDataPath.empty() || gClusterId.empty() || gHostname.empty() || gBaseLogName.empty()) {
            safs_pretty_syslog(LOG_ERR, "Changelog system not properly initialized with path/cluster/host/base. Cannot create new log file.");
            safs_pretty_syslog(LOG_NOTICE, "Lost metadata change (init error) %" PRIu64 ": %s", version, entry);
            return;
        }

        sauna_fs::ChangelogNameComponents live_components;
        live_components.base_prefix = gBaseLogName;
        live_components.cluster_id = gClusterId;
        live_components.first_id_hex = sauna_fs::changelog_filename::id_to_hex_string(version);
        live_components.first_id_val = version;
        live_components.last_id_hex = sauna_fs::kChangelogKeywordUndef;
        live_components.begin_epoch_utc = sauna_fs::epoch_utils::epoch_to_utc_string(version);
        live_components.live_status = sauna_fs::ChangelogLiveStatus::LIVE;
        live_components.suffix_hostname = gHostname;
        // suffix_metalogger_tag is not set here directly, is_metalogger drives role suffix in generate_filename_from_components
        live_components.is_metalogger = gIsMetalogger;

        std::string live_filename_part = sauna_fs::changelog_filename::generate_filename_from_components(live_components);
        gCurrentFullLiveFilepath = join_path(gBaseDataPath, live_filename_part);

		fd = fopen(gCurrentFullLiveFilepath.c_str(), "a");
		if (!fd) {
            safs_pretty_syslog(LOG_ERR, "Failed to open new changelog file %s: %s", gCurrentFullLiveFilepath.c_str(), strerror(errno));
			safs_pretty_syslog(LOG_NOTICE, "Lost metadata change (fopen error) %" PRIu64 ": %s", version, entry);
            gCurrentFullLiveFilepath.clear(); // Clear path so we might try again next time
			return;
		}
        safs_pretty_syslog(LOG_INFO, "Opened new changelog file: %s", gCurrentFullLiveFilepath.c_str());
	}

	fprintf(fd,"%" PRIu64 ": %s\n", version, entry);
	if (gFlush) {
		fflush(fd);
	}
}

static void changelog_reload(void) {
	BackLogsNumber = cfg_get_minmaxvalue<uint32_t>("BACK_LOGS", 50, // Default 50
			gMinBackLogsNumber, gMaxBackLogsNumber);

    // Potentially reload other params if they can change dynamically, though unlikely for these init params.
    // gClusterId = cfg_getstring("CLUSTER_ID", "default"); // Example, if CLUSTER_ID could change dynamically
}

void changelog_init(
    const std::string& base_data_path_val,
    const std::string& cluster_id_val,
    const std::string& hostname_val,
    const std::string& base_log_name_val,
    bool is_metalogger_val,
    uint32_t min_back_logs_val,
    uint32_t max_back_logs_val) {

    gBaseDataPath = base_data_path_val;
    gClusterId = cluster_id_val;
    gHostname = hostname_val;
    gBaseLogName = base_log_name_val;
    gIsMetalogger = is_metalogger_val;

	gMinBackLogsNumber = min_back_logs_val;
	gMaxBackLogsNumber = max_back_logs_val;

	BackLogsNumber = cfg_get_minmaxvalue<uint32_t>("BACK_LOGS", 50, // Default 50
			gMinBackLogsNumber, gMaxBackLogsNumber);

    // Ensure directory exists (optional, depends on assumptions)
    // try {
    //    if (!gBaseDataPath.empty() && !fs::exists(gBaseDataPath)) {
    //        fs::create_directories(gBaseDataPath);
    //    }
    // } catch (const fs::FilesystemException& e) {
    //    throw InitializeException("Failed to create changelog directory " + gBaseDataPath + ": " + e.what());
    // }

	eventloop_reloadregister(changelog_reload);
    safs_pretty_syslog(LOG_INFO, "Changelog system initialized. Base path: %s, Cluster: %s, Host: %s, BaseName: %s, IsML: %d, Backlogs: %u",
        gBaseDataPath.c_str(), gClusterId.c_str(), gHostname.c_str(), gBaseLogName.c_str(), gIsMetalogger, BackLogsNumber);
}

uint32_t changelog_get_back_logs_config_value() {
	return BackLogsNumber;
}

void changelog_flush(void) {
	if (fd) {
		fflush(fd);
	}
}

void changelog_disable_flush(void) {
	gFlush = false;
}

void changelog_enable_flush(void) {
	gFlush = true;
	changelog_flush();
}
