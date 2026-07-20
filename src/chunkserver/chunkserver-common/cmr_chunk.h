/*
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

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

#include "chunkserver-common/chunk_with_fd.h"

/// Specialization of FDChunk for CMR disks.
/// CmrChunk reuses attributes and functions of FDChunk because it is also based
/// on file descriptors.
/// CmrChunk overrides some functions to add support for CMR disks.
class CmrChunk : public FDChunk {
public:
	explicit CmrChunk(uint64_t chunkId, ChunkPartType type, ChunkState state);

	// No need to copy or move them so far

	CmrChunk(const CmrChunk &) = delete;
	CmrChunk(CmrChunk &&) = delete;
	CmrChunk &operator=(const CmrChunk &) = delete;
	CmrChunk &operator=(CmrChunk &&) = delete;

	~CmrChunk() override = default;

	/// Generates the name of the data file for the given version. For CmrChunk,
	/// the data file looks like this:
	/// /mnt/saunafs/data/hdd0/chunks00/chunk_0000000000000001_00000001.dat
	std::string generateDataFilenameForVersion(
	    uint32_t _version) const override;

	/// Renames metadata and data filenames according to the new version.
	/// This function also renames both files on the filesystem.
	int renameChunkFile(uint32_t new_version) override;

	/// Returns a pointer to the buffer containing the Chunk header.
	/// The returned pointer is assumed to be thread local.
	uint8_t *getChunkHeaderBuffer() const override;

	/// Returns the Chunk header size. The header size contains the size of the
	/// signature and the CRC blocks, rounded up to the disk block size.
	size_t getHeaderSize() const override;

	/// Returns the offset for the CRC. For CmrChunk, this value is the size of
	/// the signature.
	off_t getCrcOffset() const override;

	/// Shrink the chunk to the given number of blocks. For CmrChunk, this
	/// function does nothing, but we need the function in the interface.
	void shrinkToBlocks(uint16_t newBlocks) override;

	/// String representation of CmrChunk members, useful for debugging.
	std::string toString() const override;
};
