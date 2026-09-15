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
#include <optional>
#include <string_view>

/// Shared wrapper isolating compression-library usage, so the compressed
/// on-disk formats stay byte compatible across disk plugins.
///
/// One frame per SFSBLOCKSIZE block, so a random read decompresses only the
/// block it asked for. A chunk may carry one immutable dictionary shared by all
/// its frames, prepared once per chunk because preparing costs more than
/// compressing a block; usesDictionary() says which algorithms get one.
namespace block_compression {

/// Compression library for a chunk's blocks, fixed when the chunk is created.
/// Never serialized - each on-disk format spells its algorithm out its own way
/// - so these values can be renumbered freely.
enum class Algorithm : uint8_t {
	None,  ///< Blocks are stored uncompressed; the (de)compression calls reject it.
	Zstd,
	Lz4,
};

/// Maps an HDD_COMPRESSION_ALGORITHM value ("none", "zstd", "lz4", case
/// insensitive) to its algorithm; here, so every plugin accepts the same
/// spellings. std::nullopt for an unrecognized name.
std::optional<Algorithm> algorithmFromName(std::string_view name);

/// The config spelling of @p algorithm, as accepted by algorithmFromName().
const char *algorithmName(Algorithm algorithm);

/// Whether a per-chunk dictionary is worth building for @p algorithm. Asked
/// only when creating a chunk; a chunk that already carries one keeps using it,
/// which today means a Zstd chunk, the only kind that can hold one.
///
/// True only for Zstd, which digests one per chunk and then applies it for
/// free. LZ4's equivalent is stable only from liblz4 1.10.0, so portably it
/// loads one per block, and its match window already spans the whole block.
bool usesDictionary(Algorithm algorithm);

/// The level at which @p algorithm stops improving; levels above it cost and
/// save nothing more. Zero for None. Lets a caller pick a default effort
/// without knowing each library's dial.
int strongestLevel(Algorithm algorithm);

/// Per-chunk dictionaries, prepared into whatever form the algorithm wants: a
/// digest for Zstd, the raw bytes for LZ4. Opaque to keep the library headers
/// out of every includer.
class CompressDict;
class DecompressDict;

/// Deleters, so the incomplete types above can still be held in unique_ptr.
struct CompressDictDeleter {
	void operator()(CompressDict *dict) const;
};
struct DecompressDictDeleter {
	void operator()(DecompressDict *dict) const;
};

using CompressDictPtr = std::unique_ptr<CompressDict, CompressDictDeleter>;
using DecompressDictPtr = std::unique_ptr<DecompressDict, DecompressDictDeleter>;

/// Prepares dictionary bytes for compression: Zstd digests them at @p level
/// (auto-detecting trained vs raw-content dictionaries), LZ4 keeps them as they
/// are. nullptr for an empty dictionary or on failure - compress without one.
CompressDictPtr createCompressDict(Algorithm algorithm, const uint8_t *dict, size_t dictSize,
                                   int level);

/// Prepares dictionary bytes for decompression; nullptr if empty or on
/// failure.
DecompressDictPtr createDecompressDict(Algorithm algorithm, const uint8_t *dict, size_t dictSize);

/// Compresses one block into @p dst, using @p dict or nullptr for none.
///
/// @p level is effort, higher being harder, the way Zstd's levels read; LZ4
/// inverts it onto acceleration. Ignored when @p dict is a Zstd dictionary,
/// which carries the level digested into it, and capped at strongestLevel().
///
/// @return the compressed size, or <= 0 when the block was not compressed -
///         the algorithm failed, or the result did not fit. The caller stores
///         the block raw either way, so the two need not be told apart.
ssize_t compressBlock(Algorithm algorithm, const CompressDict *dict, const uint8_t *src,
                      size_t srcSize, uint8_t *dst, size_t dstCapacity, int level);

/// Decompresses one block produced by compressBlock() with the same algorithm
/// and dictionary. @return the decompressed size, or negative on error.
ssize_t decompressBlock(Algorithm algorithm, const DecompressDict *dict, const uint8_t *src,
                        size_t srcSize, uint8_t *dst, size_t dstCapacity);

/// Upper bound on the compressed size of @p srcSize bytes - above srcSize for
/// both algorithms, since framing costs a few bytes. 0 for None.
size_t compressBound(Algorithm algorithm, size_t srcSize);

}  // namespace block_compression
