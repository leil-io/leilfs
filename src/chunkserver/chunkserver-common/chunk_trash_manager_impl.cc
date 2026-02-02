/*
   Copyright 2023-2024  Leil Storage OÜ

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

#include <sys/statvfs.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>
#include <utility>

#include "chunkserver-common/chunk_trash_manager.h"
#include "chunkserver-common/chunk_trash_manager_impl.h"
#include "chunkserver-common/global_shared_resources.h"
#include "chunkserver-common/hdd_stats.h"
#include "config/cfg.h"
#include "errors/saunafs_error_codes.h"
#include "slogger/slogger.h"

namespace fs = std::filesystem;

std::atomic<size_t> ChunkTrashManagerImpl::availableThresholdGB = kDefaultAvailableThresholdGB;
std::atomic<size_t> ChunkTrashManagerImpl::trashTimeLimitSeconds = kDefaultTrashTimeLimitSeconds;
std::atomic<size_t> ChunkTrashManagerImpl::trashGarbageCollectorBulkSize =
    kDefaultTrashGarbageCollectorBulkSize;
std::atomic<size_t> ChunkTrashManagerImpl::garbageCollectorSpaceRecoveryStep =
    kDefaultGarbageCollectorSpaceRecoveryStep;
uint16_t ChunkTrashManagerImpl::numberOfTrashPurgeWorkers = kDefaultNumberOfTrashPurgeWorkers;

inline constexpr uint64_t kNumberOfBytesPerMebiByte = 1024 * 1024;

uint64_t ChunkTrashManagerImpl::maxBytesReadPerDisk = 1 * kNumberOfBytesPerMebiByte;
uint64_t ChunkTrashManagerImpl::maxBytesWritePerDisk = 1 * kNumberOfBytesPerMebiByte;

uint64_t ChunkTrashManagerImpl::previousBytesReadPerDisk = 1 * kNumberOfBytesPerMebiByte;   // 1MiB
uint64_t ChunkTrashManagerImpl::previousBytesWritePerDisk = 1 * kNumberOfBytesPerMebiByte;  // 1MiB

const std::string ChunkTrashManagerImpl::kTrashGuardString =
    std::string("/") + ChunkTrashManager::kTrashDirname + "/";

/**
 * @brief Encapsulates global state for the trash purger.
 *
 * ChunkTrashManagerImpl requires non-trivial global state (threads, queue,
 * atomics, mutex, condition_variable). Originally these were separate static
 * class members in a PIC library shared by the main binary and plugins, which
 * can lead to unsafe initialization/destruction patterns across images.
 *
 * This aggregates the state into a single function-local static to ensure
 * lazy initialization, well-defined destruction order, and reduced global
 * symbol exposure.
 */
namespace {
struct TrashPurgerState {
	std::vector<std::thread> threads;
	std::unique_ptr<ProducerConsumerQueue> queue;
	std::atomic<uint32_t> jobsCount{0};
	std::mutex mutex;
	std::condition_variable cv;
};

TrashPurgerState &trashPurgerState() {
	static TrashPurgerState state;
	return state;
}
}  // namespace

/**
 * @brief Job representing a trash file removal task.
 */
struct TrashRemovalJob {
	ChunkTrashIndex::TrashIndexFileEntries files;  // Files to remove from trash
	std::string diskPath;                          // Disk where the files are located
};

/**
 * @brief Operations that the trash purger worker thread can process.
 *
 * These values are used to signal the type of job that should be executed
 * by the trash purger worker.
 */
enum TrashPurgerOperation : uint8_t {
	Purge,  // Remove trash files from disk as part of the garbage collection process
	Exit    // Signal the worker thread to terminate gracefully
};

void ChunkTrashManagerImpl::reloadConfig() {
	availableThresholdGB =
	    cfg_get("CHUNK_TRASH_FREE_SPACE_THRESHOLD_GB", kDefaultAvailableThresholdGB);
	trashTimeLimitSeconds =
	    cfg_get("CHUNK_TRASH_EXPIRATION_SECONDS", kDefaultTrashTimeLimitSeconds);
	trashGarbageCollectorBulkSize =
	    cfg_get("CHUNK_TRASH_GC_BATCH_SIZE", kDefaultTrashGarbageCollectorBulkSize);
	garbageCollectorSpaceRecoveryStep = cfg_get("CHUNK_TRASH_GC_SPACE_RECOVERY_BATCH_SIZE",
	                                            kDefaultGarbageCollectorSpaceRecoveryStep);

	safs::log_info(
	    "Reloaded chunk trash manager configuration: "
	    "CHUNK_TRASH_FREE_SPACE_THRESHOLD_GB={}, "
	    "CHUNK_TRASH_EXPIRATION_SECONDS={}, "
	    "CHUNK_TRASH_GC_BATCH_SIZE={}, "
	    "CHUNK_TRASH_GC_SPACE_RECOVERY_BATCH_SIZE={}, "
	    "CHUNK_TRASH_GC_PURGE_WORKERS_NR={}",
	    availableThresholdGB.load(), trashTimeLimitSeconds.load(),
	    trashGarbageCollectorBulkSize.load(), garbageCollectorSpaceRecoveryStep.load(),
	    numberOfTrashPurgeWorkers);
}

void ChunkTrashManagerImpl::init() {
	numberOfTrashPurgeWorkers =
	    cfg_get_minmaxvalue("CHUNK_TRASH_GC_PURGE_WORKERS_NR", kDefaultNumberOfTrashPurgeWorkers,
	                        kMinNumberOfTrashPurgeWorkers, kMaxNumberOfTrashPurgeWorkers);

	auto &state = trashPurgerState();
	if (!state.queue) { state.queue = std::make_unique<ProducerConsumerQueue>(); }

	if (!state.threads.empty()) { return; }

	std::unique_lock lock(state.mutex);
	for (uint16_t i = 0; i < numberOfTrashPurgeWorkers; ++i) {
		state.threads.emplace_back(&ChunkTrashManagerImpl::removeTrashFilesFromDiskWorker, i);
	}
}

std::string ChunkTrashManagerImpl::getStringFromTime(std::time_t time) {
	std::tm utcTime;

	if (gmtime_r(&time, &utcTime) == nullptr) {
		safs::log_error_code(SAUNAFS_ERROR_EINVAL, "Failed to convert time to UTC: {}",
		                     std::strerror(errno));
		return "";
	}

	std::ostringstream oss;
	oss << std::put_time(&utcTime, kTimeStampFormat.c_str());
	return oss.str();
}

std::time_t ChunkTrashManagerImpl::getTimeFromString(const std::string &timeString,
                                                     int &errorCode) {
	errorCode = SAUNAFS_STATUS_OK;
	std::tm time = {};
	std::istringstream stringReader(timeString);
	stringReader >> std::get_time(&time, kTimeStampFormat.c_str());
	if (stringReader.fail()) {
		errorCode = SAUNAFS_ERROR_EINVAL;
		safs::log_error_code(static_cast<error_type>(errorCode), "Failed to parse time string: {}",
		                     timeString.c_str());
		return 0;
	}

	const std::time_t parsedTime = timegm(&time);

	if (parsedTime == static_cast<std::time_t>(-1)) {
		errorCode = SAUNAFS_ERROR_EINVAL;
		safs::log_error_code(static_cast<error_type>(errorCode),
		                     "Failed to convert UTC time string: {}", timeString.c_str());
		return 0;
	}

	return parsedTime;
}

ChunkTrashManagerImpl::error_type ChunkTrashManagerImpl::getMoveDestinationPath(
    const std::string &filePath, const std::string &sourceRoot, const std::string &destinationRoot,
    std::string &destinationPath) {
	auto error_code = SAUNAFS_STATUS_OK;
	if (filePath.find(sourceRoot) != 0) {
		error_code = SAUNAFS_ERROR_EINVAL;
		safs::log_error_code(error_code, "File path is outside the source root: {}",
		                     filePath.c_str());
		return error_code;
	}

	destinationPath = destinationRoot + "/" + filePath.substr(sourceRoot.size());
	return error_code;
}

int ChunkTrashManagerImpl::moveToTrash(const fs::path &filePath, const fs::path &diskPath,
                                       const std::time_t &deletionTime) {
	std::error_code errorCode_;
	if (!fs::exists(filePath, errorCode_)) {
		safs::log_error_code(errorCode_, "File does not exist: {}", filePath.string().c_str());
		return SAUNAFS_ERROR_ENOENT;
	}

	const fs::path trashDir = getTrashDir(diskPath);
	fs::create_directories(trashDir, errorCode_);
	if (errorCode_) {
		safs::log_error_code(errorCode_, "Failed to create trash directory: {}",
		                     trashDir.string().c_str());
		return SAUNAFS_ERROR_NOTDONE;
	}

	const std::string deletionTimestamp = getStringFromTime(deletionTime);
	std::string trashFilename;

	auto errorCode = getMoveDestinationPath(filePath.string(), diskPath.string(), trashDir.string(),
	                                        trashFilename);
	if (errorCode != SAUNAFS_STATUS_OK) {
		safs::log_error_code(errorCode, "Failed to get destination path for file: {}",
		                     filePath.string().c_str());
		return errorCode;
	}

	trashFilename += "." + deletionTimestamp;

	fs::create_directories(fs::path(trashFilename).parent_path(), errorCode_);
	if (errorCode_) {
		safs::log_error_code(errorCode_, "Failed to create trash directory: {}",
		                     trashFilename.c_str());
		return SAUNAFS_ERROR_NOTDONE;
	}

	fs::rename(filePath, trashFilename, errorCode_);
	if (errorCode_) {
		safs::log_error_code(errorCode_, "Failed to move file to trash: {}",
		                     filePath.string().c_str());
		return SAUNAFS_ERROR_NOTDONE;
	}

	getTrashIndex().add(deletionTime, trashFilename, diskPath.string());

	return SAUNAFS_STATUS_OK;
}

void ChunkTrashManagerImpl::removeTrashFiles(
    ChunkTrashIndex::TrashIndexDiskEntries &filesToRemove) const {
	auto &state = trashPurgerState();

	for (auto &[diskPath, fileEntries] : filesToRemove) {
		state.jobsCount++;
		auto job = std::make_unique<TrashRemovalJob>(std::move(fileEntries), diskPath);
		state.queue->put(0, TrashPurgerOperation::Purge, reinterpret_cast<uint8_t *>(job.get()),
		                 1);
		job.release();
	}
	std::unique_lock<std::mutex> trashPurgerLock(state.mutex);
	state.cv.wait(trashPurgerLock, [&] { return state.jobsCount == 0; });
}

void ChunkTrashManagerImpl::removeTrashFilesFromDiskWorker(uint16_t workerId) {
	std::string threadName = "purgeWorker " + std::to_string(workerId);
	pthread_setname_np(pthread_self(), threadName.c_str());

	auto &state = trashPurgerState();

	uint32_t jobId;
	uint32_t operation;
	uint8_t *jobPtrArg;

	while (true) {
		state.queue->get(&jobId, &operation, &jobPtrArg, nullptr);

		if (operation == TrashPurgerOperation::Exit) { break; }
		auto *raw = reinterpret_cast<TrashRemovalJob *>(jobPtrArg);

		std::unique_ptr<TrashRemovalJob> job(raw);

		const ChunkTrashIndex::TrashIndexFileEntries &filesToRemove = job->files;
		const std::string &diskPath = job->diskPath;

		for (const auto &fileEntry : filesToRemove) {
			if (removeFileFromTrash(fileEntry.second) != SAUNAFS_STATUS_OK) { continue; }
			HddStats::gStatsOperationsGCPurge++;
			getTrashIndex().remove(fileEntry.first, fileEntry.second, diskPath);
		}

		state.jobsCount--;
		std::unique_lock<std::mutex> trashPurgerLock(state.mutex);
		state.cv.notify_one();
	}
}

fs::path ChunkTrashManagerImpl::getTrashDir(const fs::path &diskPath) {
	return diskPath / ChunkTrashManager::kTrashDirname;
}

int ChunkTrashManagerImpl::registerDiskPath(const std::string &diskPath) {
	const fs::path trashDir = getTrashDir(diskPath);

	if (!fs::exists(trashDir)) {
		std::error_code errorCode_;
		fs::create_directories(trashDir, errorCode_);
		if (errorCode_) {
			safs::log_error_code(errorCode_, "Failed to create trash directory: {}",
			                     trashDir.string().c_str());
			return SAUNAFS_ERROR_NOTDONE;
		}
	}

	getTrashIndex().erase(diskPath);

	for (const auto &file : fs::recursive_directory_iterator(trashDir)) {
		if (fs::is_regular_file(file) && isTrashPath(file.path().string())) {
			const std::string filename = file.path().filename().string();
			const std::string deletionTimeStr = filename.substr(filename.find_last_of('.') + 1);
			if (!isValidTimestampFormat(deletionTimeStr)) {
				safs::log_error_code(SAUNAFS_ERROR_EINVAL,
				                     "Invalid timestamp format in file: {}, skipping.",
				                     file.path().string().c_str());
				continue;
			}
			int errorCode;
			const std::time_t deletionTime = getTimeFromString(deletionTimeStr, errorCode);
			if (errorCode != SAUNAFS_STATUS_OK) {
				safs::log_error_code(static_cast<error_type>(errorCode),
				                     "Failed to parse deletion time from file: {}, skipping.",
				                     file.path().string().c_str());
				continue;
			}
			getTrashIndex().add(deletionTime, file.path().string(), diskPath);
		}
	}

	return SAUNAFS_STATUS_OK;
}

void ChunkTrashManagerImpl::eraseDisk(const std::string &diskPath) {
	getTrashIndex().erase(diskPath);
}

void ChunkTrashManagerImpl::terminate() {
	auto &state = trashPurgerState();

	if (!state.queue) {
		state.threads.clear();
		state.jobsCount.store(0, std::memory_order_relaxed);
		return;
	}

	for (size_t i = 0; i < state.threads.size(); i++) {
		state.queue->put(0, TrashPurgerOperation::Exit, nullptr, 1);
	}

	for (auto &thread : state.threads) {
		if (thread.joinable()) { thread.join(); }
	}

	state.threads.clear();
	state.queue.reset();
	state.jobsCount.store(0, std::memory_order_relaxed);
}

bool ChunkTrashManagerImpl::isValidTimestampFormat(const std::string &timestamp) {
	return timestamp.size() == kTimeStampLength && std::ranges::all_of(timestamp, ::isdigit);
}

void ChunkTrashManagerImpl::removeExpiredFiles(const time_t &timeLimit, size_t bulkSize) const {
	auto expiredFilesCollection = getTrashIndex().getExpiredFiles(timeLimit, bulkSize);
	removeTrashFiles(expiredFilesCollection);
}

size_t ChunkTrashManagerImpl::checkAvailableSpace(const std::string &diskPath) {
	struct statvfs stat {};
	if (statvfs(diskPath.c_str(), &stat) != 0) {
		safs::log_error_code(errno, "Failed to get file system statistics");
		return 0;
	}
	constexpr size_t kGiBMultiplier = 1 << 30;
	size_t const availableGb = stat.f_bavail * stat.f_frsize / kGiBMultiplier;
	return availableGb;
}

void ChunkTrashManagerImpl::makeSpace(const std::string &diskPath,
                                      const size_t spaceAvailabilityThreshold,
                                      const size_t recoveryStep) const {
	size_t availableSpace = checkAvailableSpace(diskPath);
	while (availableSpace < spaceAvailabilityThreshold) {
		const auto olderFilesCollection = getTrashIndex().getOlderFiles(diskPath, recoveryStep);
		if (olderFilesCollection.empty()) { break; }
		ChunkTrashIndex::TrashIndexDiskEntries temp = {{diskPath, olderFilesCollection}};
		removeTrashFiles(temp);
		availableSpace = checkAvailableSpace(diskPath);
	}
}

void ChunkTrashManagerImpl::makeSpace(const size_t spaceAvailabilityThreshold,
                                      const size_t recoveryStep) const {
	for (const auto &diskPath : getTrashIndex().getDiskPaths()) {
		makeSpace(diskPath, spaceAvailabilityThreshold, recoveryStep);
	}
}

double ChunkTrashManagerImpl::computeGCThrottlingFactor(uint64_t currentBytesRead,
                                                        uint64_t currentBytesWrite,
                                                        uint64_t currentDiskCount) {
	currentBytesRead /= currentDiskCount;
	currentBytesWrite /= currentDiskCount;

	// Exponential decay applied on each GC cycle to gradually lower the learned
	// per-disk I/O baseline after sustained periods of lower activity.
	constexpr double kMaxIoDecayFactor = 0.99997;

	maxBytesReadPerDisk = std::max({uint64_t(maxBytesReadPerDisk * kMaxIoDecayFactor),
	                                currentBytesRead, kNumberOfBytesPerMebiByte});
	maxBytesWritePerDisk = std::max({uint64_t(maxBytesWritePerDisk * kMaxIoDecayFactor),
	                                 currentBytesWrite, kNumberOfBytesPerMebiByte});

	double totalIOPercentage =
	    (static_cast<double>(currentBytesRead) * 100.0 / maxBytesReadPerDisk) +
	    (static_cast<double>(currentBytesWrite) * 100.0 / maxBytesWritePerDisk);

	/// Inverted sigmoid mapping a value in [0,100] to (0,1].
	/// https://en.wikipedia.org/wiki/Sigmoid_function
	///
	/// kSigmoidCenter: midpoint of the curve (f(val) = 0.5 when val/100 == center).
	/// kSigmoidSteepness: controls how sharp the transition is around the midpoint.
	/// Larger values make the drop steeper.
	///
	/// Input is normalized to [0,1] before applying the sigmoid.
	auto invertedSigmoid = [](double val) -> double {
		const double kSigmoidSteepness = 10.0;       // steepness
		const double kSigmoidCenter = 0.15;          // center in [0,1]
		const double kSigmoidValNorm = val / 100.0;  // normalize so that 100% total I/O maps to 1.0

		const double res =
		    1.0 / (1.0 + std::exp(-kSigmoidSteepness * (kSigmoidValNorm - kSigmoidCenter)));
		return 1.0 - res;
	};

	constexpr uint32_t kIoSpikeRatioThreshold = 10;

	double throttlingFactor = invertedSigmoid(totalIOPercentage);

	double ioSpikeRatio =
	    (static_cast<double>(currentBytesRead) + 1) / (previousBytesReadPerDisk + 1) +
	    (static_cast<double>(currentBytesWrite) + 1) / (previousBytesWritePerDisk + 1);

	if (ioSpikeRatio >= kIoSpikeRatioThreshold) { throttlingFactor = 0; }

	previousBytesReadPerDisk = currentBytesRead;
	previousBytesWritePerDisk = currentBytesWrite;

	return throttlingFactor;
}

void ChunkTrashManagerImpl::collectGarbage() {
	std::time_t const currentTime = std::time(nullptr);
	std::time_t const expirationTime = currentTime - trashTimeLimitSeconds;

	uint64_t currentBytesWrite = HddStats::gBytesWrittenSinceLastGCSweep.exchange(0);
	uint64_t currentBytesRead = HddStats::gBytesReadSinceLastGCSweep.exchange(0);

	uint64_t currentDiskCount = 1;
	{
		std::lock_guard disksLockGuard(gDisksMutex);
		currentDiskCount = std::max(currentDiskCount, gDisks.size());
	}

	double factor =
	    computeGCThrottlingFactor(currentBytesRead, currentBytesWrite, currentDiskCount);

	uint64_t bulkSizeScaled = trashGarbageCollectorBulkSize * factor;

	static constexpr uint64_t kMinGCBulkSizeForActivation = 5;

	if (bulkSizeScaled >= kMinGCBulkSizeForActivation) {
		removeExpiredFiles(expirationTime, bulkSizeScaled);
	}
	makeSpace(availableThresholdGB, garbageCollectorSpaceRecoveryStep);
}

bool ChunkTrashManagerImpl::isTrashPath(const std::string &filePath) {
	return filePath.find("/" + ChunkTrashManager::kTrashDirname + "/") != std::string::npos;
}

ChunkTrashManagerImpl::error_type ChunkTrashManagerImpl::removeFileFromTrash(
    const std::string &filePath) {
	if (!isTrashPath(filePath)) {
		safs::log_error_code(SAUNAFS_ERROR_EINVAL, "Invalid trash path: {}", filePath.c_str());
		return SAUNAFS_ERROR_EINVAL;
	}
	std::error_code errorCode;
	fs::remove(filePath, errorCode);  // Remove the file or directory
	if (errorCode) {
		safs::log_error_code(errorCode, "Failed to remove file or directory: {}", filePath.c_str());
		return SAUNAFS_ERROR_NOTDONE;
	}

	return SAUNAFS_STATUS_OK;
}
