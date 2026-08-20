/*
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

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
#include <sstream>

#define SAUNAFS_VERSION(major, minor, micro) (0x010000 * major + 0x0100 * minor + micro)

constexpr uint32_t saunafsVersion(uint32_t major, uint32_t minor, uint32_t micro) {
	return 0x010000 * major + 0x0100 * minor + micro;
}

inline std::string saunafsVersionToString(uint32_t version) {
	std::ostringstream ss;
	ss << version / 0x010000 << '.' << (version % 0x010000) / 0x0100 << '.' << version % 0x0100;
	return ss.str();
}

// A special version reported for disconnected chunkservers in MATOCL_CSSERV_LIST
constexpr uint32_t kDisconnectedChunkserverVersion = saunafsVersion(256, 0, 0);

// Definitions of milestone SaunaFS versions
constexpr uint32_t kStdVersion = saunafsVersion(2, 6, 0);
constexpr uint32_t kFirstXorVersion = saunafsVersion(2, 9, 0);
constexpr uint32_t kFirstECVersion = saunafsVersion(3, 9, 5);
constexpr uint32_t kACL11Version = saunafsVersion(3, 11, 0);
constexpr uint32_t kRichACLVersion = saunafsVersion(3, 12, 0);
constexpr uint32_t kEC2Version = saunafsVersion(3, 13, 0);
constexpr uint32_t kFirstVersionWithPathByInodeHiddenFile = saunafsVersion(4, 8, 0);
constexpr uint32_t kFirstVersionWithUseQuotaInVolumeSize = saunafsVersion(4, 9, 0);
constexpr uint32_t kFirstVersionWithMountInfoOnMonitoring = saunafsVersion(5, 0, 0);
constexpr uint32_t kFirstVersionWithChunkBasedWriteAlgorithm = saunafsVersion(5, 0, 0);
constexpr uint32_t kFirstVersionWithClusterId = saunafsVersion(5, 0, 0);
constexpr uint32_t kFirstVersionWithReadTrashReservedByHandleOffset = saunafsVersion(5, 3, 0);
constexpr uint32_t kFirstVersionWithUnlockChunkNotice = saunafsVersion(5, 8, 0);
constexpr uint32_t kFirstVersionWithChunkserverSideChunkLock = saunafsVersion(5, 8, 0);
constexpr uint32_t kFirstVersionWithWriteFlushPacket = saunafsVersion(5, 11, 0);
constexpr uint32_t kFirstVersionWithFusedCreate = saunafsVersion(5, 11, 0);
constexpr uint32_t kFirstVersionWithMdsRegistry = saunafsVersion(5, 12, 0);
