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

#include "chunk_trash_manager.h"
#include "errors/saunafs_error_codes.h"

std::string ChunkTrashManager::getDeletionTimeString() {
	auto now = std::chrono::system_clock::now();
	const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
	std::tm* utcTime = std::gmtime(&nowTime);  // Convert to UTC

	std::ostringstream oss;
	oss << std::put_time(utcTime, "%Y%m%d%H%M%S");
	return oss.str();
}

int ChunkTrashManager::moveToTrash(const std::filesystem::path &filePath,
                                   const std::filesystem::path &diskPath,
                                   const std::string &deletionTime) {
	if (!std::filesystem::exists(filePath)) {
		return SAUNAFS_ERROR_ENOENT;
	}

	const std::filesystem::path trashDir = diskPath / kTrashDirname;
	std::filesystem::create_directories(trashDir);

	if (!filePath.string().starts_with(diskPath.string())) {
		return SAUNAFS_ERROR_EINVAL;
	}

	const std::filesystem::path trashPath =
			trashDir / (filePath.filename().string() + "." + deletionTime);

	try {
		std::filesystem::rename(filePath, trashPath);
	} catch (const std::filesystem::filesystem_error &e) {
		return SAUNAFS_ERROR_IO;
	}

	return SAUNAFS_STATUS_OK;
}
