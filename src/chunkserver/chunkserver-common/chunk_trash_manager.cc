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

#include <syslog.h>
#include <atomic>
#include <memory>

#include "chunkserver-common/chunk_trash_manager.h"
#include "chunkserver-common/chunk_trash_manager_impl.h"
#include "config/cfg.h"
#include "errors/saunafs_error_codes.h"
#include "slogger/slogger.h"

std::atomic<uint8_t> ChunkTrashManager::isEnabled = 0;
std::mutex ChunkTrashManager::implMutex;

// Using the Meyer's singleton pattern to ensure proper initialization and
// cleanup
ChunkTrashManager::ImplementationPtr &ChunkTrashManager::getImpl() {
	static ImplementationPtr instance = std::make_shared<ChunkTrashManagerImpl>();
	return instance;
}

void ChunkTrashManager::setImpl(ImplementationPtr newImpl) {
	// Protect against concurrent access
	std::lock_guard<std::mutex> lock(implMutex);
	if (!newImpl) {
		safs::log_error_code(SAUNAFS_ERROR_EINVAL,
		                     "Attempt to set null implementation for ChunkTrashManager");
		return;  // Don't set null implementation
	}
	getImpl() = newImpl;
}

int ChunkTrashManager::moveToTrash(const std::filesystem::path &filePath,
                                   const std::filesystem::path &diskPath,
                                   const std::time_t &deletionTime) {
	if (!isEnabled) { return 0; }

	// Protect against concurrent access
	ImplementationPtr impl;
	{
		std::lock_guard<std::mutex> lock(implMutex);
		impl = getImpl();
	}
	if (!impl) {
		safs::log_error_code(SAUNAFS_ERROR_EINVAL,
		                     "ChunkTrashManager implementation not initialized");
		return SAUNAFS_ERROR_NOTDONE;
	}
	return impl->moveToTrash(filePath, diskPath, deletionTime);
}

void ChunkTrashManager::init() {
	// Protect against concurrent access
	ImplementationPtr impl;
	{
		std::lock_guard<std::mutex> lock(implMutex);
		impl = getImpl();
	}
	if (!impl) {
		safs::log_error_code(SAUNAFS_ERROR_EINVAL,
		                     "ChunkTrashManager implementation not initialized");
		return;
	}
	impl->init();
	reloadConfig();
}

int ChunkTrashManager::registerDiskPath(const std::string &diskPath) {
	// Protect against concurrent access
	ImplementationPtr impl;
	{
		std::lock_guard<std::mutex> lock(implMutex);
		impl = getImpl();
	}
	if (!impl) {
		safs::log_error_code(SAUNAFS_ERROR_EINVAL,
		                     "ChunkTrashManager implementation not initialized");
		return SAUNAFS_ERROR_NOTDONE;
	}
	return impl->registerDiskPath(diskPath);
}

void ChunkTrashManager::eraseDisk(const std::string &diskPath) {
	// Protect against concurrent access
	ImplementationPtr impl;
	{
		std::lock_guard<std::mutex> lock(implMutex);
		impl = getImpl();
	}
	if (!impl) {
		safs::log_error_code(SAUNAFS_ERROR_EINVAL,
		                     "ChunkTrashManager implementation not initialized");
		return;
	}
	impl->eraseDisk(diskPath);
}

void ChunkTrashManager::terminate() {
	// Protect against concurrent access
	ImplementationPtr impl;
	{
		std::lock_guard<std::mutex> lock(implMutex);
		impl = getImpl();
	}
	if (!impl) {
		safs::log_error_code(SAUNAFS_ERROR_EINVAL,
		                     "ChunkTrashManager implementation not initialized");
		return;
	}
	impl->terminate();
}

void ChunkTrashManager::collectGarbage() {
	// Protect against concurrent access
	ImplementationPtr impl;
	{
		std::lock_guard<std::mutex> lock(implMutex);
		impl = getImpl();
	}
	if (!impl) {
		safs::log_error_code(SAUNAFS_ERROR_EINVAL,
		                     "ChunkTrashManager implementation not initialized");
		return;
	}
	impl->collectGarbage();
}

void ChunkTrashManager::reloadConfig() {
	// Protect against concurrent access
	ImplementationPtr impl;
	{
		std::lock_guard<std::mutex> lock(implMutex);
		impl = getImpl();
	}
	if (!impl) {
		safs::log_error_code(SAUNAFS_ERROR_EINVAL,
		                     "ChunkTrashManager implementation not initialized");
		return;
	}

	isEnabled = cfg_get("CHUNK_TRASH_ENABLED", static_cast<unsigned short>(kDefaultIsEnabled));
	safs::log_info("Chunk trash manager is {}", isEnabled ? "enabled" : "disabled");
	impl->reloadConfig();
}
