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
#include "protocol/cstocs.h"

#include <gtest/gtest.h>

#include "unittests/chunk_type_constants.h"
#include "unittests/inout_pair.h"
#include "unittests/packet.h"

TEST(CstocsCommunicationTests, GetChunkBlocks) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, chunkId, 0x0123456789ABCDEF, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, chunkVersion, 0x01234567, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(ChunkPartType, chunkType, xor_2_of_6, standard);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(cstocs::getChunkBlocks::serialize(buffer,
			chunkIdIn, chunkVersionIn, chunkTypeIn));

	verifyHeader(buffer, SAU_CSTOCS_GET_CHUNK_BLOCKS);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(cstocs::getChunkBlocks::deserialize(buffer.data(), buffer.size(),
			chunkIdOut, chunkVersionOut, chunkTypeOut));

	SAUNAFS_VERIFY_INOUT_PAIR(chunkId);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkVersion);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkType);
}

TEST(CstocsCommunicationTests, GetChunkBlocksStatus) {
	SAUNAFS_DEFINE_INOUT_PAIR(uint64_t, chunkId, 0x0123456789ABCDEF, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, chunkVersion, 0x01234567, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(ChunkPartType, chunkType, xor_p_of_7, standard);
	SAUNAFS_DEFINE_INOUT_PAIR(uint32_t, blocks, 0xFEED, 0);
	SAUNAFS_DEFINE_INOUT_PAIR(uint8_t, status, 123, 0);

	std::vector<uint8_t> buffer;
	ASSERT_NO_THROW(cstocs::getChunkBlocksStatus::serialize(buffer,
			chunkIdIn, chunkVersionIn, chunkTypeIn, blocksIn, statusIn));

	verifyHeader(buffer, SAU_CSTOCS_GET_CHUNK_BLOCKS_STATUS);
	removeHeaderInPlace(buffer);
	ASSERT_NO_THROW(cstocs::getChunkBlocksStatus::deserialize(buffer,
			chunkIdOut, chunkVersionOut, chunkTypeOut, blocksOut, statusOut));

	SAUNAFS_VERIFY_INOUT_PAIR(chunkId);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkVersion);
	SAUNAFS_VERIFY_INOUT_PAIR(chunkType);
	SAUNAFS_VERIFY_INOUT_PAIR(blocks);
	SAUNAFS_VERIFY_INOUT_PAIR(status);
}
