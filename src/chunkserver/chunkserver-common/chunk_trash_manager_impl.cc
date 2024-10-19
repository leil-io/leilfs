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
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"

#include <algorithm>

#pragma GCC diagnostic pop

#include <sys/syslog.h>
#include <sys/statvfs.h>
#include <stack>

#include "errors/saunafs_error_codes.h"
#include "slogger/slogger.h"

#include "chunk_trash_manager_impl.h"
#include "common/cfg.h"

namespace fs = std::filesystem;

size_t ChunkTrashManagerImpl::kAvailableThresholdGB = 10;
size_t ChunkTrashManagerImpl::kTrashTimeLimitSeconds = 259200;
size_t ChunkTrashManagerImpl::kTrashGarbageCollectorBulkSize = 1000;
size_t ChunkTrashManagerImpl::kGarbageCollectorSpaceRecoveryStep = 10;

void ChunkTrashManagerImpl::reloadConfig() {
	kAvailableThresholdGB = cfg_get("CHUNK_TRASH_FREE_SPACE_THRESHOLD_GB",
	                                kAvailableThresholdGB);
	kTrashTimeLimitSeconds = cfg_get("CHUNK_TRASH_EXPIRATION_SECONDS",
	                                 kTrashTimeLimitSeconds);
	kTrashGarbageCollectorBulkSize = cfg_get("CHUNK_TRASH_GC_BATCH_SIZE",
	                                         kTrashGarbageCollectorBulkSize);
	kGarbageCollectorSpaceRecoveryStep = cfg_get(
			"CHUNK_TRASH_GC_SPACE_RECOVERY_BATCH_SIZE",
			kGarbageCollectorSpaceRecoveryStep);
}

std::string
ChunkTrashManagerImpl::getTimeString(std::time_t time1) {
//
	std::tm *utcTime = std::gmtime(&time1);  // Convert to UTC

	std::ostringstream oss;
	oss << std::put_time(utcTime, kTimeStampFormat.c_str());
	return oss.str();
}

std::time_t ChunkTrashManagerImpl::getTimeFromString(
		const std::string &timeString, int &errorCode) {
	errorCode = SAUNAFS_STATUS_OK;
	std::tm time = {};
	std::istringstream stringReader(timeString);
	stringReader >> std::get_time(&time, kTimeStampFormat.c_str());
	if (stringReader.fail()) {
		safs_pretty_syslog(LOG_ERR, "Failed to parse time string: %s",
		                   timeString.c_str());
		errorCode = SAUNAFS_ERROR_EINVAL;
	}
	return std::mktime(&time);
}

int ChunkTrashManagerImpl::getMoveDestinationPath(const std::string &filePath,
                                                  const std::string &sourceRoot,
                                                  const std::string &destinationRoot,
                                                  std::string &destinationPath) {
	if (filePath.find(sourceRoot) != 0) {
		safs_pretty_syslog(LOG_ERR,
		                   "File path does not contain source root: %s",
		                   filePath.c_str());
		return SAUNAFS_ERROR_EINVAL;
	}

	destinationPath =
			destinationRoot + "/" + filePath.substr(sourceRoot.size());
	return SAUNAFS_STATUS_OK;
}


int ChunkTrashManagerImpl::moveToTrash(
		const fs::path &filePath,
		const fs::path &diskPath,
		const std::time_t &deletionTime) {

	if (!fs::exists(filePath)) {
		safs_pretty_syslog(LOG_ERR, "File does not exist: %s",
		                   filePath.string().c_str());
		return SAUNAFS_ERROR_ENOENT;
	}

	const fs::path trashDir = getTrashDir(diskPath);
	fs::create_directories(trashDir);

	const std::string deletionTimestamp = getTimeString(deletionTime);
	std::string trashFilename;

	auto errorCode = getMoveDestinationPath(filePath.string(),
	                                        diskPath.string(),
	                                        trashDir.string(), trashFilename);
	if (errorCode != SAUNAFS_STATUS_OK) {
		return errorCode;
	}

	trashFilename += "." + deletionTimestamp;

	try {
		fs::create_directories(fs::path(trashFilename).parent_path());
		fs::rename(filePath, trashFilename);
		trashIndex->add(deletionTime, trashFilename,
		                diskPath.string());
	} catch (const fs::filesystem_error &e) {
		safs_pretty_syslog(LOG_ERR,
		                   "Failed to move file to trash: %s, error: %s",
		                   filePath.string().c_str(), e.what());
		return SAUNAFS_ERROR_NOTDONE;
	}

	return SAUNAFS_STATUS_OK;
}

void ChunkTrashManagerImpl::removeTrashFiles(
		const ChunkTrashIndex::TrashIndexDiskEntries &filesToRemove) {
	for (const auto &[diskPath, fileEntries]: filesToRemove) {
		for (const auto &fileEntry: fileEntries) {
			if (removeFileFromTrash(fileEntry.second) != SAUNAFS_STATUS_OK) {
				continue;
			}
			trashIndex->remove(fileEntry.first, fileEntry.second, diskPath);
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
		fs::create_directories(trashDir);
	}

	trashIndex->reset(diskPath);

	for (const auto &file: fs::recursive_directory_iterator(trashDir)) {
		if (fs::is_regular_file(file) && isTrashPath(file.path().string())) {
			const std::string filename = file.path().filename().string();
			const std::string deletionTimeStr = filename.substr(
					filename.find_last_of('.') + 1);
			if (!isValidTimestampFormat(deletionTimeStr)) {
				safs_pretty_syslog(LOG_ERR,
				                   "Invalid timestamp format in file: %s, skipping.",
				                   file.path().string().c_str());
				continue;
			}
			int errorCode;
			const std::time_t deletionTime = getTimeFromString(
					deletionTimeStr, errorCode);
			if (errorCode != SAUNAFS_STATUS_OK) {
				safs_pretty_syslog(LOG_ERR, "Failed to parse deletion time "
				                            "from file: %s, skipping.",
				                   file.path().string().c_str());
				continue;
			}
			trashIndex->add(deletionTime, file.path().string(),
			                diskPath);
		}
	}

	return SAUNAFS_STATUS_OK;
}

bool ChunkTrashManagerImpl::isValidTimestampFormat(
		const std::string &timestamp) {
	return timestamp.size() == kTimeStampLength &&
	       std::all_of(timestamp.begin(), timestamp.end(), ::isdigit);
}

void ChunkTrashManagerImpl::removeExpiredFiles(
		const time_t &timeLimit, size_t bulkSize) {
	const auto expiredFilesCollection = trashIndex->getExpiredFiles(timeLimit,
	                                                                bulkSize);
	removeTrashFiles(expiredFilesCollection);
}

void ChunkTrashManagerImpl::cleanEmptyFolders(const std::string &directory) {
	if (!fs::exists(directory) || !fs::is_directory(directory)) {
		return; // Invalid path, so we do nothing
	}

	// Iterate through the directory's contents
	for (const auto &entry: fs::directory_iterator(directory)) {
		if (fs::is_directory(entry.path())) {
			// Recursively clean subdirectories
			cleanEmptyFolders(entry.path());
		}
	}

	if (!isTrashPath(directory)) {
		return; // We only clean up trash directories
	}

	// After processing subdirectories, check if the directory is now empty
	if (fs::is_empty(directory)) {
		removeFileFromTrash(directory);
	}
}

size_t ChunkTrashManagerImpl::checkAvailableSpace(
		const std::string &diskPath) {
	struct statvfs stat{};
	if (statvfs(diskPath.c_str(), &stat) != 0) {
		safs_pretty_syslog(LOG_ERR, "Failed to get file system statistics");
		return 0;
	}
	size_t const kGiBMultiplier = 1 << 30;
	size_t const availableGb = stat.f_bavail * stat.f_frsize / kGiBMultiplier;
	return availableGb;
}

void ChunkTrashManagerImpl::makeSpace(
		const std::string &diskPath,
		const size_t spaceAvailabilityThreshold,
		const size_t recoveryStep) {
	size_t availableSpace = checkAvailableSpace(diskPath);
	while (availableSpace < spaceAvailabilityThreshold) {
		const auto olderFilesCollection = trashIndex->getOlderFiles(diskPath,
		                                                            recoveryStep);
		if (olderFilesCollection.empty()) {
			break;
		}
		removeTrashFiles({{diskPath, olderFilesCollection}});
		availableSpace = checkAvailableSpace(diskPath);
	}
}

void ChunkTrashManagerImpl::makeSpace(
		const size_t spaceAvailabilityThreshold, const size_t recoveryStep) {
	for (const auto &diskPath: trashIndex->getDiskPaths()) {
		makeSpace(diskPath, spaceAvailabilityThreshold, recoveryStep);
	}
}

void ChunkTrashManagerImpl::cleanEmptyFolders() {
	for (const auto &diskPath: trashIndex->getDiskPaths()) {
		fs::path trashDir = getTrashDir(diskPath);
		cleanEmptyFolders(trashDir.string());
	}
}

void ChunkTrashManagerImpl::collectGarbage() {
	if (!ChunkTrashManager::isEnabled) {
		return;
	}
	std::time_t const currentTime = std::time(nullptr);
	std::time_t const expirationTime = currentTime - kTrashTimeLimitSeconds;
	removeExpiredFiles(expirationTime, kTrashGarbageCollectorBulkSize);
	makeSpace(kAvailableThresholdGB,
	          kGarbageCollectorSpaceRecoveryStep);
//	cleanEmptyFolders();
}

bool ChunkTrashManagerImpl::isTrashPath(const std::string &filePath) {
	return filePath.find("/" + ChunkTrashManager::kTrashDirname + "/") !=
	       std::string::npos;
}

int ChunkTrashManagerImpl::removeFileFromTrash(const std::string &filePath) {
	if (!isTrashPath(filePath)) {
		safs_pretty_syslog(LOG_ERR, "Invalid trash path: %s", filePath.c_str());
		return SAUNAFS_ERROR_EINVAL;
	}
	std::error_code errorCode;
	fs::remove(filePath, errorCode); // Remove the file or directory
	if (errorCode) {
		safs_pretty_syslog(LOG_ERR, "Failed to remove file or directory: %s",
		                   filePath.c_str());
		return SAUNAFS_ERROR_NOTDONE;
	}

	return SAUNAFS_STATUS_OK;
}
