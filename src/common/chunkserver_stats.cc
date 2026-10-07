/*
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"
#include "common/chunkserver_stats.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

// ChunkserverEntry implementation

constexpr int ChunkserverStats::ChunkserverEntry::defectiveTimeout_ms;
constexpr uint32_t ChunkserverStats::ChunkserverEntry::kReferenceRoundTripTime_ms;
constexpr float ChunkserverStats::ChunkserverEntry::kLatencyInfluence;
constexpr uint32_t ChunkserverStats::ChunkserverEntry::kRoundTripTimeSmoothingFactor;

ChunkserverStats::ChunkserverEntry::ChunkserverEntry(): pendingReads_(0), pendingWrites_(0),
		defects_(0), roundTripTime_ms_(0), hasRoundTripTime_(false),
		defectiveTimeout_(std::chrono::milliseconds(defectiveTimeout_ms)) {
}

// ChunkserverStats implementation

const ChunkserverStats::ChunkserverEntry ChunkserverStats::getStatisticsFor(
		const NetworkAddress& address) {
	std::unique_lock<std::mutex> lock(mutex_);
	return chunkserverEntries_[address];
}

void ChunkserverStats::registerReadOperation(const NetworkAddress& address) {
	std::unique_lock<std::mutex> lock(mutex_);
	chunkserverEntries_[address].pendingReads_++;
}

void ChunkserverStats::unregisterReadOperation(const NetworkAddress& address) {
	std::unique_lock<std::mutex> lock(mutex_);
	chunkserverEntries_[address].pendingReads_--;
}

void ChunkserverStats::registerWriteOperation(const NetworkAddress& address) {
	std::unique_lock<std::mutex> lock(mutex_);
	chunkserverEntries_[address].pendingWrites_++;
}

void ChunkserverStats::unregisterWriteOperation(const NetworkAddress& address) {
	std::unique_lock<std::mutex> lock(mutex_);
	chunkserverEntries_[address].pendingWrites_--;
}

void ChunkserverStats::setUseRoundTripTime(bool useRoundTripTime) {
	useRoundTripTime_.store(useRoundTripTime);
}

bool ChunkserverStats::useRoundTripTime() const {
	return useRoundTripTime_.load();
}

void ChunkserverStats::updateRoundTripTime(const NetworkAddress& address,
		uint32_t roundTripTime_ms) {
	// Dropped here so no reader has to filter out round trip times it must ignore.
	if (!useRoundTripTime_.load(std::memory_order_relaxed)) {
		return;
	}

	std::unique_lock<std::mutex> lock(mutex_);
	ChunkserverEntry& chunkserver = chunkserverEntries_[address];
	roundTripTime_ms = std::max<uint32_t>(1, roundTripTime_ms);
	if (!chunkserver.hasRoundTripTime_) {
		chunkserver.roundTripTime_ms_ = roundTripTime_ms;
		chunkserver.hasRoundTripTime_ = true;
		return;
	}

	constexpr uint32_t kFactor = ChunkserverEntry::kRoundTripTimeSmoothingFactor;
	chunkserver.roundTripTime_ms_ = std::max<uint32_t>(1,
			(chunkserver.roundTripTime_ms_ * (kFactor - 1) + roundTripTime_ms) / kFactor);
}

void ChunkserverStats::markWorking(const NetworkAddress& address) {
	std::unique_lock<std::mutex> lock(mutex_);
	chunkserverEntries_[address].defects_ = 0;
}

void ChunkserverStats::markDefective(const NetworkAddress& address) {
	std::unique_lock<std::mutex> lock(mutex_);
	ChunkserverEntry &chunkserver = chunkserverEntries_[address];
	if (chunkserver.defects_ < 1000) {   // don't be too pedantic to prevent overflows
		chunkserver.defects_++;
	}
	chunkserver.defectiveTimeout_.reset();
}

float ChunkserverStats::ChunkserverEntry::score() const {
	// Maps the round trip time onto [-1, 1] around the reference latency.
	float latencyBias = 0.f;
	if (hasRoundTripTime_) {
		const float reference = static_cast<float>(kReferenceRoundTripTime_ms);
		const float rtt = static_cast<float>(roundTripTime_ms_);
		latencyBias = (reference - rtt) / (reference + rtt);
	}
	const float latencyFactor = 1.f + kLatencyInfluence * latencyBias;

	if (defects_ > 0 && !defectiveTimeout_.expired()) {
		return latencyFactor / (defects_ + 1);
	} else {
		return latencyFactor;
	}
}

// ChunkserverStatsProxy implementation

ChunkserverStatsProxy::~ChunkserverStatsProxy() {
	for (auto entry : readOperations_) {
		for (uint32_t i = 0; i < entry.second; i++) {
			stats_.unregisterReadOperation(entry.first);
		}
	}
	for (auto entry : writeOperations_) {
		for (uint32_t i = 0; i < entry.second; i++) {
			stats_.unregisterWriteOperation(entry.first);
		}
	}
}

void ChunkserverStatsProxy::registerReadOperation(const NetworkAddress& address) {
	stats_.registerReadOperation(address);
	readOperations_[address]++;
}

void ChunkserverStatsProxy::unregisterReadOperation(const NetworkAddress& address) {
	stats_.unregisterReadOperation(address);
	readOperations_[address]--;
}

void ChunkserverStatsProxy::registerWriteOperation(const NetworkAddress& address) {
	stats_.registerWriteOperation(address);
	writeOperations_[address]++;
}

void ChunkserverStatsProxy::unregisterWriteOperation(const NetworkAddress& address) {
	stats_.unregisterWriteOperation(address);
	writeOperations_[address]--;
}

void ChunkserverStatsProxy::updateRoundTripTime(const NetworkAddress& address,
		uint32_t roundTripTime_ms) {
	stats_.updateRoundTripTime(address, roundTripTime_ms);
}

void ChunkserverStatsProxy::markDefective(const NetworkAddress& address) {
	stats_.markDefective(address);
}

void ChunkserverStatsProxy::markWorking(const NetworkAddress& address) {
	stats_.markWorking(address);
}

void ChunkserverStatsProxy::allPendingDefective() {
	for (auto entry : readOperations_) {
		if (entry.second > 0) {
			stats_.markDefective(entry.first);
		}
	}
	for (auto entry : writeOperations_) {
		if (entry.second > 0) {
			stats_.markDefective(entry.first);
		}
	}
}
