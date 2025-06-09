/*

   Copyright 2016 Skytechnology sp. z o.o.
   Copyright 2023 Leil Storage OÜ

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

#include "common/small_vector.h"
#include "common/time_utils.h"

#include <atomic>
#include <cassert>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <boost/intrusive/list.hpp>
#include <boost/intrusive/set.hpp>

#define MISSING_OFFSET_PTR nullptr

inline std::atomic<uint64_t> gReadCacheMaxSize;
inline std::mutex gReadCacheMemoryMutex;
inline uint64_t gUsedReadCacheMemory;
inline std::atomic<bool> gReadCacheMemoryAlmostExceeded = false;
inline std::atomic<uint32_t> gCacheExpirationTime_ms;

constexpr double kReadCacheThreshold = 0.8;

inline void updateReadCacheMemoryAlmostExceeded() {
	gReadCacheMemoryAlmostExceeded =
	    gUsedReadCacheMemory >=
	    static_cast<uint64_t>(kReadCacheThreshold * gReadCacheMaxSize.load());
}

inline void increaseUsedReadCacheMemory(size_t bytesToReadLeft) {
	gUsedReadCacheMemory += bytesToReadLeft;
	updateReadCacheMemoryAlmostExceeded();
}

inline void decreaseUsedReadCacheMemory(size_t bytesToReadLeft) {
	gUsedReadCacheMemory -= bytesToReadLeft;
	updateReadCacheMemoryAlmostExceeded();
}

class ReadCache {
public:
	using Offset = uint64_t;
	using Size = uint32_t;

	struct Entry {
		Offset offset;
		std::vector<uint8_t> buffer;
		std::atomic<Timer> timer;
		std::atomic<int> refcount = 0;
		std::atomic<Size> requested_size;
		std::atomic<bool> done = false;
		boost::intrusive::set_member_hook<> set_member_hook;
		boost::intrusive::list_member_hook<> lru_member_hook;
		boost::intrusive::list_member_hook<> reserved_member_hook;
		std::mutex mutex;

		struct OffsetComp {
			bool operator()(Offset offset, const Entry &entry) const;
		};

		Entry(Offset offset, Size requested_size);

		~Entry();

		bool operator<(const Entry &other) const;

		bool expired(uint32_t expiration_time) const ;

		void reset_timer();

		Offset endOffset() const;

		void acquire();

		void release();
	};

	using EntrySet = boost::intrusive::set<
	    Entry, boost::intrusive::member_hook<Entry, boost::intrusive::set_member_hook<>,
	                                         &Entry::set_member_hook>>;
	using EntryList = boost::intrusive::list<
	    Entry, boost::intrusive::member_hook<Entry, boost::intrusive::list_member_hook<>,
	                                         &Entry::lru_member_hook>>;
	using ReservedEntryList = boost::intrusive::list<
	    Entry, boost::intrusive::member_hook<Entry, boost::intrusive::list_member_hook<>,
	                                         &Entry::reserved_member_hook>>;

	struct Result {
		constexpr static size_t kResultEntriesSize = 8;
		small_vector<Entry *, kResultEntriesSize> entries;
		bool is_fake;

		Result();
		Result(Result &&other) noexcept;

		Result &operator=(Result &&other) noexcept;

		// Wrapper for returning data not really residing in cache
		Result(std::vector<uint8_t> &&data);

		~Result();

		Offset frontOffset() const;

		Offset remainingOffset() const;

		Offset endOffset() const;

		/*!
		 * \brief Give access to a buffer which should be filled with data.
		 *
		 * If cache result is incomplete (i.e. some data should be read to fulfill the request),
		 * it should be read into this buffer in order to write it straight to cache.
		 */
		std::vector<uint8_t> &inputBuffer();

		/*!
		 * \brief Serialize cache query result to an iovector.
		 *
		 * An iovector can be any structure that accepts pushing back
		 * a pair of {address, length}, which represents a consecutive array
		 * of bytes extracted from cache.
		 *
		 * \return number of bytes added to iovector
		 */
		template <typename IoVec>
		Size toIoVec(IoVec &output, Offset real_offset, Size real_size) const
		{
			assert(real_offset >= frontOffset());
			uint64_t offset = real_offset;
			Size bytes_left = real_size;
			for (const auto &entry_ptr : entries) {
				const ReadCache::Entry &entry = *entry_ptr;
				if (bytes_left <= 0) { break; }
				// Special case: Read request was past the end of the file
				if (entry.buffer.empty() || offset >= entry.endOffset()) { break; }
				assert(offset >= entry.offset && offset < entry.endOffset());
				const auto *start = entry.buffer.data() + (offset - entry.offset);
				const auto *end = std::min(start + bytes_left, entry.buffer.data() + entry.buffer.size());
				assert(start < end);
				size_t length = std::distance(start, end);

				output.push_back({(void *)start, length});
				offset += length;
				bytes_left -= length;
			}
			return offset - real_offset;
		}

		Size copyToBuffer(uint8_t *output, Offset real_offset, Size real_size) const;

		bool empty() const;

		void release();

		void add(Entry &entry);

		Size requestSize(Offset real_offset, Size real_size) const;

		Entry *back();

		std::string toString() const;
	};

	explicit ReadCache(uint32_t expiration_time);

	~ReadCache();

	void collectGarbage(unsigned count = kCollectGarbageCountDefault_);

	/*!
	 * \brief Try to get data from cache.
	 *
	 * If all data is available in cache, it can be obtained from result
	 * as an iovector via result.toIoVec() call.
	 * If some or no data is available, the rest should be read into the result buffer
	 * via result.inputBuffer(). Then, it can be obtain as an iovector via result.toIoVec().
	 *
	 * \return cache query result
	 */
	Entry *query(Offset offset, Size size, ReadCache::Result &result, bool insertPending = true);

	Entry *forceInsert(Offset offset, Size size);

	void clear();

	void selective_clear(uint32_t chunkIndex);

	Entry *find(uint64_t offset);

protected:
	EntrySet::iterator insert(EntrySet::iterator it, Offset offset, Size size);

	EntrySet::iterator erase(EntrySet::iterator it);

	void clearReserved(unsigned count);

	EntrySet::iterator clearCollisions(EntrySet::iterator it, Offset start_offset);

	std::string toString() const;

private:
	EntrySet entries_;
	EntryList lru_;
	ReservedEntryList reserved_entries_;
	uint32_t expiration_time_;
	static constexpr size_t kCollectGarbageCountDefault_ = 100000;
};

class ReadCacheEntriesPool {
public:
	ReadCacheEntriesPool(const ReadCacheEntriesPool &) = delete;
	ReadCacheEntriesPool &operator=(const ReadCacheEntriesPool &) = delete;
	ReadCacheEntriesPool(ReadCacheEntriesPool &&) = delete;
	ReadCacheEntriesPool &operator=(ReadCacheEntriesPool &&) = delete;

	ReadCacheEntriesPool() {
		cleanerThread_ =
		    std::jthread([this](std::stop_token stopToken) { cleanerThreadFunc_(stopToken); });
	}

	~ReadCacheEntriesPool();

	ReadCache::Entry *getEntry(size_t offset, size_t size);

	void putEntry(ReadCache::Entry *entry);

private:
	void cleanerThreadFunc_(std::stop_token stopToken);

	struct BufferEntry {
		ReadCache::Entry *entry_;
		Timer timer_;

		BufferEntry(ReadCache::Entry *entry) : entry_(entry) {}

		BufferEntry(BufferEntry &&other) noexcept
		    : entry_(other.entry_), timer_(std::move(other.timer_)) {
			other.entry_ = nullptr;
		}

		~BufferEntry() = default;

		bool expired(uint32_t expiration_time) const {
			return timer_.elapsed_ms() >= expiration_time;
		}

		ReadCache::Entry *getEntry() const { return entry_; }
	};

	static constexpr uint32_t kMaxUnusedTime_ms = 1000 * 10;  // 10 seconds
	std::jthread cleanerThread_;
	std::unordered_map<size_t, std::deque<BufferEntry>> entriesMap_;
	std::mutex mutex_;
};

inline std::unique_ptr<ReadCacheEntriesPool> gReadCacheEntriesPool;
