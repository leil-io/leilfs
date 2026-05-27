/*
   Copyright 2025      Leil Storage OÜ

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

#include "master/chunk_operations_interface.h"

/// Storage-agnostic base for the chunk-operations hierarchy.
///
/// Intended to hold logic shared by every backend (orchestration that does not
/// touch storage directly, plus the protected storage primitives that
/// InMemory/KV implement). It is empty for now: chunk logic is currently tied to
/// the in-memory `Chunk` representation, so little is storage-agnostic yet. Its
/// real role is decided once `ChunkOperationsKV` exists -- if InMemory and KV end
/// up sharing decision logic it moves here; if not, this hinge can be dropped.
class ChunkOperationsBase : public IChunkOperations {};
