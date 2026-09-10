/*

   Copyright 2015 Skytechnology sp. z o.o.
   Copyright 2023 Leil Storage OÜ

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
#include "common/chunk_part_type.h"
#include "common/serialization_macros.h"

namespace legacy {

SAUNAFS_DEFINE_SERIALIZABLE_CLASS(ChunkWithType,
	uint64_t, id,
	ChunkPartType, type);

} // legacy

SAUNAFS_DEFINE_SERIALIZABLE_CLASS(ChunkWithType,
	uint64_t, id,
	ChunkPartType, type);
