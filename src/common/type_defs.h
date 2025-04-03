/*

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

/// inode_t is meant to be used as type for all inode number references.
/// Needs to be compatible with C++ and C code.
#ifdef __cplusplus
#include <cstdint>

using inode_t = uint32_t;
constexpr std::size_t kinode_t_size = sizeof(inode_t);
#else
#include <stdint.h>

typedef uint32_t inode_t;
#define kinode_t_size (sizeof(inode_t))
#endif  // __cplusplus
