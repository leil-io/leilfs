/*
   Copyright 2013-2014 EditShare
   Copyright 2013-2017 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

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
#include <iostream>
#include <ostream>

#include "admin/chunk_health_command.h"
#include "admin/delete_sessions_command.h"
#include "admin/dump_config_command.h"
#include "admin/info_command.h"
#include "admin/io_limits_status_command.h"
#include "admin/list_chunkservers_command.h"
#include "admin/list_defective_files_command.h"
#include "admin/list_disk_groups_command.h"
#include "admin/list_disks_command.h"
#include "admin/list_goals_command.h"
#include "admin/list_inotifiers_command.h"
#include "admin/list_metadataservers_command.h"
#include "admin/list_mounts_command.h"
#include "admin/list_sessions_command.h"
#include "admin/list_tasks_command.h"
#include "admin/magic_recalculate_metadata_checksum_command.h"
#include "admin/manage_locks_command.h"
#include "admin/metadataserver_status_command.h"
#include "admin/mount_info_list_command.h"
#include "admin/promote_shadow_command.h"
#include "admin/ready_chunkservers_count_command.h"
#include "admin/reload_config_command.h"
#include "admin/save_metadata_command.h"
#include "admin/stop_master_without_saving_metadata.h"
#include "admin/stop_task_command.h"
#include "common/sockets.h"
#include "common/version.h"

using CommandPtr = std::shared_ptr<SaunaFsAdminCommand>;
using CommandList = std::vector<CommandPtr>;

int printVersion() {
	std::cout << "SaunaFS Admin Tool\n\n";
	std::cout << "Version: " << common::version() << "\n";
	return 0;
}

void printCommandHelp(const CommandPtr &command) {
	command->usage();
	if (!command->supportedOptions().empty()) {
		std::cerr << "  Possible command-line options:\n";
		for (const auto& optionWithDescription : command->supportedOptions()) {
			std::cerr << "\n    " << optionWithDescription.first << "\n";
			std::cerr << "      " << optionWithDescription.second << "\n";
		}
	}
	std::cerr << std::endl;
}

int printHelp(const std::string& programName,
	const CommandList& commands, bool _printVersion = false) {
	if (_printVersion) { printVersion(); }
	std::cerr << std::endl;
	std::cerr << "Usage:\n";
	std::cerr << "    " << programName << " COMMAND [OPTIONS...] [ARGUMENTS...]\n\n";
	std::cerr << "Available COMMANDs:\n\n";
	for (const auto &command : commands) {
		if (command->name().substr(0, 6) == "magic-") {
			// Treat magic-* commands as undocumented
			continue;
		}
		printCommandHelp(command);
	}
	return 0;
}

int main(int argc, const char** argv) {
	CommandList allCommands = {
			std::make_shared<ChunksHealthCommand>(),
			std::make_shared<InfoCommand>(),
			std::make_shared<IoLimitsStatusCommand>(),
			std::make_shared<ListChunkserversCommand>(),
			std::make_shared<ListDefectiveFilesCommand>(),
			std::make_shared<ListDisksCommand>(),
			std::make_shared<ListDiskGroupsCommand>(),
			std::make_shared<ListGoalsCommand>(),
			std::make_shared<ListMountsCommand>(),
			std::make_shared<ListMetadataserversCommand>(),
			std::make_shared<ListInotifiersCommand>(),
			std::make_shared<ListTasksCommand>(),
			std::make_shared<ManageLocksCommand>(),
			std::make_shared<MetadataserverStatusCommand>(),
			std::make_shared<ReadyChunkserversCountCommand>(),
			std::make_shared<PromoteShadowCommand>(),
			std::make_shared<MetadataserverStopWithoutSavingMetadataCommand>(),
			std::make_shared<ReloadConfigCommand>(),
			std::make_shared<SaveMetadataCommand>(),
			std::make_shared<StopTaskCommand>(),
			std::make_shared<MagicRecalculateMetadataChecksumCommand>(),
			std::make_shared<ListSessionsCommand>(),
			std::make_shared<DeleteSessionsCommand>(),
			std::make_shared<DumpConfigurationCommand>(),
			std::make_shared<MountInfoListCommand>(),
	};

	std::string command_name;
	try {
		if (argc < 2) {
			throw WrongUsageException("No command name provided");
		}
#ifdef _WIN32
		socketinit();
#endif
		command_name = argv[1];
		if (command_name == "-v" || command_name == "--version") {
			return printVersion();
		}
		if (command_name == "-h" || command_name == "--help" || command_name == "help") {
			return printHelp(argv[0], allCommands, true);
		}
		std::vector<std::string> arguments(argv + 2, argv + argc);
		for (auto command : allCommands) {
			if (command->name() == command_name) {
				try {
					std::vector<std::string> supportedOptions;
					for (const auto& optionWithDescription : command->supportedOptions()) {
						supportedOptions.push_back(optionWithDescription.first);
					}
					command->run(Options(supportedOptions, arguments));
#ifdef _WIN32
					socketrelease();
#endif
					return 0;
				} catch (Options::ParseError& ex) {
					throw WrongUsageException("Wrong usage of " + command->name()
							+ "; " + ex.what());
				}
			}
		}
		throw WrongUsageException("Unknown command " + command_name +
			                  ". Use saunafs-admin help for a list of available commands");
	} catch (WrongUsageException& ex) {
		std::cerr << ex.message() << std::endl << std::endl;

		std::cerr << "Options:\n";
		std::cerr << "-v --version: Print version\n";
		std::cerr << "-h --help: Print this help message\n\n";
		if (command_name.empty()) {
			printHelp(argv[0], allCommands, false);
		} else {
			for (const auto &command : allCommands) {
				if (command->name() == command_name) {
					printCommandHelp(command);
					break;
				}
			}
		}
#ifdef _WIN32
		socketrelease();
#endif
		return 1;
	} catch (Exception& ex) {
#ifdef _WIN32
		socketrelease();
#endif
		std::cerr << "Error: " << ex.what() << "\n";
		return 1;
	}
}
