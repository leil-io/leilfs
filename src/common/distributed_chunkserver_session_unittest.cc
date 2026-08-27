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

#include "common/platform.h"

#include "common/distributed_chunkserver_session.h"

#include <gtest/gtest.h>

namespace {

constexpr uint32_t kStableId = 7;
constexpr uint64_t kIncarnation = 0x1122334455667788ULL;
constexpr uint32_t kRenewerId = 2;
constexpr uint64_t kRenewerIncarnation = 0xAABBCCDDEEFF0011ULL;

/// The receiver's own clock, and the widest lease it will believe from anyone.
constexpr uint64_t kNow = 900;
constexpr uint64_t kMaxLease = 300;

ChunkserverSessionLease acceptedLease() {
	return {kStableId, kIncarnation, kRenewerId, kRenewerIncarnation, 5, 1000, 30};
}

LeaseTupleAcceptance evaluate(const std::optional<ChunkserverSessionLease> &accepted,
                              const ChunkserverSessionLease &incoming) {
	return evaluateLeaseTuple(accepted, incoming, kStableId, kIncarnation,
	                          incoming.renewerMdsId, incoming.renewerMdsIncarnation, kNow,
	                          kMaxLease);
}

}  // namespace

TEST(DistributedChunkserverSessionTest, FirstTupleFromTheNamedRenewerIsAccepted) {
	EXPECT_EQ(LeaseTupleAcceptance::kAcceptNewer, evaluate(std::nullopt, acceptedLease()));
}

TEST(DistributedChunkserverSessionTest, HigherSequenceIsAccepted) {
	ChunkserverSessionLease incoming = acceptedLease();
	incoming.claimSequence = 6;
	incoming.leaseDeadline = 1100;
	EXPECT_EQ(LeaseTupleAcceptance::kAcceptNewer, evaluate(acceptedLease(), incoming));
}

TEST(DistributedChunkserverSessionTest, HigherSequenceWithLowerDeadlineIsStillAccepted) {
	// The durable claim moved; enforcing the shorter deadline fails closed.
	ChunkserverSessionLease incoming = acceptedLease();
	incoming.claimSequence = 6;
	incoming.leaseDeadline = 900;
	EXPECT_EQ(LeaseTupleAcceptance::kAcceptNewer, evaluate(acceptedLease(), incoming));
}

TEST(DistributedChunkserverSessionTest, ExactDuplicateIsIdempotent) {
	EXPECT_EQ(LeaseTupleAcceptance::kAcceptDuplicate,
	          evaluate(acceptedLease(), acceptedLease()));
}

TEST(DistributedChunkserverSessionTest, LowerSequenceIsRejected) {
	ChunkserverSessionLease incoming = acceptedLease();
	incoming.claimSequence = 4;
	incoming.leaseDeadline = 2000;
	EXPECT_EQ(LeaseTupleAcceptance::kRejectLowerSequence, evaluate(acceptedLease(), incoming));
}

TEST(DistributedChunkserverSessionTest, SameSequenceDifferentContentIsRejected) {
	ChunkserverSessionLease incoming = acceptedLease();
	incoming.leaseDeadline = 1001;
	EXPECT_EQ(LeaseTupleAcceptance::kRejectSameSequenceConflict,
	          evaluate(acceptedLease(), incoming));
}

TEST(DistributedChunkserverSessionTest, WrongStableIdOrIncarnationIsRejected) {
	ChunkserverSessionLease incoming = acceptedLease();
	incoming.claimSequence = 6;
	EXPECT_EQ(LeaseTupleAcceptance::kRejectWrongHolder,
	          evaluateLeaseTuple(acceptedLease(), incoming, kStableId + 1, kIncarnation,
	                             kRenewerId, kRenewerIncarnation, kNow, kMaxLease));
	EXPECT_EQ(LeaseTupleAcceptance::kRejectWrongHolder,
	          evaluateLeaseTuple(acceptedLease(), incoming, kStableId, kIncarnation + 1,
	                             kRenewerId, kRenewerIncarnation, kNow, kMaxLease));
}

TEST(DistributedChunkserverSessionTest, SenderMustBeTheNamedRenewer) {
	ChunkserverSessionLease incoming = acceptedLease();
	incoming.claimSequence = 6;
	EXPECT_EQ(LeaseTupleAcceptance::kRejectWrongSender,
	          evaluateLeaseTuple(acceptedLease(), incoming, kStableId, kIncarnation,
	                             kRenewerId + 1, kRenewerIncarnation, kNow, kMaxLease));
	EXPECT_EQ(LeaseTupleAcceptance::kRejectWrongSender,
	          evaluateLeaseTuple(acceptedLease(), incoming, kStableId, kIncarnation,
	                             kRenewerId, kRenewerIncarnation + 1, kNow, kMaxLease));
}

TEST(DistributedChunkserverSessionTest, AnyZeroFieldIsMalformed) {
	for (int field = 0; field < 6; ++field) {
		ChunkserverSessionLease incoming = acceptedLease();
		switch (field) {
		case 0: incoming.stableId = 0; break;
		case 1: incoming.chunkserverIncarnation = 0; break;
		case 2: incoming.renewerMdsId = 0; break;
		case 3: incoming.renewerMdsIncarnation = 0; break;
		case 4: incoming.claimSequence = 0; break;
		case 5: incoming.leaseDeadline = 0; break;
		}
		EXPECT_EQ(LeaseTupleAcceptance::kRejectMalformed, evaluate(acceptedLease(), incoming))
		    << "field " << field;
	}
}

// A deadline is a promise about time, and the receiver is the only party that can check it
// against a clock the sender does not control. Measured on the running system: a metadata server
// whose clock jumps forward mints a claim on that same clock, and without this bound the
// chunkserver believes an hour that has not happened.
TEST(DistributedChunkserverSessionTest, DeadlineBeyondTheWidestBelievedLeaseIsRejected) {
	ChunkserverSessionLease incoming = acceptedLease();
	incoming.claimSequence = 6;

	incoming.leaseDeadline = kNow + kMaxLease + incoming.cutoffReserveSeconds;
	EXPECT_EQ(LeaseTupleAcceptance::kAcceptNewer, evaluate(acceptedLease(), incoming));

	incoming.leaseDeadline = kNow + kMaxLease + incoming.cutoffReserveSeconds + 1;
	EXPECT_EQ(LeaseTupleAcceptance::kRejectImplausibleDeadline,
	          evaluate(acceptedLease(), incoming));
}

// The reserve is a slice of the lease, so a reserve wider than any believed lease is not a
// conservative setting, it is a claim that serving should stop before it started.
TEST(DistributedChunkserverSessionTest, ReserveWiderThanTheWidestBelievedLeaseIsRejected) {
	ChunkserverSessionLease incoming = acceptedLease();
	incoming.claimSequence = 6;
	incoming.cutoffReserveSeconds = kMaxLease + 1;
	incoming.leaseDeadline = kNow + 1;
	EXPECT_EQ(LeaseTupleAcceptance::kRejectImplausibleDeadline,
	          evaluate(acceptedLease(), incoming));
}

// An unreadable clock cannot justify any deadline, so it justifies none.
TEST(DistributedChunkserverSessionTest, UnreadableClockRefusesEveryTuple) {
	ChunkserverSessionLease incoming = acceptedLease();
	incoming.claimSequence = 6;
	EXPECT_EQ(LeaseTupleAcceptance::kRejectImplausibleDeadline,
	          evaluateLeaseTuple(acceptedLease(), incoming, kStableId, kIncarnation, kRenewerId,
	                             kRenewerIncarnation, std::nullopt, kMaxLease));
}

// The reserve rides beside the tuple on the wire but is judged as part of it: a redelivery
// carrying a different reserve is a conflict, not an invitation to move the local cutoff.
TEST(DistributedChunkserverSessionTest, DuplicateWithAnAlteredReserveIsAConflict) {
	ChunkserverSessionLease incoming = acceptedLease();
	EXPECT_EQ(LeaseTupleAcceptance::kAcceptDuplicate, evaluate(acceptedLease(), incoming));

	incoming.cutoffReserveSeconds = acceptedLease().cutoffReserveSeconds + 1;
	EXPECT_EQ(LeaseTupleAcceptance::kRejectSameSequenceConflict,
	          evaluate(acceptedLease(), incoming));
}
