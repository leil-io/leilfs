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
	FSNode *lookup(FSNodeDirectory *node, const HString &name) const override;

	FSNode *createNode(uint32_t ts, FSNodeDirectory *parent, const HString &name, FSNodeType type,
	                   uint16_t mode, uint16_t umask, uint32_t uid, uint32_t gid, uint8_t copysgid,
	                   AclInheritance inheritacl, inode_t req_inode = 0) override;
	void link(uint32_t ts, FSNodeDirectory *parent, FSNode *child, const HString &name) override;
	void unlink(uint32_t ts, FSNodeDirectory *parent, const HString &node_name,
	            FSNode *node) override;
	void removeEdge(uint32_t ts, FSNodeDirectory *parent, const HString &node_name,
	                FSNode *node) override;

	void updateCTime(FSNode *node, uint32_t ctime) override;
	void fillAttr(FSNode *node, FSNode *parent, uint32_t uid, uint32_t gid, uint32_t auid,
	              uint32_t agid, uint8_t sesflags, Attributes &attr) override;
	void fillAttr(const FsContext &context, FSNode *node, FSNode *parent,
	              Attributes &attr) override;
	void getStats(FSNode *node, StatsRecord *sr) override;
	void addStats(FSNodeDirectory *parent, StatsRecord *sr) override;
	void addSubStats(FSNodeDirectory *parent, StatsRecord *newsr, StatsRecord *prevsr) override;
	void changeUidGid(FSNode *p, uint32_t uid, uint32_t gid) override;

	void setLength(FSNodeFile *obj, uint64_t length, bool eraseFurtherChunks) override;
	uint8_t appendChunks(uint32_t ts, FSNodeFile *dstobj, FSNodeFile *srcobj) override;
	void changeFileGoal(FSNodeFile *obj, uint8_t goal) override;
#ifndef METARESTORE
	void checkFile(FSNodeFile *p, uint32_t chunkcount[CHUNK_MATRIX_SIZE]) override;
#endif
	int64_t getSize(FSNode *node) override;

#ifndef METARESTORE
	uint32_t getDirSize(const FSNodeDirectory *p, uint8_t withattr) override;
	void getDirData(inode_t rootinode, uint32_t uid, uint32_t gid, uint32_t auid, uint32_t agid,
	                uint8_t sesflags, FSNodeDirectory *p, uint8_t *dbuff,
	                uint8_t withattr) override;
	void getDir(inode_t rootinode, uint32_t uid, uint32_t gid, uint32_t auid, uint32_t agid,
	            uint8_t sesflags, FSNodeDirectory *p, uint64_t first_entry,
	            uint64_t number_of_entries, std::vector<DirectoryEntry> &dir_entries) override;
#endif
	int isNameUsed(FSNodeDirectory *node, const HString &name) override;

	// Trash/Reserved operations
	int purge(uint32_t ts, FSNode *p) override;
	uint8_t undel(uint32_t ts, FSNodeFile *node) override;
#ifndef METARESTORE
	uint32_t getDetachedSize(const TrashPathContainer &data) override;
	void getDetachedData(const TrashPathContainer &data, uint8_t *dbuff) override;
	void getDetachedData(const TrashPathContainer &data, uint32_t off, uint32_t max_entries,
	                     std::vector<NamedInodeEntry> &entries) override;
	uint32_t getDetachedSize(const ReservedPathContainer &data) override;
	void getDetachedData(const ReservedPathContainer &data, uint8_t *dbuff) override;
	void getDetachedData(const ReservedPathContainer &data, uint32_t off, uint32_t max_entries,
	                     std::vector<NamedInodeEntry> &entries) override;
	void getDetachedData(const HandleIndexContainer &data, uint64_t handleOffset,
	                     uint32_t maxEntries, std::vector<HandleInodeEntry> &entries,
	                     bool fromTrash) override;
#endif

	// Path operations
	void getPath(FSNodeDirectory *parent, FSNode *child, std::string &path) override;
	uint32_t getPathSize(FSNodeDirectory *parent, FSNode *child) override;
	void getPathData(FSNodeDirectory *parent, FSNode *child, uint8_t *path, uint32_t size) override;
	std::string escapeName(const std::string &name) override;

	// ACL operations
	uint8_t setAcl(FSNode *p, const RichACL &acl, uint32_t ts) override;
	uint8_t setAcl(FSNode *p, AclType type, const AccessControlList &acl, uint32_t ts) override;
#ifndef METARESTORE
	uint8_t getAcl(FSNode *p, RichACL &acl) override;
#endif  // METARESTORE
	uint8_t deleteAcl(FSNode *p, AclType type, uint32_t ts) override;

	// Recursive operations
#ifndef METARESTORE
	void getGoalRecursive(FSNode *node, uint8_t gmode, GoalStatistics &fgtab,
	                      GoalStatistics &dgtab) override;
	void getTrashTimeRecursive(FSNode *node, uint8_t gmode, TrashtimeMap &fileTrashtimes,
	                           TrashtimeMap &dirTrashtimes) override;
	void getEAttrRecursive(FSNode *node, uint8_t gmode, uint32_t feattrtab[16],
	                       uint32_t deattrtab[16]) override;
#endif  // METARESTORE
	void setgoalRecursive(FSNode *node, uint32_t ts, uint32_t uid, uint8_t goal, uint8_t smode,
	                      inode_t *sinodes, inode_t *ncinodes, inode_t *nsinodes) override;

	void setTrashTimeRecursive(FSNode *node, uint32_t ts, uint32_t uid, uint32_t trashtime,
	                           uint8_t smode, inode_t *sinodes, inode_t *ncinodes,
	                           inode_t *nsinodes) override;
	void setEAttrRecursive(FSNode *node, uint32_t ts, uint32_t uid, uint8_t eattr, uint8_t smode,
	                       inode_t *sinodes, inode_t *ncinodes, inode_t *nsinodes) override;

	// Access control operations
	int access(const FsContext &context, FSNode *node, uint8_t modemask) override;
	int stickyAccess(FSNode *parent, FSNode *node, uint32_t uid) override;
	int nameCheck(const std::string &name) override;
	uint8_t verifySession(const FsContext &context, OperationMode operationMode,
	                      SessionType sessionType) override;
	uint8_t getNodeForOperation(const FsContext &context, ExpectedNodeType expectedNodeType,
	                            uint8_t modemask, inode_t inode, FSNode **ret,
	                            FSNodeDirectory **ret_rn = nullptr) override;

	// Ancestry operations
	bool isAncestor(FSNodeDirectory *f, FSNode *p) override;
	bool isAncestorOrNodeReservedOrTrash(FSNodeDirectory *f, FSNode *p) override;
	FSNodeDirectory *getFirstParent(FSNode *node) override;

protected:
	/// Internal node lookup operation - override in subclasses for custom storage
	FSNode *idToNodeInternal(inode_t id) override;

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
