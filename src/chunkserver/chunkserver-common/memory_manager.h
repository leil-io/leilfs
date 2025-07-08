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

#pragma once

#include "common/platform.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

using EventLoopTimerHandle = void *;

/// \brief MemoryManager class handles memory management tasks.
///  The initial implementation focuses on trimming unused memory and managing the trim interval.
class MemoryManager {
public:
	/// Does the actual memory trimming.
	/// This function is registered in the event loop and called periodically.
	static void trimMemory();

	/// Reloads the memory trim interval from configuration and updates the event loop registration.
	static void reload();

	/// RunTab for the init.h file
	static int init();

	/// For clean shutdown
	static void shutdown();

	static uint32_t effectiveInterval() {
		return effectiveInterval_.load(std::memory_order_relaxed);
	}

private:
	/// Background thread function for memory trimming
	static void trimThreadFunc(std::stop_token stoken);

	static constexpr uint32_t kMinMallocTrimIntervalSeconds = 1;
	static constexpr uint32_t kDefaultMallocTrimIntervalSeconds = 3600;

	static bool initialized_;  ///< Whether the trimMemory function was registered in the event loop
	static EventLoopTimerHandle eventLoopHandle_;  ///< Handle for the registered function

	static std::atomic_uint32_t effectiveInterval_;  ///< Interval in seconds for malloc_trim

	// Threading
	static std::jthread trimThread_;
	static std::condition_variable_any trimCv_;
	static std::mutex trimMutex_;
};
