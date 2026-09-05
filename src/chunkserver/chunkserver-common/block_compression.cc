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

#include "common/platform.h"

#include "chunkserver-common/block_compression.h"

#include <zstd.h>

namespace block_compression {

namespace {

struct CCtxDeleter {
	void operator()(ZSTD_CCtx *cctx) const { ZSTD_freeCCtx(cctx); }
};
struct DCtxDeleter {
	void operator()(ZSTD_DCtx *dctx) const { ZSTD_freeDCtx(dctx); }
};

// One reusable compression/decompression context per thread, to avoid
// per-block allocation.
ZSTD_CCtx *threadLocalCCtx() {
	static thread_local std::unique_ptr<ZSTD_CCtx, CCtxDeleter> cctx(ZSTD_createCCtx());
	return cctx.get();
}

ZSTD_DCtx *threadLocalDCtx() {
	static thread_local std::unique_ptr<ZSTD_DCtx, DCtxDeleter> dctx(ZSTD_createDCtx());
	return dctx.get();
}

}  // namespace

void CDictDeleter::operator()(ZSTD_CDict *cdict) const { ZSTD_freeCDict(cdict); }

void DDictDeleter::operator()(ZSTD_DDict *ddict) const { ZSTD_freeDDict(ddict); }

CDictPtr createCDict(const uint8_t *dict, size_t dictSize, int level) {
	if (dict == nullptr || dictSize == 0) { return nullptr; }
	// ZSTD_createCDict auto-detects trained (ZDICT magic) vs raw-content
	// dictionaries and copies the bytes, so the source buffer may be freed.
	return CDictPtr(ZSTD_createCDict(dict, dictSize, level));
}

DDictPtr createDDict(const uint8_t *dict, size_t dictSize) {
	if (dict == nullptr || dictSize == 0) { return nullptr; }
	return DDictPtr(ZSTD_createDDict(dict, dictSize));
}

ssize_t compressBlock(const ZSTD_CDict *cdict, const uint8_t *src, size_t srcSize, uint8_t *dst,
                      size_t dstCapacity, int level) {
	ZSTD_CCtx *cctx = threadLocalCCtx();
	if (cctx == nullptr) { return -1; }

	const size_t result =
	    cdict != nullptr ? ZSTD_compress_usingCDict(cctx, dst, dstCapacity, src, srcSize, cdict)
	                     : ZSTD_compressCCtx(cctx, dst, dstCapacity, src, srcSize, level);

	if (ZSTD_isError(result)) { return -1; }

	return static_cast<ssize_t>(result);
}

ssize_t decompressBlock(const ZSTD_DDict *ddict, const uint8_t *src, size_t srcSize, uint8_t *dst,
                        size_t dstCapacity) {
	ZSTD_DCtx *dctx = threadLocalDCtx();
	if (dctx == nullptr) { return -1; }

	const size_t result =
	    ddict != nullptr ? ZSTD_decompress_usingDDict(dctx, dst, dstCapacity, src, srcSize, ddict)
	                     : ZSTD_decompressDCtx(dctx, dst, dstCapacity, src, srcSize);

	if (ZSTD_isError(result)) { return -1; }

	return static_cast<ssize_t>(result);
}

size_t compressBound(size_t srcSize) { return ZSTD_compressBound(srcSize); }

}  // namespace block_compression
