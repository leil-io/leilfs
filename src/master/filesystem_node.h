/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2017 Skytechnology sp. z o.o.
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

#include "master/filesystem_node_operations_interface.h"
#include "master/filesystem_node_types.h"
#include "master/fs_context.h"
#include "protocol/directory_entry.h"
#include "protocol/handle_inode_entry.h"
#include "protocol/named_inode_entry.h"

/// Base class for filesystem node operations extensibility.
///
/// Provides default implementations for all methods of IFilesystemNodeOperations interface,
/// assuming an in-memory metadata representation.
class FilesystemNodeOperationsBase : public IFilesystemNodeOperations {
public:
	FSNode *fsnodes_lookup(FSNodeDirectory *node, const HString &name) const override;

	FSNode *fsnodes_create_node(uint32_t ts, FSNodeDirectory *parent, const HString &name,
	                            FSNodeType type, uint16_t mode, uint16_t umask, uint32_t uid,
	                            uint32_t gid, uint8_t copysgid, AclInheritance inheritacl,
	                            inode_t req_inode = 0) override;
	void fsnodes_link(uint32_t ts, FSNodeDirectory *parent, FSNode *child,
	                  const HString &name) override;
	void fsnodes_unlink(uint32_t ts, FSNodeDirectory *parent, const HString &node_name,
	                    FSNode *node) override;
	void fsnodes_remove_edge(uint32_t ts, FSNodeDirectory *parent, const HString &node_name,
	                         FSNode *node) override;

	void fsnodes_update_ctime(FSNode *node, uint32_t ctime) override;
	void fsnodes_fill_attr(FSNode *node, FSNode *parent, uint32_t uid, uint32_t gid, uint32_t auid,
	                       uint32_t agid, uint8_t sesflags, Attributes &attr) override;
	void fsnodes_fill_attr(const FsContext &context, FSNode *node, FSNode *parent,
	                       Attributes &attr) override;
	void fsnodes_get_stats(FSNode *node, StatsRecord *sr) override;
	void fsnodes_add_stats(FSNodeDirectory *parent, StatsRecord *sr) override;
	void fsnodes_add_sub_stats(FSNodeDirectory *parent, StatsRecord *newsr,
	                           StatsRecord *prevsr) override;
	void fsnodes_change_uid_gid(FSNode *p, uint32_t uid, uint32_t gid) override;

	void fsnodes_setlength(FSNodeFile *obj, uint64_t length, bool eraseFurtherChunks) override;
	uint8_t fsnodes_appendchunks(uint32_t ts, FSNodeFile *dstobj, FSNodeFile *srcobj) override;
	void fsnodes_changefilegoal(FSNodeFile *obj, uint8_t goal) override;
#ifndef METARESTORE
	void fsnodes_checkfile(FSNodeFile *p, uint32_t chunkcount[CHUNK_MATRIX_SIZE]) override;
#endif
	int64_t fsnodes_get_size(FSNode *node) override;

#ifndef METARESTORE
	uint32_t fsnodes_getdirsize(const FSNodeDirectory *p, uint8_t withattr) override;
	void fsnodes_getdirdata(inode_t rootinode, uint32_t uid, uint32_t gid, uint32_t auid,
	                        uint32_t agid, uint8_t sesflags, FSNodeDirectory *p, uint8_t *dbuff,
	                        uint8_t withattr) override;
	void fsnodes_getdir(inode_t rootinode, uint32_t uid, uint32_t gid, uint32_t auid, uint32_t agid,
	                    uint8_t sesflags, FSNodeDirectory *p, uint64_t first_entry,
	                    uint64_t number_of_entries,
	                    std::vector<DirectoryEntry> &dir_entries) override;
#endif
	int fsnodes_nameisused(FSNodeDirectory *node, const HString &name) override;

	// Trash/Reserved operations
	int fsnodes_purge(uint32_t ts, FSNode *p) override;
	uint8_t fsnodes_undel(uint32_t ts, FSNodeFile *node) override;
#ifndef METARESTORE
	uint32_t fsnodes_getdetachedsize(const TrashPathContainer &data) override;
	void fsnodes_getdetacheddata(const TrashPathContainer &data, uint8_t *dbuff) override;
	void fsnodes_getdetacheddata(const TrashPathContainer &data, uint32_t off, uint32_t max_entries,
	                             std::vector<NamedInodeEntry> &entries) override;
	uint32_t fsnodes_getdetachedsize(const ReservedPathContainer &data) override;
	void fsnodes_getdetacheddata(const ReservedPathContainer &data, uint8_t *dbuff) override;
	void fsnodes_getdetacheddata(const ReservedPathContainer &data, uint32_t off,
	                             uint32_t max_entries,
	                             std::vector<NamedInodeEntry> &entries) override;
	void fsnodes_getdetacheddata(const HandleIndexContainer &data, uint64_t handleOffset,
	                             uint32_t maxEntries, std::vector<HandleInodeEntry> &entries,
	                             bool fromTrash) override;
#endif

	// Path operations
	void fsnodes_getpath(FSNodeDirectory *parent, FSNode *child, std::string &path) override;
	uint32_t fsnodes_getpath_size(FSNodeDirectory *parent, FSNode *child) override;
	void fsnodes_getpath_data(FSNodeDirectory *parent, FSNode *child, uint8_t *path,
	                          uint32_t size) override;
	std::string fsnodes_escape_name(const std::string &name) override;

	// ACL operations
	uint8_t fsnodes_setacl(FSNode *p, const RichACL &acl, uint32_t ts) override;
	uint8_t fsnodes_setacl(FSNode *p, AclType type, const AccessControlList &acl,
	                       uint32_t ts) override;
#ifndef METARESTORE
	uint8_t fsnodes_getacl(FSNode *p, RichACL &acl) override;
#endif  // METARESTORE
	uint8_t fsnodes_deleteacl(FSNode *p, AclType type, uint32_t ts) override;

	// Recursive operations
#ifndef METARESTORE
	void fsnodes_getgoal_recursive(FSNode *node, uint8_t gmode, GoalStatistics &fgtab,
	                               GoalStatistics &dgtab) override;
	void fsnodes_gettrashtime_recursive(FSNode *node, uint8_t gmode, TrashtimeMap &fileTrashtimes,
	                                    TrashtimeMap &dirTrashtimes) override;
	void fsnodes_geteattr_recursive(FSNode *node, uint8_t gmode, uint32_t feattrtab[16],
	                                uint32_t deattrtab[16]) override;
#endif  // METARESTORE
	void fsnodes_setgoal_recursive(FSNode *node, uint32_t ts, uint32_t uid, uint8_t goal,
	                               uint8_t smode, inode_t *sinodes, inode_t *ncinodes,
	                               inode_t *nsinodes) override;

	void fsnodes_settrashtime_recursive(FSNode *node, uint32_t ts, uint32_t uid, uint32_t trashtime,
	                                    uint8_t smode, inode_t *sinodes, inode_t *ncinodes,
	                                    inode_t *nsinodes) override;
	void fsnodes_seteattr_recursive(FSNode *node, uint32_t ts, uint32_t uid, uint8_t eattr,
	                                uint8_t smode, inode_t *sinodes, inode_t *ncinodes,
	                                inode_t *nsinodes) override;

	// Access control operations
	int fsnodes_access(const FsContext &context, FSNode *node, uint8_t modemask) override;
	int fsnodes_sticky_access(FSNode *parent, FSNode *node, uint32_t uid) override;
	int fsnodes_namecheck(const std::string &name) override;
	uint8_t verify_session(const FsContext &context, OperationMode operationMode,
	                       SessionType sessionType) override;
	uint8_t fsnodes_get_node_for_operation(const FsContext &context,
	                                       ExpectedNodeType expectedNodeType, uint8_t modemask,
	                                       inode_t inode, FSNode **ret,
	                                       FSNodeDirectory **ret_rn = nullptr) override;

	// Ancestry operations
	bool fsnodes_isancestor(FSNodeDirectory *f, FSNode *p) override;
	bool fsnodes_isancestor_or_node_reserved_or_trash(FSNodeDirectory *f, FSNode *p) override;
	FSNodeDirectory *fsnodes_get_first_parent(FSNode *node) override;

protected:
	/// Internal node lookup operation - override in subclasses for custom storage
	FSNode *fsnodes_id_to_node_internal(inode_t id) override;

private:
	// Private helpers
	void fsnodes_sub_stats(FSNodeDirectory *parent, StatsRecord *sr);
	void fsnodes_remove_node(uint32_t ts, FSNode *node);

	uint32_t last_chunk_blocks(FSNodeFile *node);
	bool last_chunk_nonempty(FSNodeFile *node);
	uint32_t file_chunks(FSNodeFile *node);
	uint64_t file_size(FSNodeFile *node, uint32_t nonzero_chunks);
	uint64_t file_realsize(FSNodeFile *node, uint32_t nonzero_chunks, uint64_t file_size);
#ifndef METARESTORE
	uint32_t ec_chunk_realsize(uint32_t blocks, uint32_t data_part_count,
	                           uint32_t parity_part_count);
#endif
};
