/*
   Copyright 2026      Leil Storage OÜ

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

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace chunkserver {

using ChunkserverId = std::array<uint8_t, 16>;
using ChunkserverIdGenerator = std::function<ChunkserverId()>;

inline constexpr const char *kChunkserverIdFilename = "chunkserver_id";

/// Parses a canonical lowercase UUIDv4, with one optional trailing newline.
///
/// @throws std::invalid_argument if the text is not a canonical UUIDv4.
ChunkserverId parseChunkserverId(std::string_view text);

/// Formats an id as a canonical lowercase UUID.
std::string formatChunkserverId(const ChunkserverId &chunkserverId);

/// Creates a UUIDv4 using a cryptographically strong random generator.
ChunkserverId generateChunkserverId();

/// Loads an existing id or durably creates one at @p path.
///
/// @throws std::exception if existing state is invalid, @p generator fails, or creation cannot be
/// made durable.
ChunkserverId loadOrCreateChunkserverId(const std::filesystem::path &path,
                                        const ChunkserverIdGenerator &generator);

/// Resolves the process identity from the chunkserver data directory.
int chunkserverIdInit();

/// Returns the identity resolved by @ref chunkserverIdInit.
const ChunkserverId &chunkserverId();

}  // namespace chunkserver
