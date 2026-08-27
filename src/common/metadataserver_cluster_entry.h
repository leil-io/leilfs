/*
   Copyright 2026      Leil Storage OÜ

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

#pragma once

#include "common/platform.h"

#include "common/serialization_macros.h"

/// One metadata server of a distributed cluster as pushed to chunkservers: its stable id,
/// its advertised address and its chunkserver-facing port.
SAUNAFS_DEFINE_SERIALIZABLE_CLASS(MetadataserverClusterEntry,
		uint32_t, mdsId,
		uint32_t, ip,
		uint16_t, matocsPort,
		uint32_t, version);
