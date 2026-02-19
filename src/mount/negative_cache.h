/*

   Copyright 2026 Leil Storage OÜ

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

#include <atomic>
#include <cstdint>
#include <list>
#include <mutex>
#include <unordered_map>

#include "common/hashfn.h"
#include "common/shared_mutex.h"
#include "common/type_defs.h"
#include "common/time_utils.h"

inline std::atomic<uint32_t> gNegativeCacheTimeoutMs;
inline std::atomic<uint32_t> gNegativeCacheMaxSize;

class NegativeCache {
public:
    NegativeCache() = default;

	// To prevent accidental copies of the lock/map/list
    NegativeCache(const NegativeCache&) = delete;
    NegativeCache& operator=(const NegativeCache&) = delete;
    NegativeCache(NegativeCache&&) = delete;
    NegativeCache& operator=(NegativeCache&&) = delete;

	// Atomic access
	static uint32_t getGlobalMaxSize() noexcept {
		return gNegativeCacheMaxSize.load();
	}

	// Atomic access
	static void setGlobalMaxSize(uint32_t maxSize) noexcept {
		gNegativeCacheMaxSize.store(maxSize);
	}

	// Locked
    // Reserve enough buckets without the map needing to rehash
    void setMaxSize(uint32_t maxSize) {
		std::unique_lock<shared_mutex> lock(rwLock_);
        maxSize_ = maxSize;
		negativeCache_.reserve(maxSize_);
	}

	// Atomic access
	static uint32_t getGlobalTimeoutMs() noexcept {
		return gNegativeCacheTimeoutMs.load();
	}

	// Atomic access
	static void setGlobalTimeoutMs(uint32_t timeoutMs) noexcept {
		gNegativeCacheTimeoutMs.store(timeoutMs);
	}

	// Locked
	void setTimeoutMs(uint32_t timeoutMs) {
		std::unique_lock<shared_mutex> lock(rwLock_);
		timeoutMs_ = timeoutMs;
	}

	// Locked
	size_t size() const {
		shared_lock<shared_mutex> lock(rwLock_);
		return negativeCache_.size();
	}

	// Locked
	bool isMaxSizeAndTimeoutMsSet() const {
		shared_lock<shared_mutex> lock(rwLock_);
		return (maxSize_ > 0) && (timeoutMs_ > 0);
	}

	// Locked
	// Works only if max size and timeout are set
	bool lookup(inode_t parent, std::string_view name) const {
		if (!isMaxSizeAndTimeoutMsSet()) { return false; }

		shared_lock<shared_mutex> lock(rwLock_);
		auto it = negativeCache_.find(InodeNameLookupPair{ parent, name });
		if (it != negativeCache_.end()) {
			NegativeCacheEntry entry = it->second;
			if (static_cast<uint64_t>(timer_.elapsed_ms()) < entry.timeoutMs) {
				return true;
			} // else ignore expired
		}
		return false;
	}

	// Locked, LRU eviction policy, entries refreshed
	// Works only if max size and timeout are set
	void add(inode_t parent, std::string_view name) {
		if (!isMaxSizeAndTimeoutMsSet()) { return; }

		std::unique_lock<shared_mutex> lock(rwLock_);

		// sfsnegativecachetimeout and sfsnegativecachesize
		// are not changed in Tweaks frequently, so it is okay
		// for consistency
		bool clearCache = false;
		uint32_t globalTimeoutMs = getGlobalTimeoutMs();
		if (timeoutMs_ != globalTimeoutMs) {
			timeoutMs_ = globalTimeoutMs;
			clearCache = true;
		}
		uint32_t globalMaxSize = getGlobalMaxSize();
		if (maxSize_ != globalMaxSize) {
			maxSize_ = globalMaxSize;
			negativeCache_.reserve(maxSize_); // doesn't shrink
			clearCache = true;
		}
		if (clearCache) {
			negativeCache_.clear();
			timeoutList_.clear();
		}

		InodeNamePairPtr inodeNamePtr;
		
		// Update existing
		auto it = negativeCache_.find(InodeNameLookupPair{ parent, name });
		if (it != negativeCache_.end()) {
			inodeNamePtr = it->first;
			timeoutList_.erase(it->second.listIt);
		} else {
			if (negativeCache_.size() >= maxSize_) {
				// Remove oldest
				negativeCache_.erase(timeoutList_.front());
				timeoutList_.pop_front();
			}
			inodeNamePtr = std::make_shared<const InodeNamePair>(parent, name);
		}
		timeoutList_.push_back(inodeNamePtr);
		
		// Update timeout if the same parent/name
		negativeCache_.insert_or_assign(inodeNamePtr, NegativeCacheEntry{
			.timeoutMs = static_cast<uint64_t>(timer_.elapsed_ms()) + timeoutMs_,
			.listIt = std::prev(timeoutList_.end())
		});
	}

private:
	using InodeNamePair = std::pair<inode_t, std::string>;
	using InodeNamePairPtr = std::shared_ptr<const InodeNamePair>;
	using InodeNameLookupPair = std::pair<inode_t, std::string_view>;

	struct NegativeCacheEntry {
		uint64_t timeoutMs;
		std::list<InodeNamePairPtr>::iterator listIt;
	};

	struct InodeNameHash {
		using is_transparent = void; // zero allocation for string

		static uint64_t hashCompute(inode_t parent, std::string_view name) noexcept {
			uint64_t seed = 0;
			hashCombine(seed, parent, ByteArray(name.data(), name.size()));
			return seed;
		}

		// Storage hash calculation
		size_t operator()(const InodeNamePairPtr& obj) const noexcept {
			return hashCompute(obj->first, obj->second);
		}

		// Lookup hash calculation
		size_t operator()(const InodeNameLookupPair& obj) const noexcept {
			return hashCompute(obj.first, obj.second);
		}
	};

	struct InodeNameEqual {
		using is_transparent = void; // zero allocation for string

		// Compare two stored pairs
		bool operator()(const InodeNamePairPtr& lsp,
						const InodeNamePairPtr& rsp) const noexcept {
			return lsp->first == rsp->first && lsp->second == rsp->second;
		}

		// Compare stored pair against lookup pair
		bool operator()(const InodeNamePairPtr& lsp,
						const InodeNameLookupPair& rsp) const noexcept {
			return lsp->first == rsp.first && lsp->second == rsp.second;
		}

		bool operator()(const InodeNameLookupPair& lsp,
						const InodeNamePairPtr& rsp) const noexcept {
			return lsp.first == rsp->first && lsp.second == rsp->second;
		}
	};

	mutable shared_mutex rwLock_;
	std::unordered_map<
		InodeNamePairPtr,
		NegativeCacheEntry,
		InodeNameHash,
		InodeNameEqual
	> negativeCache_;
	Timer timer_;
    size_t maxSize_;
	uint32_t timeoutMs_;
	std::list<InodeNamePairPtr> timeoutList_;
};

inline NegativeCache gNegativeCache;
