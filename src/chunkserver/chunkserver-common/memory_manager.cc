/*
   Copyright 2023      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include "chunkserver-common/memory_manager.h"

#include <malloc.h>

#include "common/event_loop.h"
#include "config/cfg.h"
#include "slogger/slogger.h"

// Static members initialization

std::atomic_uint32_t MemoryManager::effectiveInterval_{
    MemoryManager::kDefaultMallocTrimIntervalSeconds};

bool MemoryManager::initialized_ = false;

EventLoopTimerHandle MemoryManager::eventLoopHandle_ = nullptr;

// MemoryManager functions

void MemoryManager::trimMemory() {
	if (malloc_trim(0) == 1) {
		safs::log_info("Memory trimmed successfully.");
	}  // 0 is not an error, it just means no memory was trimmed
}

void MemoryManager::reload() {
	auto interval = cfg_get_minvalue<uint32_t>(
	    "MALLOC_TRIM_INTERVAL", kDefaultMallocTrimIntervalSeconds, kMinMallocTrimIntervalSeconds);

	// No change
	if (interval == effectiveInterval_.load() && initialized_) { return; }

	effectiveInterval_.store(interval, std::memory_order_relaxed);

	if (initialized_) {
		eventloop_timechange(eventLoopHandle_, TIMEMODE_RUN_LATE, interval, 0);
	} else {
		eventLoopHandle_ = eventloop_timeregister(TIMEMODE_RUN_LATE, interval, 0,
		                                          []() { MemoryManager::trimMemory(); });

		if (eventLoopHandle_ == nullptr) {
			safs::log_err("Failed to register memory trim event loop handler.");
			return;
		}
	}

	safs::log_info("Effective MALLOC_TRIM_INTERVAL: {} seconds", interval);

	if (!initialized_) { initialized_ = true; }
}

int MemoryManager::init() {
	reload();

	eventloop_reloadregister(MemoryManager::reload);

	return 0;
}
