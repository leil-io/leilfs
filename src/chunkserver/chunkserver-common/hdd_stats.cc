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

#include "common/platform.h"

#include <cstdint>
#include "chunkserver-common/hdd_stats.h"

#include "chunkserver-common/disk_interface.h"
#include "devtools/TracePrinter.h"

namespace HddStats {

template <typename T> void atomicMax(std::atomic<T> &result, T value) {
	T prev_value = result;
	while (prev_value < value &&
	       !result.compare_exchange_weak(prev_value, value)) {
	}
}

static inline void totalRead(IDisk *disk, uint64_t size, MicroSeconds duration) {
	TRACETHIS();

	if (duration <= 0) {
		return;
	}

	gStatsTotalOperationsRead++;
	gStatsTotalBytesRead += size;
	gBytesReadSinceLastGCSweep += size;
	gStatsTotalTimeRead += duration;

	auto &diskStats = disk->getCurrentStats();
	diskStats.rops++;
	diskStats.rbytes += size;
	diskStats.usecreadsum += duration;
	atomicMax<uint32_t>(diskStats.usecreadmax, duration);
}

static inline void totalWrite(IDisk *disk, uint64_t size,
                              MicroSeconds duration) {
	TRACETHIS();

	if (duration <= 0) {
		return;
	}

	gStatsTotalOperationsWrite++;
	gStatsTotalBytesWrite += size;
	gBytesWrittenSinceLastGCSweep += size;
	gStatsTotalTimeWrite += duration;

	auto &diskStats = disk->getCurrentStats();
	diskStats.wops++;
	diskStats.wbytes += size;
	diskStats.usecwritesum += duration;
	atomicMax<uint32_t>(diskStats.usecwritemax, duration);
}

void stats(statsReport report) {
	TRACETHIS();
	*report.overheadBytesRead = gStatsOverheadBytesRead.exchange(0);
	*report.overheadBytesWrite = gStatsOverheadBytesWrite.exchange(0);
	*report.overheadOperationsRead = gStatsOverheadOperationsRead.exchange(0);
	*report.overheadOperationsWrite = gStatsOverheadOperationsWrite.exchange(0);
	*report.totalBytesRead = gStatsTotalBytesRead.exchange(0);
	*report.totalBytesWrite = gStatsTotalBytesWrite.exchange(0);
	*report.totalOperationsRead = gStatsTotalOperationsRead.exchange(0);
	*report.totalOperationsWrite = gStatsTotalOperationsWrite.exchange(0);
	*report.totalReadTime = gStatsTotalTimeRead.exchange(0);
	*report.totalWriteTime = gStatsTotalTimeWrite.exchange(0);
}

void operationStats(uint32_t *opsCreate, uint32_t *opsDelete, uint32_t *opsUpdateVersion,
                    uint32_t *opsDuplicate, uint32_t *opsTruncate, uint32_t *opsDupTrunc,
                    uint32_t *opsTest, uint32_t *opsGCPurge) {
	TRACETHIS();
	*opsCreate = gStatsOperationsCreate.exchange(0);
	*opsDelete = gStatsOperationsDelete.exchange(0);
	*opsTest = gStatsOperationsTest.exchange(0);
	*opsUpdateVersion = gStatsOperationsVersion.exchange(0);
	*opsDuplicate = gStatsOperationsDuplicate.exchange(0);
	*opsTruncate = gStatsOperationsTruncate.exchange(0);
	*opsDupTrunc = gStatsOperationsDupTrunc.exchange(0);
	*opsGCPurge = gStatsOperationsGCPurge.exchange(0);
}

void overheadRead(uint32_t size) {
	TRACETHIS();
	gStatsOverheadOperationsRead++;
	gStatsOverheadBytesRead += size;
}

void overheadWrite(uint32_t size) {
	TRACETHIS();
	gStatsOverheadOperationsWrite++;
	gStatsOverheadBytesWrite += size;
}

void dataFSync(IDisk *disk, MicroSeconds fsyncTime) {
	TRACETHIS();

	if (fsyncTime <= 0) {
		return;
	}

	gStatsTotalTimeWrite += fsyncTime;

	auto &diskStats = disk->getCurrentStats();
	diskStats.fsyncops++;
	diskStats.usecfsyncsum += fsyncTime;
	atomicMax<uint32_t>(diskStats.usecfsyncmax, fsyncTime);
}

} //namespace HddStats

IOStatsUpdater::IOStatsUpdater(IDisk *disk, uint64_t dataSize,
                               StatsUpdateFunc updateFunc)
    : dataSize_(dataSize), disk_(disk), updateFunc_(updateFunc) {}

IOStatsUpdater::~IOStatsUpdater() {
	if (success_) {
		MicroSeconds duration = getMicroSecsTime() - startTime_;
		updateFunc_(disk_, dataSize_, duration);
	}
}

void IOStatsUpdater::markIOAsFailed() noexcept { success_ = false; }

DiskWriteStatsUpdater::DiskWriteStatsUpdater(IDisk *disk, uint64_t dataSize)
    : updater_(disk, dataSize, HddStats::totalWrite) {}

void DiskWriteStatsUpdater::markWriteAsFailed() noexcept {
	updater_.markIOAsFailed();
}

DiskReadStatsUpdater::DiskReadStatsUpdater(IDisk *disk, uint64_t dataSize)
    : updater_(disk, dataSize, HddStats::totalRead) {}

void DiskReadStatsUpdater::markReadAsFailed() noexcept {
	updater_.markIOAsFailed();
}
