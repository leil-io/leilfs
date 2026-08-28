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

#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/evidence_item.h"

/// The durable local outbox behind the acknowledged evidence channel.
///
/// A one-shot observation is appended here before anything tries to deliver it, and it leaves
/// only when a metadata server acknowledges committing it. Sends never consume: the drain reads
/// the unacknowledged tail and the same items go out again next tick until the acknowledgement
/// names their sequences, so a dropped packet, a dead connection, a metadata server failure or a
/// chunkserver restart all end in replay rather than loss. Sequences are one monotonic space per
/// stable identity, persisted across restarts, so the receiver's single committed position
/// deduplicates every replay; the items keep the incarnation that made them, and a statement
/// from an earlier process start is never a statement about the present.
///
/// The file lives beside the stable id file in the chunkserver's data directory and is rewritten
/// whole with the write-fsync-rename idiom on every change; the capacity bound fails admission
/// visibly rather than discarding a correctness-bearing item.
namespace evidence_outbox {

/// Loads the outbox from the working directory (the daemon's data path). Called once at start,
/// after the process has changed into its data directory.
void init();

/// Appends one observation, assigning its sequence. @p item's sequence field is ignored.
/// Returns false and changes nothing when the outbox is full; the refusal is the visible
/// admission stop the design requires, never a silent discard.
bool append(EvidenceItem item);

/// The unacknowledged tail, oldest first, at most @p limit items. Never consumes.
std::vector<EvidenceItem> unacked(size_t limit);

/// Drops every item with sequence <= @p upToSequence and persists the remainder.
void ack(uint64_t upToSequence);

/// Test support: pending item count.
size_t pendingCount();

}  // namespace evidence_outbox
