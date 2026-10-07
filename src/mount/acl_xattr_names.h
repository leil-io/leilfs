/*

   Copyright 2026 Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "common/platform.h"

#include <cstdint>
#include <vector>

/// True for the extended attribute names through which ACLs are exposed.
bool isAclXattrName(const char *name);

/// Copies a NUL separated xattr name list, dropping ACL names and any unterminated tail.
std::vector<uint8_t> filterAclXattrNames(const uint8_t *names, uint32_t length);
