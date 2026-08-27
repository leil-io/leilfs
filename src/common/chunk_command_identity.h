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

#include "common/serialization_macros.h"

/// The chunk command families a metadata server issues, and the one status it receives without
/// having issued anything.
///
/// Shared by both sides because the fenced reply names the family it answers: a participant that
/// numbers these differently from the metadata server would answer the right command with the
/// wrong meaning, which is the one mistake a single reply type makes possible.
enum class ChunkCommandFamily : uint8_t {
	kCreate,
	kDelete,
	kReplicate,
	/// A source named by a replication. Nothing is sent to it and nothing answers for it, so
	/// only the authority gate applies.
	kReplicateSource,
	kChunkLock,
	kChunkUnlock,
	kSetVersion,
	kDuplicate,
	kTruncate,
	kDuptrunc,

	/// Asking whether a part the published set expects on this server is still there. It is a
	/// question, so its answer is an observation rather than a completion: a positive one
	/// confirms only what was already expected, and a negative one is the explicit absence that
	/// a fenced reconciliation is allowed to act on.
	kVerifyPart,

	/// Closing status of a client write chain. This metadata server never sent anything, so
	/// there is nothing to correlate it against and only the authority gate applies.
	kWriteEnd,
};

inline const char *chunkCommandFamilyName(ChunkCommandFamily family) {
	switch (family) {
	case ChunkCommandFamily::kCreate: return "create";
	case ChunkCommandFamily::kDelete: return "delete";
	case ChunkCommandFamily::kReplicate: return "replicate";
	case ChunkCommandFamily::kReplicateSource: return "replicate_source";
	case ChunkCommandFamily::kChunkLock: return "chunklock";
	case ChunkCommandFamily::kChunkUnlock: return "chunkunlock";
	case ChunkCommandFamily::kSetVersion: return "setversion";
	case ChunkCommandFamily::kDuplicate: return "duplicate";
	case ChunkCommandFamily::kTruncate: return "truncate";
	case ChunkCommandFamily::kDuptrunc: return "duptrunc";
	case ChunkCommandFamily::kVerifyPart: return "verifypart";
	case ChunkCommandFamily::kWriteEnd: return "writeend";
	}
	return "unknown";
}

/// True for a family this metadata server issues and then waits on, and so one whose answer can
/// be matched back to the admission that authorized it.
///
/// The two exclusions are different things. A write end is a completion the chunkserver raises
/// on its own, authorized by a write grant rather than by a command from here, and a replication
/// source is told to serve a copy and answers nothing. A chunk unlock is a third: this metadata
/// server does send it, and no status ever comes back. Recording an entry for any of them would
/// leave a row nothing can ever remove, and a ledger that only grows is a ledger with a
/// capacity bound it will reach.
inline bool chunkCommandFamilyExpectsReply(ChunkCommandFamily family) {
	return family != ChunkCommandFamily::kWriteEnd &&
	       family != ChunkCommandFamily::kReplicateSource &&
	       family != ChunkCommandFamily::kChunkUnlock;
}

/// Says exactly which command a chunkserver reply is answering.
///
/// A reply that names only a chunk and a command family answers a question that can have more
/// than one asker. Two commands of the same family for one chunk can be outstanding at once, and
/// the only way to tell their answers apart without this is the order they arrive in, which is a
/// guess dressed as a fact: the network is free to reorder them and a slow participant is free to
/// answer the older one last.
///
/// The identity is echoed back unchanged, and the sender requires an exact match on all of it.
/// Each field rules out a different way for a stale answer to look current: @a sequence separates
/// two commands, @a targetStableId and @a targetIncarnation separate two processes that share a
/// name, @a targetServingEra separates two admissions of one process, and @a operationNonce with
/// @a generation separate two rounds of one durable operation.
///
/// @note @a operationNonce and @a generation are carried and matched but are still zero on every
/// path, because the durable operation record that owns those numbers is not yet what drives the
/// send. They are in the format now so that closing that gap costs no format break, and matching
/// zero against zero is exact rather than skipped, so nothing silently accepts a mismatch later.
SAUNAFS_DEFINE_SERIALIZABLE_CLASS(ChunkCommandIdentity,
		uint64_t, operationNonce,
		uint64_t, generation,
		uint64_t, sequence,
		uint64_t, targetIncarnation,
		uint64_t, targetServingEra,
		uint32_t, targetStableId);

inline bool operator==(const ChunkCommandIdentity &first, const ChunkCommandIdentity &second) {
	return first.operationNonce == second.operationNonce && first.generation == second.generation &&
	       first.sequence == second.sequence &&
	       first.targetIncarnation == second.targetIncarnation &&
	       first.targetServingEra == second.targetServingEra &&
	       first.targetStableId == second.targetStableId;
}

inline bool operator!=(const ChunkCommandIdentity &first, const ChunkCommandIdentity &second) {
	return !(first == second);
}
