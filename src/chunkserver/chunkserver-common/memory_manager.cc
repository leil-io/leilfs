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
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "common/event_loop.h"
#include "config/cfg.h"
#include "slogger/slogger.h"

// Static members initialization

std::atomic_uint32_t MemoryManager::effectiveInterval_{
    MemoryManager::kDefaultMallocTrimIntervalSeconds};
bool MemoryManager::isThreadRunning_ = false;
std::jthread MemoryManager::trimThread_;
std::condition_variable_any MemoryManager::trimCv_;
std::mutex MemoryManager::trimMutex_;

// MemoryManager functions

void MemoryManager::trimThreadFunc(std::stop_token stoken) {
	pthread_setname_np(pthread_self(), "malloc_trim");

	while (!stoken.stop_requested()) {
		uint32_t interval = MemoryManager::effectiveInterval();

		// If interval is 0, the feature is disabled, exit the thread
		if (interval == kDisableTrimmingInterval) {
			break;
		}

		// Wait for the interval or until stop is requested or config changes (reload)
		std::unique_lock lock(MemoryManager::trimMutex_);
		bool timeout = !MemoryManager::trimCv_.wait_for(lock, std::chrono::seconds(interval), [&] {
			return stoken.stop_requested() || MemoryManager::effectiveInterval() != interval;
		});

		if (stoken.stop_requested()) { break; }

		// Trim if we timed out OR if this is the initial run after config change
		if (timeout) { MemoryManager::trimMemory(); }
	}
}

void MemoryManager::trimMemory() {
#if defined(SAUNAFS_HAVE_MALLOC_TRIM)
	auto start = std::chrono::high_resolution_clock::now();
	int result = malloc_trim(0);
	auto end = std::chrono::high_resolution_clock::now();

	auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

	if (result == 1) {
		safs::log_info("Memory trimmed successfully in {} us.", duration.count());
	} else {
		safs::log_info("Memory trim returned {} (0 means no memory was trimmed).", result);
	}
#else
	// malloc_trim() is a glibc-specific extension (e.g. absent on musl libc /
	// Alpine Linux). There is no portable equivalent, so trimming is simply
	// unavailable here; the background thread still runs but has nothing to do.
	safs::log_info(
	    "Memory trim requested, but malloc_trim() is not available on this "
	    "platform (non-glibc malloc); skipping.");
#endif
}

void MemoryManager::reload() {
	auto interval = cfg_getuint32("MALLOC_TRIM_INTERVAL", kDefaultMallocTrimIntervalSeconds);

	// interval == 0 here will mean to disable the trimming, including the thread
	uint32_t currentInterval = effectiveInterval_.load();
	bool wasThreadRunning = isThreadRunning_;

	if (interval == currentInterval && wasThreadRunning == (interval > 0)) {
		return;
	}

	effectiveInterval_.store(interval, std::memory_order_relaxed);

	// Handle thread lifecycle based on interval
	if (interval == kDisableTrimmingInterval) {
		// Stop the thread if it's running
		if (isThreadRunning_ && trimThread_.joinable()) {
			trimThread_.request_stop();
			trimCv_.notify_all();
			trimThread_.join();
			isThreadRunning_ = false;
			safs::log_info("Memory trimming disabled - thread stopped");
		}
	} else {
		// Start the thread if it's not running
		if (!isThreadRunning_) {
			trimThread_ = std::jthread(MemoryManager::trimThreadFunc);
			isThreadRunning_ = true;
			safs::log_info("Memory trimming enabled - thread started");
		} else {
			// Thread is already running, just notify about interval change
			trimCv_.notify_all();
		}
	}

	if (interval > kDisableTrimmingInterval) {
		safs::log_info("Effective MALLOC_TRIM_INTERVAL: {} seconds", interval);
	}
}

int MemoryManager::init() {
	reload();
	eventloop_reloadregister(MemoryManager::reload);
	eventloop_destructregister(MemoryManager::shutdown);

	return 0;
}

void MemoryManager::shutdown() {
	if (trimThread_.joinable()) {
		trimThread_.request_stop();
		trimCv_.notify_all();
		trimThread_.join();
		isThreadRunning_ = false;
	}
}
