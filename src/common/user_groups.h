/*
   Copyright 2013-2020 Sauna sp. z o.o.
   Copyright 2023      Leil Storage OÜ

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

namespace user_groups {

constexpr std::uint32_t kSecondaryGroupsBit = (std::uint32_t)1 << 31;
constexpr std::uint64_t kPrimaryGroupPosition = 0;

constexpr bool isGroupCacheId(std::uint32_t groupsId) noexcept {
  return static_cast<bool>(groupsId & kSecondaryGroupsBit);
}

constexpr std::uint32_t encodeGroupCacheId(std::uint32_t groupsId) noexcept {
  return groupsId | kSecondaryGroupsBit;
}

constexpr std::uint32_t decodeGroupCacheId(std::uint32_t groupsId) noexcept {
  return groupsId & ~kSecondaryGroupsBit;
}

} // namespace user_groups
