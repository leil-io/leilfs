#include "hdd_stats.h"

#include "chunkserver-common/disk_interface.h"
#include "devtools/TracePrinter.h"
#include "metrics/metrics.h"

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

	metrics::Counter::increment(metrics::chunkserver::Counters::HDD_TOTAL_READ_OPERATIONS);
	gStatsTotalOperationsRead++;
	metrics::Counter::increment(metrics::chunkserver::Counters::HDD_TOTAL_BYTES_READ,
	                            static_cast<double>(size));
	gStatsTotalBytesRead += size;
	metrics::Counter::increment(metrics::chunkserver::Counters::HDD_TOTAL_TIME_READ,
	                            static_cast<double>(duration));
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

	metrics::Counter::increment(metrics::chunkserver::Counters::HDD_TOTAL_WRITE_OPERATIONS);
	gStatsTotalOperationsWrite++;
	metrics::Counter::increment(metrics::chunkserver::Counters::HDD_TOTAL_BYTES_WRITE,
	                            static_cast<double>(size));
	gStatsTotalBytesWrite += size;
	metrics::Counter::increment(metrics::chunkserver::Counters::HDD_TOTAL_TIME_WRITE,
	                            static_cast<double>(duration));
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

void operationStats(uint32_t *opsCreate, uint32_t *opsDelete,
                    uint32_t *opsUpdateVersion, uint32_t *opsDuplicate,
                    uint32_t *opsTruncate, uint32_t *opsDupTrunc,
                    uint32_t *opsTest) {
	TRACETHIS();
	*opsCreate = gStatsOperationsCreate.exchange(0);
	*opsDelete = gStatsOperationsDelete.exchange(0);
	*opsTest = gStatsOperationsTest.exchange(0);
	*opsUpdateVersion = gStatsOperationsVersion.exchange(0);
	*opsDuplicate = gStatsOperationsDuplicate.exchange(0);
	*opsTruncate = gStatsOperationsTruncate.exchange(0);
	*opsDupTrunc = gStatsOperationsDupTrunc.exchange(0);
}

void overheadRead(uint32_t size) {
	TRACETHIS();
	metrics::Counter::increment(metrics::chunkserver::Counters::HDD_TOTAL_READ_OPERATIONS_OVERHEAD);
	gStatsOverheadOperationsRead++;
	metrics::Counter::increment(metrics::chunkserver::Counters::HDD_TOTAL_OVERHEAD_BYTES_READ, size);
	gStatsOverheadBytesRead += size;
}

void overheadWrite(uint32_t size) {
	TRACETHIS();
	metrics::Counter::increment(metrics::chunkserver::Counters::HDD_TOTAL_WRITE_OPERATIONS_OVERHEAD);
	gStatsOverheadOperationsWrite++;
	metrics::Counter::increment(metrics::chunkserver::Counters::HDD_TOTAL_OVERHEAD_BYTES_WRITE, size);
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
