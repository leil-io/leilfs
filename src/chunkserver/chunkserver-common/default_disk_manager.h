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

#pragma once

#include "common/platform.h"

#include <map>
#include <string>

#include "chunkserver-common/disk_manager_interface.h"
#include "chunkserver-common/global_shared_resources.h"

/**
 * \brief Default implementation of the disk manager interface.
 *
 * This class implements a strategy for assigning chunks across all available
 * drives. The strategy aims to maintain a balanced distribution of chunks,
 * ensuring that each drive is utilized effectively.
 *
 * This was the approach used so far.
 */
class DefaultDiskManager : public IDiskManager {
public:
	/// Default constructor
	DefaultDiskManager() = default;

	// No need to copy or move them so far

	DefaultDiskManager(const DefaultDiskManager &) = delete;
	DefaultDiskManager(DefaultDiskManager &&) = delete;
	DefaultDiskManager &operator=(const DefaultDiskManager &) =
	    delete;
	DefaultDiskManager &operator=(DefaultDiskManager &&) = delete;

	/// Virtual destructor needed for correct polymorphism
	virtual ~DefaultDiskManager() = default;

	/// Reload the configuration from the configuration file.
	/// No need to reload the configuration so far for this implementation.
	void reloadConfiguration() override {};

	/// Parse a configuration line for a disk.
	virtual int parseCfgLine(std::string hddCfgLine);

	/// Reload the disks from the configuration and populates the gDisks vector.
	void reloadDisksFromCfg() override;

	/**
	 * \brief Get the disk to store a new chunk.
	 *
	 * This concrete implementation obtains a balanced distribution using all
	 * available disks.
	 */
	IDisk *getDiskForNewChunk(
	    [[maybe_unused]] const ChunkPartType &chunkType) override;

	/// Update the space usage of the disks.
	/// No need to update the space usage here for this implementation.
	void updateSpaceUsage() override {};

	/// Gets the disk groups information.
	/// Not supported by the default disk manager.
	std::string getDiskGroupsInfo() override { return "Not supported"; }

	/// Selects the disk to use for GC.
	IDisk *getDiskForGC() override;

	/// Selects next chunk to test.
	IChunk *getChunkToTest(uint32_t &elapsedTimeMs) override;

	/// Reset the disk iterators used for tests.
	/// Usually when gResetTester changes or after a reload.
	void resetDiskIteratorForTests() override;

private:
	/// Plugin prefix each metadata path was last configured with.
	std::map<std::string, std::string> diskPrefixes_;

	/// Next disk index for GC. Helps in the round-robin strategy.
	uint32_t nextDiskIndexForGC_ = 0;

	// Iterators used to select the disks for chunk testing.
	using DisksIterator = std::vector<std::unique_ptr<IDisk>>::iterator;

	DisksIterator diskItForTests_ = gDisks.begin();
	DisksIterator previousDiskItForTests_ = gDisks.begin();
};
