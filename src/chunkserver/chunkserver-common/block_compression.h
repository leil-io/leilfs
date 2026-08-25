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
#include <optional>
#include <string_view>

/// Thin wrapper isolating all compression-library usage for disk plugins that
/// store chunk blocks compressed. Shared, so the compressed on-disk formats
/// stay byte compatible across plugins.
///
/// Blocks are compressed independently, one frame per SFSBLOCKSIZE block, so a
/// random read only needs to decompress the block it asked for. Each chunk
/// shares a single immutable per-chunk dictionary across its frames; preparing
/// a dictionary costs more than compressing one block, so callers prepare it
/// once per chunk (createCompressDict()/createDecompressDict()) and reuse the
/// prepared form for every per-block call.
namespace block_compression {

/// Compression library used for a chunk's blocks. Chosen per chunk when it is
/// created and never changed afterwards, so blocks written at different times
/// always decode the same way.
///
/// These values are never serialized: each on-disk format spells its algorithm
/// out its own way (the ZoneFS plugin uses a distinct chunk-signature id), so
/// the enumerators can be renumbered freely.
enum class Algorithm : uint8_t {
	None,  ///< Blocks are stored uncompressed; the (de)compression calls reject it.
	Zstd,
	Lz4,
};

/// Maps an HDD_COMPRESSION_ALGORITHM config value ("none", "zstd", "lz4", case
/// insensitive) to its algorithm. Lives here rather than in a plugin so every
/// disk plugin accepts exactly the same spellings.
/// @return std::nullopt for an unrecognized name, leaving the caller to decide
///         how loudly to complain.
std::optional<Algorithm> algorithmFromName(std::string_view name);

/// The config spelling of @p algorithm, as accepted by algorithmFromName().
const char *algorithmName(Algorithm algorithm);

/// Whether a per-chunk dictionary is worth building for @p algorithm, for a
/// disk plugin deciding whether to sample and store one.
///
/// True only for Zstd, which digests a dictionary once per chunk and then
/// applies it to every block for free. LZ4's stable API can only apply one
/// through LZ4_loadDict() on the very stream that compresses the block, which
/// re-initializes the stream and inserts a hash every three dictionary bytes -
/// per block, not per chunk. Its match window already spans a whole
/// SFSBLOCKSIZE block, so a dictionary only helps matches near the block's
/// start, which does not pay for that.
///
/// This governs new chunks only. A chunk that already carries a dictionary
/// keeps using it whatever its algorithm, since that is how its blocks were
/// written.
bool usesDictionary(Algorithm algorithm);

/// Per-chunk dictionaries, prepared once into whatever form the algorithm
/// wants: a digest for Zstd, the raw bytes for LZ4. Opaque so this header does
/// not drag the compression-library headers into every includer.
class CompressDict;
class DecompressDict;

/// Deleters so the prepared dictionaries can be held in std::unique_ptr without
/// the complete types being visible here.
struct CompressDictDeleter {
	void operator()(CompressDict *dict) const;
};
struct DecompressDictDeleter {
	void operator()(DecompressDict *dict) const;
};

/// Owning handles for the prepared per-chunk dictionaries.
using CompressDictPtr = std::unique_ptr<CompressDict, CompressDictDeleter>;
using DecompressDictPtr = std::unique_ptr<DecompressDict, DecompressDictDeleter>;

/// Prepares the per-chunk dictionary bytes for compression with @p algorithm.
/// Zstd digests them at @p level (and auto-detects trained (ZDICT) vs
/// raw-content dictionaries); LZ4 keeps the bytes as they are, its effort being
/// chosen per block instead. Returns nullptr for an empty dictionary or on
/// failure (callers then compress without one).
CompressDictPtr createCompressDict(Algorithm algorithm, const uint8_t *dict, size_t dictSize,
                                   int level);

/// Prepares the per-chunk dictionary bytes for decompression with @p algorithm.
/// Returns nullptr for an empty dictionary or on failure.
DecompressDictPtr createDecompressDict(Algorithm algorithm, const uint8_t *dict, size_t dictSize);

/// Compresses a single block using the prepared per-chunk dictionary.
///
/// @param algorithm     Compression algorithm of the chunk being written. Named
///                      separately from @p dict because a chunk may have no
///                      dictionary at all.
/// @param dict          Prepared dictionary, or nullptr to compress without one.
/// @param src           Uncompressed block bytes.
/// @param srcSize       Number of uncompressed bytes.
/// @param dst           Destination buffer for the compressed frame.
/// @param dstCapacity   Capacity of the destination buffer.
/// @param level         How hard to compress: higher spends more CPU for a
///                      smaller result, the way Zstd's own levels read. Zstd
///                      takes it as its level; LZ4 maps it onto its inverted
///                      acceleration dial, reaching its strongest setting at
///                      level 9 and compressing no harder above that.
/// @return The compressed size on success, or a value <= 0 when the block was
///         not compressed - because the algorithm failed, or because the result
///         did not fit in @p dstCapacity. Either way the caller stores the block
///         raw, so the two cases need not be told apart.
ssize_t compressBlock(Algorithm algorithm, const CompressDict *dict, const uint8_t *src,
                      size_t srcSize, uint8_t *dst, size_t dstCapacity, int level);

/// Decompresses a single block previously produced by compressBlock() with the
/// same algorithm and dictionary.
///
/// @param algorithm     Compression algorithm the block was written with.
/// @param dict          Prepared dictionary used at compression time, or
///                      nullptr if the block was compressed without one.
/// @param src           Compressed frame bytes.
/// @param srcSize       Number of compressed bytes.
/// @param dst           Destination buffer for the decompressed block.
/// @param dstCapacity   Capacity of the destination buffer.
/// @return The decompressed size on success, or a negative value on error.
ssize_t decompressBlock(Algorithm algorithm, const DecompressDict *dict, const uint8_t *src,
                        size_t srcSize, uint8_t *dst, size_t dstCapacity);

/// Upper bound on the compressed size of a block of srcSize bytes. Algorithm
/// dependent, and larger than srcSize for both real algorithms: an
/// incompressible block costs a few bytes of framing. Returns 0 for
/// Algorithm::None, which compresses nothing.
size_t compressBound(Algorithm algorithm, size_t srcSize);

}  // namespace block_compression
