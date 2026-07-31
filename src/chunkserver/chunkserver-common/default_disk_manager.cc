/*
   Copyright 2023 Leil Storage

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

#include "chunkserver-common/default_disk_manager.h"

#include <fstream>

#include "chunkserver-common/cmr_disk.h"
#include "chunkserver-common/global_shared_resources.h"
#include "chunkserver-common/plugin_manager.h"
#include "common/exceptions.h"
#include "config/cfg.h"
#include "devtools/TracePrinter.h"

int DefaultDiskManager::parseCfgLine(std::string hddCfgLine) {
	TRACETHIS();
	uint8_t lockNeeded;

	disk::Configuration configuration(hddCfgLine);

	if (configuration.isComment || configuration.isEmpty) { return 0; }

	if (!configuration.isValid) {
		throw InitializeException("HDD configuration line not valid: " +
		                          hddCfgLine);
	}

	IDisk *currentDisk = DiskNotFound;

	if (configuration.isZoned) {
		currentDisk = pluginManager.createDisk(configuration);
	} else {
		currentDisk = new CmrDisk(configuration);
	}

	if (currentDisk == DiskNotFound) {
		throw InitializeException("HDD configuration line not valid: " +
		                          hddCfgLine);
	}

	{
		// Ensure meta + data path remain consistent between reloads
		std::lock_guard disksLockGuard(gDisksMutex);

		for (const auto &disk : gDisks) {
			if (disk->metaPath() == currentDisk->metaPath()) {
				if (disk->dataPath() != currentDisk->dataPath()) {
					throw InitializeException(
					    "Combination of metadata and "
					    "data paths have changed between "
					    "reloads for line: " +
					    hddCfgLine);
				}
			}
		}

		// Lock is not needed if the disk already exists in memory (reload)
		lockNeeded = 1;
		for (const auto &disk : gDisks) {
			if (disk->metaPath() == currentDisk->metaPath()) {
				lockNeeded = 0;
				break;
			}
		}
	}

	try {
		currentDisk->createPathsAndSubfolders();
		{
			std::lock_guard disksLockGuard(gDisksMutex);
			currentDisk->createLockFiles(lockNeeded, gDisks);
		}
	} catch (const InitializeException &ie) {
		delete currentDisk;
		throw InitializeException(ie.what());
	}

	std::unique_lock disksUniqueLock(gDisksMutex);

	// This loop is used at reload time, we need to update the already existing
	// Disks' properties according to the (probably new) hdd.cfg file.
	for (auto &disk : gDisks) {
		if (disk->metaPath() ==
		    currentDisk->metaPath()) {  // Disk already in memory
			if (disk->isDamaged()) {
				disk->setScanState(IDisk::ScanState::kNeeded);
				disk->setScanProgress(0);
				disk->setIsDamaged(currentDisk->isDamaged());
				disk->setAvailableSpace(0ULL);
				disk->setTotalSpace(0ULL);
				disk->setLeaveFreeSpace(disk::gLeaveFree);
				disk->getCurrentStats().clear();
				for (int l = 0; l < disk::kStatsHistoryIn24Hours; ++l) {
					disk->stats()[l].clear();
				}
				disk->setStatsPos(0);
				for (int l = 0; l < disk::kLastErrorSize; ++l) {
					disk->lastErrorTab()[l].chunkid = 0ULL;
					disk->lastErrorTab()[l].timestamp = 0;
				}
				disk->setLastErrorIndex(0);
				disk->setLastRefresh(0);
				disk->setNeedRefresh(true);
			} else {
				if (disk->isMarkedForRemoval() !=
				        currentDisk->isMarkedForRemoval() ||
				    disk->isReadOnly() != currentDisk->isReadOnly()) {
					// important change - chunks need to be send to master again
					disk->setScanState(IDisk::ScanState::kSendNeeded);
				}
			}

			disk->setWasRemovedFromConfig(false);
			disk->setIsReadOnly(currentDisk->isReadOnly());
			disk->setIsMarkedForRemoval(currentDisk->isMarkedForRemoval());
			disksUniqueLock.unlock();

			delete currentDisk;  // Also deletes the lock files (smart pointer)
			return 1;
		}
	}

	gDisks.emplace_back(currentDisk);
	gResetTester = true;

	return 2;
}

void DefaultDiskManager::reloadDisksFromCfg() {
	TRACETHIS();

	const std::string defaultHddFilename = ETC_PATH "/leil-hdd.cfg";
	const std::string legacyHddFilename = ETC_PATH_LEGACY "/sfshdd.cfg";
	std::string hddFilename = cfg_get("HDD_CONF_FILENAME", defaultHddFilename);
	if (hddFilename == defaultHddFilename && !std::ifstream(defaultHddFilename).good() &&
	    std::ifstream(legacyHddFilename).good()) {
		safs::log_warn(
		    "using legacy hdd configuration file {} because default file {} was not found",
		    legacyHddFilename.c_str(), defaultHddFilename.c_str());
		hddFilename = legacyHddFilename;
	}
	std::ifstream hddFile(hddFilename);

	if (!hddFile.is_open()) {
		throw InitializeException("can't open hdd config file " + hddFilename +
		                          ": " + strerr(errno) +
		                          " - new file can be created using " +
		                          APP_EXAMPLES_SUBDIR "/leil-hdd.cfg");
	}

	safs_pretty_syslog(LOG_INFO, "hdd configuration file %s opened",
	                   hddFilename.c_str());

	{
		std::lock_guard disksLockGuard(gDisksMutex);

		gDiskActions = 0; // stop disk actions

		// Makes sense only at the reload scenario. All Disks are marked as
		// removed and later scanned and unmarked as they appear in the hdd.cfg.
		// After reading the hdd.cfg, the missing entries will be actually
		// deleted from the in-memory data structures.
		for (auto &disk : gDisks) {
			disk->setWasRemovedFromConfig(true);
		}
	}

	std::string line;
	while (std::getline(hddFile, line)) {
		parseCfgLine(std::move(line));
	}

	hddFile.close();

	bool anyDiskAvailable = false;

	{
		std::lock_guard disksLockGuard(gDisksMutex);

		for (auto &disk : gDisks) {
			if (!disk->wasRemovedFromConfig()) {
				anyDiskAvailable = true;
				if (disk->scanState() == IDisk::ScanState::kNeeded) {
					safs_pretty_syslog(LOG_NOTICE,
					    "hdd space manager: disk %s will be scanned",
					    disk->getPaths().c_str());
				} else if (disk->scanState() == IDisk::ScanState::kSendNeeded) {
					safs_pretty_syslog(LOG_NOTICE,
					    "hdd space manager: disk %s will be resend",
					    disk->getPaths().c_str());
				} else {
					safs_pretty_syslog(LOG_NOTICE,
					    "hdd space manager: disk %s didn't change",
					     disk->getPaths().c_str());
				}
			} else {
				safs_pretty_syslog(LOG_NOTICE,
				                   "hdd space manager: disk %s will be removed",
				                   disk->getPaths().c_str());
			}
		}
		gDiskActions = 1; // continue disk actions
	}

	std::vector<std::string> dataPaths;

	{
		std::lock_guard disksLockGuard(gDisksMutex);
		for (const auto &disk : gDisks) {
			dataPaths.emplace_back(disk->dataPath());
		}
	}

	gIoStat.resetPaths(dataPaths);

	if (!anyDiskAvailable) {
		throw InitializeException("no data paths defined in the " +
		                          hddFilename + " file");
	}
}

IDisk *DefaultDiskManager::getDiskForNewChunk(
    [[maybe_unused]] const ChunkPartType &chunkType) {
	TRACETHIS();
	IDisk *bestDisk = DiskNotFound;
	double maxCarry = 1.0;
	double minPercentAvail = std::numeric_limits<double>::max();
	double maxPercentAvail = 0.0;
	double s,d;
	double percentAvail;

	if (gDisks.empty()) {
		return DiskNotFound;
	}

	for (const auto &disk : gDisks) {
		if (!disk->isSelectableForNewChunk()) {
			continue;
		}

		if (disk->carry() >= maxCarry) {
			maxCarry = disk->carry();
			bestDisk = disk.get();
		}

		percentAvail = static_cast<double>(disk->availableSpace()) /
		               static_cast<double>(disk->totalSpace());
		minPercentAvail = std::min(minPercentAvail, percentAvail);
		maxPercentAvail = std::max(maxPercentAvail, percentAvail);
	}

	if (bestDisk != DiskNotFound) {
		// Lower the probability of being chosen again
		bestDisk->setCarry(bestDisk->carry() - 1.0);
		return bestDisk;
	}

	if (maxPercentAvail == 0.0) {  // no space
		return DiskNotFound;
	}

	if (maxPercentAvail < 0.01) {
		s = 0.0;
	} else {
		s = minPercentAvail * 0.8;
		if (s < 0.01) {
			s = 0.01;
		}
	}

	d = maxPercentAvail - s;
	maxCarry = 1.0;

	for (auto &disk : gDisks) {
		if (!disk->isSelectableForNewChunk()) {
			continue;
		}

		percentAvail = static_cast<double>(disk->availableSpace()) /
		               static_cast<double>(disk->totalSpace());

		if (percentAvail > s) {
			disk->setCarry(disk->carry() + ((percentAvail - s) / d));
		}

		if (disk->carry() >= maxCarry) {
			maxCarry = disk->carry();
			bestDisk = disk.get();
		}
	}

	if (bestDisk != DiskNotFound) {  // should be always true here
		bestDisk->setCarry(bestDisk->carry() - 1.0);
	}

	return bestDisk;
}

IDisk *DefaultDiskManager::getDiskForGC() {
	TRACETHIS();
	IDisk *bestDisk = DiskNotFound;

	std::lock_guard disksLockGuard(gDisksMutex);

	if (gDisks.empty()) {
		return DiskNotFound;
	}

	auto diskCount = gDisks.size();

	for (size_t i = 0; i < diskCount; ++i) {
		size_t index = (nextDiskIndexForGC_ + i) % diskCount;
		const auto &disk = gDisks[index];

		if (!disk->isZonedDevice() || !disk->isSelectableForNewChunk()) {
			continue;
		}

		bestDisk = disk.get();
		nextDiskIndexForGC_ = (index + 1) % diskCount;
		break;
	}

	return bestDisk;
}

IChunk *DefaultDiskManager::getChunkToTest(uint32_t &elapsedTimeMs) {
	IChunk *chunk = ChunkNotFound;

	elapsedTimeMs += std::min(gHDDTestFreq_ms.load(), kMaxTestFreqMs);

	if (elapsedTimeMs < gHDDTestFreq_ms || gDiskActions == 0 ||
	    diskItForTests_ == gDisks.end()) {
		return ChunkNotFound;
	}

	elapsedTimeMs = 0;
	previousDiskItForTests_ = diskItForTests_;

	if (!gDisks.empty()) {
		do {
			++diskItForTests_;
			if (diskItForTests_ == gDisks.end()) {
				diskItForTests_ = gDisks.begin();
			}
		} while (
		    ((*diskItForTests_)->isDamaged() ||
		     (*diskItForTests_)->isMarkedForDeletion() ||
		     (*diskItForTests_)->wasRemovedFromConfig() ||
		     (*diskItForTests_)->scanState() != IDisk::ScanState::kWorking) &&
		    previousDiskItForTests_ != diskItForTests_);
	}

	if (previousDiskItForTests_ == diskItForTests_ &&
	    ((*diskItForTests_)->isDamaged() ||
	     (*diskItForTests_)->isMarkedForDeletion() ||
	     (*diskItForTests_)->wasRemovedFromConfig() ||
	     (*diskItForTests_)->scanState() != IDisk::ScanState::kWorking)) {
		return ChunkNotFound;
	}

	chunk = (*diskItForTests_)->chunks().chunkToTest();

	if (chunk != ChunkNotFound && chunk->state() == ChunkState::Available) {
		return chunk;
	}

	return ChunkNotFound;
}

void DefaultDiskManager::resetDiskIteratorForTests() {
	diskItForTests_ = gDisks.begin();
}
