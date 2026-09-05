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

#include <sys/types.h>
#include <cstdint>

#include "common/massert.h"

namespace disk {

constexpr int kStatsHistoryIn24Hours = 24 * 60;
constexpr uint8_t kLastErrorSize = 30;

constexpr uint32_t kSecondsInOneMinute = 60;
constexpr uint32_t kMinutesInOneHour = 60;

/// Number of bytes which should be added to each disk's used space
inline uint64_t gLeaveFree;

/// Default value for HDD_LEAVE_SPACE_DEFAULT
constexpr char gLeaveSpaceDefaultDefaultStrValue[] = "4GiB";

constexpr uint32_t kMaxUInt32Number = std::numeric_limits<uint32_t>::max();
inline constexpr uint64_t kIoBlockSize = 4096UL;

static constexpr size_t kMarkedForDeletionFlagIndex = 0;
static constexpr size_t kIsDamageFlagIndex = 1;
static constexpr size_t kScanInProgressFlagIndex = 2;

static constexpr mode_t kDefaultOpenMode = 0666;

/// Characters allowed in an hdd.cfg "<prefix>:" plugin selector. Excludes '/'
/// and '.', so a plain path is never parsed as a prefix.
constexpr char kDiskPrefixCharacters[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";

/// The only prefix denoting a zoned device; every other plugin serves a
/// conventional one.
constexpr char kZonedDiskPrefix[] = "zonefs";

/// Possible modes to call the `hddChunkFindOrCreatePlusLock` function.
enum class ChunkGetMode {
	kFindOnly,      ///< Do not create any new Chunk, just look for existing.
	kCreateOnly,    ///< Only create a new Chunk; error out if it exists.
	kFindOrCreate,  ///< If the Chunk does not exists, create a new one.
};

/// An I/O error which happened when accessing chunks in this Disk.
struct IoError {
	/// A constructor.
	IoError() = default;

	uint64_t chunkid{0};    ///< The ID of the chunk which caused the error.
	uint32_t timestamp{0};  ///< The timestamp of the error.
	int errornumber{0};     ///< The error number (a.k.a., errno) of the error.
};

/// Lock class to avoid different chunkservers using the same Disk
class LockFile {
public:
	/// Constructor of a LockFile object.
	///
	/// \param fd    Lock-file's file descriptor.
	/// \param dev   Lock-file's device number (probably from stat).
	/// \param inode Lock-file's Inode number.
	LockFile(int fileDescriptor, dev_t dev, ino_t inode)
	    : fd_(fileDescriptor), device_(dev), inode_(inode) {
		sassert(fileDescriptor != -1);
	}

	// No need for copying or moving lock file objects.
	LockFile(const LockFile &) = delete;
	LockFile(LockFile &&) = delete;
	LockFile &operator=(const LockFile &) = delete;
	LockFile &operator=(LockFile &&) = delete;

	/// Releases the lock file if needed.
	~LockFile() {
		if (fd_ >= 0) {
			close(fd_);
		}
	}

	/// True if this lock file is in the device `dev`.
	bool isInTheSameDevice(dev_t dev) const { return device_ == dev; }

	/// True if this lock file is the same.
	bool isTheSameFile(dev_t dev, ino_t inode) const {
		return device_ == dev && inode_ == inode;
	}

private:
	int fd_ = -1;   ///< Lock-file's file descriptor.
	dev_t device_;  ///< Lock-file's device number.
	ino_t inode_;   ///< Lock-file's Inode number.
};

/// Disk description parsed from the HDD config file.
struct Configuration {
	/// Constructor: parses the hdd configuration line.
	explicit Configuration(std::string hddCfgLine);

	/// Constructor: mostly intended for tests.
	Configuration(const std::string &_metaPath, const std::string &_dataPath,
	              bool _isMarkedForRemoval, bool _isZonedDevice);

	/// The path to store the chunk metadata files.
	std::string metaPath;

	/// The path to store the chunk data files. Can be the same as metaPath
	/// if there is only one path in the hdd cfg line.
	std::string dataPath;

	/// Used to determine the type of Disk to instantiate. E.g.: zonefs.
	/// Empty prefix is handled by CmrDisks.
	std::string prefix;

	/// Tells if the entry is marked for removal (i.e.,
	/// prefixed with '*') in the configuration file.
	bool isMarkedForRemoval = false;

	/// It is a zoned device, probably SMR.
	bool isZoned = false;

	/// The hdd cfg line was parsed correctly or no need to parse
	bool isValid = false;

	/// The line was prefixed with '#' character
	bool isComment = false;

	/// It is a blank line
	bool isEmpty = false;
};

}  // namespace disk
