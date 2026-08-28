/*
   Copyright 2026      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
*/

#include "chunkserver/evidence_outbox.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include "unittests/TemporaryDirectory.h"

namespace {

EvidenceItem lostItem(uint64_t chunkId, uint64_t incarnation) {
	EvidenceItem item;
	item.incarnation = incarnation;
	item.originClaimSequence = 7;
	item.scanEpoch = 1;
	item.kind = static_cast<uint8_t>(EvidenceItemKind::kLost);
	item.chunkId = chunkId;
	item.partType = 0;
	item.observedAtMs = 123456;
	return item;
}

class EvidenceOutboxTest : public ::testing::Test {
protected:
	void SetUp() override {
		temp_ = std::make_unique<TemporaryDirectory>("/tmp", "evidence_outbox_test");
		ASSERT_EQ(0, chdir(temp_->name().c_str()));
		evidence_outbox::init();
	}
	std::unique_ptr<TemporaryDirectory> temp_;
};

TEST_F(EvidenceOutboxTest, AppendAssignsMonotonicSequencesAndRetainsUntilAck) {
	// The statement is made once and kept: sends never consume, only the acknowledgement does,
	// and only up to the sequence it names.
	ASSERT_TRUE(evidence_outbox::append(lostItem(11, 0xAA)));
	ASSERT_TRUE(evidence_outbox::append(lostItem(22, 0xAA)));
	ASSERT_TRUE(evidence_outbox::append(lostItem(33, 0xAA)));
	auto batch = evidence_outbox::unacked(10);
	ASSERT_EQ(batch.size(), 3U);
	EXPECT_EQ(batch[0].sequence, 1U);
	EXPECT_EQ(batch[2].sequence, 3U);
	EXPECT_EQ(evidence_outbox::unacked(10).size(), 3U);

	evidence_outbox::ack(2);
	batch = evidence_outbox::unacked(10);
	ASSERT_EQ(batch.size(), 1U);
	EXPECT_EQ(batch[0].sequence, 3U);
	EXPECT_EQ(batch[0].chunkId, 33U);
}

TEST_F(EvidenceOutboxTest, PendingItemsAndSequenceSurviveRestart) {
	// The whole point: a chunkserver restart replays rather than loses, and the sequence space
	// never rewinds, so the receiver's committed position keeps deduplicating after the restart.
	ASSERT_TRUE(evidence_outbox::append(lostItem(11, 0xAA)));
	ASSERT_TRUE(evidence_outbox::append(lostItem(22, 0xAA)));
	evidence_outbox::ack(1);

	evidence_outbox::init();  // a fresh process start in the same data directory
	auto batch = evidence_outbox::unacked(10);
	ASSERT_EQ(batch.size(), 1U);
	EXPECT_EQ(batch[0].sequence, 2U);
	EXPECT_EQ(batch[0].chunkId, 22U);
	EXPECT_EQ(batch[0].incarnation, 0xAAU);

	// The next statement continues the persisted sequence space.
	ASSERT_TRUE(evidence_outbox::append(lostItem(33, 0xBB)));
	batch = evidence_outbox::unacked(10);
	ASSERT_EQ(batch.size(), 2U);
	EXPECT_EQ(batch[1].sequence, 3U);
}

TEST_F(EvidenceOutboxTest, FullOutboxRefusesVisiblyAndKeepsEverything) {
	// Admission stops; nothing already stated is discarded to make room.
	for (uint64_t i = 0; i < 4096; ++i) {
		ASSERT_TRUE(evidence_outbox::append(lostItem(i + 1, 0xAA)));
	}
	EXPECT_FALSE(evidence_outbox::append(lostItem(5000, 0xAA)));
	EXPECT_EQ(evidence_outbox::pendingCount(), 4096U);
	evidence_outbox::ack(1);
	EXPECT_TRUE(evidence_outbox::append(lostItem(5000, 0xAA)));
}

TEST_F(EvidenceOutboxTest, AckBelowFrontIsANoOp) {
	ASSERT_TRUE(evidence_outbox::append(lostItem(11, 0xAA)));
	evidence_outbox::ack(0);
	EXPECT_EQ(evidence_outbox::pendingCount(), 1U);
}

}  // namespace
