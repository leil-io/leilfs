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

#include <array>
#include <cassert>

#include "common/ring_buffer.h"
#include "common/time_utils.h"
#include "mount/sauna_client.h"

/*! Adviser class for readahead mechanism.
 *
 * This class is used to predict size of readahead requests on the basis of
 * sequentiality of read operations and estimated latency.
 */
class ReadaheadAdviser {
public:
	struct HistoryEntry {
		int64_t timestamp;
		uint32_t request_size;
	};

	static constexpr int kOppositeRequestThreshold = 4;

	ReadaheadAdviser(uint32_t timeout_ms, uint32_t window_size_limit =
	    SaunaClient::FsInitParams::kDefaultReadaheadMaxWindowSize * 1024,
	    int oppositeRequestThreshold = kOppositeRequestThreshold) :
		current_offset_(),
		window_(kInitWindowSize),
		random_candidates_(),
		oppositeRequestThreshold_(oppositeRequestThreshold),
		max_window_size_(window_size_limit),
		window_size_limit_(window_size_limit),
		random_threshold_(kRandomThreshold),
		requested_bytes_(),
		timer_(),
		timeout_ms_(timeout_ms) {}

	/*!
	 * \brief Acknowledge read request and judge whether it is sequential or random.
	 * \param offset offset of read operation
	 * \param size size of read operation
	 * \param is_sequential type of the last request
	 */
	void feed(uint64_t offset, uint32_t size, bool &is_sequential) {
		addToHistory(size);
		is_sequential = difference(offset, current_offset_) <= kErrorThreshold;
		updateShouldUseReadahead(is_sequential);
		current_offset_ = offset + size;

		if (timeout_ms_ == 0) {
			window_ = 0;
			return;
		}

		if (is_sequential) {
			random_candidates_ = 0;
			expand();
		} else {
			random_candidates_++;
			if (looksRandom()) {
				reduce();
			}
		}
	}

	void feed(uint64_t offset, uint32_t size) {
		bool dummy;
		feed(offset, size, dummy);
	}

	/*!
	* \brief Estimate the time a readahead request will be needed.
	*/
	double expectedNeededTime_us(uint64_t aheadSize) {
		return aheadSize / throughputMBps();
	}

	/*!
	 * \brief Estimation of the size of data requested by the process
	 * considering the last read requests made.
	 */
	uint64_t throughputWindow() {
		return kConservativeMultiplier * throughputMBps() * timeout_ms_
		       * kBytesInOneKiB;
	}

	/*!
	 * \brief Count suggested readahead window size.
	 * \return suggested readahead window size
	 */
	int window() const {
		return std::min(window_, max_window_size_);
	}

	/*!
	 * \brief Update the adviser's internal members for suggesting whether to
	 * use or not the readahead mechanism. The following rule is applied:
	 * - after ```oppositeRequestThreshold_``` consecutive sequential reads, if
	 * the current advise was to not use the readahead mechanism, it is changed
	 * to use it.
	 * - after ```oppositeRequestThreshold_``` consecutive non-sequential reads,
	 * if the current advise was to use the readahead mechanism, it is changed
	 * to not use it.
	 */
	void updateShouldUseReadahead(bool is_sequential) {
		if (is_sequential == shouldUseReadahead_) {
			continuousRequestType_ = 0;
		} else {
			continuousRequestType_++;
			if (continuousRequestType_ >= oppositeRequestThreshold_) {
				continuousRequestType_ = 0;
				shouldUseReadahead_ = !shouldUseReadahead_;
			}
		}
	}

	/*!
	 * \brief Return whether it is suggested to use the readahead mechanism.
	*/
	bool shouldUseReadahead() const {
		return shouldUseReadahead_;
	}

private:

	/*!
	* \brief Calculates an estimation of the throughput in MB/s considering the current history.
	* The timestamp that could be provided is to save the call to ```elapsed_us``` if already done.
	*/
	double throughputMBps(int64_t timestamp = 0) {
		if (timestamp == 0) {
			timestamp = timer_.elapsed_us();
		}
		// Make sure non-zero division
		double throughput_MBps = static_cast<double>(requested_bytes_) /
		                         std::max<int64_t>(timestamp - history_.front().timestamp, 1);

		if (throughput_MBps <= 0) {
			safs::log_warn("Throughput must be greater than 0, current value: {}", throughput_MBps);
			throughput_MBps = kNeutralThroughput;
		}
		return throughput_MBps;
	}

	/*!
	 * \brief Calculates the absolute difference between two ```uint64_t``` values.
	 */
	static uint64_t difference(uint64_t x, uint64_t y) {
		return x > y ? x - y : y - x;
	}

	/*!
	 * \brief Convert from ms to ns.
	 */
	static inline int64_t millisecondsToNanoseconds(int64_t valueInMs) {
		static constexpr int64_t kNanosecondsInOneMillisecond = 1000000;
		return valueInMs * kNanosecondsInOneMillisecond;
	}

	/*!
	 * \brief Check if history entry is not overdue.
	 * \return true if history entry is overdue
	 */
	bool expired(HistoryEntry entry, int64_t timestamp) {
		return entry.timestamp + millisecondsToNanoseconds(timeout_ms_) < timestamp;
	}

	/*!
	 * \brief Add size of read request to history.
	 * \param size size of read request
	 */
	void addToHistory(uint32_t size) {
		int64_t timestamp = timer_.elapsed_us();
		// Remove stale history entries
		while (history_.full() || (!history_.empty() && expired(history_.front(), timestamp))) {
			requested_bytes_ -= history_.front().request_size;
			history_.pop_front();
		}

		history_.push_back(HistoryEntry{timestamp, size});
		requested_bytes_ += size;

		// If there is enough data in history to predict max window size, do it
		if (history_.size() >= kHistoryValidityThreshold  && timestamp != history_.front().timestamp) {
			adjustMaxWindowSize(timestamp);
		}
	}

	/*!
	 * \brief Adjust max window size on the basis of estimated latency.
	 * \param timestamp time point used for latency estimation
	 */
	void adjustMaxWindowSize(int64_t timestamp) {
		// Max window size is set on the basis of estimated throughput
		max_window_size_ = std::min<uint64_t>(
		    window_size_limit_,
		    kConservativeMultiplier * throughputMBps(timestamp) * timeout_ms_ * kBytesInOneKiB);
		max_window_size_ = std::max(max_window_size_, kInitWindowSize);
	}

	/*!
	 * \brief Increase window size.
	 */
	void expand() {
		if (window_ >= max_window_size_) {
			return;
		}

		if (window_ < max_window_size_ / 16) {
			window_ *= 4;
		} else {
			window_ *= 2;
		}
	}

	/*!
	 * \brief Decrease window size.
	 */
	void reduce() {
		if (window_ >= 2 * kInitWindowSize) {
			window_ /= 2;
		}
	}

	/*!
	 * \brief Check if read operations seem to be random.
	 */
	bool looksRandom() {
		return random_candidates_ > random_threshold_;
	}

	static const unsigned kInitWindowSize = 1 << 16;
	static const int kRandomThreshold = 3;
	static const int kHistoryEntryLifespan_ns = 1 << 20;
	static const int kHistoryCapacity = 64;
	static const unsigned kHistoryValidityThreshold = 3;
	// This multiplier makes the raw estimation of the throughput window a much
	// conservative one -- it is much safer to assume the process won't ask more
	// data than expected.
	static const int64_t kConservativeMultiplier = 1;
	static const uint64_t kNeutralThroughput = 1;
	static const uint16_t kBytesInOneKiB = 1024;
	// up to add a command line options for these parameters
	static const int64_t kErrorThreshold = SFSBLOCKSIZE;
	static_assert(kHistoryCapacity >= (int)kHistoryValidityThreshold,
			"History validity threshold must not be greater than history capacity");

	uint64_t current_offset_;
	unsigned window_;
	int random_candidates_;

	int oppositeRequestThreshold_;
	unsigned max_window_size_;
	unsigned window_size_limit_;
	int random_threshold_;

	int continuousRequestType_ = 0;
	bool shouldUseReadahead_ = false;

	RingBuffer<HistoryEntry, kHistoryCapacity> history_;
	uint64_t requested_bytes_;
	Timer timer_;
	uint32_t timeout_ms_;
};
