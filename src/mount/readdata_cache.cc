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

#include "mount/readdata_cache.h"

#include "protocol/SFSCommunication.h"

// ReadCache::Entry implementation

bool ReadCache::Entry::OffsetComp::operator()(Offset offset, const Entry &entry) const {
	return offset < entry.offset;
}

ReadCache::Entry::Entry(Offset offset, Size requested_size)
    : offset(offset),
      buffer(),
      timer(),
      requested_size(requested_size),
      set_member_hook(),
      lru_member_hook() {}

ReadCache::Entry::~Entry() {
	mutex.lock();  // Make helgrind happy
	mutex.unlock();
	pthread_mutex_destroy(mutex.native_handle());
}

bool ReadCache::Entry::operator<(const Entry &other) const { return offset < other.offset; }

bool ReadCache::Entry::expired(uint32_t expiration_time) const {
	return timer.load().elapsed_ms() >= expiration_time;
}

void ReadCache::Entry::reset_timer() {
	timer = Timer();
}

ReadCache::Offset ReadCache::Entry::endOffset() const { return offset + buffer.size(); }

void ReadCache::Entry::acquire() {
	refcount++;
}

void ReadCache::Entry::release() {
	assert(refcount > 0);
	refcount--;
}

// ReadCache::Result implementation

ReadCache::Result::Result() : entries(), is_fake(false) {}

ReadCache::Result::Result(Result &&other) noexcept
    : entries(std::move(other.entries)), is_fake(other.is_fake) {}

ReadCache::Result &ReadCache::Result::operator=(Result &&other) noexcept {
	entries = std::move(other.entries);
	is_fake = other.is_fake;
	return *this;
}

ReadCache::Result::Result(std::vector<uint8_t> &&data) : entries(), is_fake(true) {
	// This is ok, as it is a fake result and we don't need to
	// put it in the cache
	auto *entry = new Entry(0, 0);
	entry->buffer = std::move(data);
	entries.push_back(entry);
}

ReadCache::Result::~Result() {
	if (is_fake) {
		assert(entries.size() == 1);
		delete entries.front();
	} else {
		release();
	}
}

ReadCache::Offset ReadCache::Result::frontOffset() const {
	assert(!entries.empty());
	return entries.front()->offset;
}

ReadCache::Offset ReadCache::Result::remainingOffset() const {
	assert(!entries.empty());
	return entries.back()->offset;
}

ReadCache::Offset ReadCache::Result::endOffset() const {
	assert(!entries.empty());
	return entries.back()->offset + (entries.back()->done
	                                     ? entries.back()->buffer.size()
	                                     : static_cast<size_t>(entries.back()->requested_size));
}

std::vector<uint8_t> &ReadCache::Result::inputBuffer() {
	assert(!entries.empty());
	assert(entries.back()->buffer.empty());
	assert(entries.back()->refcount > 0);
	return entries.back()->buffer;
}

ReadCache::Size ReadCache::Result::copyToBuffer(uint8_t *output, Offset real_offset,
                                                Size real_size) const {
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

		std::memcpy(output, (void *)start, length);
		output += length;
		offset += length;
		bytes_left -= length;
	}
	return offset - real_offset;
}

bool ReadCache::Result::empty() const { return entries.empty(); }

void ReadCache::Result::release() {
	for (auto &entry : entries) {
		std::unique_lock entryLock(entry->mutex);  // Make helgrind happy
		entry->release();
	}
	entries.clear();
}

void ReadCache::Result::add(Entry &entry) {
	entry.acquire();
	assert(entries.empty() || endOffset() >= entry.offset);
	entries.push_back(std::addressof(entry));
}

ReadCache::Size ReadCache::Result::requestSize(Offset real_offset, Size real_size) const {
	if (entries.empty()) { return 0; }
	assert(real_offset >= frontOffset());
	assert(real_offset <= endOffset());
	return std::min<Size>(endOffset() - real_offset, real_size);
}

ReadCache::Entry *ReadCache::Result::back() { return entries.back(); }

std::string ReadCache::Result::toString() const {
	std::string text;
	for (const auto &entry : entries) {
		text += "(" + std::to_string(entry->refcount.load()) + "|" + std::to_string(entry->offset) +
		        ":" + std::to_string(entry->buffer.size()) + "),";
	}
	return text;
}

// ReadCache implementation

ReadCache::ReadCache(uint32_t expiration_time)
    : entries_(), lru_(), reserved_entries_(), expiration_time_(expiration_time) {}

ReadCache::~ReadCache() {
	clear();
	clearReserved(std::numeric_limits<unsigned>::max());
	assert(entries_.empty());
	assert(lru_.empty());
	assert(reserved_entries_.empty());
}

void ReadCache::collectGarbage(unsigned count) {
	unsigned reserved_count = count;
	expiration_time_ = gCacheExpirationTime_ms.load();

	while (!lru_.empty() && count-- > 0) {
		Entry *e = std::addressof(lru_.front());
		if (e->expired(expiration_time_) && e->done) {
			erase(entries_.iterator_to(*e));
		} else {
			break;
		}
	}

	clearReserved(reserved_count);
}

ReadCache::Entry *ReadCache::query(Offset offset, Size size, ReadCache::Result &result,
                                   bool insertPending) {
	collectGarbage();

	auto it = entries_.upper_bound(offset, Entry::OffsetComp());
	if (it != entries_.begin()) { --it; }

	assert(size > 0);

	Size bytes_left = size;
	while (it != entries_.end() && bytes_left > 0) {
		if (offset < it->offset) { break; }

		if (!it->done) {
			++it;
			continue;
		}

		if (it->expired(expiration_time_) || it->buffer.empty()) {
			it = erase(it);
			continue;
		}

		if (offset < it->endOffset()) {
			Size bytes_from_buffer =
			    std::min<Size>(it->buffer.size() - (offset - it->offset), bytes_left);

			bytes_left -= bytes_from_buffer;
			offset += bytes_from_buffer;
			result.add(*it);
		}
		++it;
	}

	if (bytes_left > 0 && insertPending) {
		it = entries_.upper_bound(offset, Entry::OffsetComp());
		if (it != entries_.begin()) { --it; }

		if (it != entries_.end() && it->offset == offset) {
			assert(!it->done);
			it = erase(it);
		}
		auto inserted = insert(it, offset, bytes_left);
		result.add(*inserted);
		return &(*inserted);
	}
	return nullptr;
}

ReadCache::Entry *ReadCache::forceInsert(Offset offset, Size size) {
	collectGarbage();

	auto it = entries_.upper_bound(offset, Entry::OffsetComp());
	if (it != entries_.begin()) { --it; }

	auto inserted = insert(it, offset, size);
	return std::addressof(*inserted);
}

void ReadCache::clear() {
	auto it = entries_.begin();
	while (it != entries_.end()) { it = erase(it); }
}

void ReadCache::selective_clear(uint32_t chunkIndex) {
	// The least starting offset such that the end of the entry could overlap with the chunk
	uint64_t leastOverlappingOffset = 0;
	if (chunkIndex > 0) { leastOverlappingOffset = (chunkIndex - 1) * SFSCHUNKSIZE; }
	uint64_t chunkStart = chunkIndex * SFSCHUNKSIZE;
	uint64_t chunkEnd = chunkStart + SFSCHUNKSIZE;

	// Find the first entry that could overlap with the chunk
	auto it = entries_.begin();
	if (leastOverlappingOffset > 0) {
		it = entries_.upper_bound(leastOverlappingOffset - 1, Entry::OffsetComp());
	}
	while (it != entries_.end()) {
		// If the entry ends before the chunk, skip it
		if (it->endOffset() < chunkStart) {
			++it;
			continue;
		}
		// If it starts after the chunk, stop
		if (it->offset >= chunkEnd) { break; }

		it = erase(it);
	}
}

ReadCache::Entry *ReadCache::find(uint64_t offset) {
	auto it = entries_.upper_bound(offset, Entry::OffsetComp());
	if (it != entries_.begin()) { --it; }

	if (it != entries_.end() && it->offset == offset) {
		if (it->requested_size == 0) {
			it = erase(it);
			return MISSING_OFFSET_PTR;
		}
		return std::addressof(*it);
	}
	return MISSING_OFFSET_PTR;
}

ReadCache::EntrySet::iterator ReadCache::insert(EntrySet::iterator it, Offset offset, Size size) {
	it = clearCollisions(it, offset + size);
	Entry *e = gReadCacheEntriesPool->getEntry(offset, size);
	lru_.push_back(*e);
	assert(entries_.find(*e) == entries_.end());
	return entries_.insert(it, *e);
}

ReadCache::EntrySet::iterator ReadCache::erase(EntrySet::iterator it) {
	assert(it != entries_.end());
	Entry *e = std::addressof(*it);
	auto ret = entries_.erase(it);
	lru_.erase(lru_.iterator_to(*e));
	if (e->refcount > 0) {
		reserved_entries_.push_back(*e);
	} else {
		assert(e->refcount == 0);
		std::unique_lock usedMemoryLock(gReadCacheMemoryMutex);
		decreaseUsedReadCacheMemory(e->buffer.size());
		usedMemoryLock.unlock();

		// If we are almost out of memory, delete the entry instead of putting it back to the pool
		if (gReadCacheMemoryAlmostExceeded) {
			delete e;
		} else {
			gReadCacheEntriesPool->putEntry(e);
		}
	}
	return ret;
}

void ReadCache::clearReserved(unsigned count) {
	std::unique_lock<std::mutex> usedMemoryLock(gReadCacheMemoryMutex, std::defer_lock);
	while (!reserved_entries_.empty() && count-- > 0) {
		Entry *e = std::addressof(reserved_entries_.front());
		if (e->refcount == 0) {
			usedMemoryLock.lock();
			decreaseUsedReadCacheMemory(e->buffer.size());
			usedMemoryLock.unlock();
			reserved_entries_.pop_front();

			// If we are almost out of memory, delete the entry instead of putting it back to the pool
			if (gReadCacheMemoryAlmostExceeded) {
				delete e;
			} else {
				gReadCacheEntriesPool->putEntry(e);
			}
		} else {
			assert(e->refcount >= 0);
			reserved_entries_.splice(reserved_entries_.end(), reserved_entries_,
			                         reserved_entries_.begin());
		}
	}
}

ReadCache::EntrySet::iterator ReadCache::clearCollisions(EntrySet::iterator it,
                                                         Offset start_offset) {
	while (it != entries_.end() && it->offset < start_offset) {
		if (it->done && it->offset + it->requested_size > start_offset) {
			it = erase(it);
		} else {
			it++;
		}
	}
	return it;
}

std::string ReadCache::toString() const {
	std::string text;
	for (const auto &entry : entries_) {
		text += "(" + std::to_string(entry.refcount.load()) + "|" + std::to_string(entry.offset) +
		        ":" + std::to_string(entry.buffer.size()) + "),";
	}
	return text;
}

// ReadCacheEntriesPool implementation

ReadCacheEntriesPool::~ReadCacheEntriesPool() {
	cleanerThread_.request_stop();
	cleanerThread_.join();

	for (auto &pair : entriesMap_) {
		for (auto &bufferEntry : pair.second) { delete bufferEntry.getEntry(); }
	}
}

ReadCache::Entry *ReadCacheEntriesPool::getEntry(size_t offset, size_t size) {
	std::unique_lock<std::mutex> lock(mutex_);

	auto it = entriesMap_.find(size);
	if (it == entriesMap_.end() || it->second.empty()) {
		lock.unlock();

		// No entry found, create a new one
		auto *entry = new ReadCache::Entry(offset, size);
		return entry;
	}

	// Exists and not empty
	auto &buffers = it->second;

	auto *entry = buffers.back().getEntry();
	buffers.pop_back();
	lock.unlock();

	// Reinitialize the entry
	entry->offset = offset;
	entry->buffer.clear();
	entry->timer = Timer();
	entry->refcount = 0;
	entry->requested_size = size;
	entry->done = false;

	return entry;
}

void ReadCacheEntriesPool::putEntry(ReadCache::Entry *entry) {
	assert(entry != nullptr);
	std::unique_lock<std::mutex> lock(mutex_);
	entriesMap_[entry->buffer.size()].emplace_front(entry);
}

void ReadCacheEntriesPool::cleanerThreadFunc_(std::stop_token stopToken) {
	pthread_setname_np(pthread_self(), "RCEP cleaner");

	while (!stopToken.stop_requested()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		std::unique_lock lock(mutex_);
		std::vector<ReadCache::Entry *> entriesToDelete;
		for (auto it = entriesMap_.begin(); it != entriesMap_.end();) {
			auto &buffers = it->second;
			while (!buffers.empty() && buffers.back().expired(maxUnusedTime_ms)) {
				entriesToDelete.push_back(buffers.back().getEntry());
				buffers.pop_back();
			}
			if (buffers.empty()) {
				it = entriesMap_.erase(it);
			} else {
				++it;
			}
		}
		lock.unlock();

		// Delete buffers outside the lock to avoid contention overhead.
		for (auto &entry : entriesToDelete) { delete entry; }
	}
}
