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

#pragma once

#include "common/platform.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>

template<typename T>
void deleterByType(uint8_t *p) {
	delete ((T*)p);
}

inline void deleterDummy(uint8_t * /*unused*/) {}

/// @class ProducerConsumerQueue
/// @brief A thread-safe queue for producer-consumer scenarios.
///
/// Can be configured to support several priority levels. Final interface is queue-like,
/// but preferring higher priority items and preserving order within each priority level.
///
/// This class provides a thread-safe queue implementation that allows multiple
/// producers and consumers to add and remove items concurrently. It uses a
/// mutex and condition variables to ensure thread safety and to manage the
/// queue's state.
///
/// This class is particularly useful in scenarios where you need to decouple
/// the production and consumption of data, such as:
/// * Multi-threaded applications where one or more threads are generating data
///   and one or more threads are processing that data.
/// * Asynchronous applications where one or more tasks are scheduled and
///   another module processes the tasks.
///
/// // Example usage:
/// ProducerConsumerQueue queue(10, deleterByType<YourDataType>);
///
/// // Producer thread
/// std::thread producer([&queue]() {
///     for (int i = 0; i < 100; ++i) {
///         auto data = new YourDataType();
///         // Initialize data...
///         queue.put(i, 0, reinterpret_cast<uint8_t*>(data), sizeof(YourDataType));
///     }
/// });
///
/// // Consumer thread
/// std::thread consumer([&queue]() {
///     for (int i = 0; i < 100; ++i) {
///         uint32_t jobId, jobType, length;
///         uint8_t* data;
///         queue.get(&jobId, &jobType, &data, &length);
///         auto yourData = reinterpret_cast<YourDataType*>(data);
///         // Process data...
///         delete yourData;
///     }
/// });
///
/// producer.join();
/// consumer.join();
class ProducerConsumerQueue {
public:
	using Deleter = std::function<void(uint8_t*)>;

	/// @brief Constructs a ProducerConsumerQueue with a specified maximum size
	/// and deleter.
	///
	/// @param priorityLevels The number of priority levels. Default is 1 (no priorities).
	/// @param maxSize The maximum number of elements the queue can hold.
	/// Default is 0 (unlimited).
	/// @param deleter A callable type that defines how to delete the data
	/// stored in the queue. Default is deleterDummy.
	explicit ProducerConsumerQueue(uint8_t priorityLevels = 1, uint32_t maxSize = 0,
	                               Deleter deleter = deleterDummy);

	/// @brief Destructor for the ProducerConsumerQueue.
	~ProducerConsumerQueue();

	/// @brief Checks if the queue is empty.
	///
	/// @return true if the queue is empty, false otherwise.
	bool isEmpty() const;

	/// @brief Checks if the queue is full.
	///
	/// @return true if the queue is full, false otherwise.
	bool isFull() const;

	/// @brief Returns the number of elements that can still be added to the
	/// queue.
	///
	/// @return The number of elements that can still be added to the queue.
	uint32_t sizeLeft() const;

	/// @brief Returns the number of elements currently in the queue.
	///
	/// @return The number of elements currently in the queue.
	uint32_t elements() const;

	/// @brief Adds an element to the queue.
	///
	/// @param jobId The job ID associated with the element.
	/// @param jobType The job type associated with the element.
	/// @param data A pointer to the data to be added.
	/// @param length The length of the data to be added.
	/// @param priority The priority level of the element (0 is the highest
	/// priority). Default is 0.
	/// @return true if the element was added successfully, false otherwise.
	bool put(uint32_t jobId, uint32_t jobType, uint8_t *data, uint32_t length,
	         uint8_t priority = 0);

	/// @brief Tries to add an element to the queue without blocking.
	///
	/// @param jobId The job ID associated with the element.
	/// @param jobType The job type associated with the element.
	/// @param data A pointer to the data to be added.
	/// @param length The length of the data to be added.
	/// @param priority The priority level of the element (0 is the highest
	/// priority). Default is 0.
	/// @return true if the element was added successfully, false otherwise.
	bool tryPut(uint32_t jobId, uint32_t jobType, uint8_t *data, uint32_t length,
	            uint8_t priority = 0);

	/// @brief Removes an element from the queue.
	///
	/// @param jobId A pointer to store the job ID of the removed element.
	/// @param jobType A pointer to store the job type of the removed element.
	/// @param data A pointer to store the data of the removed element.
	/// @param length A pointer to store the length of the data of the removed
	/// element.
	/// @return true if an element was removed successfully, false otherwise.
	bool get(uint32_t *jobId, uint32_t *jobType, uint8_t **data,
	         uint32_t *length);

	/// @brief Tries to remove an element from the queue without blocking.
	///
	/// @param jobId A pointer to store the job ID of the removed element.
	/// @param jobType A pointer to store the job type of the removed element.
	/// @param data A pointer to store the data of the removed element.
	/// @param length A pointer to store the length of the data of the removed
	/// element.
	/// @return true if an element was removed successfully, false otherwise.
	bool tryGet(uint32_t *jobId, uint32_t *jobType, uint8_t **data,
	            uint32_t *length);

private:
	/// @brief Adds an element to the queue assuming non-fullness.
	/// mutex_: LOCKED
	inline void put_(uint32_t jobId, uint32_t jobType, uint8_t *data, uint32_t length,
	                 uint8_t priority);

	/// @brief Removes an element from the queue assuming non-emptiness.
	/// mutex_: LOCKED
	inline void get_(uint32_t *jobId, uint32_t *jobType, uint8_t **data, uint32_t *length);

	/// @brief Represents an entry in the queue.
	struct QueueEntry {
		uint32_t jobId;    ///< The job ID associated with the entry.
		uint32_t jobType;  ///< The job type associated with the entry.
		uint8_t *data;     ///< A pointer to the data of the entry.
		uint32_t length;   ///< The length of the data of the entry.

		/// @brief Constructs a QueueEntry with the specified parameters.
		///
		/// @param jobId The job ID associated with the entry.
		/// @param jobType The job type associated with the entry.
		/// @param data A pointer to the data of the entry.
		/// @param length The length of the data of the entry.
		QueueEntry(uint32_t jobId, uint32_t jobType, uint8_t *data,
		           uint32_t length)
		    : jobId(jobId), jobType(jobType), data(data), length(length) {}

		// Remove default constructor to avoid uninitialized entries.
		QueueEntry() = delete;

		// Allowing copy and move semantics for QueueEntry is not desirable, but required
		// for queuesByPriority_ vector of std::queue.
		QueueEntry(const QueueEntry &) = default;
		QueueEntry(QueueEntry &&) = default;
		QueueEntry &operator=(const QueueEntry &) = default;
		QueueEntry &operator=(QueueEntry &&) = default;

		/// @brief Destructor for the QueueEntry.
		~QueueEntry() = default;
	};

	///< The underlying queues storing the entries.
	std::vector<std::queue<QueueEntry>> queuesByPriority_;
	///< The maximum number of elements the queue can hold.
	uint32_t maxSize_;
	///< The current number of elements in the queue.
	uint32_t currentElements_;
	///< The current amount of data in the queue.
	uint32_t currentSize_;
	///< Mutex for synchronizing access to the queue.
	mutable std::mutex mutex_;
	///< Condition variable to signal when the queue is not full.
	std::condition_variable notFull_;
	///< Condition variable to signal when the queue is not empty.
	std::condition_variable notEmpty_;
	///< The deleter function used to delete the data stored in the queue.
	Deleter deleter_;
};
