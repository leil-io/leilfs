/*
   Copyright 2026 Leil Storage OÜ

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

#include <cstdint>
#include <vector>

#include "chunkserver-common/cmr_disk.h"

namespace mockdisk {

/// A Disk which fakes a configurable number of chunks without creating any
/// chunk file. Used by tests to exercise chunk registration and read paths
/// at scales (hundreds of millions of chunks) where creating real files is
/// not feasible.
///
/// hdd.cfg line format:
///   mock:<chunkCount>:<firstChunkId>:<path>
/// where <path> is a real (empty) directory used for lock files and trash
/// bookkeeping only.
///
/// All chunks share the same static in-memory data block, so every read is
/// served from memory and is CRC-valid. Writes are accepted and discarded.
/// The disk is never selectable for new chunks.
class MockDisk : public CmrDisk {
public:
	/// Version assigned to every fake chunk. Must match the version the
	/// master expects from its metadata (chunks created via changelog
	/// replay/metarestore get version 1).
	static constexpr uint32_t kMockChunkVersion = 1;

	/// Number of chunks registered per SyntheticChunkSink::emitBulk call.
	static constexpr uint64_t kScanBulkSize = 10'000;

	MockDisk(const disk::Configuration &configuration, uint64_t chunkCount, uint64_t firstChunkId);

	MockDisk(const MockDisk &) = delete;
	MockDisk(MockDisk &&) = delete;
	MockDisk &operator=(const MockDisk &) = delete;
	MockDisk &operator=(MockDisk &&) = delete;

	~MockDisk() override = default;

	/// Synthesizes chunkCount_ chunks instead of scanning directories.
	bool scanSyntheticChunks(const SyntheticChunkSink &sink) override;

	/// Mock disks never accept new chunks.
	bool isSelectableForNewChunk() const override { return false; }

	/// Reports synthetic full-disk usage so the disk is also unattractive
	/// for placement by space heuristics.
	void refreshDataDiskUsage() override;

	/// No stat() calls: every fake chunk is a full, valid chunk.
	int updateChunkAttributes(IChunk *chunk, bool isFromScan) override;

	/// Creates chunks whose version changes do not touch the filesystem.
	IChunk *instantiateNewConcreteChunk(uint64_t chunkId, ChunkPartType type) override;

	// IO: file descriptors point to /dev/null so the generic fd-based
	// bookkeeping (gOpenChunks, crc resources) keeps working.

	void creat(IChunk *chunk) override;
	void open(IChunk *chunk) override;
	int unlinkChunk(IChunk *chunk) override;
	int fsyncChunk(IChunk *chunk) override;
	int ftruncateData(IChunk *chunk, uint64_t size) override;

	/// Fills crcData with the static pattern-block CRC for every block.
	int readChunkCrc(IChunk *chunk, uint32_t chunkVersion, uint8_t *crcData) override;

	/// Serves the static pattern block, CRC-valid, for any block number.
	int readBlockAndCrc(IChunk *chunk, uint8_t *blockBuffer, uint8_t *crcData, uint16_t blocknum,
	                    const char *errorMsg) override;

	/// Serves the static pattern for any byte range.
	ssize_t preadData(IChunk *chunk, uint8_t *blockBuffer, uint64_t size, uint64_t offset) override;

	void prefetchChunkBlocks(IChunk &chunk, uint16_t firstBlock, uint32_t blockCount) override;

	/// Updates the in-memory version only; there is no metadata file.
	int overwriteChunkVersion(IChunk *chunk, uint32_t newVersion) override;

	// Writes: accept and discard both data and CRCs, preserving the static
	// synthetic contents (avoids fd/punch-hole side effects).

	int writePartialBlockAndCrc(IChunk *chunk, const uint8_t *buffer, uint32_t offsetInBlock,
	                            uint32_t size, const uint8_t *crcBuff, uint8_t *crcData,
	                            uint16_t blockNum, bool isNewBlock, const char *errorMsg) override;

	int writeFullBlocksAndCrcs(IChunk *chunk, const std::vector<uint16_t> &blocksPerBuffer,
	                           const std::vector<const uint8_t *> &buffers, uint16_t startBlock,
	                           const uint8_t *crcBuff, uint8_t *crcData, bool areNewBlocks,
	                           const char *errorMsg) override;

	/// The static 64KiB data block shared by all fake chunks.
	static const uint8_t *patternBlock();

	/// CRC of patternBlock(), big-endian serialized as stored in crcData.
	static const uint8_t *patternBlockCrcBigEndian();

private:
	uint64_t chunkCount_;
	uint64_t firstChunkId_;
};

}  // namespace mockdisk
