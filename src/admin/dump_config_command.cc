/*
   Copyright 2023 Leil Storage OÜ

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
#include "admin/dump_config_command.h"

#include <unistd.h>
#include <yaml-cpp/yaml.h>
#include <iostream>

#include "admin/registered_admin_connection.h"
#include "protocol/cltoma.h"
#include "protocol/matocl.h"

std::string add_defaults(std::string &config);

std::string DumpConfigurationCommand::name() const { return "dump-config"; }

void DumpConfigurationCommand::usage() const {
	std::cerr << name() << " <master ip> <master port>"
	          << "\n";
	std::cerr << "    Dumps the configuration files of the master server\n";
	std::cerr << "    Authentication with the admin password is required\n";
}

SaunaFsAdminCommand::SupportedOptions DumpConfigurationCommand::supportedOptions() const {
	return {
		{
			defaultsMode,
			    "Return default values as well. This is informational and may "
			    "not be correct in all cases."
		}
	};
}

void DumpConfigurationCommand::run(const Options &options) const {
	if (options.arguments().size() != 2) {
		throw WrongUsageException(
		    "Expected <master ip> and <master port> for " + name());
	}

	auto connection = RegisteredAdminConnection::create(options.argument(0),
	                                                    options.argument(1));
	auto adminResponse = connection->sendAndReceive(
	    cltoma::adminDumpConfiguration::build(), SAU_MATOCL_ADMIN_DUMP_CONFIG);

	std::string configs;
	matocl::adminDumpConfiguration::deserialize(adminResponse, configs);
	if (options.isSet(defaultsMode)) {
		configs = add_defaults(configs);
	}
	std::cout << configs << "\n";
}

// Default configuration options below...
// This is informational only, and not used anywhere else in the codebase.

// clang-format off
const static std::unordered_map<std::string, std::string> defaultOptionsMaster = {
	{"PERSONALITY", "master"},
    {"DATA_PATH", DATA_PATH},
    {"WORKING_USER", DEFAULT_USER},
    {"WORKING_GROUP", DEFAULT_GROUP},
    {"SYSLOG_IDENT", "sfsmaster"},
    {"LOCK_MEMORY", "0"},
    {"NICE_LEVEL", "-19"},
    {"TOPOLOGY_FILENAME", ETC_PATH "/sfstopology.cfg"},
    {"CUSTOM_GOALS_FILENAME", ""},
    {"PREFER_LOCAL_CHUNKSERVER", "1"},
    {"BACK_LOGS", "50"},
    {"BACK_META_KEEP_PREVIOUS", "3"},
    {"AUTO_RECOVERY", "0"},
    {"REPLICATIONS_DELAY_INIT", "300"},
    {"REPLICATIONS_DELAY_DISCONNECT", "3600"},
    {"OPERATIONS_DELAY_INIT", "300"},
    {"OPERATIONS_DELAY_DISCONNECT", "3600"},
    {"MATOML_LISTEN_HOST", "*"},
    {"MATOML_LISTEN_PORT", "9419"},
    {"MATOML_LOG_PRESERVE_SECONDS", "600"},
    {"MATOCS_LISTEN_HOST", "*"},
    {"MATOCS_LISTEN_PORT", "9420"},
    {"MATOCL_LISTEN_HOST", "*"},
    {"MATOCL_LISTEN_PORT", "9421"},
    {"MATOTS_LISTEN_HOST", "*"},
    {"MATOTS_LISTEN_PORT", "9424"},
    {"CHUNKS_LOOP_MAX_CPS", "100000"},
    {"CHUNKS_LOOP_MIN_TIME", "300"},
    {"CHUNKS_LOOP_PERIOD", "1000"},
    {"CHUNKS_LOOP_MAX_CPU", "60"},
    {"USE_LINEAR_ASSIGNMENT_OPTIMIZER", "1"},
    {"CHUNKS_SOFT_DEL_LIMIT", "10"},
    {"CHUNKS_HARD_DEL_LIMIT", "25"},
    {"CHUNKS_WRITE_REP_LIMIT", "2"},
    {"CHUNKS_READ_REP_LIMIT", "10"},
    {"ENDANGERED_CHUNKS_PRIORITY", "0.0"},
    {"ENDANGERED_CHUNKS_MAX_CAPACITY", "1048576"},
    {"ACCEPTABLE_DIFFERENCE", "0.1"},
    {"CHUNKS_REBALANCING_BETWEEN_LABELS", "0"},
    {"REJECT_OLD_CLIENTS", "0"},
    {"GLOBALIOLIMITS_FILENAME", ""},
    {"GLOBALIOLIMITS_RENEGOTIATION_PERIOD_SECONDS", "0.1"},
    {"GLOBALIOLIMITS_ACCUMULATE_MS", "250"},
    {"METADATA_CHECKSUM_INTERVAL", "50"},
    {"METADATA_CHECKSUM_RECALCULATION_SPEED", "100"},
    {"DISABLE_METADATA_CHECKSUM_VERIFICATION", "0"},
    {"NO_ATIME", "0"},
    {"METADATA_SAVE_REQUEST_MIN_PERIOD", "1800"},
    {"SESSION_SUSTAIN_TIME", "86400"},
    {"USE_BDB_FOR_NAME_STORAGE", "0"},
    {"BDB_NAME_STORAGE_CACHE_SIZE", "10"},
    {"AVOID_SAME_IP_CHUNKSERVERS", "0"},
    {"REDUNDANCY_LEVEL", "0"},
    {"SNAPSHOT_INITIAL_BATCH_SIZE", "1000"},
    {"SNAPSHOT_INITIAL_BATCH_SIZE_LIMIT", "10000"},
    {"FILE_TEST_LOOP_MIN_TIME", "3600"},
    {"PRIORITIZE_DATA_PARTS", "1"},
};

const static std::unordered_map<std::string, std::string> defaultOptionsShadow = {
    {"MASTER_HOST", "sfsmaster"},
	{"MASTER_PORT", "9419"},
    {"MASTER_RECONNECTION_DELAY", "1"},
	{"MASTER_TIMEOUT", "60"},
    {"LOAD_FACTOR_PENALTY", "0.0"},
};

const static std::unordered_map<std::string, std::string> defaultOptionsCS = {
    {"DATA_PATH", DATA_PATH},
    {"LABEL", "_"},
    {"WORKING_USER", DEFAULT_USER},
    {"WORKING_GROUP", DEFAULT_GROUP},
    {"SYSLOG_IDENT", "sfschunkserver"},
    {"LOCK_MEMORY", "0"},
    {"NICE_LEVEL", "-19"},
    {"MASTER_HOST", "sfsmaster"},
    {"MASTER_PORT", "9420"},
    {"MASTER_RECONNECTION_DELAY", "1"},
    {"MASTER_TIMEOUT", "60"},
    {"BIND_HOST", "*"},
    {"CSSERV_LISTEN_HOST", "*"},
    {"CSSERV_LISTEN_PORT", "9422"},
    {"HDD_CONF_FILENAME", ETC_PATH "/sfshdd.cfg"},
    {"HDD_TEST_FREQ", "10.0"},
    {"HDD_CHECK_CRC_WHEN_READING", "1"},
    {"HDD_CHECK_CRC_WHEN_WRITING", "1"},
    {"HDD_ADVISE_NO_CACHE", "0"},
    {"HDD_PUNCH_HOLES", "0"},
    {"ENABLE_LOAD_FACTOR", "0"},
    {"REPLICATION_BANDWIDTH_LIMIT_KBPS", "0"},
    {"NR_OF_NETWORK_WORKERS", "4"},
    {"NR_OF_HDD_WORKERS_PER_NETWORK_WORKER", "16"},
    {"BGJOBSCNT_PER_NETWORK_WORKER", "1000"},
    {"MAX_READ_BEHIND_KB", "0"},
    {"PERFORM_FSYNC", "1"},
    {"STAT_CHUNKS_AT_DISK_SCAN", "1"},
    {"REPLICATION_TOTAL_TIMEOUT_MS", "60000"},
    {"REPLICATION_CONNECTION_TIMEOUT_MS", "1000"},
    {"REPLICATION_WAVE_TIMEOUT_MS", "500"},
    {"CHUNK_TRASH_ENABLED", "1"},
    {"CHUNK_TRASH_EXPIRATION_SECONDS", "259200"},
    {"CHUNK_TRASH_FREE_SPACE_THRESHOLD_GB", "1024"},
    {"CHUNK_TRASH_GC_BATCH_SIZE", "1000"},
    {"CHUNK_TRASH_GC_SPACE_RECOVERY_BATCH_SIZE", "100"},
};

const static std::unordered_map<std::string, std::string> defaultOptionsMeta = {
    {"DATA_PATH", DATA_PATH},
    {"WORKING_USER", DEFAULT_USER},
    {"WORKING_GROUP", DEFAULT_GROUP},
    {"SYSLOG_IDENT", "sfsmetalogger"},
    {"LOCK_MEMORY", "0"},
    {"NICE_LEVEL", "-19"},
    {"BACK_LOGS", "50"},
    {"BACK_META_KEEP_PREVIOUS", "3"},
    {"META_DOWNLOAD_FREQ", "24"},
    {"MASTER_HOST", "sfsmaster"},
    {"MASTER_PORT", "9419"},
    {"MASTER_RECONNECTION_DELAY", "1"},
};
// clang-format on

std::unordered_map<std::string, std::string> select_defaults(
    std::string const &service) {
	if (service == "master") { return defaultOptionsMaster; }
	if (service == "shadow") { return defaultOptionsShadow; }
	if (service == "metaloggers") { return defaultOptionsMeta; }
	if (service == "chunkservers") { return defaultOptionsCS; }
	return {};
}

std::string add_defaults(std::string &config) {
	std::vector<std::string> singleConfigs = {"master", "shadow"};
	std::vector<std::string> multiConfigs = {"metaloggers", "chunkservers"};
	YAML::Node fullConfig = YAML::Load(config);

	for (auto const &config_key : singleConfigs) {
		YAML::Node keyNode = fullConfig[config_key];
		if (keyNode.size() == 0) { continue; };

		auto defaultValues = select_defaults(config_key);
		for (auto const &[key, value] : defaultValues) {
			if (!keyNode[key]) { keyNode[key] = value; }
		}
	}

	// For multiconfigs, they contain a list of keys with their own key-value
	// pairs, so the process is a bit different.
	for (auto const &config_key : multiConfigs) {
		YAML::Node keyNode = fullConfig[config_key];
		if (keyNode.size() == 0) { continue; };

		auto defaultValues = select_defaults(config_key);
		for (auto key : keyNode) {
			for (auto const &[keyDefault, valueDefault] : defaultValues) {
				if (!key.second[keyDefault]) {
					key.second[keyDefault] = valueDefault;
				}
			}
		}
	}

	YAML::Emitter ret;
	ret << fullConfig;
	return ret.c_str();
}
