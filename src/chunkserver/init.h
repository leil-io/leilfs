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

#include <vector>

#include "chunkserver-common/memory_manager.h"
#include "chunkserver/chartsdata.h"
#include "chunkserver/chunkserver_id.h"
#include "chunkserver/hddspacemgr.h"
#include "chunkserver/masterconn.h"
#include "chunkserver/network_main_thread.h"
#include "common/random.h"
#include "common/run_tab.h"

/// Functions to call before normal startup
inline const std::vector<RunTab> earlyRunTabs = {};

/// Functions to call during normal startup
inline const std::vector<RunTab> runTabs = {
    RunTab{.function = chunkserver::chunkserverIdInit, .name = "chunkserver identity"},
    RunTab{.function = rnd_init, .name = "random generator"},
    RunTab{.function = MemoryManager::init, .name = "memory manager"},
    RunTab{.function = initDiskManager, .name = "disk manager"},  // Always before "plugin manager"
    RunTab{.function = loadPlugins, .name = "plugin manager"},
    RunTab{.function = hddInit, .name = "hdd space manager"},
    // Has to be before "masterconn"
    RunTab{.function = mainNetworkThreadInit, .name = "main server module"},
    RunTab{.function = masterconn_init_threads, .name = "master connection module - threads"},
    RunTab{.function = masterconn_init, .name = "master connection module"},
    RunTab{.function = chartsdata_init, .name = "charts module"}};

/// Functions to call delayed after the initialization is correct
inline const std::vector<RunTab> lateRunTabs = {
    RunTab{.function = hddLateInit, .name = "hdd space manager - threads"},
    RunTab{.function = mainNetworkThreadInitThreads, .name = "main server module - threads"}};
