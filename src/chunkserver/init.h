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

#include "chunkserver/chartsdata.h"
#include "chunkserver/hddspacemgr.h"
#include "chunkserver/masterconn.h"
#include "chunkserver/network_main_thread.h"
#include "common/random.h"
#include "common/run_tab.h"

/// Functions to call before normal startup
inline const std::vector<RunTab> earlyRunTabs = {};

/// Functions to call during normal startup
inline const std::vector<RunTab> runTabs = {
    RunTab{rnd_init, "random generator"},
    RunTab{initDiskManager, "disk manager"},  // Always before "plugin manager"
    RunTab{loadPlugins, "plugin manager"},
	RunTab{hddInit, "hdd space manager"},
    // Has to be before "masterconn"
    RunTab{mainNetworkThreadInit, "main server module"},
    RunTab{masterconn_init, "master connection module"},
    RunTab{chartsdata_init, "charts module"}};

/// Functions to call delayed after the initialization is correct
inline const std::vector<RunTab> lateRunTabs = {
    RunTab{masterconn_init_threads, "master connection module - threads"},
    RunTab{hddLateInit, "hdd space manager - threads"},
    RunTab{mainNetworkThreadInitThreads, "main server module - threads"}};
