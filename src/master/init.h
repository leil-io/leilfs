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

#pragma once

#include "common/platform.h"

#include <sys/syslog.h>
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
#include "master/kv_connector_fdb.h"
#include "master/kv_connector_interface.h"
#include "master/masterconn.h"
#include "master/matoclserv.h"
#include "master/matoclserv_sessions.h"
#include "master/matocsserv.h"
#include "master/matomlserv.h"
#include "master/matontserv.h"
#include "master/metadata_backend_file.h"
#include "master/metadata_backend_interface.h"
#include "master/personality.h"
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

inline int kv_connector_init() {
	if (gKVConnector == nullptr) {
		try {
			gKVConnector = std::make_shared<KVConnectorFDB>();
			return gKVConnector->init();
		} catch (const std::exception &e) {
			constexpr auto kErrorMessage = "Failed to initialize KV connector";
			safs::log_err("{}: {}", kErrorMessage, e.what());
			throw Exception(kErrorMessage);
		}
	}
	return 0;
}

inline int metadata_backend_init() {
	if (gMetadataBackend == nullptr) {
		try {
			gMetadataBackend = std::make_unique<MetadataBackendFile>();
			gMetadataBackend->init();
			gInodeIdGenerator = std::make_unique<IdGeneratorWithDetainer>();
		} catch (const std::exception &e) {
			constexpr auto kErrorMessage = "Failed to initialize metadata backend";
			safs::log_err("{}: {}", kErrorMessage, e.what());
			throw Exception(kErrorMessage);
		}
	}

	return 0;
}

/// Functions to call before normal startup
inline const std::vector<RunTab> earlyRunTabs = {
    RunTab{metadataserver::personality_validate, "validate personality"}};

/// Functions to call during normal startup
inline const std::vector<RunTab> runTabs = {
    RunTab{prometheus_init, "prometheus module"},
    // has to be first
    RunTab{hstorage_init, "name storage"},
    // has to be second
    RunTab{metadataserver::personality_init, "personality"},
    RunTab{rnd_init, "random generator"},
    // has to be before 'fs_init' and 'matoclserv_networkinit'
    RunTab{dcm_init, "data cache manager"},
    // has to be before 'fs_init'
    RunTab{matoclserv_sessions_init, "load stored sessions"},
    RunTab{exports_init, "exports manager"},
    RunTab{topology_init, "net topology module"},
	RunTab{kv_connector_init, "key-value backend connector"},
    RunTab{metadata_backend_init, "metadata backend initialization"},
    // the lambda is used to select the correct fs_init overload
    RunTab{[]() { return fs_init(); }, "file system manager"},
    RunTab{chartsdata_init, "charts module"},
    RunTab{masterconn_init, "communication with master server"},
    RunTab{matomlserv_init, "communication with metalogger"},
    RunTab{matocsserv_init, "communication with chunkserver"},
    RunTab{matontserv_init, "communication with notifier"},
    RunTab{matoclserv_network_init, "communication with clients"}};

/// Functions to call delayed after the initialization is correct
inline const std::vector<RunTab> lateRunTabs = {};
