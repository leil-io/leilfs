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

#include "chunkserver/bgjobs.h"
#include "common/platform.h"

#include <cstdint>
#include <vector>

#include "common/metadataserver_cluster_entry.h"

void masterconn_stats(uint64_t *bin, uint64_t *bout, uint32_t *maxjobscnt);
int masterconn_init(void);
int masterconn_init_threads(void);
MasterJobPool* masterconn_get_job_pool();
bool masterconn_canexit();
void masterconn_apply_cluster_members(const std::vector<MetadataserverClusterEntry> &members);
