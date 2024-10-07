// chunk_trash_manager.cc

#include <iostream> // For debugging output

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

int ChunkTrashManager::moveToTrash(const std::filesystem::path& filePath, const std::filesystem::path& diskPath, const std::string& deletionTime) {
	if (!std::filesystem::exists(filePath)) {
		// Log warning or error
		return SAUNAFS_ERROR_ENOENT;
	}

	const std::filesystem::path trashDir = diskPath / kTrashDirname;
	std::filesystem::create_directories(trashDir);

	if (!filePath.string().starts_with(diskPath.string())) {
		// Log warning or error
		return SAUNAFS_ERROR_EINVAL;
	}

	const std::filesystem::path trashPath = trashDir / (filePath.filename().string() + "." + deletionTime);

	try {
		std::filesystem::rename(filePath, trashPath);
	} catch (const std::filesystem::filesystem_error& e) {
		// Log error
		return SAUNAFS_ERROR_IO;
	}

	return SAUNAFS_STATUS_OK;
}
