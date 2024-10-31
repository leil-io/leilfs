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

#include <map>
#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>

#include "chunk_trash_manager.h"
#include "chunk_trash_index.h"

class ChunkTrashManagerImpl : public IChunkTrashManagerImpl {
	using TrashIndex = ChunkTrashIndex;
public:
	/// A string representing the guard that distinguishes trash directories.
	static constexpr const std::string kTrashGuardString =
			"/" + (ChunkTrashManager::kTrashDirname) + "/";

	/// The format of the timestamp used in file names, representing the deletion time.
	static constexpr const std::string kTimeStampFormat = "%Y%m%d%H%M%S";

	/// Length of the timestamp string expected in file names.
	static constexpr const u_short kTimeStampLength = 14;

	/**
	 * @brief Initializes the trash manager for the specified disk path.
	 * @param diskPath The path of the disk to be initialized.
	 * @return Status code indicating success or failure.
	 */
	int init(const std::string &diskPath) override;

	/**
	 * @brief Moves a specified file to the trash directory.
	 * @param filePath The path of the file to be moved.
	 * @param diskPath The target disk path for the trash location.
	 * @param deletionTime The time at which the file was marked for deletion.
	 * @return Status code indicating success or failure.
	 */
	int moveToTrash(const std::filesystem::path &filePath,
	                const std::filesystem::path &diskPath,
	                const std::time_t &deletionTime) override;

	/**
	 * @brief Converts a given time value to a formatted string representation.
	 * @param time1 The time to be converted.
	 * @return A string representation of the time.
	 */
	static std::string getTimeString(time_t time1);

	/**
	 * @brief Removes files from the trash that are older than a specified time limit.
	 * @param timeLimit The cutoff time; files older than this will be removed.
	 * @param bulkSize The number of files to process in a batch operation.
	 */
	void removeExpiredFiles(const std::time_t &timeLimit, size_t bulkSize = 0);

	/**
	 * @brief Removes a set of specified files from the trash.
	 * @param filesToRemove The list of files to be permanently deleted.
	 */
	void removeTrashFiles(const ChunkTrashIndex::TrashIndexDiskEntries
	                      &filesToRemove);

	/**
	 * @brief Checks if a given timestamp string matches the expected format.
	 * @param timestamp The timestamp string to validate.
	 * @return True if the timestamp format is valid; otherwise, false.
	 */
	static bool isValidTimestampFormat(const std::string &timestamp);

	/**
	 * @brief Recursively cleans empty folders from the specified directory.
	 * @param directory The directory in which to clean up empty folders.
	 */
	static void cleanEmptyFolders(const std::string &directory);

	/**
	 * @brief Cleans empty folders from the trash directories.
	 */
	void cleanEmptyFolders();

	/**
	 * @brief Frees up disk space by removing files from the trash if the available space falls below a threshold.
	 * @param spaceAvailabilityThreshold The minimum required space in GB before taking action.
	 * @param recoveryStep The number of files to delete in each step until sufficient space is recovered.
	 */
	void makeSpace(size_t spaceAvailabilityThreshold, size_t recoveryStep);

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
	time_t getTimeFromString(const std::string &timeString, int &errorCode);

	/**
	 * @brief Checks the available disk space on the specified disk path.
	 * @param diskPath The path of the disk to check.
	 * @return The amount of available space in GB.
	 */
	static size_t checkAvailableSpace(const std::string &diskPath);

	/**
	 * @brief Frees up disk space on the specified disk by removing trash files.
	 * @param diskPath The path of the disk where space should be freed.
	 * @param spaceAvailabilityThreshold The minimum required space in GB before taking action.
	 * @param recoveryStep The number of files to delete in each step until sufficient space is recovered.
	 */
	void makeSpace(const std::string &diskPath,
	               size_t spaceAvailabilityThreshold,
	               size_t recoveryStep);

	/**
	 * @brief Reloads the configuration settings for the trash manager.
	 * Assuming that the configuration is already loaded by the ChunkServer.
	 */
	void reloadConfig() override;

private:
	/// Pointer to the singleton instance of the trash index used for managing trash files.
	TrashIndex *trashIndex = &ChunkTrashIndex::instance();

	/// Minimum available space threshold (in GB) before triggering garbage collection.
	static size_t kAvailableThresholdGB;

	/// Time limit (in seconds) for files to be considered eligible for deletion.
	static size_t kTrashTimeLimitSeconds;

	/// Number of files processed in each bulk operation during garbage collection.
	static size_t kTrashGarbageCollectorBulkSize;

	/// Number of files to remove in each step to free up space when required.
	static size_t kGarbageCollectorSpaceRecoveryStep;

	/**
	 * @brief Gets the trash directory for the specified disk path.
	 * @param diskPath The path of the disk to get the trash directory for.
	 * @return The path to the trash directory.
	 */
	static std::filesystem::path getTrashDir(const std::filesystem::path
	                                         &diskPath);

	/**
	 * @brief Gets the destination path for a file to be moved to the trash.
	 * @param filePath The path of the file to be moved.
	 * @param sourceRoot The root directory of the source file.
	 * @param destinationRoot The root directory of the destination file.
	 * @param destinationPath The path to the destination file (output).
	 * @return Status code indicating success or failure.
	 */
	int getMoveDestinationPath(const std::string &filePath,
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
	static int removeFileFromTrash(const std::string &filePath);

};
