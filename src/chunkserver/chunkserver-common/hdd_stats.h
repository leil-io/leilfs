/*
   Copyright 2023 Leil Storage

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

#include <sys/time.h>
#include <atomic>
#include <functional>

class IDisk;
using MicroSeconds = uint64_t;

static inline MicroSeconds getMicroSecsTime() {
	struct timeval tv;
	gettimeofday(&tv, nullptr);
	return (static_cast<MicroSeconds>(tv.tv_sec)) * 1000000 + tv.tv_usec;
}

namespace HddStats {

// These gStats* variables are for charts only. Therefore there's no need
// to keep an absolute consistency with a mutex.
inline std::atomic<uint64_t> gStatsOverheadBytesRead(0);
inline std::atomic<uint64_t> gStatsOverheadBytesWrite(0);
inline std::atomic<uint32_t> gStatsOverheadOperationsRead(0);
inline std::atomic<uint32_t> gStatsOverheadOperationsWrite(0);
inline std::atomic<uint64_t> gStatsTotalBytesRead(0);
inline std::atomic<uint64_t> gStatsTotalBytesWrite(0);
inline std::atomic<uint32_t> gStatsTotalOperationsRead(0);
inline std::atomic<uint32_t> gStatsTotalOperationsWrite(0);
inline std::atomic<uint64_t> gStatsTotalTimeRead(0);
inline std::atomic<uint64_t> gStatsTotalTimeWrite(0);

inline std::atomic<uint32_t> gStatsOperationsCreate(0);
inline std::atomic<uint32_t> gStatsOperationsDelete(0);
inline std::atomic<uint32_t> gStatsOperationsTest(0);
inline std::atomic<uint32_t> gStatsOperationsVersion(0);
inline std::atomic<uint32_t> gStatsOperationsDuplicate(0);
inline std::atomic<uint32_t> gStatsOperationsTruncate(0);
inline std::atomic<uint32_t> gStatsOperationsDupTrunc(0);

struct statsReport {
	statsReport(uint64_t *overBytesRead, uint64_t *overBytesWrite,
	            uint32_t *overOpsRead, uint32_t *overOpsWrite,
	            uint64_t *_totalBytesRead, uint64_t *_totalBytesWrite,
	            uint32_t *totalOpsRead, uint32_t *totalOpsWrite,
	            uint64_t *_totalReadTime, uint64_t *_totalWriteTime) {
		overheadBytesRead = overBytesRead;
		overheadBytesWrite = overBytesWrite;
		overheadOperationsRead = overOpsRead;
		overheadOperationsWrite = overOpsWrite;

		totalBytesRead = _totalBytesRead;
		totalBytesWrite = _totalBytesWrite;
		totalOperationsRead = totalOpsRead;
		totalOperationsWrite = totalOpsWrite;
		totalReadTime = _totalReadTime;
		totalWriteTime = _totalWriteTime;
	}

	// overhead operations
	uint64_t *overheadBytesRead;
	uint64_t *overheadBytesWrite;
	uint32_t *overheadOperationsRead;
	uint32_t *overheadOperationsWrite;

	// total operations
	uint64_t *totalBytesRead;
	uint64_t *totalBytesWrite;
	uint32_t *totalOperationsRead;
	uint32_t *totalOperationsWrite;
	uint64_t *totalReadTime;
	uint64_t *totalWriteTime;
};

/// Only called from chartsdata_refresh every minute
/// The information is saved later (every hour) in the csstats file
void stats(statsReport report);

/// Only called from chartsdata_refresh every minute
/// The information is saved later (every hour) in the csstats file
void operationStats(uint32_t *opsCreate, uint32_t *opsDelete,
                    uint32_t *opsUpdateVersion, uint32_t *opsDuplicate,
                    uint32_t *opsTruncate, uint32_t *opsDupTrunc,
                    uint32_t *opsTest);

void overheadRead(uint32_t size);
void overheadWrite(uint32_t size);
void dataFSync(IDisk *disk, MicroSeconds fsyncTime);

} //namespace HddStats

/// RAII scoped updater for timed IO operations, using a delegate function
///
/// The constructor starts counting the time immediately, while the destructor
/// updates the duration and calls the concrete delegate function to actually
/// updated the related stats (global variables).
class IOStatsUpdater {
public:
	using StatsUpdateFunc =
	    std::function<void(IDisk *disk, uint64_t size, MicroSeconds duration)>;

	/// Constructs the object from the parameters and starts counting the time
	IOStatsUpdater(IDisk *disk, uint64_t dataSize, StatsUpdateFunc updateFunc);

	/// Updates the duration and calls the delegate function
	~IOStatsUpdater();

	void markIOAsFailed() noexcept;

private:
	///< Initialized immediately in the constructor
	MicroSeconds startTime_ = getMicroSecsTime();

	uint64_t dataSize_;          ///< Size of the IO operation
	IDisk *disk_;                 ///< Disk for this operation
	StatsUpdateFunc updateFunc_; ///< Delegate function to call at destruction
	bool success_ = true;        ///< Tells if the operation succeeded
};

/// Concrete updater for write operations
class DiskWriteStatsUpdater {
public:
	DiskWriteStatsUpdater(IDisk *disk, uint64_t dataSize);

	void markWriteAsFailed() noexcept;

private:
	IOStatsUpdater updater_;
};

/// Concrete updater for read operations
class DiskReadStatsUpdater {
public:
	DiskReadStatsUpdater(IDisk *disk, uint64_t dataSize);

	void markReadAsFailed() noexcept;

private:
	IOStatsUpdater updater_;
};
