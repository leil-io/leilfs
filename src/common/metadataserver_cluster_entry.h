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

#include <cstdint>

#include "common/serialization_macros.h"

// Keeps the discovery packet comfortably below the chunkserver's 10,000 byte packet limit.
constexpr uint32_t kMaxMetadataserverClusterEntries = 512;

// One MDS registry entry as advertised to a chunkserver for passive discovery.
SAUNAFS_DEFINE_SERIALIZABLE_CLASS(MetadataserverClusterEntry, uint32_t, mdsId, uint32_t, ip,
                                  uint16_t, matocsPort, uint32_t, version);

inline bool operator==(const MetadataserverClusterEntry &lhs,
                       const MetadataserverClusterEntry &rhs) {
	return lhs.mdsId == rhs.mdsId && lhs.ip == rhs.ip && lhs.matocsPort == rhs.matocsPort &&
	       lhs.version == rhs.version;
}
