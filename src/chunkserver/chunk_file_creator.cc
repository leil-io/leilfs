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

#include "chunk_file_creator.h"

#include "chunkserver-common/global_shared_resources.h"
#include "chunkserver/hddspacemgr.h"
#include "chunkserver-common/hdd_utils.h"

ChunkFileCreator::ChunkFileCreator(uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType)
	: chunk_id_(chunkId),
	  chunk_version_(chunkVersion),
	  chunk_type_(chunkType),
	  chunk_(nullptr),
	  is_created_(false),
	  is_open_(false),
	  is_commited_(false) {
}

ChunkFileCreator::~ChunkFileCreator() {
	if (is_open_) {
		assert(chunk_);
		hddClose(chunk_);
	}
	if (is_created_ && !is_commited_) {
		assert(chunk_);
		hddInternalDelete(chunk_, 0);
		chunk_ = nullptr;
	}
	if (chunk_) {
		hddChunkRelease(chunk_);
	}
}

void ChunkFileCreator::create() {
	assert(!is_created_ && !chunk_);

	auto [creationStatus, newChunk] =
	    hddInternalCreateChunk(chunk_id_, 0, chunk_type_);

	if (creationStatus == SAUNAFS_STATUS_OK) {
		chunk_ = newChunk;
		is_created_ = true;
	} else {
		throw Exception("failed to create chunk", creationStatus);
	}

	int status = hddOpen(chunk_);

	if (status == SAUNAFS_STATUS_OK) {
		is_open_ = true;
	} else {
		throw Exception("failed to open created chunk", status);
	}
}

void ChunkFileCreator::write(const uint8_t *buffer, uint16_t startBlock, uint16_t numBlocks,
                             std::vector<uint32_t> &crc) {
	assert(is_open_ && !is_commited_ && chunk_);
	auto *crcData = gOpenChunks.getResource(chunk_->metaFD()).crcData();
	int writtenBytes = chunk_->owner()->writeChunkBlocks(chunk_, 0, startBlock, numBlocks, crc,
	                                                     crcData, buffer, true);
	if (writtenBytes != static_cast<int>(numBlocks * SFSBLOCKSIZE)) {
		if (writtenBytes < 0) {
			int status = -writtenBytes;
			throw Exception("failed to write chunk", status);
		}

		throw Exception("failed to write chunk", SAUNAFS_ERROR_IO);
	}
}

void ChunkFileCreator::commit() {
	assert(is_open_ && !is_commited_);
	int status = hddClose(chunk_);
	if (status == SAUNAFS_STATUS_OK) {
		is_open_ = false;
	} else {
		throw Exception("failed to close chunk", status);
	}
	status = hddInternalUpdateVersion(chunk_, 0, chunk_version_);
	if (status == SAUNAFS_STATUS_OK) {
		is_commited_ = true;
	} else {
		throw Exception("failed to set chunk's version", status);
	}
}
