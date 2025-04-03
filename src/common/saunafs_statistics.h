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

#include "common/serialization_macros.h"
#include "common/type_defs.h"

SERIALIZABLE_CLASS_BEGIN(SaunaFsStatistics)
SERIALIZABLE_CLASS_BODY(SaunaFsStatistics, uint32_t, version, uint64_t,
                        memoryUsage, uint64_t, totalSpace, uint64_t,
                        availableSpace, uint64_t, trashSpace, inode_t,
                        trashNodes, uint64_t, reservedSpace, inode_t,
                        reservedNodes, inode_t, allNodes, inode_t, dirNodes,
                        inode_t, fileNodes, inode_t, symlinkNodes, uint32_t,
                        chunks, uint32_t, chunkCopies, uint32_t, regularCopies)
SERIALIZABLE_CLASS_END;
