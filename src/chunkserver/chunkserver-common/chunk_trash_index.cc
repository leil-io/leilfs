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

#include "chunk_trash_index.h"
#include "chunk_trash_manager.h"

ChunkTrashIndex &ChunkTrashIndex::instance() {
	static ChunkTrashIndex instance;
	return instance;
}

void ChunkTrashIndex::reset(const std::filesystem::path &diskPath) {
	std::scoped_lock<std::mutex> const lock(trashIndexMutex);
	trashIndex.erase(diskPath);
	trashIndex[diskPath] = {};
}

void
ChunkTrashIndex::add(const time_t &deletionTime, const std::string &filePath,
                     const std::string &diskPath) {
	std::scoped_lock<std::mutex> const lock(trashIndexMutex);
	trashIndex[diskPath].emplace(deletionTime, filePath);
}

void ChunkTrashIndex::removeInternal(const time_t &deletionTime,
                             const std::string &filePath,
                             const std::string &diskPath) {
	auto range = trashIndex[diskPath].equal_range(deletionTime);
	for (auto it = range.first; it != range.second; ++it) {
		if (it->second == filePath) {
			trashIndex[diskPath].erase(it);
			return; // Avoid further iteration after removal
		}
	}
}

void ChunkTrashIndex::remove(const time_t &deletionTime,
                             const std::string &filePath,
                             const std::string &diskPath) {
	std::scoped_lock<std::mutex> const lock(trashIndexMutex);
	removeInternal(deletionTime, filePath, diskPath);
}

void ChunkTrashIndex::remove(const time_t &deletionTime,
                             const std::string &filePath) {
	std::scoped_lock<std::mutex> const lock(trashIndexMutex);
	for (const auto &diskEntry: trashIndex) {
		removeInternal(deletionTime, filePath, diskEntry.first);
		return; // Avoid further iteration after removal
	}
}

ChunkTrashIndex::TrashIndexDiskEntries
ChunkTrashIndex::getExpiredFiles(const time_t &timeLimit, size_t bulkSize) {
	std::scoped_lock<std::mutex> const lock(trashIndexMutex);
	return getExpiredFilesInternal(timeLimit, bulkSize);
}


ChunkTrashIndex::TrashIndexDiskEntries
ChunkTrashIndex::getExpiredFilesInternal(const time_t &timeLimit, size_t bulkSize) {
	TrashIndexDiskEntries expiredFiles;
	size_t count = 0;
	for (const auto &diskEntry: trashIndex) {
		count += getExpiredFilesInternal(diskEntry.first, timeLimit,
		                                 expiredFiles,
		                                 bulkSize);
		if (bulkSize != 0 && count >= bulkSize) {
			break;
		}
	}

	return expiredFiles;
}

size_t ChunkTrashIndex::getExpiredFilesInternal(const std::filesystem::path &diskPath,
                                                const std::time_t &timeLimit,
                                                std::unordered_map<std::string, std::multimap<std::time_t, std::string>> &expiredFiles,
                                                size_t bulkSize) {
	auto &diskTrashIndex = trashIndex[diskPath];
	auto limit = diskTrashIndex.upper_bound(timeLimit);

	expiredFiles[diskPath] = {};
	size_t count = 0;
	for (auto it = diskTrashIndex.begin(); it != limit; ++it) {
		expiredFiles[diskPath].emplace(it->first, it->second);
		if (bulkSize != 0 && ++count >= bulkSize) {
			break;
		}
	}

	return count;
}

ChunkTrashIndex::TrashIndexFileEntries
ChunkTrashIndex::getOlderFiles(const std::string &diskPath,
                               const size_t removalStepSize) {
	std::scoped_lock<std::mutex> const lock(trashIndexMutex);
	auto &diskTrashIndex = trashIndex[diskPath];
	TrashIndexFileEntries olderFiles;
	size_t count = 0;
	for (auto it = diskTrashIndex.begin(); it != diskTrashIndex.end(); ++it) {
		olderFiles.emplace(it->first, it->second);
		if (removalStepSize != 0 && ++count >= removalStepSize) {
			break;
		}
	}

	return olderFiles;
}

std::vector<std::string> ChunkTrashIndex::getDiskPaths()  {
	std::scoped_lock<std::mutex> const lock(trashIndexMutex);
	std::vector<std::string> diskPaths;
	for (const auto &diskEntry: trashIndex) {
		diskPaths.push_back(diskEntry.first);
	}

	return diskPaths;
}
