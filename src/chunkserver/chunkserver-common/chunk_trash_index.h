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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"

#include <filesystem>

#pragma GCC diagnostic pop

#include <map>
#include <unordered_map>
#include <string>
#include <ctime>
#include <mutex>
#include <vector>

/**
 * @brief Manages the index of files in the chunk trash.
 *
 * This class provides functionality to add, remove, and retrieve files
 * based on their deletion time, ensuring thread safety with mutex protection.
 */
class ChunkTrashIndex {
public:
	using TrashIndexFileEntries = std::multimap<std::time_t, std::string>; ///< Type for storing file entries with their deletion time.
	using TrashIndexDiskEntries = std::unordered_map<std::string, TrashIndexFileEntries>; ///< Type for storing disk path entries and their associated file entries.
	using TrashIndexType = TrashIndexDiskEntries; ///< Alias for the trash index type.

	/**
	 * @brief Gets the singleton instance of the ChunkTrashIndex.
	 *
	 * @return Reference to the singleton instance of ChunkTrashIndex.
	 */
	static ChunkTrashIndex &instance();

	/**
	 * @brief Resets the trash index for a specific disk path.
	 *
	 * This method clears all entries associated with the specified disk path.
	 *
	 * @param diskPath The path of the disk whose index will be reset.
	 */
	void reset(const std::filesystem::path &diskPath);

	/**
	 * @brief Retrieves expired files from the trash index.
	 *
	 * This method returns a map of expired files across all disks with the
	 * specified time limit and bulk size.
	 *
	 * @param timeLimit The time limit to determine expired files.
	 * @param bulkSize The maximum number of files to retrieve (default is 0, which means no limit).
	 * @return A map containing expired files.
	 */
	TrashIndexDiskEntries getExpiredFiles(const std::time_t &timeLimit,
	                                      size_t bulkSize = 0);

	/**
	 * @brief Adds a file entry to the trash index with its deletion time.
	 *
	 * @param deletionTime The time when the file was deleted.
	 * @param filePath The path of the file being added.
	 * @param diskPath The path of the disk associated with the file.
	 */
	void add(const std::time_t &deletionTime, const std::string &filePath,
	         const std::string &diskPath);

	/**
	 * @brief Removes a file entry from the trash index by its deletion time and path.
	 *
	 * @param deletionTime The time when the file was deleted.
	 * @param filePath The path of the file being removed.
	 */
	void remove(const time_t &deletionTime, const std::string &filePath);

	/**
	 * @brief Removes a file entry from the trash index for a specific disk path.
	 *
	 * @param deletionTime The time when the file was deleted.
	 * @param filePath The path of the file being removed.
	 * @param diskPath The path of the disk associated with the file.
	 */
	void remove(const time_t &deletionTime, const std::string &filePath,
	            const std::string &diskPath);

	// Deleted to enforce singleton behavior
	ChunkTrashIndex(
			const ChunkTrashIndex &) = delete; ///< Copy constructor is deleted.

	ChunkTrashIndex &operator=(
			const ChunkTrashIndex &) = delete; ///< Copy assignment operator is deleted.

	ChunkTrashIndex(
			ChunkTrashIndex &&) = delete; ///< Move constructor is deleted.

	ChunkTrashIndex &operator=(
			ChunkTrashIndex &&) = delete; ///< Move assignment operator is deleted.

	TrashIndexFileEntries
	getOlderFiles(const std::string &diskPath, const size_t removalStepSize);

	std::vector<std::string> getDiskPaths();

private:
	// Constructor is private to enforce singleton behavior
	ChunkTrashIndex() = default; ///< Default constructor.

	~ChunkTrashIndex() = default; ///< Destructor.


	TrashIndexType trashIndex; ///< The main data structure holding the trash index.
	std::mutex trashIndexMutex; ///< Mutex for thread-safe access to the trash index.



	/**
	 * @brief Retrieves expired files from the trash index for a specific disk path.
	 *
	 * This method populates the provided expiredFiles map with entries that
	 * have a deletion time earlier than the specified time limit.
	 *
	 * @param diskPath The path of the disk to retrieve expired files from.
	 * @param timeLimit The time limit to determine expired files.
	 * @param expiredFiles Reference to a map that will be populated with expired files.
	 * @param bulkSize The maximum number of files to retrieve (default is 0, which means no limit).
	 * @return The number of expired files retrieved.
	 */
	size_t getExpiredFilesInternal(const std::filesystem::path &diskPath,
	                               const std::time_t &timeLimit,
	                               std::unordered_map<std::string, std::multimap<std::time_t, std::string>> &expiredFiles,
	                               size_t bulkSize = 0);

	/**
	 * @brief Retrieves expired files from the trash index.
	 *
	 * This method returns a map of expired files across all disks with the
	 * specified time limit and bulk size.
	 *
	 * @param timeLimit The time limit to determine expired files.
	 * @param bulkSize The maximum number of files to retrieve (default is 0, which means no limit).
	 * @return A map containing expired files.
	 */
	TrashIndexDiskEntries getExpiredFilesInternal(const std::time_t &timeLimit,
	                                              size_t bulkSize = 0);

	/**
	 * @brief Removes a file entry from the trash index for a specific disk path.
	 * @param deletionTime
	 * @param filePath
	 * @param diskPath
	 */
	void removeInternal(const time_t &deletionTime, const std::string &filePath,
	                    const std::string &diskPath);
};
