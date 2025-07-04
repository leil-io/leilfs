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

#include "master/changelog.h"
#include "config/cfg.h" // For cfg_load, cfg_term, and to set BACK_LOGS
#include "common/cwrap.h" // For fs::exists, fs::create_directories (if needed)
#include "slogger/slogger.h" // For safs_pretty_syslog

#include <iostream>
#include <string>
#include <vector>
#include <fstream> // For writing temp config

// Dummy eventloop_reloadregister for this standalone tool
// as changelog_init calls it.
extern "C" void eventloop_reloadregister(void (*)(void)) {}
// Dummy cfg_filename
std::string cfg_filename() { return "/tmp/dummy_cfg_for_util.cfg"; }


int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: changelog_test_util <command> [args...]" << std::endl;
        std::cerr << "Commands:" << std::endl;
        std::cerr << "  init <data_path> <cluster_id> <hostname> <base_name> <is_ml_flag (0/1)> <back_logs_val (uint)> <min_bl (uint)> <max_bl (uint)>" << std::endl;
        std::cerr << "  write <timestamp (uint64)> <data_string>" << std::endl;
        std::cerr << "  rotate" << std::endl;
        std::cerr << "  flush" << std::endl;
        std::cerr << "  disableflush" << std::endl;
        std::cerr << "  enableflush" << std::endl;
        return 1;
    }

    // Basic slogger init for logs from changelog.cc
    safs::setup_logs_appname("changelog_test_util");

    std::string command = argv[1];

    if (command == "init") {
        if (argc != 10) {
            std::cerr << "init usage: <data_path> <cluster_id> <hostname> <base_name> <is_ml_flag> <back_logs_val> <min_bl> <max_bl>" << std::endl;
            return 1;
        }
        std::string data_path = argv[2];
        std::string cluster_id = argv[3];
        std::string hostname = argv[4];
        std::string base_name = argv[5];
        bool is_ml = (std::stoi(argv[6]) == 1);
        uint32_t back_logs_val = std::stoul(argv[7]);
        uint32_t min_bl = std::stoul(argv[8]);
        uint32_t max_bl = std::stoul(argv[9]);

        // Create a temporary config file to set BACK_LOGS
        // because changelog_init internally reads it via cfg_get_minmaxvalue.
        // The path for cfg_load needs to be consistent with what cfg_filename() might return,
        // or cfg_filename needs to be controllable. For this util, just create it.
        std::string temp_cfg_for_util = data_path + "/dummy_test_util.cfg";
        std::ofstream cfg_out(temp_cfg_for_util);
        if (!cfg_out) {
            std::cerr << "Failed to create temporary config file: " << temp_cfg_for_util << std::endl;
            return 1;
        }
        cfg_out << "BACK_LOGS = " << back_logs_val << std::endl;
        cfg_out.close();

        // Ensure data_path exists
        // Using std::filesystem for simplicity, requires C++17
        // If not available, use platform-specific or common/cwrap.h alternatives
        try {
            // A proper fs::create_directories from cwrap.h would be better if it exists
             if (!fs::exists(data_path)) {
                if (mkdir(data_path.c_str(), 0755) != 0 && errno != EEXIST) {
                     std::cerr << "Failed to create data_path: " << data_path << " error: " << strerror(errno) << std::endl;
                     return 1;
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error with data_path " << data_path << ": " << e.what() << std::endl;
            return 1;
        }


        if (cfg_load(temp_cfg_for_util.c_str(), 0) != 0) {
             std::cerr << "Failed to load temporary config file: " << temp_cfg_for_util << std::endl;
            // continue anyway, will use default BACK_LOGS from changelog.cc
        }

        try {
            changelog_init(data_path, cluster_id, hostname, base_name, is_ml, min_bl, max_bl);
            std::cout << "Changelog initialized." << std::endl;
        } catch (const InitializeException& e) {
            std::cerr << "Initialization failed: " << e.what() << std::endl;
            cfg_term();
            return 1;
        }
        // cfg_term(); // Terminate cfg after init uses it.
    } else if (command == "write") {
        if (argc != 4) {
            std::cerr << "write usage: <timestamp> <data_string>" << std::endl;
            return 1;
        }
        uint64_t ts = std::stoull(argv[2]);
        std::string data = argv[3];
        changelog(ts, data.c_str());
        // std::cout << "Wrote: " << ts << ": " << data << std::endl;
    } else if (command == "rotate") {
        changelog_rotate();
        std::cout << "Rotate called." << std::endl;
    } else if (command == "flush") {
        changelog_flush();
        std::cout << "Flush called." << std::endl;
    } else if (command == "disableflush") {
        changelog_disable_flush();
        std::cout << "Disable flush called." << std::endl;
    } else if (command == "enableflush") {
        changelog_enable_flush();
        std::cout << "Enable flush called." << std::endl;
    } else {
        std::cerr << "Unknown command: " << command << std::endl;
        return 1;
    }

    // Clean up the dummy config if it was loaded by this tool specifically for init
    // Only if cfg_term was not called right after init.
    // This might be problematic if changelog_reload is triggered by other means.
    // For this simple tool, it might be okay to leave cfg loaded or term at end.
    cfg_term();

    return 0;
}
