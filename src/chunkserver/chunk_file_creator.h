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

#include "chunkserver-common/chunk_interface.h"
#include "common/chunk_part_type.h"

/**
 * @brief Helper class to create chunk with data.
 *
 * This class is used to safely create chunk with data.
 * If data isn't written or committed before d-tor,
 * chunk will be deleted.
 */
class ChunkFileCreator {
public:
	ChunkFileCreator(uint64_t chunkId, uint32_t chunkVersion, ChunkPartType chunkType);
	~ChunkFileCreator();

	void create();
	void write(const uint8_t* buffer, uint32_t numBlocks, std::vector<uint32_t> &crc);
	void commit();

	uint64_t chunkId() const { return chunk_id_; }
	uint32_t chunkVersion() const { return chunk_version_; }
	ChunkPartType chunkType() const { return chunk_type_; }

protected:
	uint64_t chunk_id_;
	uint32_t chunk_version_;
	ChunkPartType chunk_type_;

	IChunk *chunk_;

	bool is_created_;
	bool is_open_;
	bool is_commited_;
};
