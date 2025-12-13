/*
   Copyright 2023      Leil Storage OÜ

   This file is part of SaunaFS.

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

#include "kv/ikv_engine.h"
#include "master/kv_connector_interface.h"
#include "master/metadata_backend_interface.h"
#include "master/metadata_writer_fdb.h"

// Forward declarations to avoid heavy includes in the header
struct ChangelogEvent;
namespace hstorage { class Handle; }

/// Simplified Metadata Section structure for FoundationDB
struct MetadataSectionFDB {
	std::string name;         ///< Name of the section
	std::string_view prefix;  ///< Prefix for the section keys

	std::function<int8_t(bool)> loadFunction;  ///< Function to load the section
};

class MetadataBackendForkless : public IMetadataBackend {
public:
	MetadataBackendForkless();
	~MetadataBackendForkless() override;

	// This instance registers itself in the gForklessBackend global; copying or
	// moving it would break that identity, so disable both.
	MetadataBackendForkless(const MetadataBackendForkless &) = delete;
	MetadataBackendForkless &operator=(const MetadataBackendForkless &) = delete;
	MetadataBackendForkless(MetadataBackendForkless &&) = delete;
	MetadataBackendForkless &operator=(MetadataBackendForkless &&) = delete;

	/// Initializes the metadata backend.
	/// This method should be called before any other methods of the backend.
	void init() override;

	/// Returns version of the metadata.
	/// @param file -- path to the metadata binary file (Ignored in FDB).
	uint64_t getVersion(const std::string &file) override;

	/// Returns the current metadata header signature
	std::string getHeaderSignature() override;

	std::string backendType() override { return "MetadataBackendForkless"; }

#ifndef METALOGGER
	/// Store metadata to the given file descriptor.
	void store_fd(FILE *fd) override;

	/// Load complete metadata.
	void loadall(int ignoreflag) override;
#endif  // #ifndef METALOGGER

#if !defined(METARESTORE) && !defined(METALOGGER)
	/// Broadcasts information about status of the freshly finished
	/// metadata save process to interested modules.
	void broadcast_metadata_saved(uint8_t status) override;

	/// Commits the metadata dump by rotating the metadata files and renaming
	/// the temporary file.
	///
	/// This function attempts to rotate the metadata files and rename the
	/// temporary metadata file to the main metadata file. If the renaming
	/// fails, it tries to create an emergency metadata file with a unique name
	/// based on the current time.
	/// @return true if the metadata dump was successfully committed.
	bool commit_metadata_dump() override;

	/// Save metadata to an emergency location (most likely an error occurred
	/// during the metadata dump).
	int emergency_saves() override;

	/// Performs the actual metadata dump to persistent location.
	/// @param dumpType -- type of the dump (foreground, background, etc.).
	/// @return false in case of error.
	uint8_t fs_storeall(DumpType dumpType) override;

	IMetadataDumper *dumper() override { return dumper_.get(); }
#endif  // #if !defined(METARESTORE) && !defined(METALOGGER)

	/// Returns a pointer to the underlying key-value engine.
	kv::IKVEngine *getKVEngine() { return kvConnector_->getKVEngine(); }

	/// Flush all pending batched updates
	void flushPendingUpdates();

private:
	/// Initializes the vector of metadata sections for later loading
	void initSections();

	///  Initializes new metadata in FDB
	void initializeNewMetadataHeader();

	///  Registers observers/watchers on selected metadata properties
	void createConnections();

	// FS Load from FDB

	/// Loads all sections
	int fsLoad(bool ignoreFlag);

	/// Loads CHNK_ metadata
	int8_t loadChunks(bool ignoreFlag);

	/// Provides connection to the key-value store (FoundationDB for this implementation)
	std::shared_ptr<IKVConnector> kvConnector_;

	/// Metadata sections with their loading functions
	std::vector<MetadataSectionFDB> metadataSections_;

#if !defined(METARESTORE) && !defined(METALOGGER)
	std::unique_ptr<IMetadataDumper> dumper_;
#endif  // #ifndef METARESTORE

	/// Metadata writer for all metadata updates
	std::unique_ptr<MetadataWriterFDB> metadataWriter_;
};
