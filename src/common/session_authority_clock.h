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

/// The H5 session authority clock: the one clock consulted where an absolute durable
/// session deadline is compared (chunkserver-session claims on every daemon). It never
/// feeds event-loop timers, socket timeouts, reconnect backoff or heartbeats; those stay
/// on real time so an injected offset cannot fake a cutoff by disconnecting a peer.
namespace session_authority_clock {

/// Wall-clock seconds plus the injected test offset. nullopt when the sum leaves the
/// unsigned storage domain (or the wall clock itself is broken); the caller must fail
/// the affected authority decision closed.
std::optional<uint64_t> now();

/// Applies the test-clock configuration; reloadable while the process lives. A nonzero
/// offset is honored only when the explicit test gate is set; otherwise it is refused
/// and the offset stays zero. Emits the H9 authority_clock_applied event (clock kind,
/// old offset, new offset, resulting logical now) whenever the event stream is enabled,
/// so a harness can wait for the application instead of inferring it from a reload.
void configure(bool testClocksEnabled, int64_t offsetSeconds);

/// The currently applied offset (diagnostics and tests).
int64_t currentOffset();

}  // namespace session_authority_clock
