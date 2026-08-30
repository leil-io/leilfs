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

/// The per-chunk operation generation fence.
///
/// A fenced command carries the generation of the durable operation round that issued it, and
/// generations for one chunk only rise: every acquire and takeover of the round advances the
/// stored number, whichever metadata server does it. So a command whose generation is below the
/// highest this process has already seen for that chunk was issued by a round the cluster has
/// moved past. Executing it would rewrite state a newer round already owns; the classic shape is
/// a slow set-version from a superseded owner arriving over its own healthy connection after the
/// new owner's command, where every session check passes and only the generation says stale.
///
/// The fence is volatile and process wide, shared by every metadata server connection, because
/// the reorder it refuses happens across connections. A restart empties it safely: commands
/// addressed to the previous incarnation die on the incarnation check, and ordering among rounds
/// admitted after readmission rebuilds from their own generations. Generation zero bypasses the
/// fence and never disturbs it: it marks a command no round issued, and the legacy plane.
namespace chunk_generation_fence {

/// Admits or refuses @p generation for @p chunkId, advancing the recorded high water on
/// admission. Equal generations admit: one round retries its own command. @p stage names the
/// boundary for the test event stream ("accept" or "execute"); refusals emit
/// fenced_generation_refused and a full table emits fence_table_full. Fail closed when the
/// table cannot hold a new chunk: an untracked generation cannot be ordered, so it is refused.
bool admit(uint64_t chunkId, uint64_t generation, const char *stage);

/// Read-only check: whether @p generation for @p chunkId has already been overtaken by a higher
/// generation admitted for that chunk, meaning a newer round has fenced it. Records and advances
/// nothing, so a data frame can be checked on every arrival without disturbing the table. A chunk
/// with no recorded high water is not superseded, and generation zero (the unfenced legacy plane)
/// is never superseded.
bool isSuperseded(uint64_t chunkId, uint64_t generation);

/// Test support: empties the table and restores the default capacity.
void resetForTest();

/// Test support: shrinks the table capacity so the bounded refusal is reachable.
void setCapacityForTest(size_t capacity);

}  // namespace chunk_generation_fence
