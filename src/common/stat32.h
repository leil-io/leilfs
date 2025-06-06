/*
   Copyright 2023      Leil Storage OÜ

   This file is part of SaunaFS

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

/**
 * This file redefines the stat struct coming with the system for the Windows
 * builds, for the Linux builds it just takes the coming definition with
 * sys/stat. The target of this file is to align the stat structure with what
 * is expecting the SaunaFS system to reduce warnings like narrow-conversion and
 * to tackle the issue with uint16_t gid field coming with Windows default stat.
 * Those 16 bits for the gid field causes overflow in our Windows client and
 * must be changed.
 */

#include <sys/stat.h>

#include <common/type_defs.h>

#ifdef _WIN32
#include <cstdint>
#ifdef stat
#undef stat
#endif
#define stat stat32

struct stat32 {
    uint32_t st_dev;
    inode_t st_ino;
    uint16_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t st_rdev;
    int64_t st_size;
    uint32_t st_atime;
    uint32_t st_mtime;
    uint32_t st_ctime;
    uint32_t st_birthtime;
};
#endif
