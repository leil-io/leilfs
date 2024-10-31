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

#include <string>

class IChunkTrashManagerImpl {
public:
	virtual ~IChunkTrashManagerImpl() = default;

	virtual int
	moveToTrash(const std::filesystem::path &, const std::filesystem::path &,
	            const std::time_t &) = 0;

	virtual int init(const std::string &) = 0;

	virtual void collectGarbage() = 0;

	virtual void reloadConfig() = 0;
};

/**
 * @brief Manages the trash files in the system.
 *
 * This class provides functionality to initialize the trash, move files to the trash,
 * and manage expired files. It enforces singleton behavior to ensure only one instance
 * of the manager is active.
 */
class ChunkTrashManager {
public:
	using Implentation = IChunkTrashManagerImpl;
	using ImplentationPtr = std::shared_ptr<Implentation>;

	/**
	 * @brief Gets the singleton instance of the ChunkTrashManager.
	 *
	 * @return Reference to the singleton instance of ChunkTrashManager.
	 */
	static ChunkTrashManager &instance(ImplentationPtr newImpl = nullptr);

	/**
	 * @brief The name of the trash directory.
	 */
	static constexpr const std::string kTrashDirname = ".trash.bin";

	static u_short isEnabled; ///< Flag to enable or disable the trash manager.

	/**
	 * @brief Initializes the trash directory for the specified disk.
	 *
	 * @param diskPath The path of the disk where the trash directory will be initialized.
	 */
	int init(const std::string &diskPath);

	/**
	 * @brief Moves a file to the trash directory.
	 *
	 * @param filePath The path of the file to be moved.
	 * @param diskPath The path to the disk containing the file.
	 * @param deletionTime The time when the file was deleted.
	 * @return 0 on success, error code otherwise.
	 */
	int moveToTrash(const std::filesystem::path &filePath,
	                const std::filesystem::path &diskPath,
	                const std::time_t &deletionTime);

	/**
	 * @brief Runs the garbage collection process, which includes
	 * removing expired files, freeing up disk space, and cleaning
	 * empty directories.
	 */
	void collectGarbage();

	/// Reloads the configuration for the trash manager.
	void reloadConfig();

	// Deleted to enforce singleton behavior
	ChunkTrashManager(const ChunkTrashManager &) = delete;
	ChunkTrashManager &operator=(const ChunkTrashManager &) = delete;
	ChunkTrashManager(ChunkTrashManager &&) = delete;
	ChunkTrashManager &operator=(ChunkTrashManager &&) = delete;

	~ChunkTrashManager() = default; ///< Destructor

private:
	/// Constructor is private to enforce singleton behavior
	ChunkTrashManager() = default;

	/// Pointer to the singleton instance of the trash manager implementation.
	static ChunkTrashManager::ImplentationPtr pImpl;

};
