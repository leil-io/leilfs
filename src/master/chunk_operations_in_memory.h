/*
   Copyright 2026      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "common/platform.h"

#include "master/chunk_operations_base.h"

/// leil-master binding: chunk metadata lives in gChunksMetadata (RAM).
///
/// An empty leaf over ChunkOperationsBase, whose defaults already forward to the
/// in-memory engine in chunks.{h,cc}. It exists as a named binding point (the
/// in-memory counterpart of ChunkOperationsKV) and a seam for any future
/// master-only behavior.
class ChunkOperationsInMemory : public ChunkOperationsBase {};
