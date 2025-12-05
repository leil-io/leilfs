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

#include <sys/syslog.h>
#include <string>
#include <vector>

#include "common/event_loop.h"
#include "common/random.h"
#include "common/run_tab.h"
#include "config/cfg.h"
#include "master/chartsdata.h"
#include "master/datacachemgr.h"
#include "master/exports.h"
#include "master/filesystem.h"
#include "master/filesystem_freenode.h"
#include "master/hstorage_init.h"
#include "master/masterconn.h"
#include "master/matoclserv.h"
#include "master/matoclserv_sessions.h"
#include "master/matocsserv.h"
#include "master/matomlserv.h"
#include "master/matontserv.h"
#include "master/metadata_backend_file.h"
#include "master/metadata_backend_forkless.h"
#include "master/metadata_backend_interface.h"
#include "master/personality.h"
#include "master/session_manager.h"
#include "master/topology.h"
#include "metrics/metrics.h"

inline int prometheus_init() {
	if (cfg_getuint8("ENABLE_PROMETHEUS", 0) != 1) {
		safs::log_info(
		    "Prometheus disabled, no Prometheus metrics will be "
		    "gathered");
		return 0;
	}
	metrics::init(cfg_getstr("PROMETHEUS_HOST", "0.0.0.0:9499"));
	eventloop_destructregister(metrics::destroy);
	return 0;
}

inline int metadata_backend_init() {
	std::string backendType = cfg_getstr("METADATA_BACKEND", "FILE");
	safs::log_info("Initializing metadata backend of type: {}", backendType);

	if (gMetadataBackend == nullptr) {
		if (backendType == "FILE") {
			try {
				gMetadataBackend = std::make_unique<MetadataBackendFile>();
				gMetadataBackend->init();
				gInodeIdGenerator = std::make_unique<IdGeneratorWithDetainer>();
				safs::log_info("Initialized FILE metadata backend");
			} catch (const std::exception &e) {
				constexpr auto kErrorMessage = "Failed to initialize metadata backend";
				safs::log_err("{}: {}", kErrorMessage, e.what());
				throw Exception(kErrorMessage);
			}
		} else if (backendType == "FORKLESS") {
			try {
				gMetadataBackend = std::make_unique<MetadataBackendForkless>();
				gMetadataBackend->init();
				gInodeIdGenerator = std::make_unique<IdGeneratorWithDetainer>();
				safs::log_info("Initialized FORKLESS metadata backend");
			} catch (const std::exception &e) {
				constexpr auto kErrorMessage = "Failed to initialize metadata forkless backend";
				safs::log_err("{}: {}", kErrorMessage, e.what());
				throw Exception(kErrorMessage);
			}
		} else {
			std::string errorMessage = "Unsupported METADATA_BACKEND type: " + backendType;
			safs::log_err("{}", errorMessage);
			throw Exception(errorMessage);
		}
	}

	return 0;
}

inline int session_manager_init() {
	matoclserv_set_session_manager(std::make_unique<SessionManagerFile>());
	return 0;
}

/// Functions to call before normal startup
inline const std::vector<RunTab> earlyRunTabs = {
    RunTab{.function = metadataserver::personality_validate, .name = "validate personality"}};

/// Functions to call during normal startup
inline const std::vector<RunTab> runTabs = {
    RunTab{.function = prometheus_init, .name = "prometheus module"},
    // has to be first
    RunTab{.function = hstorage_init, .name = "name storage"},
    // has to be second
    RunTab{.function = metadataserver::personality_init, .name = "personality"},
    RunTab{.function = rnd_init, .name = "random generator"},
    // has to be before 'fs_init' and 'matoclserv_networkinit'
    RunTab{.function = dcm_init, .name = "data cache manager"},
    // must run before 'matoclserv_sessions_init' so the dispatcher has a backing manager.
    RunTab{.function = session_manager_init, .name = "attach session manager"},
    // has to be before 'fs_init'
    RunTab{.function = matoclserv_sessions_init, .name = "load stored sessions"},
    RunTab{.function = exports_init, .name = "exports manager"},
    RunTab{.function = topology_init, .name = "net topology module"},
    RunTab{.function = metadata_backend_init, .name = "metadata backend initialization"},
    // the lambda is used to select the correct fs_init overload
    RunTab{.function = []() { return fs_init(); }, .name = "file system manager"},
    RunTab{.function = chartsdata_init, .name = "charts module"},
    RunTab{.function = masterconn_init, .name = "communication with master server"},
    RunTab{.function = matomlserv_init, .name = "communication with metalogger"},
    RunTab{.function = matocsserv_init, .name = "communication with chunkserver"},
    RunTab{.function = matontserv_init, .name = "communication with notifier"},
    RunTab{.function = matoclserv_network_init, .name = "communication with clients"}};

/// Functions to call delayed after the initialization is correct
inline const std::vector<RunTab> lateRunTabs = {};
