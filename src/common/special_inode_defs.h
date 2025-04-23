/*

   Copyright 2015 Skytechnology sp. z o.o.
   Copyright 2023 Leil Storage OÜ

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

#include "common/type_defs.h"

#ifdef SAUNAFS_USE_INODE64
#define SPECIAL_INODE_BASE 0xFFFFFFF0LU
#define SPECIAL_INODE_ROOT 0x01LU
#else
#define SPECIAL_INODE_BASE 0xFFFFFFF0U
#define SPECIAL_INODE_ROOT 0x01U
#endif  // SAUNAFS_USE_INODE64

#define SPECIAL_INODE_MASTERINFO      (SPECIAL_INODE_BASE + 0xFU)
#define SPECIAL_INODE_STATS           (SPECIAL_INODE_BASE + 0x0U)
#define SPECIAL_INODE_OPLOG           (SPECIAL_INODE_BASE + 0x1U)
#define SPECIAL_INODE_OPHISTORY       (SPECIAL_INODE_BASE + 0x2U)
#define SPECIAL_INODE_TWEAKS          (SPECIAL_INODE_BASE + 0x3U)
#define SPECIAL_INODE_FILE_BY_INODE   (SPECIAL_INODE_BASE + 0x4U)
#define SPECIAL_INODE_META_TRASH      (SPECIAL_INODE_BASE + 0x5U)
#define SPECIAL_INODE_META_UNDEL      (SPECIAL_INODE_BASE + 0x6U)
#define SPECIAL_INODE_META_RESERVED   (SPECIAL_INODE_BASE + 0x7U)
#define SPECIAL_INODE_PATH_BY_INODE   (SPECIAL_INODE_BASE + 0x8U)
#define SPECIAL_INODE_MOUNT_INFO      (SPECIAL_INODE_BASE + 0x9U)

#define SPECIAL_FILE_NAME_MASTERINFO      ".masterinfo"
#define SPECIAL_FILE_NAME_STATS           ".stats"
#define SPECIAL_FILE_NAME_OPLOG           ".oplog"
#define SPECIAL_FILE_NAME_OPHISTORY       ".ophistory"
#define SPECIAL_FILE_NAME_TWEAKS          ".saunafs_tweaks"
#define SPECIAL_FILE_NAME_FILE_BY_INODE   ".saunafs_file_by_inode"
#define SPECIAL_FILE_NAME_META_TRASH      "trash"
#define SPECIAL_FILE_NAME_META_UNDEL      "undel"
#define SPECIAL_FILE_NAME_META_RESERVED   "reserved"
#define SPECIAL_FILE_NAME_PATH_BY_INODE   ".saunafs_path_by_inode"
#define SPECIAL_FILE_NAME_MOUNT_INFO      ".saunafs_mount_info"

#define MAX_REGULAR_INODE (SPECIAL_INODE_BASE - 0x01U)
