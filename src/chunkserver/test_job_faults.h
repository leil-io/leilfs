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

/// The H6 seam: holds a client facing job inside the worker pool, between the moment it was
/// admitted and the moment it touches a disk.
///
/// That window is where the drain question lives. Every gate this chunkserver has refuses work
/// at its entrance, which says nothing about work already inside; and work already inside is
/// exactly what the drain component of the cutoff reserve is a promise about. On a ramdisk a
/// read finishes in less time than it takes to move a clock, so without a way to hold one open
/// the window cannot be observed at all, and a test that tried would be racing rather than
/// asserting.
///
/// Off by default and fail closed: a spec is honored only when the explicit test gate is set.
namespace test_job_faults {

/// Applies the configuration; reloadable. The spec is `count:milliseconds`, where count is how
/// many jobs the rule holds before it retires. A nonempty spec is honored only when
/// @p testFaultsEnabled; otherwise it is refused and nothing is armed.
void configure(bool testFaultsEnabled, const std::string &spec);

/// The same seam for jobs raised by a metadata server command rather than by a client.
///
/// They need their own rule because they need their own window. A client connection is closed
/// after ten idle seconds and a mount gives up sooner than that, so a client job cannot be held
/// across a cutoff and a readmission without the connection dying first and taking the boundary
/// with it. A metadata server keeps its connection alive, which makes the control plane the
/// place where "the work finished under an authority that has since been replaced" can actually
/// be observed.
void configureMaster(bool testFaultsEnabled, const std::string &spec);

/// True while either rule is still armed; lets the worker skip the call entirely.
bool enabled();

/// Holds the calling worker if a rule is armed, spending one. Emits the H9 job_hold_started and
/// job_hold_finished events, so a run waits for the window to open instead of guessing at it.
/// @p clientFacing selects which rule applies, so a run can hold one plane and leave the other
/// running.
void holdIfArmed(uint32_t jobId, bool clientFacing);

}  // namespace test_job_faults
