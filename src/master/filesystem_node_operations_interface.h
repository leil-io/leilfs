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
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "common/platform.h"

#include <string>
#include <vector>

#include "common/attributes.h"
#include "master/filesystem_node_types.h"
#include "master/filesystem_trash_reserved_files.h"
#include "master/fs_context.h"
#include "protocol/directory_entry.h"
#include "protocol/handle_inode_entry.h"
#include "protocol/named_inode_entry.h"

class HString;
class RichACL;
class AccessControlList;
class FsContext;
struct StatsRecord;
struct DirectoryEntry;
struct HandleInodeEntry;
struct NamedInodeEntry;

/// Interface for filesystem node operations extensibility.
///
/// Classes implementing this interface can be used to override default filesystem node behavior.
class IFilesystemNodeOperations {
public:
	/// Default constructor
	IFilesystemNodeOperations() = default;

	/// Virtual destructor
	virtual ~IFilesystemNodeOperations() = default;

	/// Non-copyable and non-movable
	IFilesystemNodeOperations(const IFilesystemNodeOperations &) = delete;
	IFilesystemNodeOperations &operator=(const IFilesystemNodeOperations &) = delete;
	IFilesystemNodeOperations(IFilesystemNodeOperations &&) = delete;
	IFilesystemNodeOperations &operator=(IFilesystemNodeOperations &&) = delete;

	// Type-safe node lookup operations

	template <class NodeType>
	NodeType *fsnodes_id_to_node_verify(inode_t id) {
		auto *node = static_cast<NodeType *>(this->fsnodes_id_to_node_internal(id));
		this->fsnodes_check_node_type(node);
		return node;
	}

	template <class NodeType = FSNode>
	NodeType *fsnodes_id_to_node(inode_t id) {
		return static_cast<NodeType *>(this->fsnodes_id_to_node_internal(id));
	}

	// Main node operations

	virtual FSNode *fsnodes_lookup(FSNodeDirectory *node, const HString &name) const = 0;

	virtual FSNode *fsnodes_create_node(uint32_t ts, FSNodeDirectory *parent, const HString &name,
	                                    FSNodeType type, uint16_t mode, uint16_t umask,
	                                    uint32_t uid, uint32_t gid, uint8_t copysgid,
	                                    AclInheritance inheritacl, inode_t req_inode = 0) = 0;
	virtual void fsnodes_link(uint32_t ts, FSNodeDirectory *parent, FSNode *child,
	                          const HString &name) = 0;
	virtual void fsnodes_unlink(uint32_t ts, FSNodeDirectory *parent, const HString &node_name,
	                            FSNode *node) = 0;
	virtual void fsnodes_remove_edge(uint32_t ts, FSNodeDirectory *parent, const HString &node_name,
	                                 FSNode *node) = 0;

	virtual void fsnodes_update_ctime(FSNode *node, uint32_t ctime) = 0;
	virtual void fsnodes_fill_attr(FSNode *node, FSNode *parent, uint32_t uid, uint32_t gid,
	                               uint32_t auid, uint32_t agid, uint8_t sesflags,
	                               Attributes &attr) = 0;
	virtual void fsnodes_fill_attr(const FsContext &context, FSNode *node, FSNode *parent,
	                               Attributes &attr) = 0;
	virtual void fsnodes_get_stats(FSNode *node, StatsRecord *sr) = 0;
	virtual void fsnodes_add_stats(FSNodeDirectory *parent, StatsRecord *sr) = 0;
	virtual void fsnodes_add_sub_stats(FSNodeDirectory *parent, StatsRecord *newsr,
	                                   StatsRecord *prevsr) = 0;
	virtual void fsnodes_change_uid_gid(FSNode *p, uint32_t uid, uint32_t gid) = 0;

	virtual void fsnodes_setlength(FSNodeFile *obj, uint64_t length, bool eraseFurtherChunks) = 0;
	virtual uint8_t fsnodes_appendchunks(uint32_t ts, FSNodeFile *dstobj, FSNodeFile *srcobj) = 0;
	virtual void fsnodes_changefilegoal(FSNodeFile *obj, uint8_t goal) = 0;
#ifndef METARESTORE
	virtual void fsnodes_checkfile(FSNodeFile *p, uint32_t chunkcount[CHUNK_MATRIX_SIZE]) = 0;
#endif
	virtual int64_t fsnodes_get_size(FSNode *node) = 0;

#ifndef METARESTORE
	virtual uint32_t fsnodes_getdirsize(const FSNodeDirectory *p, uint8_t withattr) = 0;
	virtual void fsnodes_getdirdata(inode_t rootinode, uint32_t uid, uint32_t gid, uint32_t auid,
	                                uint32_t agid, uint8_t sesflags, FSNodeDirectory *p,
	                                uint8_t *dbuff, uint8_t withattr) = 0;
	virtual void fsnodes_getdir(inode_t rootinode, uint32_t uid, uint32_t gid, uint32_t auid,
	                            uint32_t agid, uint8_t sesflags, FSNodeDirectory *p,
	                            uint64_t first_entry, uint64_t number_of_entries,
	                            std::vector<DirectoryEntry> &dir_entries) = 0;
#endif
	virtual int fsnodes_nameisused(FSNodeDirectory *node, const HString &name) = 0;

	// Trash/Reserved operations

	virtual int fsnodes_purge(uint32_t ts, FSNode *p) = 0;
	virtual uint8_t fsnodes_undel(uint32_t ts, FSNodeFile *node) = 0;
#ifndef METARESTORE
	virtual uint32_t fsnodes_getdetachedsize(const TrashPathContainer &data) = 0;
	virtual void fsnodes_getdetacheddata(const TrashPathContainer &data, uint8_t *dbuff) = 0;
	virtual void fsnodes_getdetacheddata(const TrashPathContainer &data, uint32_t off,
	                                     uint32_t max_entries,
	                                     std::vector<NamedInodeEntry> &entries) = 0;
	virtual uint32_t fsnodes_getdetachedsize(const ReservedPathContainer &data) = 0;
	virtual void fsnodes_getdetacheddata(const ReservedPathContainer &data, uint8_t *dbuff) = 0;
	virtual void fsnodes_getdetacheddata(const ReservedPathContainer &data, uint32_t off,
	                                     uint32_t max_entries,
	                                     std::vector<NamedInodeEntry> &entries) = 0;
	virtual void fsnodes_getdetacheddata(const HandleIndexContainer &data, uint64_t handleOffset,
	                                     uint32_t maxEntries,
	                                     std::vector<HandleInodeEntry> &entries,
	                                     bool fromTrash) = 0;
#endif

	// Path operations
	virtual void fsnodes_getpath(FSNodeDirectory *parent, FSNode *child, std::string &path) = 0;
	virtual uint32_t fsnodes_getpath_size(FSNodeDirectory *parent, FSNode *child) = 0;
	virtual void fsnodes_getpath_data(FSNodeDirectory *parent, FSNode *child, uint8_t *path,
	                                  uint32_t size) = 0;
	virtual std::string fsnodes_escape_name(const std::string &name) = 0;

	// ACL operations
	virtual uint8_t fsnodes_setacl(FSNode *p, const RichACL &acl, uint32_t ts) = 0;
	virtual uint8_t fsnodes_setacl(FSNode *p, AclType type, const AccessControlList &acl,
	                               uint32_t ts) = 0;
#ifndef METARESTORE
	virtual uint8_t fsnodes_getacl(FSNode *p, RichACL &acl) = 0;
#endif  // METARESTORE
	virtual uint8_t fsnodes_deleteacl(FSNode *p, AclType type, uint32_t ts) = 0;

	// Recursive operations
#ifndef METARESTORE
	virtual void fsnodes_getgoal_recursive(FSNode *node, uint8_t gmode, GoalStatistics &fgtab,
	                                       GoalStatistics &dgtab) = 0;
	virtual void fsnodes_gettrashtime_recursive(FSNode *node, uint8_t gmode,
	                                            TrashtimeMap &fileTrashtimes,
	                                            TrashtimeMap &dirTrashtimes) = 0;
	virtual void fsnodes_geteattr_recursive(FSNode *node, uint8_t gmode, uint32_t feattrtab[16],
	                                        uint32_t deattrtab[16]) = 0;
#endif
	virtual void fsnodes_setgoal_recursive(FSNode *node, uint32_t ts, uint32_t uid, uint8_t goal,
	                                       uint8_t smode, inode_t *sinodes, inode_t *ncinodes,
	                                       inode_t *nsinodes) = 0;

	virtual void fsnodes_settrashtime_recursive(FSNode *node, uint32_t ts, uint32_t uid,
	                                            uint32_t trashtime, uint8_t smode, inode_t *sinodes,
	                                            inode_t *ncinodes, inode_t *nsinodes) = 0;

	virtual void fsnodes_seteattr_recursive(FSNode *node, uint32_t ts, uint32_t uid, uint8_t eattr,
	                                        uint8_t smode, inode_t *sinodes, inode_t *ncinodes,
	                                        inode_t *nsinodes) = 0;

	// Access control operations
	virtual int fsnodes_access(const FsContext &context, FSNode *node, uint8_t modemask) = 0;
	virtual int fsnodes_sticky_access(FSNode *parent, FSNode *node, uint32_t uid) = 0;
	virtual int fsnodes_namecheck(const std::string &name) = 0;
	virtual uint8_t verify_session(const FsContext &context, OperationMode operationMode,
	                               SessionType sessionType) = 0;
	virtual uint8_t fsnodes_get_node_for_operation(const FsContext &context,
	                                               ExpectedNodeType expectedNodeType,
	                                               uint8_t modemask, inode_t inode, FSNode **ret,
	                                               FSNodeDirectory **ret_rn = nullptr) = 0;

	// Ancestry operations
	virtual bool fsnodes_isancestor(FSNodeDirectory *f, FSNode *p) = 0;
	virtual bool fsnodes_isancestor_or_node_reserved_or_trash(FSNodeDirectory *f, FSNode *p) = 0;
	virtual FSNodeDirectory *fsnodes_get_first_parent(FSNode *node) = 0;

protected:
	// Core node lookup operation - override in subclasses for custom storage
	virtual FSNode *fsnodes_id_to_node_internal(inode_t id) = 0;

	// Type checking helper - base case
	template <class NodeType>
	void fsnodes_check_node_type(const NodeType *node) {
		assert(node);
		(void)node;
	}
};

// Template specializations for type checking
template <>
inline void IFilesystemNodeOperations::fsnodes_check_node_type(const FSNodeFile *node) {
	assert(node && (node->type == FSNodeType::kFile || node->type == FSNodeType::kTrash ||
	                node->type == FSNodeType::kReserved));
	(void)node;
}

template <>
inline void IFilesystemNodeOperations::fsnodes_check_node_type(const FSNodeDirectory *node) {
	assert(node && node->type == FSNodeType::kDirectory);
	(void)node;
}

template <>
inline void IFilesystemNodeOperations::fsnodes_check_node_type(const FSNodeSymlink *node) {
	assert(node && node->type == FSNodeType::kSymlink);
	(void)node;
}

template <>
inline void IFilesystemNodeOperations::fsnodes_check_node_type(const FSNodeDevice *node) {
	assert(node && (node->type == FSNodeType::kBlockDev || node->type == FSNodeType::kCharDev));
	(void)node;
};
