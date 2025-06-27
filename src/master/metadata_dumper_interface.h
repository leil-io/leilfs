/*
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

#include <poll.h>
#include <syslog.h>
#include <unistd.h>
#include <cstdint>
#include <string>
#include <vector>

/// Enum to represent the type of metadata dump
enum class DumpType : uint8_t {
	kForegroundDump,  ///< Foreground dump (immediate dump by the current process)
	kBackgroundDump   ///< Background dump (periodic or scheduled, maybe through forking)
};

/// Interface for metadata dumper implementations
/// This interface defines the methods required for any metadata dumper,
/// allowing for different implementations (e.g., file-based, network-based).
/// It also provides a way to check the status of the dump and manage its lifecycle.
class IMetadataDumper {
public:
	/// Default constructor
	IMetadataDumper() = default;

	/// Disable unnecessary copy and move operations
	IMetadataDumper(const IMetadataDumper &) = delete;
	IMetadataDumper &operator=(const IMetadataDumper &) = delete;
	IMetadataDumper(IMetadataDumper &&) = delete;
	IMetadataDumper &operator=(IMetadataDumper &&) = delete;

	/// Virtual destructor to allow proper cleanup of derived classes
	virtual ~IMetadataDumper() = default;

	virtual bool dumpSucceeded() const = 0;
	virtual bool inProgress() const = 0;
	virtual bool useMetarestore() const = 0;

	virtual void setMetarestorePath(const std::string& path) = 0;
	virtual void setUseMetarestore(bool val) = 0;

	/// returns true and modifies dumpType (to FOREGROUND_DUMP) if we return as a child
	virtual bool start(DumpType& dumpType, uint64_t checksum) = 0;

	// for poll
	virtual void pollDesc(std::vector<pollfd> &pdesc) = 0;
	virtual void pollServe(const std::vector<pollfd> &pdesc) = 0;

	/// waits until the metadumper finishes
	virtual void waitUntilFinished() = 0;
};
