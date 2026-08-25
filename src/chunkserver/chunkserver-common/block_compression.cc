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

#include "common/platform.h"

#include "chunkserver-common/block_compression.h"

#include <lz4.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "slogger/slogger.h"

namespace block_compression {

namespace {

/// The level from which LZ4 compresses no harder.
constexpr int kLz4StrongestLevel = 9;

/// Effort level to LZ4's acceleration, which runs the other way round. Keeps
/// the inversion here rather than in every caller's head. Clamps first, since
/// the subtraction would overflow on an absurd level.
int lz4Acceleration(int level) {
	return kLz4StrongestLevel + 1 - std::clamp(level, 1, kLz4StrongestLevel);
}

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

// One reusable context per thread, to avoid per-block allocation. Heap
// allocated, holding only a pointer here: a dlopen'd plugin's thread_locals
// live in dynamic TLS, where large objects are expensive.
ZSTD_CCtx *threadLocalCCtx() {
	static thread_local std::unique_ptr<ZSTD_CCtx, CCtxDeleter> cctx(ZSTD_createCCtx());
	return cctx.get();
}

ZSTD_DCtx *threadLocalDCtx() {
	static thread_local std::unique_ptr<ZSTD_DCtx, DCtxDeleter> dctx(ZSTD_createDCtx());
	return dctx.get();
}

// Compression needs a match-table state; decompression needs none.
LZ4_stream_t *threadLocalLz4Stream() {
	static thread_local std::unique_ptr<LZ4_stream_t, Lz4StreamDeleter> stream(LZ4_createStream());
	return stream.get();
}

/// True when @p size fits the int-typed sizes of the LZ4 API.
bool fitsLz4Size(size_t size) {
	return size <= static_cast<size_t>(std::numeric_limits<int>::max());
}

}  // namespace

/// Dictionary prepared for compression: a digest for Zstd, and for LZ4 just
/// the bytes, since its API can only load them per block.
class CompressDict {
public:
	Algorithm algorithm = Algorithm::None;
	std::unique_ptr<ZSTD_CDict, ZstdCDictDeleter> zstdDict;  ///< Zstd only
	std::vector<uint8_t> rawDict;                            ///< Lz4 only
};

/// Dictionary prepared for decompression. Owns its bytes, so the handle stays
/// usable whatever the chunk does with its own copy.
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

int strongestLevel(Algorithm algorithm) {
	switch (algorithm) {
	case Algorithm::Zstd:
		return ZSTD_maxCLevel();
	case Algorithm::Lz4:
		return kLz4StrongestLevel;
	case Algorithm::None:
		break;
	}
	return 0;
}

CompressDictPtr createCompressDict(Algorithm algorithm, const uint8_t *dict, size_t dictSize,
                                   int level) {
	if (dict == nullptr || dictSize == 0) { return nullptr; }

	auto prepared = CompressDictPtr(new CompressDict());
	prepared->algorithm = algorithm;

	switch (algorithm) {
	case Algorithm::Zstd:
		// Auto-detects trained vs raw-content, and copies the bytes.
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
	// Another algorithm's dictionary would be read as the wrong arm. Refuse,
	// rather than silently compressing without it: the read path expects it.
	if (dict != nullptr && dict->algorithm != algorithm) {
		// Every caller stores a refused block raw, so nothing else reports
		// this - unlike the read side, where the caller marks the chunk
		// damaged. Once per process: a caller bug repeats for every block of
		// every write.
		static std::atomic_flag reported;
		if (!reported.test_and_set(std::memory_order_relaxed)) {
			safs::log_err("{}: a {} dictionary was passed to {}; blocks will be stored "
			              "uncompressed",
			              __func__, algorithmName(dict->algorithm), algorithmName(algorithm));
		}
		return -1;
	}

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
			// Re-initializes the state, so one stream serves both paths.
			return LZ4_compress_fast_extState(stream, source, destination, sourceSize,
			                                  destinationCapacity, acceleration);
		}

		// Rebuilds the match table, so it both applies the dictionary and
		// clears the previous block. Per block because LZ4_attach_dictionary(),
		// which would prepare it once, is stable only from liblz4 1.10.0.
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

		// No header in the LZ4 block format: a corrupt frame shows up as the
		// decoder overrunning (negative here), and past that as a bad CRC.
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
