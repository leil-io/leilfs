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

#include <sys/statvfs.h>
#include <algorithm>

#include <ctime>
#include "errors/saunafs_error_codes.h"
#include "fmt/format.h"
#include "slogger/slogger.h"

#include "chunk_trash_manager_impl.h"
#include "config/cfg.h"

namespace fs = std::filesystem;

size_t ChunkTrashManagerImpl::availableThresholdGB = kDefaultAvailableThresholdGB;
size_t ChunkTrashManagerImpl::trashTimeLimitSeconds = kDefaultTrashTimeLimitSeconds;
size_t ChunkTrashManagerImpl::trashGarbageCollectorBulkSize = kDefaultTrashGarbageCollectorBulkSize;
size_t ChunkTrashManagerImpl::garbageCollectorSpaceRecoveryStep =
    kDefaultGarbageCollectorSpaceRecoveryStep;

const std::string ChunkTrashManagerImpl::kTrashGuardString =
    std::string("/") + ChunkTrashManager::kTrashDirname + "/";

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
	    "CHUNK_TRASH_GC_SPACE_RECOVERY_BATCH_SIZE={}",
	    availableThresholdGB, trashTimeLimitSeconds, trashGarbageCollectorBulkSize,
	    garbageCollectorSpaceRecoveryStep);
}

std::string ChunkTrashManagerImpl::getTimeString(std::time_t time1) {
	std::tm utcTime;

#ifdef _WIN32
	if (gmtime_s(&utcTime, &time1) != 0) {
		safs::log_error_code(SAUNAFS_ERROR_EINVAL, "Failed to convert time to UTC: {}",
		                     std::strerror(errno));
		return "";
	}
#else
	if (gmtime_r(&time1, &utcTime) == nullptr) {
		safs::log_error_code(SAUNAFS_ERROR_EINVAL, "Failed to convert time to UTC: {}",
		                     std::strerror(errno));
		return "";
	}
#endif

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
	}
	return std::mktime(&time);
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

	const std::string deletionTimestamp = getTimeString(deletionTime);
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
    const ChunkTrashIndex::TrashIndexDiskEntries &filesToRemove) const {
	for (const auto &[diskPath, fileEntries] : filesToRemove) {
		for (const auto &fileEntry : fileEntries) {
			if (removeFileFromTrash(fileEntry.second) != SAUNAFS_STATUS_OK) { continue; }
			getTrashIndex().remove(fileEntry.first, fileEntry.second, diskPath);
		}
	}
}

fs::path ChunkTrashManagerImpl::getTrashDir(const fs::path &diskPath) {
	return diskPath / ChunkTrashManager::kTrashDirname;
}

int ChunkTrashManagerImpl::init(const std::string &diskPath) {
	reloadConfig();
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

	getTrashIndex().reset(diskPath);

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

bool ChunkTrashManagerImpl::isValidTimestampFormat(const std::string &timestamp) {
	return timestamp.size() == kTimeStampLength && std::ranges::all_of(timestamp, ::isdigit);
}

void ChunkTrashManagerImpl::removeExpiredFiles(const time_t &timeLimit, size_t bulkSize) const {
	const auto expiredFilesCollection = getTrashIndex().getExpiredFiles(timeLimit, bulkSize);
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
		removeTrashFiles({{diskPath, olderFilesCollection}});
		availableSpace = checkAvailableSpace(diskPath);
	}
}

void ChunkTrashManagerImpl::makeSpace(const size_t spaceAvailabilityThreshold,
                                      const size_t recoveryStep) const {
	for (const auto &diskPath : getTrashIndex().getDiskPaths()) {
		makeSpace(diskPath, spaceAvailabilityThreshold, recoveryStep);
	}
}

void ChunkTrashManagerImpl::collectGarbage() {
	if (!ChunkTrashManager::isEnabled) { return; }
	std::time_t const currentTime = std::time(nullptr);
	std::time_t const expirationTime = currentTime - trashTimeLimitSeconds;
	removeExpiredFiles(expirationTime, trashGarbageCollectorBulkSize);
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
