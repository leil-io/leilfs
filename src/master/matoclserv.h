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

#include <cstdint>

#include "common/type_defs.h"

void matoclserv_stats(uint64_t stats[5]);
void matoclserv_chunk_status(uint64_t chunkid, uint8_t status);
void matoclserv_add_open_file(uint32_t sessionid, inode_t inode);
void matoclserv_remove_open_file(uint32_t sessionid, inode_t inode);
int matoclserv_sessionsinit(void);
int matoclserv_networkinit(void);
void matoclserv_session_unload(void);

/// Notify interested clients about the status of metadata saving process.
void matoclserv_broadcast_metadata_saved(uint8_t status);

/// Notify interested clients about the status of metadata checksum recalculation process.
void matoclserv_broadcast_metadata_checksum_recalculated(uint8_t status);
