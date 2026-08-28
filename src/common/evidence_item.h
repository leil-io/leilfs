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

/// What one-shot observation an evidence item states.
enum class EvidenceItemKind : uint8_t {
	kLost = 1,
};

/// One chunkserver observation as the acknowledged evidence channel carries it.
///
/// The statement is made exactly once and must survive everything between the process that made
/// it and the durable record: the item is retained in the source's durable outbox under its
/// @a sequence until a metadata server acknowledges committing it. The origin fields are a
/// non-authorizing identity: they say which process start and which admitted claim the statement
/// came from, so a replayed item from an earlier incarnation is stored as history rather than
/// treated as a statement about the present. The claim token never travels in the item; the
/// connection that carries it is what the token authenticates.
SAUNAFS_DEFINE_SERIALIZABLE_CLASS(EvidenceItem, uint64_t, sequence, uint64_t, incarnation, uint64_t,
                                  originClaimSequence, uint64_t, scanEpoch, uint8_t, kind, uint64_t,
                                  chunkId, uint16_t, partType, uint64_t, observedAtMs);
