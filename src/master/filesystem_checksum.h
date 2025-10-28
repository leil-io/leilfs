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

#include "master/checksum.h"
#include "master/filesystem_node_types.h"

void fsnodes_checksum_add_to_background(FSNode *node);
void fsnodes_update_checksum(FSNode *node);

// Namespace for in-memory checksum related functions
namespace checksum {
uint64_t fs_checksum(ChecksumMode mode);
#ifndef METARESTORE
uint8_t fs_start_checksum_recalculation();
#endif
}  // namespace checksum
