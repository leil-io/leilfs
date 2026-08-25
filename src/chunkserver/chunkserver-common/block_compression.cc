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

#include "chunkserver-common/block_compression.h"

#include <lz4.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace block_compression {

namespace {

/// The effort level from which LZ4 is already at its strongest setting;
/// anything above it compresses no harder.
constexpr int kLz4StrongestLevel = 9;

/// Turns an effort level (higher compresses harder, as Zstd's own levels do)
/// into LZ4's acceleration, which runs the other way: higher is faster and
/// compresses less. One config option can then mean the same thing for both
/// algorithms, at the cost of this inversion living here rather than in
/// everyone's head.
int lz4Acceleration(int level) { return std::max(1, kLz4StrongestLevel + 1 - level); }

struct CCtxDeleter {
	void operator()(ZSTD_CCtx *cctx) const { ZSTD_freeCCtx(cctx); }
};
struct DCtxDeleter {
	void operator()(ZSTD_DCtx *dctx) const { ZSTD_freeDCtx(dctx); }
};
struct ZstdCDictDeleter {
	void operator()(ZSTD_CDict *cdict) const { ZSTD_freeCDict(cdict); }
};
struct ZstdDDictDeleter {
	void operator()(ZSTD_DDict *ddict) const { ZSTD_freeDDict(ddict); }
};
struct Lz4StreamDeleter {
	void operator()(LZ4_stream_t *stream) const { LZ4_freeStream(stream); }
};

// One reusable compression/decompression context per thread, to avoid
// per-block allocation. The thread_local slots hold only pointers: these
// objects are heap allocated because a disk plugin's thread_locals live in
// dynamic TLS, where large objects are expensive.
ZSTD_CCtx *threadLocalCCtx() {
	static thread_local std::unique_ptr<ZSTD_CCtx, CCtxDeleter> cctx(ZSTD_createCCtx());
	return cctx.get();
}

ZSTD_DCtx *threadLocalDCtx() {
	static thread_local std::unique_ptr<ZSTD_DCtx, DCtxDeleter> dctx(ZSTD_createDCtx());
	return dctx.get();
}

// LZ4 needs a match-table state to compress, and none at all to decompress.
LZ4_stream_t *threadLocalLz4Stream() {
	static thread_local std::unique_ptr<LZ4_stream_t, Lz4StreamDeleter> stream(LZ4_createStream());
	return stream.get();
}

/// True when @p size fits the int-typed sizes of the LZ4 API.
bool fitsLz4Size(size_t size) {
	return size <= static_cast<size_t>(std::numeric_limits<int>::max());
}

}  // namespace

/// Per-chunk dictionary prepared for compression. Zstd digests the bytes into
/// a form that costs about as much to build as compressing a block, so it is
/// built once per chunk; LZ4's stable API can only take a dictionary through
/// LZ4_loadDict() on the stream that compresses the block, so all this holds
/// for LZ4 is the bytes to hand it.
class CompressDict {
public:
	Algorithm algorithm = Algorithm::None;
	std::unique_ptr<ZSTD_CDict, ZstdCDictDeleter> zstdDict;  ///< Zstd only
	std::vector<uint8_t> rawDict;                            ///< Lz4 only
};

/// Per-chunk dictionary prepared for decompression. Owns its bytes rather than
/// pointing at the chunk's copy, matching what ZSTD_createDDict() does with
/// them and keeping the handle usable no matter what the chunk does with its
/// own buffer afterwards.
class DecompressDict {
public:
	Algorithm algorithm = Algorithm::None;
	std::unique_ptr<ZSTD_DDict, ZstdDDictDeleter> zstdDict;  ///< Zstd only
	std::vector<uint8_t> rawDict;                            ///< Lz4 only
};

void CompressDictDeleter::operator()(CompressDict *dict) const { delete dict; }

void DecompressDictDeleter::operator()(DecompressDict *dict) const { delete dict; }

std::optional<Algorithm> algorithmFromName(std::string_view name) {
	static constexpr std::array<std::pair<std::string_view, Algorithm>, 3> kNames = {{
	    {"none", Algorithm::None},
	    {"zstd", Algorithm::Zstd},
	    {"lz4", Algorithm::Lz4},
	}};

	std::string lowered(name);
	std::transform(lowered.begin(), lowered.end(), lowered.begin(),
	               [](unsigned char chr) { return static_cast<char>(std::tolower(chr)); });

	for (const auto &[candidate, algorithm] : kNames) {
		if (lowered == candidate) { return algorithm; }
	}

	return std::nullopt;
}

const char *algorithmName(Algorithm algorithm) {
	switch (algorithm) {
	case Algorithm::Zstd:
		return "zstd";
	case Algorithm::Lz4:
		return "lz4";
	case Algorithm::None:
		break;
	}
	return "none";
}

bool usesDictionary(Algorithm algorithm) { return algorithm == Algorithm::Zstd; }

CompressDictPtr createCompressDict(Algorithm algorithm, const uint8_t *dict, size_t dictSize,
                                   int level) {
	if (dict == nullptr || dictSize == 0) { return nullptr; }

	auto prepared = CompressDictPtr(new CompressDict());
	prepared->algorithm = algorithm;

	switch (algorithm) {
	case Algorithm::Zstd:
		// ZSTD_createCDict auto-detects trained (ZDICT magic) vs raw-content
		// dictionaries and copies the bytes, so the source buffer may be freed.
		prepared->zstdDict.reset(ZSTD_createCDict(dict, dictSize, level));
		if (prepared->zstdDict == nullptr) { return nullptr; }
		break;
	case Algorithm::Lz4:
		if (!fitsLz4Size(dictSize)) { return nullptr; }
		prepared->rawDict.assign(dict, dict + dictSize);
		break;
	case Algorithm::None:
		return nullptr;
	}

	return prepared;
}

DecompressDictPtr createDecompressDict(Algorithm algorithm, const uint8_t *dict, size_t dictSize) {
	if (dict == nullptr || dictSize == 0) { return nullptr; }

	auto prepared = DecompressDictPtr(new DecompressDict());
	prepared->algorithm = algorithm;

	switch (algorithm) {
	case Algorithm::Zstd:
		prepared->zstdDict.reset(ZSTD_createDDict(dict, dictSize));
		if (prepared->zstdDict == nullptr) { return nullptr; }
		break;
	case Algorithm::Lz4:
		if (!fitsLz4Size(dictSize)) { return nullptr; }
		prepared->rawDict.assign(dict, dict + dictSize);
		break;
	case Algorithm::None:
		return nullptr;
	}

	return prepared;
}

ssize_t compressBlock(Algorithm algorithm, const CompressDict *dict, const uint8_t *src,
                      size_t srcSize, uint8_t *dst, size_t dstCapacity, int level) {
	// A dictionary prepared for another algorithm would be read as the wrong
	// union arm; compressing without it instead would produce blocks the read
	// path cannot decode, so refuse the block and let the caller store it raw.
	if (dict != nullptr && dict->algorithm != algorithm) { return -1; }

	switch (algorithm) {
	case Algorithm::Zstd: {
		ZSTD_CCtx *cctx = threadLocalCCtx();
		if (cctx == nullptr) { return -1; }

		const ZSTD_CDict *cdict = dict != nullptr ? dict->zstdDict.get() : nullptr;
		const size_t result =
		    cdict != nullptr
		        ? ZSTD_compress_usingCDict(cctx, dst, dstCapacity, src, srcSize, cdict)
		        : ZSTD_compressCCtx(cctx, dst, dstCapacity, src, srcSize, level);

		if (ZSTD_isError(result)) { return -1; }

		return static_cast<ssize_t>(result);
	}
	case Algorithm::Lz4: {
		LZ4_stream_t *stream = threadLocalLz4Stream();
		if (stream == nullptr) { return -1; }
		if (srcSize > static_cast<size_t>(LZ4_MAX_INPUT_SIZE) || !fitsLz4Size(dstCapacity)) {
			return -1;
		}

		const auto *source = reinterpret_cast<const char *>(src);
		auto *destination = reinterpret_cast<char *>(dst);
		const int sourceSize = static_cast<int>(srcSize);
		const int destinationCapacity = static_cast<int>(dstCapacity);

		const int acceleration = lz4Acceleration(level);

		if (dict == nullptr) {
			// LZ4_compress_fast_extState() re-initializes the state it is
			// given, so the same stream serves both paths.
			return LZ4_compress_fast_extState(stream, source, destination, sourceSize,
			                                  destinationCapacity, acceleration);
		}

		// LZ4_loadDict() rebuilds the match table from scratch, which both
		// applies the dictionary and clears whatever the previous block left
		// behind - including the undefined state a block that did not fit
		// leaves the stream in. Reloading the dictionary per block is the
		// price of the stable API: LZ4_attach_dictionary(), which would let a
		// dictionary be prepared once per chunk, is LZ4_STATIC_LINKING_ONLY.
		LZ4_loadDict(stream, reinterpret_cast<const char *>(dict->rawDict.data()),
		             static_cast<int>(dict->rawDict.size()));

		return LZ4_compress_fast_continue(stream, source, destination, sourceSize,
		                                  destinationCapacity, acceleration);
	}
	case Algorithm::None:
		break;
	}

	return -1;
}

ssize_t decompressBlock(Algorithm algorithm, const DecompressDict *dict, const uint8_t *src,
                        size_t srcSize, uint8_t *dst, size_t dstCapacity) {
	if (dict != nullptr && dict->algorithm != algorithm) { return -1; }

	switch (algorithm) {
	case Algorithm::Zstd: {
		ZSTD_DCtx *dctx = threadLocalDCtx();
		if (dctx == nullptr) { return -1; }

		const ZSTD_DDict *ddict = dict != nullptr ? dict->zstdDict.get() : nullptr;
		const size_t result =
		    ddict != nullptr
		        ? ZSTD_decompress_usingDDict(dctx, dst, dstCapacity, src, srcSize, ddict)
		        : ZSTD_decompressDCtx(dctx, dst, dstCapacity, src, srcSize);

		if (ZSTD_isError(result)) { return -1; }

		return static_cast<ssize_t>(result);
	}
	case Algorithm::Lz4: {
		if (!fitsLz4Size(srcSize) || !fitsLz4Size(dstCapacity)) { return -1; }

		const auto *source = reinterpret_cast<const char *>(src);
		auto *destination = reinterpret_cast<char *>(dst);
		const int sourceSize = static_cast<int>(srcSize);
		const int destinationCapacity = static_cast<int>(dstCapacity);

		// The LZ4 block format carries no header, so a corrupt frame is caught
		// by the decoder overrunning its input or output (both return a
		// negative value here) and, past that, by the block's CRC.
		if (dict == nullptr) {
			return LZ4_decompress_safe(source, destination, sourceSize, destinationCapacity);
		}

		return LZ4_decompress_safe_usingDict(
		    source, destination, sourceSize, destinationCapacity,
		    reinterpret_cast<const char *>(dict->rawDict.data()),
		    static_cast<int>(dict->rawDict.size()));
	}
	case Algorithm::None:
		break;
	}

	return -1;
}

size_t compressBound(Algorithm algorithm, size_t srcSize) {
	switch (algorithm) {
	case Algorithm::Zstd:
		return ZSTD_compressBound(srcSize);
	case Algorithm::Lz4:
		return fitsLz4Size(srcSize) ? static_cast<size_t>(LZ4_compressBound(
		                                  static_cast<int>(srcSize)))
		                            : 0;
	case Algorithm::None:
		break;
	}
	return 0;
}

}  // namespace block_compression
