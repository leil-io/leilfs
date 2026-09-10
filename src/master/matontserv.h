/*
   Copyright 2023      Leil Storage OÜ


   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include <cstdint>
#include <string_view>

#include "common/inotifier_list_entry.h"

void matontserv_broadcast_message(uint64_t version, std::string_view message);

int matontserv_init(void);

int matontserv_canexit(void);

std::vector<INotifierListEntry> matontserv_inotifiers();
