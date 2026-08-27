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

#include "common/session_authority_clock.h"

#include <atomic>
#include <ctime>

#include "common/test_event_stream.h"
#include "slogger/slogger.h"

namespace session_authority_clock {
namespace {

std::atomic<int64_t> gOffsetSeconds{0};

}  // namespace

std::optional<uint64_t> now() {
	const time_t wall = ::time(nullptr);
	if (wall < 0) { return std::nullopt; }

	int64_t logical = 0;
	if (__builtin_add_overflow(static_cast<int64_t>(wall),
	                           gOffsetSeconds.load(std::memory_order_relaxed), &logical) ||
	    logical < 0) {
		return std::nullopt;
	}
	return static_cast<uint64_t>(logical);
}

void configure(bool testClocksEnabled, int64_t offsetSeconds) {
	if (offsetSeconds != 0 && !testClocksEnabled) {
		safs::log_err(
		    "session authority clock: nonzero test offset refused without TEST_AUTHORITY_CLOCKS");
		offsetSeconds = 0;
	}
	const int64_t oldOffset = gOffsetSeconds.exchange(offsetSeconds, std::memory_order_relaxed);
	if (oldOffset != offsetSeconds) {
		safs::log_info("session authority clock: offset changed from {} to {} seconds", oldOffset,
		               offsetSeconds);
	}
	if (test_event_stream::enabled()) {
		const auto logicalNow = now();
		test_event_stream::emit("authority_clock_applied",
		                        {{"clock", "session"},
		                         {"old_offset", oldOffset},
		                         {"new_offset", offsetSeconds},
		                         {"now", logicalNow.value_or(0)}});
	}
}

int64_t currentOffset() { return gOffsetSeconds.load(std::memory_order_relaxed); }

}  // namespace session_authority_clock
