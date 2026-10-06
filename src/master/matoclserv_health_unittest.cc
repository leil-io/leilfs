/*
   Copyright 2026      Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/platform.h"

#include <array>
#include <cstdint>
#include <memory>
#include <utility>

#include <gtest/gtest.h>

#include "master/chunk_operations_base.h"
#include "protocol/cltoma.h"
#include "protocol/packet.h"

struct matoclserventry;
void matoclserv_chunks_health(matoclserventry *, const uint8_t *, uint32_t);
void matoclserv_info(matoclserventry *, const uint8_t *, uint32_t);
void matoclserv_chunks_matrix(matoclserventry *, const uint8_t *, uint32_t);

namespace {

struct PreparationReached {};

class PreparingChunkOperations : public ChunkOperationsBase {
public:
	void prepareChunkHealthReport() override {
		++preparations;
		// Stop the real handler before it needs a network connection or counter state.
		throw PreparationReached{};
	}
	unsigned preparations = 0;
};

class MatoclHealthPreparationTest : public ::testing::Test {
protected:
	void SetUp() override {
		previous_ = std::move(gChunkOperations);
		auto operations = std::make_unique<PreparingChunkOperations>();
		preparing_ = operations.get();
		gChunkOperations = std::move(operations);
	}
	void TearDown() override { gChunkOperations = std::move(previous_); }

	void healthRequest(const MessageBuffer &packet) {
		EXPECT_THROW(matoclserv_chunks_health(nullptr, packet.data() + PacketHeader::kSize,
		                                      packet.size() - PacketHeader::kSize),
		             PreparationReached);
		EXPECT_EQ(preparing_->preparations, 1U);
	}

	PreparingChunkOperations *preparing_ = nullptr;
	std::unique_ptr<IChunkOperations> previous_;
};

TEST_F(MatoclHealthPreparationTest, StandardHealthPreparesBeforeReadingCounters) {
	healthRequest(cltoma::chunksHealth::build(false));
}

TEST_F(MatoclHealthPreparationTest, FreshnessHealthPreparesBeforeReadingCounters) {
	healthRequest(cltoma::chunksHealth::build());
}

TEST_F(MatoclHealthPreparationTest, InvalidHealthPayloadDoesNotPrepare) {
	const auto packet = cltoma::chunksHealth::build(false);
	EXPECT_ANY_THROW(matoclserv_chunks_health(nullptr, packet.data() + PacketHeader::kSize,
	                                          sizeof(PacketVersion)));
	EXPECT_EQ(preparing_->preparations, 0U);
}

TEST_F(MatoclHealthPreparationTest, InfoPreparesBeforeReadingCounters) {
	EXPECT_THROW(matoclserv_info(nullptr, nullptr, 0), PreparationReached);
	EXPECT_EQ(preparing_->preparations, 1U);
}

TEST_F(MatoclHealthPreparationTest, MatrixPreparesWithEitherValidPacketSize) {
	const std::array<uint8_t, 1> matrixId{0};
	for (uint32_t length : {0U, 1U}) {
		preparing_->preparations = 0;
		EXPECT_THROW(matoclserv_chunks_matrix(nullptr, matrixId.data(), length),
		             PreparationReached);
		EXPECT_EQ(preparing_->preparations, 1U);
	}
}

TEST(MatoclHealthPreparation, BasePreparationDoesNotRequireChunkState) {
	ChunkOperationsBase operations;
	EXPECT_NO_THROW(operations.prepareChunkHealthReport());
}

}  // namespace
