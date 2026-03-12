/*
   Copyright 2013-2017 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

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

#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

#include "common/type_defs.h"

#ifdef _WIN32
#include <atomic>
#include <cstdint>

inline std::atomic<uint32_t> gCleanAcquiredFilesPeriod;
inline std::atomic<uint32_t> gCleanAcquiredFilesTimeout;
#endif

// Define the AcquiredFileLastTimeUsed struct
struct AcquiredFileLastTimeUsed {
	inode_t inode;
	uint32_t lastTimeUsed;

	// Constructor
	AcquiredFileLastTimeUsed(inode_t inode, uint32_t lastTimeUsed)
	    : inode(inode), lastTimeUsed(lastTimeUsed) {}
};

struct CompareByLastTimeUsed {
	bool operator()(const AcquiredFileLastTimeUsed &acquiredFile1,
	                const AcquiredFileLastTimeUsed &acquiredFile2) const {
		return acquiredFile1.lastTimeUsed < acquiredFile2.lastTimeUsed ||
		       (acquiredFile1.lastTimeUsed == acquiredFile2.lastTimeUsed &&
		        acquiredFile1.inode < acquiredFile2.inode);
	}
};

using AcquiredFilesLastTimeUsed =
    std::set<AcquiredFileLastTimeUsed, CompareByLastTimeUsed>;
using InodeToFileMap = std::map<inode_t, AcquiredFilesLastTimeUsed::iterator>;

inline AcquiredFilesLastTimeUsed acquiredFilesLastTimeUsed;
inline InodeToFileMap inodeToFileMap;

// Function to return the least value according to the custom comparator
inline AcquiredFileLastTimeUsed getOldestAcquiredFile() {
	if (!acquiredFilesLastTimeUsed.empty()) {
		return *acquiredFilesLastTimeUsed
		            .begin();  // The first element is the least according to
		                       // the comparator
	}
	throw std::runtime_error("Acquired Files With Last Time Used Set is empty");
}

// Function to add/update a new AcquiredFileLastTimeUsed
inline void addOrUpdateAcquiredFileLastTimeUsed(
    inode_t inode, uint32_t lastTimeUsed = time(nullptr)) {
	auto it = inodeToFileMap.find(inode);
	if (it != inodeToFileMap.end()) {
		// Update existing entry
		AcquiredFileLastTimeUsed updatedFile = *(it->second);
		updatedFile.lastTimeUsed = lastTimeUsed;
		acquiredFilesLastTimeUsed.erase(it->second);
		auto newIt = acquiredFilesLastTimeUsed.insert(updatedFile).first;
		inodeToFileMap[inode] = newIt;
	} else {
		// Add new entry
		AcquiredFileLastTimeUsed newFile(inode, lastTimeUsed);
		auto insertionResult = acquiredFilesLastTimeUsed.insert(newFile);
		auto newIt = insertionResult.first;
		inodeToFileMap[inode] = newIt;
	}
}

// Function to remove an AcquiredFileLastTimeUsed
inline void removeAcquiredFileLastTimeUsed(inode_t inode) {
	auto it = inodeToFileMap.find(inode);
	if (it != inodeToFileMap.end()) {
		acquiredFilesLastTimeUsed.erase(it->second);
		inodeToFileMap.erase(it);
	}
}
