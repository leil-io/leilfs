/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "common/platform.h"

#include <cstdint>

#include <common/exceptions.h>
#include <master/filesystem_node_types.h>
#include <master/metadata_dumper.h>

// Metadata related exceptions
SAUNAFS_CREATE_EXCEPTION_CLASS(MetadataCheckException, Exception);
SAUNAFS_CREATE_EXCEPTION_CLASS(MetadataException, Exception);
SAUNAFS_CREATE_EXCEPTION_CLASS(MetadataFsConsistencyException,
                               MetadataException);
SAUNAFS_CREATE_EXCEPTION_CLASS(MetadataConsistencyException, MetadataException);

// Constants
constexpr uint16_t kEdgeNameMaxSize = 65535;
constexpr uint8_t kEdgeHeaderSize =
    sizeof(FSNode::id) + sizeof(FSNode::id) + sizeof(kEdgeNameMaxSize);

constexpr uint8_t kNodeHeaderSize =
    sizeof(FSNode::type) + sizeof(FSNode::id) + sizeof(FSNode::goal) +
    sizeof(FSNode::mode) + sizeof(FSNode::uid) + sizeof(FSNode::gid) +
    sizeof(FSNode::atime) + sizeof(FSNode::mtime) + sizeof(FSNode::ctime) +
    sizeof(FSNode::trashtime);
// FSNodeFile is the longer type of FSNode, so we use it as the buffer size
constexpr uint8_t kFileSpecificHeaderSize =
    sizeof(FSNodeFile::length) + sizeof(uint32_t) + sizeof(uint16_t);
constexpr uint32_t kChunksBucketSize = 65536;
constexpr uint16_t kMaxSessionSize = 65535;
constexpr uint32_t kFileSpecificExtraSize =
    (sizeof(uint64_t) * kChunksBucketSize) +
    (sizeof(uint32_t) * kMaxSessionSize);

// TODO (Baldor): Review the need for these constants below
constexpr uint8_t kMetadataVersionLegacy = 0x15;
constexpr uint8_t kMetadataVersionSaunaFS = 0x16;
constexpr uint8_t kMetadataVersionWithSections = 0x20;
constexpr uint8_t kMetadataVersionWithLockIds = 0x29;
constexpr int8_t kOpSuccess = 0;
constexpr int8_t kOpFailure = -1;
constexpr char const MetadataStructureReadErrorMsg[] =
    "error reading metadata (structure)";

// Global variables
inline uint8_t gEdgeStoreBuffer[kEdgeHeaderSize + kEdgeNameMaxSize];
inline uint8_t gNodeStoreBuffer[kNodeHeaderSize + kFileSpecificHeaderSize +
                                kFileSpecificExtraSize];

// Number of changelog file versions
inline uint32_t gStoredPreviousBackMetaCopies;

class IMetadataBackend {
public:
	IMetadataBackend() = default;
	virtual ~IMetadataBackend() = default;

	// Remove not needed copy/move constructors to avoid misuse
	IMetadataBackend(const IMetadataBackend&) = delete;
	IMetadataBackend(IMetadataBackend&&) = delete;
	IMetadataBackend& operator=(const IMetadataBackend&) = delete;
	IMetadataBackend& operator=(IMetadataBackend&&) = delete;

	/// Returns the current metadata version
	virtual uint64_t getVersion(const std::string& file) = 0;

	/// Returns the concrete backend implementation type.
	/// To be used from configuration to instantiate the correct backend.
	virtual std::string backendType() = 0;

	/// @deprecated for version 5.0.0
	/// Rename changelog files from old to new version
	/// from <name>.X.sfs to <name>.sfs.X
	/// Used only once - after upgrade from version before 1.6.29
	/// @param name -- changelog name before first dot
	virtual void changelogsMigrateFrom_1_6_29(const std::string& fname) = 0;

// Available for master, shadow and metarestore
#ifndef METALOGGER
	/// Store metadata to the given file descriptor.
	/// This is a remanent of the old implementation, it should be removed
	/// gradually from this interface.
	virtual void store_fd(FILE *fd) = 0;

	/// Load complete metadata from the given file.
	/// @param fname -- path hint to the metadata file, directory or database
	///                 (to be defined by the concrete implementation)
	virtual void loadall(const std::string &fname, int ignoreflag) = 0;

	/// Returns version of the first entry in a changelog.
	/// @param file -- path to the changelog file
	/// @return 0 in case of any error.
	virtual uint64_t changelogGetFirstLogVersion(const std::string& fname) = 0;
	/// Returns version of the last entry in a changelog.
	/// @param file -- path to the changelog file
	/// @return 0 in case of any error.
	virtual uint64_t changelogGetLastLogVersion(const std::string& fname) = 0;
#else   // #ifndef METALOGGER
	virtual uint64_t findLastLogVersion() = 0;
#endif  // #ifndef METALOGGER

// Available for master and shadow only
#if !defined(METARESTORE) && !defined(METALOGGER)
	/// Broadcasts information about status of the freshly finished
	/// metadata save process to interested modules.
	virtual void broadcast_metadata_saved(uint8_t status) = 0;

	/// Load and apply changelogs.
	virtual void load_changelogs() = 0;
	/// Load and apply given changelog file.
	virtual void load_changelog(const std::string &path) = 0;

	/// Commits the metadata dump by rotating the metadata according the the
	/// concrete implementation.
	///
	/// If the process fails, it tries to create an emergency metadata with an
	/// unique name based on the current time.
	/// @return true if the metadata dump was successfully committed.
	virtual bool commit_metadata_dump() = 0;

	/// An error occurred during the metadata dump, save the metadata to an
	/// emergency location according to the concrete implementation.
	virtual int emergency_saves() = 0;

	/// Performs the actual metadata dump to persistent location.
	/// @param dumpType -- type of the dump (foreground, background, etc.).
	/// @return false in case of error.
	virtual uint8_t fs_storeall(MetadataDumper::DumpType dumpType) = 0;

	// TODO(guillex): Use a generic MetadaDumper later
	virtual MetadataDumper *dumper() = 0;
#endif  // #if !defined(METARESTORE) && !defined(METALOGGER)
};

/// Global variable to store the concrete metadata backend
inline std::unique_ptr<IMetadataBackend> gMetadataBackend = nullptr;
