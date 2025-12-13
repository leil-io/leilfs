/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
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

#include <master/filesystem_metadata.h>
#include <master/metadata_backend_interface.h>

class MetadataBackendFile : public IMetadataBackend {
public:
	MetadataBackendFile();

	/// Initializes the metadata backend.
	/// This method should be called before any other methods of the backend.
	/// This concrete implementation finds the metadata file to use.
	void init() override;

	/// Returns version of a metadata file.
	/// Throws MetadataCheckException if the file is corrupted, ie. contains
	/// wrong header or end marker.
	/// @param file -- path to the metadata binary file
	uint64_t getVersion(const std::string &file) override;

	/// Returns the current metadata header signature
	/// @return empty string (not implemented in MetadataBackendFile)
	std::string getHeaderSignature() override { return ""; }

	std::string backendType() override { return "MetadataBackendFile"; }

	void setMetadataFile(const std::string &metadataFile) {
		metadataFile_ = metadataFile;
	}

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

private:
	/// Fixed size section names. Examples: "NODE 1.0", "CHNK 1.0", etc.
	static constexpr size_t kSectionNameSize = 8;
	/// Section name length + size
	static constexpr size_t kSectionSize = kSectionNameSize + sizeof(uint64_t);

#ifndef METALOGGER
	// Nodes
	void storenode(FSNode *f, FILE *fd);
	void storenodes(FILE *fd);

	// Edges
	void storeedge(FSNodeDirectory *parent, FSNode *child,
	               const std::string &name, FILE *fd);
	void storeedgelist(FSNodeDirectory *parent, FILE *fd);
	void storeedgelist(const TrashPathContainer &data, FILE *fd);
	void storeedgelist(const ReservedPathContainer &data, FILE *fd);
	void storeedges_rec(FSNodeDirectory *f, FILE *fd);
	void storeedges(FILE *fd);

	// Free
	void storefree(FILE *fd);

	// XAttr
	void xattr_store(FILE *fd);

	// ACLS
	// The functions are in master/filesystem_store_acl.cc

	// Quotas
	void storequotas(FILE *fd);

	// Locks
	void storelocks(FILE *fd);

	// Full FS
	static int process_section(const char *label, uint8_t (&hdr)[kSectionSize],
	                           uint8_t *&ptr, off_t &offbegin, off_t &offend,
	                           FILE *&fd);

	void store(FILE *fd, uint8_t fver);
#endif  // #ifndef METALOGGER

#if !defined(METARESTORE) && !defined(METALOGGER)
	int emergency_storeall(const std::string &fname);

	std::unique_ptr<IMetadataDumper> dumper_;
#endif  // #ifndef METARESTORE

	std::string metadataFile_;
};
