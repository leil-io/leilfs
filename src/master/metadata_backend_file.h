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

#include <master/metadata_backend_interface.h>

class MetadataBackendFile : public IMetadataBackend {
public:
	MetadataBackendFile();

	/// Returns version of a metadata file.
	/// Throws MetadataCheckException if the file is corrupted, ie. contains
	/// wrong header or end marker.
	/// @param file -- path to the metadata binary file
	uint64_t getVersion(const std::string &file) override;

	std::string backendType() override { return "MetadataBackendFile"; }

	/// @deprecated for version 5.0.0
	/// Rename changelog files from old to new version
	/// from <name>.X.sfs to <name>.sfs.X
	/// Used only once - after upgrade from version before 1.6.29
	/// @param name -- changelog name before first dot
	void changelogsMigrateFrom_1_6_29(const std::string& fname) override;

#ifndef METALOGGER
	/// Store metadata to the given file descriptor.
	void store_fd(FILE *fd) override;

	/// Load complete metadata from the given file.
	/// @param fname -- path to the metadata file.
	void loadall(const std::string &fname, int ignoreflag) override;

	/// Returns version of the first entry in a changelog.
	/// @param file -- path to the changelog file
	/// @return 0 in case of any error.
	uint64_t changelogGetFirstLogVersion(const std::string &fname) override;
	/// Returns version of the last entry in a changelog.
	/// @param file -- path to the changelog file
	/// @return 0 in case of any error.
	uint64_t changelogGetLastLogVersion(const std::string& fname) override;
#else   // #ifndef METALOGGER
	uint64_t findLastLogVersion() override;
#endif  // #ifndef METALOGGER

#if !defined(METARESTORE) && !defined(METALOGGER)
	/// Broadcasts information about status of the freshly finished
	/// metadata save process to interested modules.
	void broadcast_metadata_saved(uint8_t status) override;

	/// Load and apply changelogs.
	void load_changelogs() override;
	/// Load and apply given changelog file.
	void load_changelog(const std::string &path) override;

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
	uint8_t fs_storeall(MetadataDumper::DumpType dumpType) override;

	MetadataDumper *dumper() override { return dumper_.get(); }
#endif  // #if !defined(METARESTORE) && !defined(METALOGGER)

private:
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
	static int process_section(const char *label, uint8_t (&hdr)[16],
	                           uint8_t *&ptr, off_t &offbegin, off_t &offend,
	                           FILE *&fd);

	void store(FILE *fd, uint8_t fver);
#endif  // #ifndef METALOGGER

#if !defined(METARESTORE) && !defined(METALOGGER)
	int emergency_storeall(const std::string &fname);

	std::unique_ptr<MetadataDumper> dumper_;
#endif  // #ifndef METARESTORE
};
