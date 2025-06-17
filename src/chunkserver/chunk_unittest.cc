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

#include "common/platform.h"

#include <gtest/gtest.h>

#include "chunkserver-common/cmr_chunk.h"
#include "chunkserver-common/cmr_disk.h"
#include "chunkserver-common/subfolder.h"
#include "common/slice_traits.h"

class ChunkTests : public testing::Test {
public:
	ChunkTests()
	    : standardChunk(1, slice_traits::standard::ChunkPartType(),
	                    ChunkState::Available),
	      chunk_1_of_2(1, slice_traits::xors::ChunkPartType(2, 1),
	                   ChunkState::Available),
	      chunk_2_of_2(1, slice_traits::xors::ChunkPartType(2, 2),
	                   ChunkState::Available),
	      chunk_p_of_2(1,
	                   slice_traits::xors::ChunkPartType(
	                       2, slice_traits::xors::kXorParityPart),
	                   ChunkState::Available),
	      chunk_1_of_3(1, slice_traits::xors::ChunkPartType(3, 1),
	                   ChunkState::Available),
	      chunk_3_of_3(1, slice_traits::xors::ChunkPartType(3, 3),
	                   ChunkState::Available),
	      chunk_p_of_3(1,
	                   slice_traits::xors::ChunkPartType(
	                       3, slice_traits::xors::kXorParityPart),
	                   ChunkState::Available) {}

protected:
	CmrChunk standardChunk;
	CmrChunk chunk_1_of_2, chunk_2_of_2, chunk_p_of_2;
	CmrChunk chunk_1_of_3, chunk_3_of_3, chunk_p_of_3;
};

TEST_F(ChunkTests, MaxBlocksInFile) {
	EXPECT_EQ(1024U, standardChunk.maxBlocksInFile());
	EXPECT_EQ(512U, chunk_1_of_2.maxBlocksInFile());
	EXPECT_EQ(512U, chunk_2_of_2.maxBlocksInFile());
	EXPECT_EQ(512U, chunk_p_of_2.maxBlocksInFile());
	EXPECT_EQ(342U, chunk_1_of_3.maxBlocksInFile());
	EXPECT_EQ(342U, chunk_3_of_3.maxBlocksInFile());
	EXPECT_EQ(342U, chunk_p_of_3.maxBlocksInFile());
}

TEST_F(ChunkTests, GetFileName) {
	CmrDisk disk("/mnt/", "/mnt", false, false);

	standardChunk.setId(0x123456);
	standardChunk.setOwner(&disk);
	standardChunk.setVersion(0xabcd);
	EXPECT_EQ("/mnt/chunks12/"
	          "chunk_0000000000123456_0000ABCD" CHUNK_METADATA_FILE_EXTENSION,
	          standardChunk.fullMetaFilename());

	chunk_1_of_3.setId(0x8765430d);
	chunk_1_of_3.setOwner(&disk);
	chunk_1_of_3.setVersion(0x654321);
	EXPECT_EQ("/mnt/chunks65/chunk_xor_1_of_3_000000008765430D_00654321"
	          CHUNK_METADATA_FILE_EXTENSION,
	          chunk_1_of_3.fullMetaFilename());

	chunk_p_of_3.setId(0x1234567890abcdef);
	chunk_p_of_3.setOwner(&disk);
	chunk_p_of_3.setVersion(0x12345678);
	EXPECT_EQ("/mnt/chunksAB/chunk_xor_parity_of_3_1234567890ABCDEF_12345678"
	          CHUNK_METADATA_FILE_EXTENSION,
	          chunk_p_of_3.fullMetaFilename());
}

TEST_F(ChunkTests, GetSubfolderName) {
	EXPECT_EQ("chunks00", Subfolder::getSubfolderNameGivenNumber(0x00));
	EXPECT_EQ("chunksAB", Subfolder::getSubfolderNameGivenNumber(0xAB));
	EXPECT_EQ("chunksFF", Subfolder::getSubfolderNameGivenNumber(0xFF));
	EXPECT_EQ("chunks00", Subfolder::getSubfolderNameGivenChunkId(0x1234512345003456LL));
	EXPECT_EQ("chunksAD", Subfolder::getSubfolderNameGivenChunkId(0x1234512345AD3456LL));
	EXPECT_EQ("chunksFF", Subfolder::getSubfolderNameGivenChunkId(0x1234512345FF3456LL));
}
