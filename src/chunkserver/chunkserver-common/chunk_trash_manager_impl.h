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

#pragma once

#include "common/platform.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "chunkserver-common/chunk_trash_index.h"
#include "chunkserver-common/chunk_trash_manager.h"
#include "common/pcqueue.h"
#include "errors/saunafs_error_codes.h"

class ChunkTrashManagerImpl : public IChunkTrashManagerImpl {
	using TrashIndex = ChunkTrashIndex;
	// Custom error type for the trash manager.
	using error_type = saunafs_error_code;

public:
	/// A string representing the guard that distinguishes trash directories.
	static const std::string kTrashGuardString;

	/// The format of the timestamp used in file names, representing the
	/// deletion time.
	static constexpr std::string kTimeStampFormat = "%Y%m%d%H%M%S";

	/// Length of the timestamp string expected in file names.
	static constexpr unsigned short kTimeStampLength = 14;

	/**
	 * @brief Initializes the chunk trash manager.
	 */
	void init() override;

	/**
	 * @brief Initializes the trash directory for the specified disk path.
	 * @param diskPath The path of the disk where the trash directory will be initialized.
	 * @return Status code indicating success or failure.
	 */
	int registerDiskPath(const std::string &diskPath) override;

	/**
	 * @brief Erases the trash directory information for the specified disk.
	 *
	 * @param diskPath Path of the disk whose trash directory information will be erased.
	 */
	void eraseDisk(const std::string &diskPath) override;

	/**
	 * @brief Join the background threads.
	 */
	void terminate() override;

	/**
	 * @brief Moves a specified file to the trash directory.
	 * @param filePath The path of the file to be moved.
	 * @param diskPath The target disk path for the trash location.
	 * @param deletionTime The time at which the file was marked for deletion.
	 * @return Status code indicating success or failure.
	 */
	int moveToTrash(const std::filesystem::path &filePath, const std::filesystem::path &diskPath,
	                const std::time_t &deletionTime) override;

	/**
	 * @brief Converts a given time value to a formatted string representation.
	 * @param time The time to be converted.
	 * @return A string representation of the time.
	 */
	static std::string getStringFromTime(time_t time);

	/**
	 * @brief Removes files from the trash that are older than a specified time
	 * limit.
	 * @param timeLimit The cutoff time; files older than this will be removed.
	 * @param bulkSize The number of files to process in a batch operation.
	 */
	void removeExpiredFiles(const std::time_t &timeLimit, size_t bulkSize = 0) const;

	/**
	 * @brief Removes a set of specified files from the trash.
	 * @param filesToRemove The list of files to be permanently deleted.
	 */
	void removeTrashFiles(ChunkTrashIndex::TrashIndexDiskEntries &filesToRemove) const;

	/**
	 * @brief Worker executing removeTrashFiles jobs.
	 * @param workId The id of this worker.
	 */
	static void removeTrashFilesFromDiskWorker(uint16_t workerId);

	/**
	 * @brief Checks if a given timestamp string matches the expected format.
	 * @param timestamp The timestamp string to validate.
	 * @return True if the timestamp format is valid; otherwise, false.
	 */
	static bool isValidTimestampFormat(const std::string &timestamp);

	/**
	 * @brief Frees up disk space by removing files from the trash if the
	 * available space falls below a threshold.
	 * @param spaceAvailabilityThreshold The minimum required space in GB before
	 * taking action.
	 * @param recoveryStep The number of files to delete in each step until
	 * sufficient space is recovered.
	 */
	void makeSpace(size_t spaceAvailabilityThreshold, size_t recoveryStep) const;

	/**
	 * @brief Calculates the GC throttling factor based on recent disk I/O.
	 *
	 * Uses the sampled read/write bytes since the last GC sweep, normalized per disk,
	 * to update the learned peak I/O baselines (with exponential decay) and compute
	 * a throttling factor in the range [0.0, 1.0].
	 *
	 * The factor decreases as the current I/O load approaches the learned peak load.
	 * Sudden I/O spikes relative to the previous sweep force the factor to 0 in order
	 * to temporarily suppress expired-file garbage collection.
	 *
	 * @param currentBytesRead  Bytes read since the previous GC sweep.
	 * @param currentBytesWrite Bytes written since the previous GC sweep.
	 * @param currentDiskCount  Number of active disks used to normalize the I/O load.
	 *
	 * @note Updates the internal GC throttling state (baseline and previous I/O values).
	 *
	 * @return A factor in [0.0, 1.0] used to scale the expired-file GC bulk size.
	 */
	static double computeGCThrottlingFactor(uint64_t currentBytesRead, uint64_t currentBytesWrite,
	                                        uint64_t currentDiskCount);

	/**
	 * @brief Runs the garbage collection process, which includes
	 * removing expired files, freeing up disk space, and cleaning
	 * empty directories.
	 */
	void collectGarbage() override;

	/**
	 * @brief Converts a formatted timestamp string to a time value.
	 * @param timeString The string representation of the timestamp.
	 * @param errorCode (output) The value to return in case of an error.
	 * @return The corresponding time value.
	 */
	static time_t getTimeFromString(const std::string &timeString, int &errorCode);

	/**
	 * @brief Checks the available disk space on the specified disk path.
	 * @param diskPath The path of the disk to check.
	 * @return The amount of available space in GB.
	 */
	static size_t checkAvailableSpace(const std::string &diskPath);

	/**
	 * @brief Frees up disk space on the specified disk by removing trash files.
	 * @param diskPath The path of the disk where space should be freed.
	 * @param spaceAvailabilityThreshold The minimum required space in GB before
	 * taking action.
	 * @param recoveryStep The number of files to delete in each step until
	 * sufficient space is recovered.
	 */
	void makeSpace(const std::string &diskPath, size_t spaceAvailabilityThreshold,
	               size_t recoveryStep) const;

	/**
	 * @brief Reloads the configuration settings for the trash manager.
	 * Assuming that the configuration is already loaded by the ChunkServer.
	 */
	void reloadConfig() override;

private:
	/// Maximum recorded BytesReadPerDisk and BytesWritePerDisk in a GC cycle
	static uint64_t maxBytesReadPerDisk;
	static uint64_t maxBytesWritePerDisk;

	/// Previous recorded BytesReadPerDisk and BytesWritePerDisk in a GC cycle
	static uint64_t previousBytesReadPerDisk;
	static uint64_t previousBytesWritePerDisk;

	/// Minimum available space threshold (in GB) before triggering garbage
	/// collection.
	static std::atomic<size_t> availableThresholdGB;
	static constexpr size_t kDefaultAvailableThresholdGB = 0;

	/// Time limit (in seconds) for files to be considered eligible for
	/// deletion.
	static std::atomic<size_t> trashTimeLimitSeconds;
	static constexpr size_t kDefaultTrashTimeLimitSeconds = 259200;

	/// Number of files processed in each bulk operation during garbage
	/// collection.
	static std::atomic<size_t> trashGarbageCollectorBulkSize;
	static constexpr size_t kDefaultTrashGarbageCollectorBulkSize = 500;

	/// Number of files to remove in each step to free up space when required.
	static std::atomic<size_t> garbageCollectorSpaceRecoveryStep;
	static constexpr size_t kDefaultGarbageCollectorSpaceRecoveryStep = 100;

	/// Number of purge workers.
	static uint16_t numberOfTrashPurgeWorkers;
	static constexpr uint16_t kDefaultNumberOfTrashPurgeWorkers = 5;
	static constexpr uint16_t kMinNumberOfTrashPurgeWorkers = 1;
	static constexpr uint16_t kMaxNumberOfTrashPurgeWorkers = 32;

	// Use a function to safely get the ChunkTrashIndex instance
	// This prevents issues during program shutdown
	/**
	 * @brief Gets the singleton instance of the ChunkTrashIndex.
	 * @return Reference to the singleton instance of ChunkTrashIndex.
	 */
	static TrashIndex &getTrashIndex() { return TrashIndex::instance(); }

	/**
	 * @brief Gets the trash directory for the specified disk path.
	 * @param diskPath The path of the disk to get the trash directory for.
	 * @return The path to the trash directory.
	 */
	static std::filesystem::path getTrashDir(const std::filesystem::path &diskPath);

	/**
	 * @brief Gets the destination path for a file to be moved to the trash.
	 * @param filePath The path of the file to be moved.
	 * @param sourceRoot The root directory of the source file.
	 * @param destinationRoot The root directory of the destination file.
	 * @param destinationPath The path to the destination file (output).
	 * @return Status code indicating success or failure.
	 */
	static error_type getMoveDestinationPath(const std::string &filePath,
	                                         const std::string &sourceRoot,
	                                         const std::string &destinationRoot,
	                                         std::string &destinationPath);

	/**
	 * @brief Checks the belonging of a file to the trash directory.
	 * @param filePath The path of the file to check.
	 * @return True if the file is in the trash directory; otherwise, false.
	 */
	static bool isTrashPath(const std::string &filePath);

	/**
	 * @brief Does the actual work of removing a file from the trash.
	 * @param filePath The path of the file to be removed.
	 * @return Status code indicating success or failure.
	 */
	static error_type removeFileFromTrash(const std::string &filePath);

	friend class ChunkTrashManagerImplTest;
};
