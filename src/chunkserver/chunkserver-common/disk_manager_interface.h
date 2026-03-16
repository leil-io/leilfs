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

#include "chunkserver-common/disk_interface.h"

/**
 * \brief Interface for disk management.
 *
 * Concrete implementations of this interface are responsible for managing the
 * chunks distribution inside the disks. The most basic feature is to decide
 * which disk to use for a new chunk.
 */
class IDiskManager {
public:
	/// Default constructor
	IDiskManager() = default;

	// No need to copy or move them so far

	IDiskManager(const IDiskManager &) = delete;
	IDiskManager(IDiskManager &&) = delete;
	IDiskManager &operator=(const IDiskManager &) = delete;
	IDiskManager &operator=(IDiskManager &&) = delete;

	/// Virtual destructor needed for correct polymorphism
	virtual ~IDiskManager() = default;

	/// Reload the configuration from the configuration file.
	virtual void reloadConfiguration() = 0;

	/// Reload the disks from the configuration and populates the gDisks vector.
	virtual void reloadDisksFromCfg() = 0;

	/// Get the disk to use for a new chunk. Concrete implementations will use
	/// different strategies.
	virtual IDisk *getDiskForNewChunk(
	    [[maybe_unused]] const ChunkPartType &chunkType) = 0;

	/// Update the space usage of the disks.
	virtual void updateSpaceUsage() = 0;

	/// Gets the disk groups information in YAML format
	virtual std::string getDiskGroupsInfo() = 0;

	/// Selects the disk to use for Garbage Collection (GC).
	/// Could return DiskNotFound if the disks does not need GC.
	virtual IDisk *getDiskForGC() = 0;

	/// Returns all the suitable disks for Garbage Collection (GC).
	/// Could return an empty vector if the disks does not need GC.
	virtual std::vector<IDisk *> getSuitableDisksForGC() = 0;

	/// Selects the disk to use for a chunk test.
	virtual IChunk *getChunkToTest(uint32_t &elapsedTimeMs) = 0;

	/// Resets the helper disk iterator for tests.
	virtual void resetDiskIteratorForTests() = 0;
};
