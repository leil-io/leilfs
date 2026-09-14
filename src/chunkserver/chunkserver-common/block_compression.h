/*
   Copyright 2026 Leil Storage

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

#include <sys/types.h>
#include <cstddef>
#include <cstdint>
#include <memory>

// Opaque digested-dictionary types, forward-declared exactly as <zstd.h> does
// so this header does not drag the zstd headers into every includer.
typedef struct ZSTD_CDict_s ZSTD_CDict;
typedef struct ZSTD_DDict_s ZSTD_DDict;

/// Shared wrapper isolating Zstandard usage, so the compressed on-disk formats
/// stay byte compatible across disk plugins.
///
/// One frame per SFSBLOCKSIZE block, so a random read decompresses only the
/// block it asked for. A chunk's frames share one immutable dictionary,
/// digested once per chunk because digesting costs more than a block does.
namespace block_compression {

/// Deleters, so the opaque types above can still be held in unique_ptr.
struct CDictDeleter {
	void operator()(ZSTD_CDict *cdict) const;
};
struct DDictDeleter {
	void operator()(ZSTD_DDict *ddict) const;
};

using CDictPtr = std::unique_ptr<ZSTD_CDict, CDictDeleter>;
using DDictPtr = std::unique_ptr<ZSTD_DDict, DDictDeleter>;

/// Digests dictionary bytes for compression at @p level, auto-detecting trained
/// vs raw-content. nullptr if empty or on failure - compress without one.
CDictPtr createCDict(const uint8_t *dict, size_t dictSize, int level);

/// Digests dictionary bytes for decompression; nullptr if empty or on failure.
DDictPtr createDDict(const uint8_t *dict, size_t dictSize);

/// Compresses one block into @p dst, using @p cdict or nullptr for none - in
/// which case @p level applies instead.
///
/// @return the compressed size, or negative on error. A size >= srcSize means
///         the caller should store the block raw.
ssize_t compressBlock(const ZSTD_CDict *cdict, const uint8_t *src, size_t srcSize, uint8_t *dst,
                      size_t dstCapacity, int level);

/// Decompresses one block produced by compressBlock() with the same dictionary.
/// @return the decompressed size, or negative on error.
ssize_t decompressBlock(const ZSTD_DDict *ddict, const uint8_t *src, size_t srcSize, uint8_t *dst,
                        size_t dstCapacity);

/// Upper bound on the compressed size of a block of srcSize bytes.
size_t compressBound(size_t srcSize);

}  // namespace block_compression
