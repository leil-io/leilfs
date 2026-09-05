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
#include "chunkserver-common/iplugin.h"

/// DiskPlugin is an interface for all Disk plugins.
class BOOST_SYMBOL_VISIBLE DiskPlugin : public IPlugin {
public:
	/// Default constructor
	DiskPlugin();

	/// Virtual destructor
	virtual ~DiskPlugin();

	/// To keep simple the constructor
	bool initialize() override;

	/// Initializes the logger for this plugin.
	/// Returns true when logger initialization succeeds.
	/// Returns false when logger initialization fails and callers should
	/// treat the plugin initialization as failed.
	bool initializeLogger();

	/// Matches the versioning system of the other packages.
	/// Override this method if you need to change the version for testing.
	unsigned int version() override { return SAUNAFS_VERSHEX; }

	/// Returns the disk prefix handled by this plugin, the part of an hdd.cfg
	/// line before the ':'. For instance, a plugin returning 'zonefs' handles
	/// zonefs:/mnt/saunafs/meta/nvme1 | /mnt/saunafs/data/smr1
	///
	/// Only letters, digits and '_' are accepted (disk::kDiskPrefixCharacters);
	/// a plugin offering anything else is loaded but never selected.
	virtual std::string prefix() = 0;

	/// Returns a newly created concrete Disk with the given configuration.
	virtual IDisk *createDisk(const disk::Configuration &configuration) = 0;

	/// Returns a string representation of the plugin. Useful for logging and
	/// debugging purposes.
	std::string toString() override;

	/// Do specific resources cleanup.
	virtual void cleanup() = 0;

private:
	/// Needed for correct logging after upgrade to c++23
	std::shared_ptr<spdlog::logger> logger_;
};
