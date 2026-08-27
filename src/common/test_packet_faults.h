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
#include <string>

/// The H4 per packet fault seam: an opt in way to drop, duplicate or delay one named packet
/// type on a live connection, leaving every other packet on that connection alone. A proxy
/// between the two processes cannot do that without reimplementing the protocol, and the
/// session slices need exactly it: one nomination reply dropped, one lease tuple duplicated,
/// one outcome held back so another overtakes it.
///
/// The decision lives here so it can be tested as pure logic; the mechanism lives at the two
/// packet dispatch points, one per side. Off by default and fail closed: a spec is honored
/// only when the explicit test gate is also set.
namespace test_packet_faults {

/// What the dispatch point must do with the packet it is holding.
enum class Verdict : uint8_t {
	kPass,       ///< nothing armed for this packet
	kDrop,       ///< discard it as if the network lost it
	kDuplicate,  ///< dispatch it twice
	kDelay,      ///< hold it for the returned number of milliseconds, then dispatch
};

/// Applies the configuration; reloadable while the process lives. The spec is a semicolon
/// separated list of `action:type:count[:milliseconds]`, where action is drop, duplicate or
/// delay, type is a decimal packet type or `*` for any, and count is how many matching
/// packets the rule consumes before it retires (0 means it never retires). A nonempty spec
/// is honored only when @p testFaultsEnabled; otherwise it is refused and nothing is armed.
void configure(bool testFaultsEnabled, const std::string &spec);

/// True when at least one rule is still armed; lets dispatch points skip the lookup.
bool enabled();

/// Consumes at most one rule matching @p packetType and returns what to do. @p stage names
/// the dispatch point for the event record. Emits the H9 packet_fault_applied event on every
/// verdict other than kPass, so a harness waits for the fault instead of assuming it landed.
Verdict decide(const char *stage, uint32_t packetType, uint32_t *delayMilliseconds);

}  // namespace test_packet_faults
