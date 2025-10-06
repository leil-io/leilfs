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
#include <memory>

#include "chunkserver-common/chunk_trash_manager.h"
#include "chunkserver-common/chunk_trash_manager_impl.h"
#include "config/cfg.h"
#include "errors/saunafs_error_codes.h"
#include "slogger/slogger.h"

u_short ChunkTrashManager::isEnabled = 0;
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
	std::lock_guard<std::mutex> lock(implMutex);

	auto &impl = getImpl();
	if (!impl) {
		safs::log_error_code(SAUNAFS_ERROR_EINVAL,
		                     "ChunkTrashManager implementation not initialized");
		return SAUNAFS_ERROR_NOTDONE;
	}
	return impl->moveToTrash(filePath, diskPath, deletionTime);
}

int ChunkTrashManager::init(const std::string &diskPath) {
	reloadConfig();

	// Protect against concurrent access
	std::lock_guard<std::mutex> lock(implMutex);

	auto &impl = getImpl();
	if (!impl) {
		safs::log_error_code(SAUNAFS_ERROR_EINVAL,
		                     "ChunkTrashManager implementation not initialized");
		return SAUNAFS_ERROR_NOTDONE;
	}
	return impl->init(diskPath);
}

void ChunkTrashManager::collectGarbage() {
	if (!isEnabled) { return; }

	// Protect against concurrent access
	std::lock_guard<std::mutex> lock(implMutex);

	auto &impl = getImpl();
	if (!impl) {
		safs::log_error_code(SAUNAFS_ERROR_EINVAL,
		                     "ChunkTrashManager implementation not initialized");
		return;
	}
	impl->collectGarbage();
}

void ChunkTrashManager::reloadConfig() {
	// Protect against concurrent access
	std::lock_guard<std::mutex> lock(implMutex);

	auto &impl = getImpl();
	if (!impl) {
		safs::log_error_code(SAUNAFS_ERROR_EINVAL,
		                     "ChunkTrashManager implementation not initialized");
		return;
	}

	isEnabled = cfg_get("CHUNK_TRASH_ENABLED", static_cast<u_short>(0));
	safs::log_debug("Chunk trash manager is {}", isEnabled ? "enabled" : "disabled");
	impl->reloadConfig();
}
