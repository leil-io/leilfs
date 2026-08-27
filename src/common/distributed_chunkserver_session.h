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

#pragma once

#include "common/platform.h"

#include <cstdint>
#include <optional>

/// Role requested by one connection during the distinct distributed-MDS handshake.
/// Values are wire-visible and therefore explicit rather than relying on enum ordering.
enum class DistributedRegistrationRole : uint8_t {
	kMintOnly = 1,
	kClaimRenewer = 2,
	kObserver = 3,
};

inline bool isValidDistributedRegistrationRole(uint8_t value) {
	return value == static_cast<uint8_t>(DistributedRegistrationRole::kMintOnly) ||
	       value == static_cast<uint8_t>(DistributedRegistrationRole::kClaimRenewer) ||
	       value == static_cast<uint8_t>(DistributedRegistrationRole::kObserver);
}

/// One session-claim tuple as exposed to the chunkserver: exactly the durable
/// CS_SESSION_ fields plus the stable id the row is keyed by. The chunkserver adopts a
/// tuple only through evaluateLeaseTuple below; a deadline arriving any other way is a
/// hint and never extends authority.
struct ChunkserverSessionLease {
	uint32_t stableId = 0;
	uint64_t chunkserverIncarnation = 0;
	uint32_t renewerMdsId = 0;
	uint64_t renewerMdsIncarnation = 0;
	uint64_t claimSequence = 0;
	uint64_t leaseDeadline = 0;
	/// How long before the deadline this chunkserver must stop serving. It is authority
	/// bearing, so it lives in the tuple: a redelivery carrying a different reserve is then a
	/// same sequence conflict by construction, and cannot quietly move the local cutoff.
	uint64_t cutoffReserveSeconds = 0;

	bool operator==(const ChunkserverSessionLease &) const = default;
};

/// The chunkserver-side acceptance verdict for one incoming lease tuple.
enum class LeaseTupleAcceptance : uint8_t {
	kAcceptNewer,               ///< Higher sequence for this exact holder; adopt it.
	kAcceptDuplicate,           ///< Exact redelivery of the accepted tuple; idempotent.
	kRejectMalformed,           ///< A zero field; the durable codec never produces one.
	kRejectWrongHolder,         ///< Names another stable id or process incarnation.
	kRejectWrongSender,         ///< The sending MDS is not the renewer the tuple names.
	kRejectLowerSequence,       ///< A delayed packet from an older claim; never adopt.
	kRejectSameSequenceConflict,///< Same sequence, different content; impossible durably.
	kRejectImplausibleDeadline, ///< Deadline or reserve the receiver's own clock cannot justify.
};

inline bool leaseTupleAccepted(LeaseTupleAcceptance verdict) {
	return verdict == LeaseTupleAcceptance::kAcceptNewer ||
	       verdict == LeaseTupleAcceptance::kAcceptDuplicate;
}

/// Pure chunkserver-side decision on one lease tuple. Only a newer exact-incarnation
/// tuple, or its exact duplicate, is acceptable; the sender must be the renewer the
/// tuple names. A higher sequence with a shorter deadline is still adopted: the durable
/// claim moved, and enforcing the shorter deadline is the fail-closed choice.
/// @p now is the receiver's own authority clock, absent when it could not be read, and
/// @p maxAcceptedLeaseSeconds is the furthest ahead of it a deadline may legitimately sit. The
/// bound belongs here, on the receiving side: a sender whose clock is wrong believes its own
/// clock, so a cluster is only as safe against clock error as the receiver's willingness to
/// refuse a promise it cannot justify. An unreadable clock refuses everything.
inline LeaseTupleAcceptance evaluateLeaseTuple(
    const std::optional<ChunkserverSessionLease> &accepted,
    const ChunkserverSessionLease &incoming, uint32_t ownStableId, uint64_t ownIncarnation,
    uint32_t senderMdsId, uint64_t senderMdsIncarnation, std::optional<uint64_t> now,
    uint64_t maxAcceptedLeaseSeconds) {
	if (incoming.stableId == 0 || incoming.chunkserverIncarnation == 0 ||
	    incoming.renewerMdsId == 0 || incoming.renewerMdsIncarnation == 0 ||
	    incoming.claimSequence == 0 || incoming.leaseDeadline == 0) {
		return LeaseTupleAcceptance::kRejectMalformed;
	}

	if (incoming.stableId != ownStableId || incoming.chunkserverIncarnation != ownIncarnation) {
		return LeaseTupleAcceptance::kRejectWrongHolder;
	}
	if (incoming.renewerMdsId != senderMdsId ||
	    incoming.renewerMdsIncarnation != senderMdsIncarnation) {
		return LeaseTupleAcceptance::kRejectWrongSender;
	}
	// Identity and ordering first. A packet from an older claim is stale whatever any clock
	// says, and saying so must not depend on being able to read one.
	if (accepted.has_value()) {
		if (incoming.claimSequence < accepted->claimSequence) {
			return LeaseTupleAcceptance::kRejectLowerSequence;
		}
		if (incoming.claimSequence == accepted->claimSequence) {
			return incoming == *accepted ? LeaseTupleAcceptance::kAcceptDuplicate
			                             : LeaseTupleAcceptance::kRejectSameSequenceConflict;
		}
	}

	// Only a tuple about to be adopted needs its promise checked. The reserve is a slice of the
	// lease, never longer than one, and the deadline may not sit further ahead than one full
	// lease plus that slice. An unreadable clock justifies nothing, so it adopts nothing.
	if (!now.has_value() || incoming.cutoffReserveSeconds > maxAcceptedLeaseSeconds ||
	    incoming.leaseDeadline > *now + maxAcceptedLeaseSeconds + incoming.cutoffReserveSeconds) {
		return LeaseTupleAcceptance::kRejectImplausibleDeadline;
	}
	return LeaseTupleAcceptance::kAcceptNewer;
}
