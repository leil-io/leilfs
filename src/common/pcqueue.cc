/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"
#include "pcqueue.h"

#include <cerrno>
#include <cstdlib>
#include <pthread.h>
#include "devtools/TracePrinter.h"
#include "slogger/slogger.h"

ProducerConsumerQueueWithPriority::ProducerConsumerQueueWithPriority(uint8_t priorityLevels,
                                                                     uint32_t maxSize,
                                                                     Deleter deleter)
    : maxSize_(maxSize), currentElements_(0), currentSize_(0), deleter_(deleter) {
	if (priorityLevels == 0) {
		safs::log_info(
		    "ProducerConsumerQueueWithPriority::{}: Given priorityLevels is 0, using 1 instead",
		    __func__);
		priorityLevels = 1;
	}
	queuesByPriority_.reserve(priorityLevels);
	queuesByPriority_.resize(priorityLevels);
	TRACETHIS();
}

ProducerConsumerQueueWithPriority::~ProducerConsumerQueueWithPriority() {
	TRACETHIS();
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &queue : queuesByPriority_) {
		while (!queue.empty()) {
			deleter_(queue.front().data);
			queue.pop();
		}
	}
}

bool ProducerConsumerQueueWithPriority::isEmpty() const {
	TRACETHIS();
	std::lock_guard<std::mutex> lock(mutex_);
	return currentSize_ == 0;
}

bool ProducerConsumerQueueWithPriority::isFull() const {
	TRACETHIS();
	std::lock_guard<std::mutex> lock(mutex_);
	return maxSize_ > 0 && currentSize_ >= maxSize_;
}

uint32_t ProducerConsumerQueueWithPriority::sizeLeft() const {
	TRACETHIS();
	std::lock_guard<std::mutex> lock(mutex_);
	return maxSize_ > 0 ? (currentSize_ <= maxSize_ ? maxSize_ - currentSize_ : 0) : UINT32_MAX;
}

uint32_t ProducerConsumerQueueWithPriority::elements() const {
	TRACETHIS();
	std::lock_guard<std::mutex> lock(mutex_);
	return currentElements_;
}

void ProducerConsumerQueueWithPriority::put(uint32_t jobId, uint32_t jobType, uint8_t *data,
                                            uint32_t length, uint8_t priority) {
	TRACETHIS();
	std::unique_lock<std::mutex> lock(mutex_);
	put_(jobId, jobType, data, length, priority);

	notEmpty_.notify_one();
}

bool ProducerConsumerQueueWithPriority::tryPut(uint32_t jobId, uint32_t jobType, uint8_t *data,
                                               uint32_t length, uint8_t priority) {
	TRACETHIS();
	std::lock_guard<std::mutex> lock(mutex_);
	if (maxSize_ > 0) {
		if (length > maxSize_) {
			errno = EDEADLK;
			return false;
		}

		if (currentSize_ + length > maxSize_) {
			errno = EBUSY;
			return false;
		}
	}

	put_(jobId, jobType, data, length, priority);

	notEmpty_.notify_one();
	return true;
}

void ProducerConsumerQueueWithPriority::get(uint32_t *jobId, uint32_t *jobType, uint8_t **data,
                                            uint32_t *length) {
	TRACETHIS();
	std::unique_lock<std::mutex> lock(mutex_);
	notEmpty_.wait(lock, [this] { return currentSize_ > 0; });

	get_(jobId, jobType, data, length);
}

bool ProducerConsumerQueueWithPriority::tryGet(uint32_t *jobId, uint32_t *jobType, uint8_t **data,
                                               uint32_t *length) {
	TRACETHIS();
	std::lock_guard<std::mutex> lock(mutex_);
	if (currentSize_ == 0) {
		if (jobId) { *jobId = 0; }
		if (jobType) { *jobType = 0; }
		if (data) { *data = nullptr; }
		if (length) { *length = 0; }
		errno = EBUSY;
		return false;
	}

	get_(jobId, jobType, data, length);
	return true;
}

void ProducerConsumerQueueWithPriority::put_(uint32_t jobId, uint32_t jobType, uint8_t *data,
                                             uint32_t length, uint8_t priority) {
	assert(queuesByPriority_.size() > 0);
	if (priority >= queuesByPriority_.size()) {
		safs::log_info(
		    "ProducerConsumerQueueWithPriority::{}: Given priority {} exceeds max priority {}, using lowest priority instead",
		    __func__, priority, queuesByPriority_.size() - 1);
		priority = queuesByPriority_.size() - 1;  // lowest priority
	}

	queuesByPriority_[priority].emplace(jobId, jobType, data, length);
	currentSize_ += length;
	currentElements_++;
}

void ProducerConsumerQueueWithPriority::retrieveFromQueue_(uint32_t *jobId, uint32_t *jobType,
                                                          uint8_t **data, uint32_t *length,
                                                          std::queue<QueueEntry> *queue) {
	if (queue == nullptr) {
		safs::log_warn(
		    "ProducerConsumerQueueWithPriority::{}: Trying to retrieve from a null queue (this should not happen)",
		    __func__);
		if (jobId) { *jobId = 0; }
		if (jobType) { *jobType = 0; }
		if (data) { *data = nullptr; }
		if (length) { *length = 0; }
		return;
	}

	auto &entry = queue->front();
	currentSize_ -= entry.length;
	currentElements_--;

	if (jobId) { *jobId = entry.jobId; }
	if (jobType) { *jobType = entry.jobType; }
	if (data) { *data = entry.data; }
	if (length) { *length = entry.length; }

	queue->pop();
}

void ProducerConsumerQueueWithPriority::get_(uint32_t *jobId, uint32_t *jobType, uint8_t **data,
                                             uint32_t *length) {
	std::queue<QueueEntry> *notEmptyQueue = nullptr;
	for (auto &queue : queuesByPriority_) {
		if (!queue.empty()) {
			notEmptyQueue = &queue;
			break;
		}
	}

	retrieveFromQueue_(jobId, jobType, data, length, notEmptyQueue);
}
