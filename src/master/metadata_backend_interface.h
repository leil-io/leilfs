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
#include <master/exceptions.h>
#include <master/filesystem_node_types.h>
#include <master/metadata_dumper_interface.h>

// TODO (Baldor): Review the need for these constants below
constexpr uint8_t kMetadataVersionLegacy = 0x15;
constexpr uint8_t kMetadataVersionSaunaFS = 0x16;
constexpr uint8_t kMetadataVersionWithSections = 0x20;
constexpr uint8_t kMetadataVersionWithLockIds = 0x29;
constexpr int8_t kOpSuccess = 0;
constexpr int8_t kOpFailure = -1;
constexpr char const MetadataStructureReadErrorMsg[] = "error reading metadata (structure)";

// Global variables
inline uint8_t gEdgeStoreBuffer[FSNode::kEdgeHeaderSize + FSNode::kEdgeNameMaxSize];
inline uint8_t gNodeStoreBuffer[FSNodeFile::kMaxBufferSize];

// Number of metadata file versions to keep
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

	/// Initializes the metadata backend.
	/// This method should be called before any other methods of the backend.
	virtual void init() = 0;

	/// Returns the current metadata version
	virtual uint64_t getVersion(const std::string& file) = 0;

	/// Returns the current metadata header signature
	virtual std::string getHeaderSignature() = 0;

	/// Returns the concrete backend implementation type.
	/// To be used from configuration to instantiate the correct backend.
	virtual std::string backendType() = 0;

// Available for master, shadow and metarestore
#ifndef METALOGGER
	/// Store metadata to the given file descriptor.
	/// This is a remanent of the old implementation, it should be removed
	/// gradually from this interface.
	virtual void store_fd(FILE *fd) = 0;

	/// Load complete metadata.
	virtual void loadall(int ignoreflag) = 0;
#endif  // #ifndef METALOGGER

// Available for master and shadow only
#if !defined(METARESTORE) && !defined(METALOGGER)
	/// Broadcasts information about status of the freshly finished
	/// metadata save process to interested modules.
	virtual void broadcast_metadata_saved(uint8_t status) = 0;

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
	virtual uint8_t fs_storeall(DumpType dumpType) = 0;

	// TODO(guillex): Use a generic MetadaDumper later
	virtual IMetadataDumper *dumper() = 0;
#endif  // #if !defined(METARESTORE) && !defined(METALOGGER)
};

/// Global variable to store the concrete metadata backend
inline std::unique_ptr<IMetadataBackend> gMetadataBackend = nullptr;
