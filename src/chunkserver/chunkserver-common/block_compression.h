/*
   Copyright 2026 Leil Storage

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

#include <sys/types.h>
#include <cstddef>
#include <cstdint>
#include <memory>

// Opaque digested-dictionary types, forward-declared exactly as <zstd.h> does
// so this header does not drag the zstd headers into every includer.
typedef struct ZSTD_CDict_s ZSTD_CDict;
typedef struct ZSTD_DDict_s ZSTD_DDict;

/// Thin wrapper isolating all Zstandard usage for disk plugins that store chunk
/// blocks compressed. Shared, so the compressed on-disk formats stay byte
/// compatible across plugins.
///
/// Blocks are compressed independently, one Zstd frame per SFSBLOCKSIZE block,
/// so a random read only needs to decompress the block it asked for. Each
/// chunk shares a single immutable per-chunk dictionary across its frames;
/// digesting a dictionary is expensive relative to compressing one block, so
/// callers digest it once per chunk (createCDict()/createDDict()) and reuse
/// the digested form for every per-block call.
namespace block_compression {

/// Deleters so the digested dictionaries can be held in std::unique_ptr without
/// exposing the zstd headers.
struct CDictDeleter {
	void operator()(ZSTD_CDict *cdict) const;
};
struct DDictDeleter {
	void operator()(ZSTD_DDict *ddict) const;
};

/// Owning handles for the digested per-chunk dictionaries.
using CDictPtr = std::unique_ptr<ZSTD_CDict, CDictDeleter>;
using DDictPtr = std::unique_ptr<ZSTD_DDict, DDictDeleter>;

/// Digests the per-chunk dictionary bytes for compression at the given level.
/// Auto-detects trained (ZDICT) vs raw-content dictionaries. Returns nullptr
/// for an empty dictionary or on failure (callers then compress without one).
CDictPtr createCDict(const uint8_t *dict, size_t dictSize, int level);

/// Digests the per-chunk dictionary bytes for decompression. Returns nullptr
/// for an empty dictionary or on failure.
DDictPtr createDDict(const uint8_t *dict, size_t dictSize);

/// Compresses a single block using the digested per-chunk dictionary.
///
/// @param cdict         Digested dictionary, or nullptr to compress without one
///                      (uses @p level instead).
/// @param src           Uncompressed block bytes.
/// @param srcSize       Number of uncompressed bytes.
/// @param dst           Destination buffer for the compressed frame.
/// @param dstCapacity   Capacity of the destination buffer.
/// @param level         Zstd compression level for the dictionary-less path.
/// @return The compressed size on success, or a negative value on error; a
///         size >= srcSize means the caller should store the block raw instead.
ssize_t compressBlock(const ZSTD_CDict *cdict, const uint8_t *src, size_t srcSize, uint8_t *dst,
                      size_t dstCapacity, int level);

/// Decompresses a single block previously produced by compressBlock() with the
/// same dictionary.
///
/// @param ddict         Digested dictionary used at compression time, or
///                      nullptr if the block was compressed without one.
/// @param src           Compressed frame bytes.
/// @param srcSize       Number of compressed bytes.
/// @param dst           Destination buffer for the decompressed block.
/// @param dstCapacity   Capacity of the destination buffer.
/// @return The decompressed size on success, or a negative value on error.
ssize_t decompressBlock(const ZSTD_DDict *ddict, const uint8_t *src, size_t srcSize, uint8_t *dst,
                        size_t dstCapacity);

/// Upper bound on the compressed size of a block of srcSize bytes.
size_t compressBound(size_t srcSize);

}  // namespace block_compression
