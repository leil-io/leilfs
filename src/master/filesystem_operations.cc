/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2017 Skytechnology sp. z o.o.
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

#include "common/platform.h"

#include "master/filesystem_operations.h"

#include <cstdarg>
#include <cstdint>

#include "common/attributes.h"
#include "common/event_loop.h"
#include "filesystem_metadata.h"
#include "master/changelog.h"
#include "master/chunks.h"
#include "master/filesystem.h"
#include "master/filesystem_checksum.h"
#include "master/filesystem_checksum_updater.h"
#include "master/filesystem_node.h"
#include "master/filesystem_quota.h"
#include "master/fs_context.h"
#include "master/locks.h"
#include "master/matoclserv.h"
#include "master/matocsserv.h"
#include "master/matomlserv.h"
#include "master/recursive_remove_task.h"
#include "master/task_manager.h"
#include "metrics/metrics.h"
#include "protocol/matocl.h"

std::array<uint32_t, FsStats::Size> gFsStatsArray = {{}};

[[maybe_unused]] static const char kAclXattrs[] = "system.richacl";

void fs_retrieve_stats(std::array<uint32_t, FsStats::Size> &output_stats) {
	output_stats = gFsStatsArray;
	gFsStatsArray.fill(0);
}

static const int kInitialTaskBatchSize = 1000;

template <class T>
bool decodeChar(const char *keys, const std::vector<T> values, char key, T &value) {
	const uint32_t count = strlen(keys);
	sassert(values.size() == count);
	for (uint32_t i = 0; i < count; i++) {
		if (key == keys[i]) {
			value = values[i];
			return true;
		}
	}
	return false;
}

void fs_changelog(uint32_t ts, const char *format, ...) {
#ifdef METARESTORE
	(void)ts;
	(void)format;
#else
	const uint32_t kMaxTimestampSize = 20;
	const uint32_t kMaxEntrySize = kMaxLogLineSize - kMaxTimestampSize;
	static char entry[kMaxLogLineSize];

	// First, put "<timestamp>|" in the buffer
	int tsLength = snprintf(entry, kMaxTimestampSize, "%" PRIu32 "|", ts);

	// Then append the entry to the buffer
	va_list ap;
	uint32_t entryLength;
	va_start(ap, format);
	entryLength = vsnprintf(entry + tsLength, kMaxEntrySize, format, ap);
	va_end(ap);

	if (entryLength >= kMaxEntrySize) {
		entry[tsLength + kMaxEntrySize - 1] = '\0';
		entryLength = kMaxEntrySize;
	} else {
		entryLength++;
	}

	uint64_t version = gMetadata->metaversion++;
	changelog(version, entry);
	matomlserv_broadcast_logstring(version, (uint8_t *)entry, tsLength + entryLength);
#endif
}

#ifndef METARESTORE
uint8_t fs_readreserved_size(uint32_t rootinode, uint8_t sesflags, uint32_t *dbuffsize) {
	if (rootinode != 0) {
		return SAUNAFS_ERROR_EPERM;
	}
	(void)sesflags;
	*dbuffsize = fsnodes_getdetachedsize(gMetadata->reserved);
	return SAUNAFS_STATUS_OK;
}

void fs_readreserved_data(uint32_t rootinode, uint8_t sesflags, uint8_t *dbuff) {
	(void)rootinode;
	(void)sesflags;
	fsnodes_getdetacheddata(gMetadata->reserved, dbuff);
}

void fs_readreserved(uint32_t off, uint32_t max_entries, std::vector<NamedInodeEntry> &entries) {
	fsnodes_getdetacheddata(gMetadata->reserved, off, max_entries, entries);
}

uint8_t fs_readtrash_size(uint32_t rootinode, uint8_t sesflags, uint32_t *dbuffsize) {
	if (rootinode != 0) {
		return SAUNAFS_ERROR_EPERM;
	}
	(void)sesflags;
	*dbuffsize = fsnodes_getdetachedsize(gMetadata->trash);
	return SAUNAFS_STATUS_OK;
}

void fs_readtrash_data(uint32_t rootinode, uint8_t sesflags, uint8_t *dbuff) {
	(void)rootinode;
	(void)sesflags;
	fsnodes_getdetacheddata(gMetadata->trash, dbuff);
}

void fs_readtrash(uint32_t off, uint32_t max_entries, std::vector<NamedInodeEntry> &entries) {
	fsnodes_getdetacheddata(gMetadata->trash, off, max_entries, entries);
}

/* common procedure for trash and reserved files */
uint8_t fs_getdetachedattr(uint32_t rootinode, uint8_t sesflags, uint32_t inode, Attributes &attr,
				uint8_t dtype) {
	FSNode *p;
	attr.fill(0);
	if (rootinode != 0) {
		return SAUNAFS_ERROR_EPERM;
	}
	(void)sesflags;
	if (!DTYPE_ISVALID(dtype)) {
		return SAUNAFS_ERROR_EINVAL;
	}
	p = fsnodes_id_to_node(inode);
	if (!p) {
		return SAUNAFS_ERROR_ENOENT;
	}
	if (p->type != FSNode::kTrash && p->type != FSNode::kReserved) {
		return SAUNAFS_ERROR_ENOENT;
	}
	if (dtype == DTYPE_TRASH && p->type == FSNode::kReserved) {
		return SAUNAFS_ERROR_ENOENT;
	}
	if (dtype == DTYPE_RESERVED && p->type == FSNode::kTrash) {
		return SAUNAFS_ERROR_ENOENT;
	}
	fsnodes_fill_attr(p, NULL, p->uid, p->gid, p->uid, p->gid, sesflags, attr);
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_gettrashpath(uint32_t rootinode, uint8_t sesflags, uint32_t inode, std::string &path) {
	FSNode *p;
	if (rootinode != 0) {
		return SAUNAFS_ERROR_EPERM;
	}
	(void)sesflags;
	p = fsnodes_id_to_node(inode);
	if (!p) {
		return SAUNAFS_ERROR_ENOENT;
	}
	if (p->type != FSNode::kTrash) {
		return SAUNAFS_ERROR_ENOENT;
	}
	path = (std::string)gMetadata->trash.at(TrashPathKey(p));
	return SAUNAFS_STATUS_OK;
}
#endif

uint8_t fs_settrashpath(const FsContext &context, uint32_t inode, const std::string &path) {
	ChecksumUpdater cu(context.ts());
	FSNode *p;
	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kOnlyMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kAny, MODE_MASK_EMPTY,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	} else if (p->type != FSNode::kTrash) {
		return SAUNAFS_ERROR_ENOENT;
	} else if (path.length() == 0) {
		return SAUNAFS_ERROR_EINVAL;
	}
	for (uint32_t i = 0; i < path.length(); i++) {
		if (path[i] == 0) {
			return SAUNAFS_ERROR_EINVAL;
		}
	}

	gMetadata->trash[TrashPathKey(p)] = HString(path);

	if (context.isPersonalityMaster()) {
		fs_changelog(context.ts(), "SETPATH(%" PRIu32 ",%s)", p->id,
		             fsnodes_escape_name(path).c_str());
	} else {
		gMetadata->metaversion++;
	}
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_undel(const FsContext &context, uint32_t inode) {
	ChecksumUpdater cu(context.ts());
	FSNode *p;
	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kOnlyMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kAny, MODE_MASK_EMPTY,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	} else if (p->type != FSNode::kTrash) {
		return SAUNAFS_ERROR_ENOENT;
	}

	status = fsnodes_undel(context.ts(), static_cast<FSNodeFile*>(p));
	if (context.isPersonalityMaster()) {
		if (status == SAUNAFS_STATUS_OK) {
			fs_changelog(context.ts(), "UNDEL(%" PRIu32 ")", p->id);
		}
	} else {
		gMetadata->metaversion++;
	}
	return status;
}

uint8_t fs_purge(const FsContext &context, uint32_t inode) {
	ChecksumUpdater cu(context.ts());
	FSNode *p;
	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kOnlyMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kAny, MODE_MASK_EMPTY,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	} else if (p->type != FSNode::kTrash) {
		return SAUNAFS_ERROR_ENOENT;
	}
	uint32_t purged_inode =
	        p->id;  // This should be equal to inode, because p is not a directory
	fsnodes_purge(context.ts(), p);

	if (context.isPersonalityMaster()) {
		fs_changelog(context.ts(), "PURGE(%" PRIu32 ")", purged_inode);
	} else {
		gMetadata->metaversion++;
	}
	return SAUNAFS_STATUS_OK;
}

#ifndef METARESTORE
void fs_info(uint64_t *totalspace, uint64_t *availspace, uint64_t *trspace,
             uint32_t *trnodes, uint64_t *respace, uint32_t *renodes,
             uint32_t *inodes, uint32_t *dnodes, uint32_t *fnodes,
             uint32_t *lnodes) {
	matocsserv_getspace(totalspace, availspace);
	*trspace = gMetadata->trashspace;
	*trnodes = gMetadata->trashnodes;
	*respace = gMetadata->reservedspace;
	*renodes = gMetadata->reservednodes;
	*inodes = gMetadata->nodes;
	*dnodes = gMetadata->dirnodes;
	*fnodes = gMetadata->filenodes;
	*lnodes = gMetadata->linknodes;
}

uint8_t fs_getrootinode(uint32_t *rootinode, const uint8_t *path) {
	HString hname;
	uint32_t nleng;
	const uint8_t *name;
	FSNodeDirectory *parent;

	name = path;
	parent = gMetadata->root;
	for (;;) {
		while (*name == '/') {
			name++;
		}
		if (*name == '\0') {
			*rootinode = parent->id;
			return SAUNAFS_STATUS_OK;
		}
		nleng = 0;
		while (name[nleng] && name[nleng] != '/') {
			nleng++;
		}
		hname = HString((const char*)name, nleng);
		if (fsnodes_namecheck(hname) < 0) {
			return SAUNAFS_ERROR_EINVAL;
		}
		FSNode *child = fsnodes_lookup(parent, hname);
		if (!child) {
			return SAUNAFS_ERROR_ENOENT;
		}
		if (child->type != FSNode::kDirectory) {
			return SAUNAFS_ERROR_ENOTDIR;
		}
		parent = static_cast<FSNodeDirectory*>(child);
		name += nleng;
	}
}

void fs_statfs(const FsContext &context, uint64_t *totalspace, uint64_t *availspace,
		uint64_t *trspace, uint64_t *respace, uint32_t *inodes) {
	FSNode *rn;
	statsrecord sr;
	if (context.rootinode() == SPECIAL_INODE_ROOT) {
		*trspace = gMetadata->trashspace;
		*respace = gMetadata->reservedspace;
		rn = gMetadata->root;
	} else {
		*trspace = 0;
		*respace = 0;
		rn = fsnodes_id_to_node(context.rootinode());
	}
	if (!rn || rn->type != FSNode::kDirectory) {
		*totalspace = 0;
		*availspace = 0;
		*inodes = 0;
	} else {
		matocsserv_getspace(totalspace, availspace);
		fsnodes_quota_adjust_space(rn, *totalspace, *availspace);
		fsnodes_get_stats(rn, &sr);
		*inodes = sr.inodes;
	}
	++gFsStatsArray[FsStats::Statfs];
	metrics::Counter::increment(metrics::Counter::Master::FS_STATFS);
}
#endif /* #ifndef METARESTORE */

uint8_t fs_apply_checksum(const std::string &version, uint64_t checksum) {
	std::string versionString = saunafsVersionToString(SAUNAFS_VERSHEX);
	uint64_t computedChecksum = fs_checksum(ChecksumMode::kGetCurrent);
	gMetadata->metaversion++;
	if (!gDisableChecksumVerification && (version == versionString)) {
		if (checksum != computedChecksum) {
			return SAUNAFS_ERROR_BADMETADATACHECKSUM;
		}
	}
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_apply_access(uint32_t ts, uint32_t inode) {
	FSNode *p;
	p = fsnodes_id_to_node(inode);
	if (!p) {
		return SAUNAFS_ERROR_ENOENT;
	}
	p->atime = ts;
	fsnodes_update_checksum(p);
	gMetadata->metaversion++;
	return SAUNAFS_STATUS_OK;
}

#ifndef METARESTORE
uint8_t fs_access(const FsContext &context, uint32_t inode, int modemask) {
	FSNode *p;

	uint8_t status = verify_session(context, (modemask & MODE_MASK_W) ? OperationMode::kReadWrite : OperationMode::kReadOnly, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	return fsnodes_get_node_for_operation(context, ExpectedNodeType::kAny, modemask,
	                                      inode, &p);
}

uint8_t fs_lookup(const FsContext &context, uint32_t parent, const HString &name, uint32_t *inode, Attributes &attr) {
	FSNode *wd;
	FSNodeDirectory *rn;

	*inode = 0;
	attr.fill(0);

	uint8_t status = verify_session(context, OperationMode::kReadOnly, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kDirectory, MODE_MASK_X,
	                                        parent, &wd, &rn);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	if (!name.empty() && name[0] == '.') {
		if (name.length() == 1) {  // self
			if (wd->id == context.rootinode()) {
				*inode = SPECIAL_INODE_ROOT;
			} else {
				*inode = wd->id;
			}
			fsnodes_fill_attr(wd, wd, context.uid(), context.gid(), context.auid(), context.agid(), context.sesflags(), attr);
			++gFsStatsArray[FsStats::Lookup];
			metrics::Counter::increment(metrics::Counter::Master::FS_LOOKUP);
			return SAUNAFS_STATUS_OK;
		}
		if (name.length() == 2 && name[1] == '.') {  // parent
			if (wd->id == context.rootinode()) {
				*inode = SPECIAL_INODE_ROOT;
				fsnodes_fill_attr(wd, wd, context.uid(), context.gid(), context.auid(), context.agid(), context.sesflags(), attr);
			} else {
				if (!wd->parent.empty()) {
					if (wd->parent[0].first == context.rootinode()) {
						*inode = SPECIAL_INODE_ROOT;
					} else {
						*inode = wd->parent[0].first;
					}
					FSNode *pp = fsnodes_id_to_node(wd->parent[0].first);
					fsnodes_fill_attr(pp, wd, context.uid(), context.gid(), context.auid(),
					                  context.agid(), context.sesflags(), attr);
				} else {
					*inode = SPECIAL_INODE_ROOT;  // rn->id;
					fsnodes_fill_attr(rn, wd, context.uid(), context.gid(), context.auid(), context.agid(), context.sesflags(),
					                  attr);
				}
			}
			++gFsStatsArray[FsStats::Lookup];
			metrics::Counter::increment(metrics::Counter::Master::FS_LOOKUP);
			return SAUNAFS_STATUS_OK;
		}
	}
	if (fsnodes_namecheck(name) < 0) {
		return SAUNAFS_ERROR_EINVAL;
	}

	FSNode *child = fsnodes_lookup(static_cast<FSNodeDirectory*>(wd), name);
	if (!child) {
		return SAUNAFS_ERROR_ENOENT;
	}
	*inode = child->id;
	fsnodes_fill_attr(child, wd, context.uid(), context.gid(), context.auid(), context.agid(), context.sesflags(), attr);
	++gFsStatsArray[FsStats::Lookup];
	metrics::Counter::increment(metrics::Counter::Master::FS_LOOKUP);
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_whole_path_lookup(const FsContext &context, uint32_t parent, const std::string &path, uint32_t *found_inode, Attributes &attr) {
	uint8_t status;
	uint32_t tmp_inode = context.rootinode();

	auto current_it = path.begin();
	while (current_it != path.end()) {
		auto delim_it = std::find(current_it, path.end(), '/');
		if (current_it != delim_it) {
			HString hstr(current_it, delim_it);
			status = fs_lookup(context, parent, hstr, &tmp_inode, attr);
			if (status != SAUNAFS_STATUS_OK) {
				return status;
			}
			parent = tmp_inode;
		}
		if (delim_it == path.end()) {
			break;
		}
		current_it = std::next(delim_it);
	}

	*found_inode = tmp_inode;
	if (tmp_inode == context.rootinode()) {
		return fs_getattr(context, SPECIAL_INODE_ROOT, attr);
	}
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_full_path_by_inode(const FsContext &context, uint32_t initial_inode,
                              std::string &fullPath) {
	uint32_t current_inode = initial_inode;
	FSNode *parent_node;
	FSNode *current_node;
	std::string current_name = "";

	uint8_t status = verify_session(context, OperationMode::kReadOnly, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kAny, MODE_MASK_R,
	                                        initial_inode, &current_node);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	while (current_inode != context.rootinode()) {
		if (!current_node || current_node->parent.empty()) { 
			return SAUNAFS_ERROR_ENOENT; 
		}
		auto [parentId, nameHandle] = current_node->parent[0];
		status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kAny, MODE_MASK_R,
		                                        parentId, &parent_node);
		if (status != SAUNAFS_STATUS_OK) { return status; }
		current_name = nameHandle->get();
		fullPath = current_inode == initial_inode
		               ? current_name
		               : current_name + "/" + fullPath;
		current_inode = parentId;
		current_node = parent_node;
	}

	return SAUNAFS_STATUS_OK;
}

uint8_t fs_getattr(const FsContext &context, uint32_t inode, Attributes &attr) {
	FSNode *p;

	attr.fill(0);

	uint8_t status = verify_session(context, OperationMode::kReadOnly, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kAny, MODE_MASK_EMPTY,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	fsnodes_fill_attr(p, NULL, context.uid(), context.gid(), context.auid(), context.agid(), context.sesflags(), attr);
	++gFsStatsArray[FsStats::Getattr];
	metrics::Counter::increment(metrics::Counter::Master::FS_GETATTR);
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_try_setlength(const FsContext &context, uint32_t inode, uint8_t opened,
			uint64_t length, bool denyTruncatingParity, uint32_t lockId, Attributes &attr,
			uint64_t *chunkid) {
	uint32_t ts = eventloop_time();
	ChecksumUpdater cu(ts);
	FSNode *p;
	attr.fill(0);

	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kFile,
	                                        opened == 0 ? MODE_MASK_W : MODE_MASK_EMPTY,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	FSNodeFile *node_file = static_cast<FSNodeFile*>(p);

	if (length & SFSCHUNKMASK) {
		uint32_t indx = (length >> SFSCHUNKBITS);
		if (indx < node_file->chunks.size()) {
			uint64_t ochunkid = node_file->chunks[indx];
			if (ochunkid > 0) {
				uint8_t status;
				uint64_t nchunkid;
				// We deny truncating parity only if truncating down
				denyTruncatingParity = denyTruncatingParity && (length < node_file->length);
				status = chunk_multi_truncate(
				    ochunkid, lockId, (length & SFSCHUNKMASK), p->goal, denyTruncatingParity,
				    fsnodes_quota_exceeded(p, {{QuotaResource::kSize, 1}}), &nchunkid);
				if (status != SAUNAFS_STATUS_OK) {
					return status;
				}
				node_file->chunks[indx] = nchunkid;
				*chunkid = nchunkid;
				fs_changelog(ts, "TRUNC(%" PRIu32 ",%" PRIu32 ",%" PRIu32 "):%" PRIu64, p->id, indx,
				             lockId, nchunkid);
				fsnodes_update_checksum(p);
				return SAUNAFS_ERROR_DELAYED;
			}
		}
	}
	fsnodes_fill_attr(p, NULL, context.uid(), context.gid(), context.auid(), context.agid(), context.sesflags(), attr);
	++gFsStatsArray[FsStats::Setattr];
	metrics::Counter::increment(metrics::Counter::Master::FS_SETATTR);
	return SAUNAFS_STATUS_OK;
}
#endif

uint8_t fs_apply_trunc(uint32_t ts, uint32_t inode, uint32_t indx, uint64_t chunkid,
			uint32_t lockid) {
	uint64_t ochunkid, nchunkid;
	uint8_t status;
	FSNodeFile *p = fsnodes_id_to_node<FSNodeFile>(inode);
	if (!p) {
		return SAUNAFS_ERROR_ENOENT;
	}
	if (p->type != FSNode::kFile && p->type != FSNode::kTrash && p->type != FSNode::kReserved) {
		return SAUNAFS_ERROR_EINVAL;
	}
	if (indx > MAX_INDEX) {
		return SAUNAFS_ERROR_INDEXTOOBIG;
	}
	if (indx >= p->chunks.size()) {
		return SAUNAFS_ERROR_EINVAL;
	}
	ochunkid = p->chunks[indx];
	if (ochunkid == 0) {
		safs::log_err("fs_apply_trunc: node does not have a chunk at index {} chunks, inode {}", indx, inode);
		return SAUNAFS_ERROR_NOCHUNK;
	}
	status = chunk_apply_modification(ts, ochunkid, lockid, p->goal, true, &nchunkid);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	if (chunkid != nchunkid) {
		return SAUNAFS_ERROR_MISMATCH;
	}
	p->chunks[indx] = nchunkid;
	gMetadata->metaversion++;
	fsnodes_update_checksum(p);
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_set_nextchunkid(const FsContext &context, uint64_t nextChunkId) {
	ChecksumUpdater cu(context.ts());
	uint8_t status = chunk_set_next_chunkid(nextChunkId);
	if (context.isPersonalityMaster()) {
		if (status == SAUNAFS_STATUS_OK) {
			fs_changelog(context.ts(), "NEXTCHUNKID(%" PRIu64 ")", nextChunkId);
		}
	} else {
		gMetadata->metaversion++;
	}
	return status;
}

#ifndef METARESTORE
uint8_t fs_end_setlength(uint64_t chunkid) {
	uint32_t ts = eventloop_time();
	ChecksumUpdater cu(ts);
	fs_changelog(ts, "UNLOCK(%" PRIu64 ")", chunkid);
	return chunk_unlock(chunkid);
}
#endif

uint8_t fs_apply_unlock(uint64_t chunkid) {
	gMetadata->metaversion++;
	return chunk_unlock(chunkid);
}

#ifndef METARESTORE
uint8_t fs_do_setlength(const FsContext &context, uint32_t inode, uint64_t length,
			Attributes &attr) {
	uint32_t ts = eventloop_time();
	ChecksumUpdater cu(ts);
	FSNode *p = NULL;

	attr.fill(0);

	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kFile, MODE_MASK_EMPTY,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	fsnodes_setlength(static_cast<FSNodeFile*>(p), length);
	fs_changelog(ts, "LENGTH(%" PRIu32 ",%" PRIu64 ")", inode, static_cast<FSNodeFile*>(p)->length);
	p->mtime = ts;
	fsnodes_update_ctime(p, ts);
	fsnodes_update_checksum(p);
	fsnodes_fill_attr(p, NULL, context.uid(), context.gid(), context.auid(), context.agid(), context.sesflags(), attr);
	++gFsStatsArray[FsStats::Setattr];
	metrics::Counter::increment(metrics::Counter::Master::FS_SETATTR);
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_setattr(const FsContext &context, uint32_t inode, uint8_t setmask, uint16_t attrmode,
		uint32_t attruid, uint32_t attrgid, uint32_t attratime, uint32_t attrmtime,
		SugidClearMode sugidclearmode, Attributes &attr) {
	uint32_t ts = eventloop_time();
	ChecksumUpdater cu(ts);
	FSNode *p = NULL;

	attr.fill(0);

	auto status = verify_session(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kAny, MODE_MASK_EMPTY,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	if (context.uid() != 0 && (context.sesflags() & SESFLAG_MAPALL) && (setmask & (SET_UID_FLAG | SET_GID_FLAG))) {
		return SAUNAFS_ERROR_EPERM;
	}
	if ((p->mode & (EATTR_NOOWNER << 12)) == 0 && context.uid() != 0 && context.uid() != p->uid) {
		if (setmask & (SET_MODE_FLAG | SET_UID_FLAG | SET_GID_FLAG)) {
			return SAUNAFS_ERROR_EPERM;
		}
		if ((setmask & SET_ATIME_FLAG) && !(setmask & SET_ATIME_NOW_FLAG)) {
			return SAUNAFS_ERROR_EPERM;
		}
		if ((setmask & SET_MTIME_FLAG) && !(setmask & SET_MTIME_NOW_FLAG)) {
			return SAUNAFS_ERROR_EPERM;
		}
		if ((setmask & (SET_ATIME_NOW_FLAG | SET_MTIME_NOW_FLAG)) &&
		    !fsnodes_access(context, p, MODE_MASK_W)) {
			return SAUNAFS_ERROR_EACCES;
		}
	}
	if (context.uid() != 0 && context.uid() != attruid && (setmask & SET_UID_FLAG)) {
		return SAUNAFS_ERROR_EPERM;
	}
	if ((context.sesflags() & SESFLAG_IGNOREGID) == 0) {
		if (context.uid() != 0 && (setmask & SET_GID_FLAG) && !context.hasGroup(attrgid)) {
			return SAUNAFS_ERROR_EPERM;
		}
	}
	// first ignore sugid clears done by kernel
	if ((setmask & (SET_UID_FLAG | SET_GID_FLAG)) &&
	    (setmask & SET_MODE_FLAG)) {  // chown+chmod = chown with sugid clears
		attrmode |= (p->mode & 06000);
	}
	// then do it yourself
	if ((p->mode & 06000) &&
	    (setmask & (SET_UID_FLAG |
	                SET_GID_FLAG))) {  // this is "chown" operation and suid or sgid bit is set
		switch (sugidclearmode) {
		case SugidClearMode::kAlways:
			p->mode &= 0171777;  // safest approach - always delete both suid and sgid
			attrmode &= 01777;
			break;
		case SugidClearMode::kOsx:
			if (context.uid() != 0) {  // OSX+Solaris - every change done by unprivileged user
				         // should clear suid and sgid
				p->mode &= 0171777;
				attrmode &= 01777;
			}
			break;
		case SugidClearMode::kBsd:
			if (context.uid() != 0 && (setmask & SET_GID_FLAG) &&
			    p->gid != attrgid) {  // *BSD - like in kOsx but only when something is
				                  // actually changed
				p->mode &= 0171777;
				attrmode &= 01777;
			}
			break;
		case SugidClearMode::kExt:
			if (p->type != FSNode::kDirectory) {
				if (p->mode & 010) {  // when group exec is set - clear both bits
					p->mode &= 0171777;
					attrmode &= 01777;
				} else {  // when group exec is not set - clear suid only
					p->mode &= 0173777;
					attrmode &= 03777;
				}
			}
			break;
		case SugidClearMode::kSfs:
			if (p->type != FSNode::kDirectory) {  // similar to EXT3, but unprivileged users
				                          // also clear suid/sgid bits on
				                          // directories
				if (p->mode & 010) {
					p->mode &= 0171777;
					attrmode &= 01777;
				} else {
					p->mode &= 0173777;
					attrmode &= 03777;
				}
			} else if (context.uid() != 0) {
				p->mode &= 0171777;
				attrmode &= 01777;
			}
			break;
		case SugidClearMode::kNever:
			break;
		}
	}
	if (setmask & SET_MODE_FLAG) {
		p->mode = (attrmode & 07777) | (p->mode & 0xF000);
		gMetadata->acl_storage.setMode(p->id, p->mode, p->type == FSNode::kDirectory);
	}
	if (setmask & (SET_UID_FLAG | SET_GID_FLAG)) {
		fsnodes_change_uid_gid(p, ((setmask & SET_UID_FLAG) ? attruid : p->uid),
		                       ((setmask & SET_GID_FLAG) ? attrgid : p->gid));
	}
	if (setmask & SET_ATIME_NOW_FLAG) {
		p->atime = ts;
	} else if (setmask & SET_ATIME_FLAG) {
		p->atime = attratime;
	}
	if (setmask & SET_MTIME_NOW_FLAG) {
		p->mtime = ts;
	} else if (setmask & SET_MTIME_FLAG) {
		p->mtime = attrmtime;
	}
	fs_changelog(ts, "ATTR(%" PRIu32 ",%d,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ")",
	             p->id, p->mode & 07777, p->uid, p->gid, p->atime, p->mtime);
	fsnodes_update_ctime(p, ts);
	fsnodes_fill_attr(p, NULL, context.uid(), context.gid(), context.auid(), context.agid(), context.sesflags(), attr);
	fsnodes_update_checksum(p);
	++gFsStatsArray[FsStats::Setattr];
	metrics::Counter::increment(metrics::Counter::Master::FS_SETATTR);
	return SAUNAFS_STATUS_OK;
}
#endif

uint8_t fs_apply_attr(uint32_t ts, uint32_t inode, uint32_t mode, uint32_t uid, uint32_t gid,
			uint32_t atime, uint32_t mtime) {
	FSNode *p = fsnodes_id_to_node(inode);
	if (!p) {
		return SAUNAFS_ERROR_ENOENT;
	}
	if (mode > 07777) {
		return SAUNAFS_ERROR_EINVAL;
	}
	p->mode = mode | (p->mode & 0xF000);
	gMetadata->acl_storage.setMode(p->id, p->mode, p->type == FSNode::kDirectory);
	if (p->uid != uid || p->gid != gid) {
		fsnodes_change_uid_gid(p, uid, gid);
	}
	p->atime = atime;
	p->mtime = mtime;
	fsnodes_update_ctime(p, ts);
	fsnodes_update_checksum(p);
	gMetadata->metaversion++;
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_apply_length(uint32_t ts, uint32_t inode, uint64_t length) {
	FSNode *p = fsnodes_id_to_node(inode);
	if (!p) {
		return SAUNAFS_ERROR_ENOENT;
	}
	if (p->type != FSNode::kFile && p->type != FSNode::kTrash && p->type != FSNode::kReserved) {
		return SAUNAFS_ERROR_EINVAL;
	}
	fsnodes_setlength(static_cast<FSNodeFile*>(p), length);
	p->mtime = ts;
	fsnodes_update_ctime(p, ts);
	fsnodes_update_checksum(p);
	gMetadata->metaversion++;
	return SAUNAFS_STATUS_OK;
}

#ifndef METARESTORE

/// Update atime of the given node and generate a changelog entry.
/// Doesn't do anything if NO_ATIME=1 is set in the config file.
static inline void fs_update_atime(FSNode *p, uint32_t ts) {
	if (!gAtimeDisabled && p->atime != ts) {
		p->atime = ts;
		fsnodes_update_checksum(p);
		fs_changelog(ts, "ACCESS(%" PRIu32 ")", p->id);
	}
}

uint8_t fs_readlink(const FsContext &context, uint32_t inode, std::string &path) {
	uint32_t ts = eventloop_time();
	ChecksumUpdater cu(ts);
	FSNode *p = NULL;

	uint8_t status = verify_session(context, OperationMode::kReadOnly, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kAny, MODE_MASK_EMPTY,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	if (p->type != FSNode::kSymlink) {
		return SAUNAFS_ERROR_EINVAL;
	}

	path = (std::string)static_cast<FSNodeSymlink*>(p)->path;
	fs_update_atime(p, ts);
	++gFsStatsArray[FsStats::Readlink];
	metrics::Counter::increment(metrics::Counter::Master::FS_READLINK);
	return SAUNAFS_STATUS_OK;
}
#endif

uint8_t fs_symlink(const FsContext &context, uint32_t parent, const HString &name,
		const std::string &path, uint32_t *inode, Attributes *attr) {
	ChecksumUpdater cu(context.ts());
	FSNode *wd;
	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kDirectory, MODE_MASK_W,
	                                        parent, &wd);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	if (path.length() == 0) {
		return SAUNAFS_ERROR_EINVAL;
	}
	for (uint32_t i = 0; i < path.length(); i++) {
		if (path[i] == 0) {
			return SAUNAFS_ERROR_EINVAL;
		}
	}
	if (fsnodes_namecheck(name) < 0) {
		return SAUNAFS_ERROR_EINVAL;
	}
	if (fsnodes_nameisused(static_cast<FSNodeDirectory*>(wd), name)) {
		return SAUNAFS_ERROR_EEXIST;
	}
	if (context.isPersonalityMaster() &&
	    (fsnodes_quota_exceeded_ug(context.uid(), context.gid(), {{QuotaResource::kInodes, 1}}) ||
	     fsnodes_quota_exceeded_dir(wd, {{QuotaResource::kInodes, 1}}))) {
		return SAUNAFS_ERROR_QUOTA;
	}
	FSNodeSymlink *p = static_cast<FSNodeSymlink *>(fsnodes_create_node(
	    context.ts(), static_cast<FSNodeDirectory *>(wd), name, FSNode::kSymlink, 0777, 0,
	    context.uid(), context.gid(), 0, AclInheritance::kDontInheritAcl, *inode));
	p->path = HString(path);
	p->path_length = path.length();
	fsnodes_update_checksum(p);
	statsrecord sr;
	memset(&sr, 0, sizeof(statsrecord));
	sr.length = path.length();
	fsnodes_add_stats(static_cast<FSNodeDirectory *>(wd), &sr);
	if (attr != NULL) {
		fsnodes_fill_attr(context, p, wd, *attr);
	}
	if (context.isPersonalityMaster()) {
		assert(*inode == 0);
		*inode = p->id;
		fs_changelog(context.ts(), "SYMLINK(%" PRIu32 ",%s,%s,%" PRIu32 ",%" PRIu32 "):%" PRIu32,
		             wd->id, fsnodes_escape_name(name).c_str(), fsnodes_escape_name(path).c_str(),
		             context.uid(), context.gid(), p->id);
	} else {
		if (*inode != p->id) {
			return SAUNAFS_ERROR_MISMATCH;
		}
		gMetadata->metaversion++;
	}
#ifndef METARESTORE
	++gFsStatsArray[FsStats::Symlink];
	metrics::Counter::increment(metrics::Counter::Master::FS_SYMLINK);
#endif /* #ifndef METARESTORE */
	return SAUNAFS_STATUS_OK;
}

#ifndef METARESTORE
uint8_t fs_mknod(const FsContext &context, uint32_t parent, const HString &name,
		uint8_t type, uint16_t mode, uint16_t umask, uint32_t rdev, uint32_t *inode,
		Attributes &attr) {
	uint32_t ts = eventloop_time();
	ChecksumUpdater cu(ts);
	FSNode *wd, *p;
	*inode = 0;
	attr.fill(0);

	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	if (type != FSNode::kFile && type != FSNode::kSocket && type != FSNode::kFifo &&
	    type != FSNode::kBlockDev && type != FSNode::kCharDev) {
		return SAUNAFS_ERROR_EINVAL;
	}

	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kDirectory, MODE_MASK_W,
	                                        parent, &wd);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	if (fsnodes_namecheck(name) < 0) {
		return SAUNAFS_ERROR_EINVAL;
	}
	if (fsnodes_nameisused(static_cast<FSNodeDirectory*>(wd), name)) {
		return SAUNAFS_ERROR_EEXIST;
	}
	if (fsnodes_quota_exceeded_ug(context.uid(), context.gid(), {{QuotaResource::kInodes, 1}}) ||
	    fsnodes_quota_exceeded_dir(wd, {{QuotaResource::kInodes, 1}})) {
		return SAUNAFS_ERROR_QUOTA;
	}
	static_cast<FSNodeDirectory *>(wd)->case_insensitive =
	    context.sesflags() & SESFLAG_CASEINSENSITIVE;
	p = fsnodes_create_node(ts, static_cast<FSNodeDirectory*>(wd), name, type, mode, umask, context.uid(), context.gid(), 0,
	                        AclInheritance::kInheritAcl);
	if (type == FSNode::kBlockDev || type == FSNode::kCharDev) {
		static_cast<FSNodeDevice*>(p)->rdev = rdev;
	}
	*inode = p->id;
	fsnodes_fill_attr(p, wd, context.uid(), context.gid(), context.auid(), context.agid(), context.sesflags(), attr);
	fs_changelog(ts,
	             "CREATE(%" PRIu32 ",%s,%c,%d,%" PRIu32 ",%" PRIu32 ",%" PRIu32 "):%" PRIu32,
	             wd->id, fsnodes_escape_name(name).c_str(), type, p->mode & 07777, context.uid(), context.gid(),
	             rdev, p->id);
	++gFsStatsArray[FsStats::Mknod];
	metrics::Counter::increment(metrics::Counter::Master::FS_MKNOD);
	fsnodes_update_checksum(p);
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_mkdir(const FsContext &context, uint32_t parent, const HString &name, uint16_t mode,
				 uint16_t umask, uint8_t copysgid, uint32_t *inode, Attributes &attr) {
	uint32_t ts = eventloop_time();
	ChecksumUpdater cu(ts);
	FSNode *wd, *p;
	*inode = 0;
	attr.fill(0);

	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kDirectory, MODE_MASK_W,
	                                        parent, &wd);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	if (fsnodes_namecheck(name) < 0) {
		return SAUNAFS_ERROR_EINVAL;
	}
	if (fsnodes_nameisused(static_cast<FSNodeDirectory*>(wd), name)) {
		return SAUNAFS_ERROR_EEXIST;
	}
	if (fsnodes_quota_exceeded_ug(context.uid(), context.gid(), {{QuotaResource::kInodes, 1}}) ||
	    fsnodes_quota_exceeded_dir(wd, {{QuotaResource::kInodes, 1}})) {
		return SAUNAFS_ERROR_QUOTA;
	}
	static_cast<FSNodeDirectory *>(wd)->case_insensitive =
	    context.sesflags() & SESFLAG_CASEINSENSITIVE;
	p = fsnodes_create_node(ts, static_cast<FSNodeDirectory *>(wd), name, FSNode::kDirectory, mode,
	                        umask, context.uid(), context.gid(), copysgid, AclInheritance::kInheritAcl);
	*inode = p->id;
	fsnodes_fill_attr(p, wd, context.uid(), context.gid(), context.auid(), context.agid(), context.sesflags(), attr);
	fs_changelog(ts, "CREATE(%" PRIu32 ",%s,%c,%d,%" PRIu32 ",%" PRIu32 ",%" PRIu32 "):%" PRIu32,
	             wd->id, fsnodes_escape_name(name).c_str(), FSNode::kDirectory, p->mode & 07777,
	             context.uid(), context.gid(), 0, p->id);
	++gFsStatsArray[FsStats::Mkdir];
	metrics::Counter::increment(metrics::Counter::Master::FS_MKDIR);
	return SAUNAFS_STATUS_OK;
}
#endif

uint8_t fs_apply_create(uint32_t ts, uint32_t parent, const HString &name,
		uint8_t type, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t rdev,
		uint32_t inode) {
	FSNode *wd, *p;
	if (type != FSNode::kFile && type != FSNode::kSocket && type != FSNode::kFifo &&
	    type != FSNode::kBlockDev && type != FSNode::kCharDev && type != FSNode::kDirectory) {
		return SAUNAFS_ERROR_EINVAL;
	}
	wd = fsnodes_id_to_node(parent);
	if (!wd) {
		return SAUNAFS_ERROR_ENOENT;
	}
	if (wd->type != FSNode::kDirectory) {
		return SAUNAFS_ERROR_ENOTDIR;
	}
	if (fsnodes_nameisused(static_cast<FSNodeDirectory*>(wd), name)) {
		return SAUNAFS_ERROR_EEXIST;
	}
	// we pass requested inode number here
	p = fsnodes_create_node(ts, static_cast<FSNodeDirectory*>(wd), name, type, mode, 0, uid, gid, 0,
	                        AclInheritance::kInheritAcl, inode);
	if (type == FSNode::kBlockDev || type == FSNode::kCharDev) {
		static_cast<FSNodeDevice*>(p)->rdev = rdev;
		fsnodes_update_checksum(p);
	}
	if (inode != p->id) {
		// if inode!=p->id then requested inode number was already acquired
		return SAUNAFS_ERROR_MISMATCH;
	}
	gMetadata->metaversion++;
	return SAUNAFS_STATUS_OK;
}

#ifndef METARESTORE
uint8_t fs_unlink(const FsContext &context, uint32_t parent, const HString &name) {
	uint32_t ts = eventloop_time();
	ChecksumUpdater cu(ts);
	FSNode *wd;

	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kDirectory, MODE_MASK_W,
	                                        parent, &wd);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	if (fsnodes_namecheck(name) < 0) {
		return SAUNAFS_ERROR_EINVAL;
	}
	FSNode *child = fsnodes_lookup(static_cast<FSNodeDirectory*>(wd), name);
	if (!child) {
		return SAUNAFS_ERROR_ENOENT;
	}
	if (!fsnodes_sticky_access(wd, child, context.uid())) {
		return SAUNAFS_ERROR_EPERM;
	}
	if (child->type == FSNode::kDirectory) {
		return SAUNAFS_ERROR_EPERM;
	}
	fs_changelog(ts, "UNLINK(%" PRIu32 ",%s):%" PRIu32, wd->id,
	             fsnodes_escape_name(name).c_str(), child->id);
	fsnodes_unlink(ts, static_cast<FSNodeDirectory*>(wd), name, child);
	++gFsStatsArray[FsStats::Unlink];
	metrics::Counter::increment(metrics::Counter::Master::FS_UNLINK);
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_recursive_remove(const FsContext &context, uint32_t parent,
			    const HString &name, const std::function<void(int)> &callback,
			    uint32_t job_id) {
	ChecksumUpdater cu(context.ts());
	FSNode *wd_tmp;

	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kDirectory, MODE_MASK_W,
	                                        parent, &wd_tmp);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	FSNode *child = fsnodes_lookup(static_cast<FSNodeDirectory*>(wd_tmp), name);
	if (!child) {
		return SAUNAFS_ERROR_ENOENT;
	}

	auto shared_context = std::make_shared<FsContext>(context);
	auto task = new RemoveTask({name}, wd_tmp->id, shared_context);

	std::string node_name;

	fsnodes_getpath(static_cast<FSNodeDirectory*>(wd_tmp), child, node_name);
	return gMetadata->task_manager.submitTask(job_id, context.ts(), kInitialTaskBatchSize,
	                                          task, RemoveTask::generateDescription(node_name),
	                                          callback);
}

uint8_t fs_rmdir(const FsContext &context, uint32_t parent, const HString &name) {
	uint32_t ts = eventloop_time();
	ChecksumUpdater cu(ts);
	FSNode *wd;

	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kDirectory, MODE_MASK_W,
	                                        parent, &wd);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	if (fsnodes_namecheck(name) < 0) {
		return SAUNAFS_ERROR_EINVAL;
	}
	FSNode *child = fsnodes_lookup(static_cast<FSNodeDirectory*>(wd), name);
	if (!child) {
		return SAUNAFS_ERROR_ENOENT;
	}
	if (!fsnodes_sticky_access(wd, child, context.uid())) {
		return SAUNAFS_ERROR_EPERM;
	}
	if (child->type != FSNode::kDirectory) {
		return SAUNAFS_ERROR_ENOTDIR;
	}
	if (!static_cast<FSNodeDirectory*>(child)->entries.empty()) {
		return SAUNAFS_ERROR_ENOTEMPTY;
	}
	fs_changelog(ts, "UNLINK(%" PRIu32 ",%s):%" PRIu32, wd->id,
	             fsnodes_escape_name(name).c_str(), child->id);
	fsnodes_unlink(ts, static_cast<FSNodeDirectory*>(wd), name, child);
	++gFsStatsArray[FsStats::Rmdir];
	metrics::Counter::increment(metrics::Counter::Master::FS_RMDIR);
	return SAUNAFS_STATUS_OK;
}
#endif

uint8_t fs_apply_unlink(uint32_t ts, uint32_t parent, const HString &name,
		uint32_t inode) {
	FSNode *wd;
	wd = fsnodes_id_to_node(parent);
	if (!wd) {
		return SAUNAFS_ERROR_ENOENT;
	}
	if (wd->type != FSNode::kDirectory) {
		return SAUNAFS_ERROR_ENOTDIR;
	}
	FSNode *child = fsnodes_lookup(static_cast<FSNodeDirectory*>(wd), name);
	if (!child) {
		return SAUNAFS_ERROR_ENOENT;
	}
	if (child->id != inode) {
		return SAUNAFS_ERROR_MISMATCH;
	}
	if (child->type == FSNode::kDirectory && !static_cast<FSNodeDirectory*>(child)->entries.empty()) {
		return SAUNAFS_ERROR_ENOTEMPTY;
	}
	fsnodes_unlink(ts, static_cast<FSNodeDirectory*>(wd), name, child);
	gMetadata->metaversion++;
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_rename(const FsContext &context, uint32_t parent_src, const HString &name_src,
		uint32_t parent_dst, const HString &name_dst, uint32_t *inode, Attributes *attr) {
	ChecksumUpdater cu(context.ts());
	FSNode *swd;
	FSNode *dwd;
	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kDirectory, MODE_MASK_W,
	                                        parent_dst, &dwd);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kDirectory, MODE_MASK_W,
	                                        parent_src, &swd);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	if (fsnodes_namecheck(name_src) < 0) {
		return SAUNAFS_ERROR_EINVAL;
	}
	FSNode *se_child = fsnodes_lookup(static_cast<FSNodeDirectory*>(swd), name_src);
	if (!se_child) {
		return SAUNAFS_ERROR_ENOENT;
	}
	if (context.canCheckPermissions() && !fsnodes_sticky_access(swd, se_child, context.uid())) {
		return SAUNAFS_ERROR_EPERM;
	}
	if ((context.personality() != metadataserver::Personality::kMaster) &&
	    (se_child->id != *inode)) {
		return SAUNAFS_ERROR_MISMATCH;
	} else {
		*inode = se_child->id;
	}
	std::array<int64_t, 2> quota_delta = {{1, 1}};
	if (se_child->type == FSNode::kDirectory) {
		if (fsnodes_isancestor(static_cast<FSNodeDirectory*>(se_child), dwd)) {
			return SAUNAFS_ERROR_EINVAL;
		}
		const statsrecord &stats = static_cast<FSNodeDirectory*>(se_child)->stats;
		quota_delta = {{(int64_t)stats.inodes, (int64_t)stats.size}};
	} else if (se_child->type == FSNode::kFile) {
		quota_delta[(int)QuotaResource::kSize] = fsnodes_get_size(se_child);
	}
	if (fsnodes_namecheck(name_dst) < 0) {
		return SAUNAFS_ERROR_EINVAL;
	}
	FSNode *de_child = fsnodes_lookup(static_cast<FSNodeDirectory*>(dwd), name_dst);

	if (de_child == se_child) {
		return SAUNAFS_STATUS_OK;
	}

	if (de_child) {
		if (de_child->type == FSNode::kDirectory && !static_cast<FSNodeDirectory*>(de_child)->entries.empty()) {
			return SAUNAFS_ERROR_ENOTEMPTY;
		}
		if (context.canCheckPermissions() &&
		    !fsnodes_sticky_access(dwd, de_child, context.uid())) {
			return SAUNAFS_ERROR_EPERM;
		}
		if (de_child->type == TYPE_DIRECTORY) {
			const statsrecord &stats = static_cast<FSNodeDirectory*>(de_child)->stats;
			quota_delta[(int)QuotaResource::kInodes] -= stats.inodes;
			quota_delta[(int)QuotaResource::kSize] -= stats.size;
		} else if (de_child->type == TYPE_FILE) {
			quota_delta[(int)QuotaResource::kInodes] -= 1;
			quota_delta[(int)QuotaResource::kSize] -= fsnodes_get_size(static_cast<FSNodeFile*>(de_child));
		} else {
			quota_delta[(int)QuotaResource::kInodes] -= 1;
			quota_delta[(int)QuotaResource::kSize] -= 1;
		}
	}

	if (fsnodes_quota_exceeded_dir(
	        static_cast<FSNodeDirectory *>(dwd), static_cast<FSNodeDirectory *>(swd),
	        {{QuotaResource::kInodes, quota_delta[(int)QuotaResource::kInodes]},
	         {QuotaResource::kSize, quota_delta[(int)QuotaResource::kSize]}})) {
		return SAUNAFS_ERROR_QUOTA;
	}

	if (de_child) {
		fsnodes_unlink(context.ts(), static_cast<FSNodeDirectory*>(dwd), name_dst, de_child);
	}
	fsnodes_remove_edge(context.ts(), static_cast<FSNodeDirectory*>(swd), name_src, se_child);
	fsnodes_link(context.ts(), static_cast<FSNodeDirectory*>(dwd), se_child, name_dst);
	if (attr) {
		fsnodes_fill_attr(context, se_child, dwd, *attr);
	}
	if (context.isPersonalityMaster()) {
		fs_changelog(context.ts(), "MOVE(%" PRIu32 ",%s,%" PRIu32 ",%s):%" PRIu32, swd->id,
		             fsnodes_escape_name(name_src).c_str(), dwd->id,
		             fsnodes_escape_name(name_dst).c_str(), se_child->id);
	} else {
		gMetadata->metaversion++;
	}
#ifndef METARESTORE
	++gFsStatsArray[FsStats::Rename];
	metrics::Counter::increment(metrics::Counter::Master::FS_RENAME);
#endif
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_link(const FsContext &context, uint32_t inode_src, uint32_t parent_dst,
		const HString &name_dst, uint32_t *inode, Attributes *attr) {
	ChecksumUpdater cu(context.ts());
	FSNode *sp;
	FSNode *dwd;
	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kDirectory, MODE_MASK_W,
	                                        parent_dst, &dwd);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kNotDirectory,
	                                        MODE_MASK_EMPTY, inode_src, &sp);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	if (sp->type == FSNode::kTrash || sp->type == FSNode::kReserved) {
		return SAUNAFS_ERROR_ENOENT;
	}
	if (fsnodes_namecheck(name_dst) < 0) {
		return SAUNAFS_ERROR_EINVAL;
	}
	if (fsnodes_nameisused(static_cast<FSNodeDirectory*>(dwd), name_dst)) {
		return SAUNAFS_ERROR_EEXIST;
	}
	fsnodes_link(context.ts(), static_cast<FSNodeDirectory*>(dwd), sp, name_dst);
	if (inode) {
		*inode = inode_src;
	}
	if (attr) {
		fsnodes_fill_attr(context, sp, dwd, *attr);
	}
	if (context.isPersonalityMaster()) {
		fs_changelog(context.ts(), "LINK(%" PRIu32 ",%" PRIu32 ",%s)", sp->id, dwd->id,
		             fsnodes_escape_name(name_dst).c_str());
	} else {
		gMetadata->metaversion++;
	}
#ifndef METARESTORE
	++gFsStatsArray[FsStats::Link];
	metrics::Counter::increment(metrics::Counter::Master::FS_LINK);
#endif
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_append(const FsContext &context, uint32_t inode, uint32_t inode_src) {
	ChecksumUpdater cu(context.ts());
	FSNode *p, *sp;
	if (inode == inode_src) {
		return SAUNAFS_ERROR_EINVAL;
	}
	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kFile, MODE_MASK_W,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kFile, MODE_MASK_R,
	                                        inode_src, &sp);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	if (context.isPersonalityMaster() && fsnodes_quota_exceeded(p, {{QuotaResource::kSize, 1}})) {
		return SAUNAFS_ERROR_QUOTA;
	}
	status = fsnodes_appendchunks(context.ts(), static_cast<FSNodeFile*>(p), static_cast<FSNodeFile*>(sp));
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	if (context.isPersonalityMaster()) {
		fs_changelog(context.ts(), "APPEND(%" PRIu32 ",%" PRIu32 ")", p->id, sp->id);
	} else {
		gMetadata->metaversion++;
	}
	return status;
}

static int fsnodes_check_lock_permissions(const FsContext &context, uint32_t inode, uint16_t op) {
	FSNode *dummy;
	uint8_t modemask = MODE_MASK_EMPTY;

	if (op == safs_locks::kExclusive) {
		modemask = MODE_MASK_W;
	} else if (op == safs_locks::kShared) {
		modemask = MODE_MASK_R;
	}

	return fsnodes_get_node_for_operation(context, ExpectedNodeType::kAny, modemask, inode, &dummy);
}

int fs_posixlock_probe(const FsContext &context, uint32_t inode, uint64_t start, uint64_t end,
		uint64_t owner, uint32_t sessionid, uint32_t reqid, uint32_t msgid, uint16_t op,
		safs_locks::FlockWrapper &info) {
	uint8_t status;

	if (op != safs_locks::kShared && op != safs_locks::kExclusive && op != safs_locks::kUnlock) {
		return SAUNAFS_ERROR_EINVAL;
	}

	if ((status = fsnodes_check_lock_permissions(context, inode, op)) != SAUNAFS_STATUS_OK) {
		return status;
	}

	FileLocks &locks = gMetadata->posix_locks;
	const FileLocks::Lock *collision;

	collision = locks.findCollision(inode, static_cast<FileLocks::Lock::Type>(op), start, end,
			FileLocks::Owner{owner, sessionid, reqid, msgid});

	if (collision == nullptr) {
		info.l_type = safs_locks::kUnlock;
		return SAUNAFS_STATUS_OK;
	} else {
		info.l_type = static_cast<int>(collision->type);
		info.l_start = collision->start;
		info.l_len = std::min<uint64_t>(collision->end - collision->start, std::numeric_limits<int64_t>::max());
		return SAUNAFS_ERROR_WAITING;
	}
}

int fs_lock_op(const FsContext &context, FileLocks &locks, uint32_t inode,
		uint64_t start, uint64_t end, uint64_t owner, uint32_t sessionid,
		uint32_t reqid, uint32_t msgid, uint16_t op, bool nonblocking,
		std::vector<FileLocks::Owner> &applied) {
	uint8_t status;

	if ((status = fsnodes_check_lock_permissions(context, inode, op)) != SAUNAFS_STATUS_OK) {
		return status;
	}

	FileLocks::LockQueue queue;
	bool success = false;

	switch (op) {
	case safs_locks::kShared:
		success = locks.sharedLock(inode, start, end,
				FileLocks::Owner{owner, sessionid, reqid, msgid}, nonblocking);
		break;
	case safs_locks::kExclusive:
		success = locks.exclusiveLock(inode, start, end,
				FileLocks::Owner{owner, sessionid, reqid, msgid}, nonblocking);
		break;
	case safs_locks::kRelease:
		locks.removePending(inode, [sessionid,owner](const FileLocks::Lock &lock) {
			const FileLocks::Lock::Owner &lock_owner = lock.owner();
			return lock_owner.sessionid == sessionid && lock_owner.owner == owner;
		});
		start = 0;
		end   = std::numeric_limits<uint64_t>::max();
		/* fallthrough */
	case safs_locks::kUnlock:
		success = locks.unlock(inode, start, end,
				FileLocks::Owner{owner, sessionid, reqid, msgid});
		break;
	default:
		return SAUNAFS_ERROR_EINVAL;
	}
	status = success ? SAUNAFS_STATUS_OK : SAUNAFS_ERROR_WAITING;

	// If lock is exclusive, no further action is required
	// For shared locks it is required to gather candidates for lock.
	// The case when it is needed is when the owner had exclusive lock applied to a file range
	// and he issued shared lock for this same range. This converts exclusive lock
	// to shared lock. In the result we may need to apply other shared pending locks
	// for this range.
	if (op == safs_locks::kExclusive) {
		return status;
	}

	locks.gatherCandidates(inode, start, end, queue);
	for (auto &candidate : queue) {
		if (locks.apply(inode, candidate)) {
			applied.insert(applied.end(), candidate.owners.begin(), candidate.owners.end());
		}
	}
	return status;
}

int fs_flock_op(const FsContext &context, uint32_t inode, uint64_t owner, uint32_t sessionid,
		uint32_t reqid, uint32_t msgid, uint16_t op, bool nonblocking,
		std::vector<FileLocks::Owner> &applied) {
	ChecksumUpdater cu(context.ts());
	int ret = fs_lock_op(context, gMetadata->flock_locks, inode, 0, 1, owner, sessionid,
			reqid, msgid, op, nonblocking, applied);
	if (context.isPersonalityMaster()) {
		fs_changelog(context.ts(), "FLCK(%" PRIu8 ",%" PRIu32 ",0,1,%" PRIu64 ",%" PRIu32 ",%" PRIu16 ")",
				(uint8_t)safs_locks::Type::kFlock, inode, owner, sessionid, op);
	} else {
		gMetadata->metaversion++;
	}
	return ret;
}

int fs_posixlock_op(const FsContext &context, uint32_t inode, uint64_t start, uint64_t end,
		uint64_t owner, uint32_t sessionid, uint32_t reqid, uint32_t msgid, uint16_t op,
		bool nonblocking, std::vector<FileLocks::Owner> &applied) {
	ChecksumUpdater cu(context.ts());
	int ret = fs_lock_op(context, gMetadata->posix_locks, inode, start, end, owner, sessionid,
			reqid, msgid, op, nonblocking, applied);
	if (context.isPersonalityMaster()) {
		fs_changelog(context.ts(), "FLCK(%" PRIu8 ",%" PRIu32 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu32 ",%" PRIu16 ")",
				(uint8_t)safs_locks::Type::kPosix, inode, start, end, owner, sessionid, op);
	} else {
		gMetadata->metaversion++;
	}
	return ret;
}

int fs_locks_clear_session(const FsContext &context, uint8_t type, uint32_t inode,
		uint32_t sessionid, std::vector<FileLocks::Owner> &applied) {

	if (type != (uint8_t)safs_locks::Type::kFlock && type != (uint8_t)safs_locks::Type::kPosix) {
		return SAUNAFS_ERROR_EINVAL;
	}

	ChecksumUpdater cu(context.ts());

	FileLocks *locks = type == (uint8_t)safs_locks::Type::kFlock ? &gMetadata->flock_locks
	                                                             : &gMetadata->posix_locks;

	locks->removePending(inode, [sessionid](const FileLocks::Lock &lock) {
		return lock.owner().sessionid == sessionid;
	});
	std::pair<uint64_t, uint64_t> range = locks->unlock(inode,
	    [sessionid](const FileLocks::Lock::Owner &owner) {
			return owner.sessionid == sessionid;
		});

	if (range.first < range.second) {
		FileLocks::LockQueue queue;
		locks->gatherCandidates(inode, range.first, range.second, queue);
		for (auto &candidate : queue) {
			applied.insert(applied.end(), candidate.owners.begin(), candidate.owners.end());
		}
	}
	if (context.isPersonalityMaster()) {
		fs_changelog(context.ts(), "CLRLCK(%" PRIu8 ",%" PRIu32 ",%" PRIu32 ")", type, inode,
		             sessionid);
	} else {
		gMetadata->metaversion++;
	}

	return SAUNAFS_STATUS_OK;
}

int fs_locks_list_all(const FsContext &context, uint8_t type, bool pending, uint64_t start,
		uint64_t max, std::vector<safs_locks::Info> &result) {
	(void)context;
	FileLocks *locks;
	if (type == (uint8_t)safs_locks::Type::kFlock) {
		locks = &gMetadata->flock_locks;
	} else if (type == (uint8_t)safs_locks::Type::kPosix) {
		locks = &gMetadata->posix_locks;
	} else {
		return SAUNAFS_ERROR_EINVAL;
	}

	if (pending) {
		locks->copyPendingToVector(start, max, result);
	} else {
		locks->copyActiveToVector(start, max, result);
	}

	return SAUNAFS_STATUS_OK;
}

int fs_locks_list_inode(const FsContext &context, uint8_t type, bool pending, uint32_t inode,
		uint64_t start, uint64_t max, std::vector<safs_locks::Info> &result) {
	(void)context;
	FileLocks *locks;

	if (type == (uint8_t)safs_locks::Type::kFlock) {
		locks = &gMetadata->flock_locks;
	} else if (type == (uint8_t)safs_locks::Type::kPosix) {
		locks = &gMetadata->posix_locks;
	} else {
		return SAUNAFS_ERROR_EINVAL;
	}

	if (pending) {
		locks->copyPendingToVector(inode, start, max, result);
	} else {
		locks->copyActiveToVector(inode, start, max, result);
	}

	return SAUNAFS_STATUS_OK;
}

static void fs_manage_lock_try_lock_pending(FileLocks &locks, uint32_t inode, uint64_t start,
		uint64_t end, std::vector<FileLocks::Owner> &applied) {
	FileLocks::LockQueue queue;
	locks.gatherCandidates(inode, start, end, queue);
	for (auto &candidate : queue) {
		if (locks.apply(inode, candidate)) {
			applied.insert(applied.end(), candidate.owners.begin(), candidate.owners.end());
		}
	}
}

int fs_locks_unlock_inode(const FsContext &context, uint8_t type, uint32_t inode,
		std::vector<FileLocks::Owner> &applied) {
	ChecksumUpdater cu(context.ts());

	if (type == (uint8_t)safs_locks::Type::kFlock) {
		gMetadata->flock_locks.unlock(inode);
		fs_manage_lock_try_lock_pending(gMetadata->flock_locks, inode, 0, 1, applied);
	} else if (type == (uint8_t)safs_locks::Type::kPosix) {
		gMetadata->posix_locks.unlock(inode);
		fs_manage_lock_try_lock_pending(gMetadata->posix_locks, inode, 0,
		                                std::numeric_limits<uint64_t>::max(), applied);
	} else {
		return SAUNAFS_ERROR_EINVAL;
	}

	if (context.isPersonalityMaster()) {
		fs_changelog(context.ts(), "FLCKINODE(%" PRIu8 ",%" PRIu32 ")", type, inode);
	} else {
		gMetadata->metaversion++;
	}

	return SAUNAFS_STATUS_OK;
}

int fs_locks_remove_pending(const FsContext &context, uint8_t type, uint64_t ownerid,
			uint32_t sessionid, uint32_t inode, uint64_t reqid) {
	ChecksumUpdater cu(context.ts());

	FileLocks *locks;

	if (type == (uint8_t)safs_locks::Type::kFlock) {
		locks = &gMetadata->flock_locks;
	} else if (type == (uint8_t)safs_locks::Type::kPosix) {
		locks = &gMetadata->posix_locks;
	} else {
		return SAUNAFS_ERROR_EINVAL;
	}

	locks->removePending(inode,
			[ownerid, sessionid, reqid](const LockRange &range) {
				const LockRange::Owner &owner = range.owner();
				if (owner.owner == ownerid
					&& owner.sessionid == sessionid
					&& owner.reqid == reqid) {
					return true;
				}
				return false;
			}
		);

	if (context.isPersonalityMaster()) {
		fs_changelog(context.ts(),
			     "RMPLOCK(%" PRIu8 ",%" PRIu64",%" PRIu32 ",%" PRIu32 ",%" PRIu64")",
			     type, ownerid, sessionid, inode, reqid);
	} else {
		gMetadata->metaversion++;
	}

	return SAUNAFS_STATUS_OK;
}

#ifndef METARESTORE

uint8_t fs_readdir_size(const FsContext &context, uint32_t inode, uint8_t flags, void **dnode, uint32_t *dbuffsize) {
	FSNode *p;
	*dnode = NULL;
	*dbuffsize = 0;

	uint8_t status = verify_session(context, OperationMode::kReadOnly, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kDirectory, MODE_MASK_R,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	*dnode = p;
	*dbuffsize = fsnodes_getdirsize(static_cast<FSNodeDirectory*>(p), flags & GETDIR_FLAG_WITHATTR);
	return SAUNAFS_STATUS_OK;
}

void fs_readdir_data(const FsContext &context, uint8_t flags, void *dnode, uint8_t *dbuff) {
	uint32_t ts = eventloop_time();
	ChecksumUpdater cu(ts);
	FSNode *p = (FSNode *)dnode;
	fs_update_atime(p, ts);
	fsnodes_getdirdata(context.rootinode(), context.uid(), context.gid(), context.auid(), context.agid(),
					   context.sesflags(), static_cast<FSNodeDirectory*>(p), dbuff,
	                   flags & GETDIR_FLAG_WITHATTR);
	++gFsStatsArray[FsStats::Readdir];
	metrics::Counter::increment(metrics::Counter::Master::FS_READDIR);
}

template <typename SerializableDirentType>
uint8_t fs_readdir(const FsContext &context, uint32_t inode, uint64_t first_entry, uint64_t number_of_entries,
		std::vector<SerializableDirentType> &dir_entries) {
	uint8_t status = verify_session(context, OperationMode::kReadOnly, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	FSNode *dir;
	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kDirectory, MODE_MASK_R,
	                                        inode, &dir);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	uint32_t ts = eventloop_time();
	ChecksumUpdater cu(ts);

	fs_update_atime(dir, ts);

	using legacy::fsnodes_getdir;
	fsnodes_getdir(context.rootinode(),
		       context.uid(), context.gid(), context.auid(), context.agid(), context.sesflags(),
		       static_cast<FSNodeDirectory*>(dir),
		       first_entry, number_of_entries, dir_entries);

	++gFsStatsArray[FsStats::Readdir];
	metrics::Counter::increment(metrics::Counter::Master::FS_READDIR);

	return SAUNAFS_STATUS_OK;
}
template uint8_t fs_readdir<legacy::DirectoryEntry>(const FsContext &context, uint32_t inode, uint64_t first_entry, uint64_t number_of_entries,
		std::vector<legacy::DirectoryEntry> &dir_entries);
template uint8_t fs_readdir<DirectoryEntry>(const FsContext &context, uint32_t inode, uint64_t first_entry, uint64_t number_of_entries,
		std::vector<DirectoryEntry> &dir_entries);

uint8_t fs_checkfile(const FsContext &context, uint32_t inode,
		uint32_t chunkcount[CHUNK_MATRIX_SIZE]) {
	FSNode *p;

	uint8_t status = verify_session(context, OperationMode::kReadOnly, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kFile, MODE_MASK_EMPTY,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	fsnodes_checkfile(static_cast<FSNodeFile*>(p), chunkcount);
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_opencheck(const FsContext &context, uint32_t inode, uint8_t flags, Attributes &attr) {
	FSNode *p;


	uint8_t status = verify_session(context, (flags & WANT_WRITE) ? OperationMode::kReadWrite : OperationMode::kReadOnly, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kFile, MODE_MASK_EMPTY,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	if ((flags & AFTER_CREATE) == 0) {
		uint8_t modemask = 0;
		if (flags & WANT_READ) {
			modemask |= MODE_MASK_R;
		}
		if (flags & WANT_WRITE) {
			modemask |= MODE_MASK_W;
		}
		if (!fsnodes_access(context, p, modemask)) {
			return SAUNAFS_ERROR_EACCES;
		}
	}
	fsnodes_fill_attr(p, NULL, context.uid(), context.gid(), context.auid(), context.agid(), context.sesflags(), attr);
	++gFsStatsArray[FsStats::Open];
	metrics::Counter::increment(metrics::Counter::Master::FS_OPEN);
	return SAUNAFS_STATUS_OK;
}
#endif

uint8_t fs_acquire(const FsContext &context, uint32_t inode, uint32_t sessionid) {
	ChecksumUpdater cu(context.ts());
#ifndef METARESTORE
	if (context.isPersonalityShadow()) {
		matoclserv_add_open_file(sessionid, inode);
	}
#endif /* #ifndef METARESTORE */
	FSNodeFile *p = fsnodes_id_to_node<FSNodeFile>(inode);
	if (!p) {
		return SAUNAFS_ERROR_ENOENT;
	}
	if (p->type != FSNode::kFile && p->type != FSNode::kTrash && p->type != FSNode::kReserved) {
		return SAUNAFS_ERROR_EPERM;
	}
	if (std::find(p->sessionid.begin(), p->sessionid.end(), sessionid) != p->sessionid.end()) {
		return SAUNAFS_ERROR_EINVAL;
	}
	p->sessionid.push_back(sessionid);
	fsnodes_update_checksum(p);
	if (context.isPersonalityMaster()) {
		fs_changelog(context.ts(), "ACQUIRE(%" PRIu32 ",%" PRIu32 ")", inode, sessionid);
	} else {
		gMetadata->metaversion++;
	}
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_release(const FsContext &context, uint32_t inode, uint32_t sessionid) {
	ChecksumUpdater cu(context.ts());
	FSNodeFile *p = fsnodes_id_to_node<FSNodeFile>(inode);
	if (!p) {
		return SAUNAFS_ERROR_ENOENT;
	}
	if (p->type != FSNode::kFile && p->type != FSNode::kTrash && p->type != FSNode::kReserved) {
		return SAUNAFS_ERROR_EPERM;
	}
	auto it = std::find(p->sessionid.begin(), p->sessionid.end(), sessionid);
	if (it != p->sessionid.end()) {
		p->sessionid.erase(it);
		if (p->type == FSNode::kReserved && p->sessionid.empty()) {
			fsnodes_purge(context.ts(), p);
		} else {
			fsnodes_update_checksum(p);
		}
#ifndef METARESTORE
		if (context.isPersonalityShadow()) {
			matoclserv_remove_open_file(sessionid, inode);
		}
#endif /* #ifndef METARESTORE */
		if (context.isPersonalityMaster()) {
			fs_changelog(context.ts(), "RELEASE(%" PRIu32 ",%" PRIu32 ")", inode, sessionid);
		} else {
			gMetadata->metaversion++;
		}
		return SAUNAFS_STATUS_OK;
	}
#ifndef METARESTORE
	safs_pretty_syslog(LOG_WARNING, "release: session not found");
#endif
	return SAUNAFS_ERROR_EINVAL;
}

#ifndef METARESTORE
uint32_t fs_newsessionid(void) {
	uint32_t ts = eventloop_time();
	ChecksumUpdater cu(ts);
	fs_changelog(ts, "SESSION():%" PRIu32, gMetadata->nextsessionid);
	return gMetadata->nextsessionid++;
}
#endif
uint8_t fs_apply_session(uint32_t sessionid) {
	if (sessionid != gMetadata->nextsessionid) {
		return SAUNAFS_ERROR_MISMATCH;
	}
	gMetadata->metaversion++;
	gMetadata->nextsessionid++;
	return SAUNAFS_STATUS_OK;
}

#ifndef METARESTORE
uint8_t fs_auto_repair_if_needed(FSNodeFile *p, uint32_t chunkIndex) {
	uint64_t chunkId =
	        (chunkIndex < p->chunks.size() ? p->chunks[chunkIndex] : 0);
	if (chunkId != 0 && chunk_has_only_invalid_copies(chunkId)) {
		uint32_t notchanged, erased, repaired;
		FsContext context = FsContext::getForMasterWithSession(0, SPECIAL_INODE_ROOT, 0, 0, 0, 0, 0);
		fs_repair(context, p->id, 0, &notchanged, &erased, &repaired);
		safs_pretty_syslog(LOG_NOTICE,
		       "auto repair inode %" PRIu32 ", chunk %016" PRIX64
		       ": "
		       "not changed: %" PRIu32 ", erased: %" PRIu32 ", repaired: %" PRIu32,
		       p->id, chunkId, notchanged, erased, repaired);
		safs_silent_syslog(LOG_DEBUG, "master.fs.file_auto_repaired: %u %u", p->id, repaired);
	}
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_readchunk(uint32_t inode, uint32_t indx, uint64_t *chunkid, uint64_t *length) {
	uint32_t ts = eventloop_time();
	ChecksumUpdater cu(ts);
	FSNodeFile *p;

	*chunkid = 0;
	*length = 0;
	p = fsnodes_id_to_node<FSNodeFile>(inode);
	if (!p) {
		return SAUNAFS_ERROR_ENOENT;
	}
	if (p->type != FSNode::kFile && p->type != FSNode::kTrash && p->type != FSNode::kReserved) {
		return SAUNAFS_ERROR_EPERM;
	}
	if (indx > MAX_INDEX) {
		return SAUNAFS_ERROR_INDEXTOOBIG;
	}
#ifndef METARESTORE
	if (gMagicAutoFileRepair) {
		fs_auto_repair_if_needed(p, indx);
	}
#endif
	if (indx < p->chunks.size()) {
		*chunkid = p->chunks[indx];
	}
	*length = p->length;
	fs_update_atime(p, ts);
	++gFsStatsArray[FsStats::Read];
	metrics::Counter::increment(metrics::Counter::Master::FS_READ);
	return SAUNAFS_STATUS_OK;
}
#endif

uint8_t fs_writechunk(const FsContext &context, uint32_t inode, uint32_t indx, bool usedummylockid,
		/* inout */ uint32_t *lockid, uint64_t *chunkid, uint8_t *opflag,
		uint64_t *length, uint32_t min_server_version) {
	ChecksumUpdater cu(context.ts());
	uint64_t ochunkid, nchunkid;
	FSNode *node;
	FSNodeFile *p;

	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kFile, MODE_MASK_EMPTY,
	                                        inode, &node);
	p = static_cast<FSNodeFile*>(node);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	if (indx > MAX_INDEX) {
		return SAUNAFS_ERROR_INDEXTOOBIG;
	}
#ifndef METARESTORE
	if (gMagicAutoFileRepair && context.isPersonalityMaster()) {
		fs_auto_repair_if_needed(p, indx);
	}
#endif

	const bool quota_exceeded = fsnodes_quota_exceeded(p, {{QuotaResource::kSize, 1}});
	statsrecord psr;
	fsnodes_get_stats(p, &psr);

	/* resize chunks structure */
	if (indx >= p->chunks.size()) {
		if (context.isPersonalityMaster() && quota_exceeded) {
			return SAUNAFS_ERROR_QUOTA;
		}
		uint32_t new_size;
		if (indx < 8) {
			new_size = indx + 1;
		} else if (indx < 64) {
			new_size = (indx & 0xFFFFFFF8) + 8;
		} else {
			new_size = (indx & 0xFFFFFFC0) + 64;
		}
		assert(new_size > indx);
		p->chunks.resize(new_size, 0);
	}

	ochunkid = p->chunks[indx];
	if (context.isPersonalityMaster()) {
#ifndef METARESTORE
		status = chunk_multi_modify(ochunkid, lockid, p->goal, usedummylockid,
		                            quota_exceeded, opflag, &nchunkid, min_server_version);
#else
		(void)usedummylockid;
		(void)min_server_version;
		// This will NEVER happen (metarestore doesn't call this in master context)
		mabort("bad code path: fs_writechunk");
#endif
	} else {
		bool increaseVersion = (*opflag != 0);
		status = chunk_apply_modification(context.ts(), ochunkid, *lockid, p->goal,
		                                  increaseVersion, &nchunkid);
	}
	if (status != SAUNAFS_STATUS_OK) {
		fsnodes_update_checksum(p);
		return status;
	}
	if (context.isPersonalityShadow() && nchunkid != *chunkid) {
		fsnodes_update_checksum(p);
		return SAUNAFS_ERROR_MISMATCH;
	}
	p->chunks[indx] = nchunkid;
	*chunkid = nchunkid;
	statsrecord nsr;
	fsnodes_get_stats(p, &nsr);
	for (const auto &[parentId, _] : p->parent) {
		FSNodeDirectory *parent =
		    fsnodes_id_to_node_verify<FSNodeDirectory>(parentId);
		fsnodes_add_sub_stats(parent, &nsr, &psr);
	}
	fsnodes_quota_update(p, {{QuotaResource::kSize, nsr.size - psr.size}});
	if (length) {
		*length = p->length;
	}
	if (context.isPersonalityMaster()) {
		fs_changelog(context.ts(),
		             "WRITE(%" PRIu32 ",%" PRIu32 ",%" PRIu8 ",%" PRIu32 "):%" PRIu64,
		             inode, indx, *opflag, *lockid, nchunkid);
	} else {
		gMetadata->metaversion++;
	}
	p->mtime = context.ts();
	fsnodes_update_ctime(p, context.ts());
	fsnodes_update_checksum(p);
#ifndef METARESTORE
	++gFsStatsArray[FsStats::Write];
	metrics::Counter::increment(metrics::Counter::Master::FS_WRITE);
#endif
	return SAUNAFS_STATUS_OK;
}

#ifndef METARESTORE
uint8_t fs_writeend(uint32_t inode, uint64_t length, uint64_t chunkid, uint32_t lockid) {
	uint32_t ts = eventloop_time();
	ChecksumUpdater cu(ts);
	uint8_t status = chunk_can_unlock(chunkid, lockid);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	if (length > 0) {
		FSNodeFile *p = fsnodes_id_to_node<FSNodeFile>(inode);
		if (!p) {
			return SAUNAFS_ERROR_ENOENT;
		}
		if (p->type != FSNode::kFile && p->type != FSNode::kTrash && p->type != FSNode::kReserved) {
			return SAUNAFS_ERROR_EPERM;
		}
		if (length > p->length) {
			fsnodes_setlength(p, length);
			p->mtime = ts;
			fsnodes_update_ctime(p, ts);
			fsnodes_update_checksum(p);
			fs_changelog(ts, "LENGTH(%" PRIu32 ",%" PRIu64 ")", inode, length);
		}
	}
	fs_changelog(ts, "UNLOCK(%" PRIu64 ")", chunkid);
	return chunk_unlock(chunkid);
}

void fs_incversion(uint64_t chunkid) {
	uint32_t ts = eventloop_time();
	ChecksumUpdater cu(ts);
	fs_changelog(ts, "INCVERSION(%" PRIu64 ")", chunkid);
}
#endif

uint8_t fs_apply_incversion(uint64_t chunkid) {
	gMetadata->metaversion++;
	return chunk_increase_version(chunkid);
}

#ifndef METARESTORE
uint8_t fs_repair(const FsContext &context, uint32_t inode,
		uint8_t correct_only, uint32_t *notchanged, uint32_t *erased, uint32_t *repaired) {
	uint32_t ts = eventloop_time();
	ChecksumUpdater cu(ts);
	uint32_t nversion, indx;
	statsrecord psr, nsr;
	FSNode *p;

	*notchanged = 0;
	*erased = 0;
	*repaired = 0;

	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kFile, MODE_MASK_W,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	FSNodeFile *node_file = static_cast<FSNodeFile*>(p);
	fsnodes_get_stats(p, &psr);
	for (indx = 0; indx < node_file->chunks.size(); indx++) {
		if (chunk_repair(p->goal, node_file->chunks[indx], &nversion, correct_only)) {
			fs_changelog(ts, "REPAIR(%" PRIu32 ",%" PRIu32 "):%" PRIu32, inode, indx,
			             nversion);
			p->mtime = ts;
			fsnodes_update_ctime(p, ts);
			if (nversion > 0) {
				(*repaired)++;
			} else {
				node_file->chunks[indx] = 0;
				(*erased)++;
			}
		} else {
			(*notchanged)++;
		}
	}
	fsnodes_get_stats(p, &nsr);
	for (const auto &[parentId, _] : p->parent) {
		FSNodeDirectory *parent =
		    fsnodes_id_to_node_verify<FSNodeDirectory>(parentId);
		fsnodes_add_sub_stats(parent, &nsr, &psr);
	}
	fsnodes_quota_update(p, {{QuotaResource::kSize, nsr.size - psr.size}});
	fsnodes_update_checksum(p);
	return SAUNAFS_STATUS_OK;
}
#endif /* #ifndef METARESTORE */

uint8_t fs_apply_repair(uint32_t ts, uint32_t inode, uint32_t indx, uint32_t nversion) {
	FSNodeFile *p;
	uint8_t status;
	statsrecord psr, nsr;

	p = fsnodes_id_to_node<FSNodeFile>(inode);
	if (!p) {
		return SAUNAFS_ERROR_ENOENT;
	}
	if (p->type != FSNode::kFile && p->type != FSNode::kTrash && p->type != FSNode::kReserved) {
		return SAUNAFS_ERROR_EPERM;
	}
	if (indx > MAX_INDEX) {
		return SAUNAFS_ERROR_INDEXTOOBIG;
	}
	if (indx >= p->chunks.size()) {
		return SAUNAFS_ERROR_NOCHUNK;
		safs::log_err("fs_apply_repair: indx {} is greater than number of chunks ({}), inode {}", indx, p->chunks.size(), inode);
	}
	if (p->chunks[indx] == 0) {
		safs::log_err("fs_apply_repair: node chunks at index {} has no chunks, inode {}", indx, inode);
		return SAUNAFS_ERROR_NOCHUNK;
	}
	fsnodes_get_stats(p, &psr);
	if (nversion == 0) {
		status = chunk_delete_file(p->chunks[indx], p->goal);
		p->chunks[indx] = 0;
	} else {
		status = chunk_set_version(p->chunks[indx], nversion);
	}
	fsnodes_get_stats(p, &nsr);
	for (const auto &[parentId, _] : p->parent) {
		FSNodeDirectory *parent =
		    fsnodes_id_to_node_verify<FSNodeDirectory>(parentId);
		fsnodes_add_sub_stats(parent, &nsr, &psr);
	}
	fsnodes_quota_update(p, {{QuotaResource::kSize, nsr.size - psr.size}});
	gMetadata->metaversion++;
	p->mtime = ts;
	fsnodes_update_ctime(p, ts);
	fsnodes_update_checksum(p);
	return status;
}

#ifndef METARESTORE
uint8_t fs_getgoal(const FsContext &context, uint32_t inode, uint8_t gmode,
		GoalStatistics &fgtab, GoalStatistics &dgtab) {
	FSNode *p;

	if (!GMODE_ISVALID(gmode)) {
		return SAUNAFS_ERROR_EINVAL;
	}

	uint8_t status = verify_session(context, OperationMode::kReadOnly, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kFileOrDirectory, 0, inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	fsnodes_getgoal_recursive(p, gmode, fgtab, dgtab);
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_gettrashtime_prepare(const FsContext &context, uint32_t inode, uint8_t gmode,
	TrashtimeMap &fileTrashtimes, TrashtimeMap &dirTrashtimes) {
	FSNode *p;

	if (!GMODE_ISVALID(gmode)) {
		return SAUNAFS_ERROR_EINVAL;
	}

	uint8_t status = verify_session(context, OperationMode::kReadOnly, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kFileOrDirectory, 0, inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	fsnodes_gettrashtime_recursive(p, gmode, fileTrashtimes, dirTrashtimes);

	return SAUNAFS_STATUS_OK;
}

void fs_gettrashtime_store(TrashtimeMap &fileTrashtimes,TrashtimeMap &dirTrashtimes,uint8_t *buff) {
	for (auto i : fileTrashtimes) {
		put32bit(&buff, i.first);
		put32bit(&buff, i.second);
	}
	for (auto i : dirTrashtimes) {
		put32bit(&buff, i.first);
		put32bit(&buff, i.second);
	}
}

uint8_t fs_geteattr(const FsContext &context, uint32_t inode, uint8_t gmode,
			uint32_t feattrtab[16], uint32_t deattrtab[16]) {
	FSNode *p;

	memset(feattrtab, 0, 16 * sizeof(uint32_t));
	memset(deattrtab, 0, 16 * sizeof(uint32_t));
	if (!GMODE_ISVALID(gmode)) {
		return SAUNAFS_ERROR_EINVAL;
	}

	uint8_t status = verify_session(context, OperationMode::kReadOnly, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kAny, 0, inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	fsnodes_geteattr_recursive(p, gmode, feattrtab, deattrtab);
	return SAUNAFS_STATUS_OK;
}

#endif

uint8_t fs_setgoal(const FsContext &context, uint32_t inode, uint8_t goal, uint8_t smode,
		std::shared_ptr<SetGoalTask::StatsArray> setgoal_stats,
		const std::function<void(int)> &callback) {
	ChecksumUpdater cu(context.ts());
	if (!SMODE_ISVALID(smode) || !GoalId::isValid(goal) ||
	    (smode & (SMODE_INCREASE | SMODE_DECREASE))) {
		return SAUNAFS_ERROR_EINVAL;
	}
	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	FSNode *p;
	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kAny, MODE_MASK_EMPTY,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	if (p->type != FSNode::kDirectory && p->type != FSNode::kFile &&
	    p->type != FSNode::kTrash && p->type != FSNode::kReserved) {
		return SAUNAFS_ERROR_EPERM;
	}
	sassert(context.hasUidGidData());
	(*setgoal_stats)[SetGoalTask::kChanged] = 0;      // - Number of inodes with changed goal
	(*setgoal_stats)[SetGoalTask::kNotChanged] = 0;   // - Number of inodes with not changed goal
	(*setgoal_stats)[SetGoalTask::kNotPermitted] = 0; // - Number of inodes with permission denied

	auto task = new SetGoalTask({p->id}, context.uid(), goal,
							  smode, setgoal_stats);
	std::string node_name;
	FSNodeDirectory *parent = fsnodes_get_first_parent(p);
	fsnodes_getpath(parent, p, node_name);

	std::string goal_name;
#ifndef METARESTORE
	goal_name = gGoalDefinitions[goal].getName();
#else
	goal_name = "goal id: " + std::to_string(goal);
#endif
	return gMetadata->task_manager.submitTask(context.ts(), kInitialTaskBatchSize,
						  task, SetGoalTask::generateDescription(node_name, goal_name),
						  callback);
}

//This function is only used by Shadow
uint8_t fs_apply_setgoal(const FsContext &context, uint32_t inode, uint8_t goal, uint8_t smode,
		uint32_t master_result) {

	assert(context.isPersonalityShadow());
	ChecksumUpdater cu(context.ts());
	if (!SMODE_ISVALID(smode) || !GoalId::isValid(goal) ||
	    (smode & (SMODE_INCREASE | SMODE_DECREASE))) {
		return SAUNAFS_ERROR_EINVAL;
	}
	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	FSNode *p;
	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kAny, MODE_MASK_EMPTY,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	if (p->type != FSNode::kDirectory && p->type != FSNode::kFile &&
	    p->type != FSNode::kTrash && p->type != FSNode::kReserved) {
		return SAUNAFS_ERROR_EPERM;
	}
	sassert(context.hasUidGidData());

	SetGoalTask task(context.uid(), goal, smode);
	uint32_t my_result = task.setGoal(p, context.ts());

	gMetadata->metaversion++;
	if (master_result != my_result) {
		return SAUNAFS_ERROR_MISMATCH;
	}

	return SAUNAFS_STATUS_OK;
}

uint8_t fs_deprecated_setgoal(const FsContext &context, uint32_t inode, uint8_t goal, uint8_t smode,
		uint32_t *sinodes, uint32_t *ncinodes, uint32_t *nsinodes) {
	ChecksumUpdater cu(context.ts());
	if (!SMODE_ISVALID(smode) || !GoalId::isValid(goal) ||
	    (smode & (SMODE_INCREASE | SMODE_DECREASE))) {
		return SAUNAFS_ERROR_EINVAL;
	}
	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kAny);
	if (status != 0) {
		return status;
	}
	FSNode *p;
	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kAny, MODE_MASK_EMPTY,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	if (p->type != FSNode::kDirectory && p->type != FSNode::kFile && p->type != FSNode::kTrash &&
	    p->type != FSNode::kReserved) {
		return SAUNAFS_ERROR_EPERM;
	}
	sassert(context.hasUidGidData());
	uint32_t si = 0;
	uint32_t nci = 0;
	uint32_t nsi = 0;

	fsnodes_setgoal_recursive(p, context.ts(), context.uid(), goal, smode, &si, &nci, &nsi);

	if (context.isPersonalityMaster()) {
		if ((smode & SMODE_RMASK) == 0 && nsi > 0 && si == 0 && nci == 0) {
			return SAUNAFS_ERROR_EPERM;
		}
		*sinodes = si;
		*ncinodes = nci;
		*nsinodes = nsi;
		fs_changelog(context.ts(), "SETGOAL(%" PRIu32 ",%" PRIu32 ",%" PRIu8 ",%" PRIu8
		                           "):%" PRIu32 ",%" PRIu32 ",%" PRIu32,
		             p->id, context.uid(), goal, smode, si, nci, nsi);
	} else {
		gMetadata->metaversion++;
		if ((*sinodes != si) || (*ncinodes != nci) || (*nsinodes != nsi)) {
			return SAUNAFS_ERROR_MISMATCH;
		}
	}
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_settrashtime(const FsContext &context, uint32_t inode, uint32_t trashtime, uint8_t smode,
			std::shared_ptr<SetTrashtimeTask::StatsArray> settrashtime_stats,
			const std::function<void(int)> &callback) {
	ChecksumUpdater cu(context.ts());
	if (!SMODE_ISVALID(smode)) {
		return SAUNAFS_ERROR_EINVAL;
	}
	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	FSNode *p;
	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kAny, MODE_MASK_EMPTY,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	if (p->type != FSNode::kDirectory && p->type != FSNode::kFile && p->type != FSNode::kTrash &&
	    p->type != FSNode::kReserved) {
		return SAUNAFS_ERROR_EPERM;
	}
	sassert(context.hasUidGidData());
	(*settrashtime_stats)[SetTrashtimeTask::kChanged] = 0;      // - Number of inodes with changed trashtime
	(*settrashtime_stats)[SetTrashtimeTask::kNotChanged] = 0;   // - Number of inodes with not changed trashtime
	(*settrashtime_stats)[SetTrashtimeTask::kNotPermitted] = 0; // - Number of inodes with permission denied

	auto task = new SetTrashtimeTask({p->id}, context.uid(), trashtime,
							  smode, settrashtime_stats);
	std::string node_name;
	FSNodeDirectory *parent = fsnodes_get_first_parent(p);
	fsnodes_getpath(parent, p, node_name);
	return gMetadata->task_manager.submitTask(context.ts(), kInitialTaskBatchSize,
	                                          task, SetTrashtimeTask::generateDescription(node_name, trashtime),
	                                          callback);
}

uint8_t fs_apply_settrashtime(const FsContext &context, uint32_t inode, uint32_t trashtime,
			      uint8_t smode, uint32_t master_result) {

	assert(context.isPersonalityShadow());
	ChecksumUpdater cu(context.ts());
	if (!SMODE_ISVALID(smode)) {
		return SAUNAFS_ERROR_EINVAL;
	}
	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	FSNode *p;
	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kAny, MODE_MASK_EMPTY,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	if (p->type != FSNode::kDirectory && p->type != FSNode::kFile && p->type != FSNode::kTrash &&
	    p->type != FSNode::kReserved) {
		return SAUNAFS_ERROR_EPERM;
	}
	sassert(context.hasUidGidData());

	SetTrashtimeTask task(context.uid(), trashtime, smode);
	uint32_t my_result = task.setTrashtime(p, context.ts());

	gMetadata->metaversion++;
	if (master_result != my_result) {
		return SAUNAFS_ERROR_MISMATCH;
	}

	return SAUNAFS_STATUS_OK;
}

uint8_t fs_deprecated_settrashtime(const FsContext &context, uint32_t inode, uint32_t trashtime,
				   uint8_t smode, uint32_t *sinodes, uint32_t *ncinodes,
				   uint32_t *nsinodes) {
	ChecksumUpdater cu(context.ts());
	if (!SMODE_ISVALID(smode)) {
		return SAUNAFS_ERROR_EINVAL;
	}
	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kAny);
	if (status != 0) {
		return status;
	}
	FSNode *p;
	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kAny, MODE_MASK_EMPTY,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	if (p->type != FSNode::kDirectory && p->type != FSNode::kFile && p->type != FSNode::kTrash &&
	    p->type != FSNode::kReserved) {
		return SAUNAFS_ERROR_EPERM;
	}
	uint32_t si = 0;
	uint32_t nci = 0;
	uint32_t nsi = 0;
	sassert(context.hasUidGidData());
	fsnodes_settrashtime_recursive(p, context.ts(), context.uid(), trashtime, smode, &si, &nci,
	                               &nsi);
	if (context.isPersonalityMaster()) {
		if ((smode & SMODE_RMASK) == 0 && nsi > 0 && si == 0 && nci == 0) {
			return SAUNAFS_ERROR_EPERM;
		}
		*sinodes = si;
		*ncinodes = nci;
		*nsinodes = nsi;
		fs_changelog(context.ts(), "SETTRASHTIME(%" PRIu32 ",%" PRIu32 ",%" PRIu32
		                           ",%" PRIu8 "):%" PRIu32 ",%" PRIu32 ",%" PRIu32,
		             p->id, context.uid(), trashtime, smode, si, nci, nsi);
	} else {
		gMetadata->metaversion++;
		if ((*sinodes != si) || (*ncinodes != nci) || (*nsinodes != nsi)) {
			return SAUNAFS_ERROR_MISMATCH;
		}
	}
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_seteattr(const FsContext &context, uint32_t inode, uint8_t eattr, uint8_t smode,
			uint32_t *sinodes, uint32_t *ncinodes, uint32_t *nsinodes) {
	ChecksumUpdater cu(context.ts());
	if (!SMODE_ISVALID(smode) ||
	    (eattr & (~(EATTR_NOOWNER | EATTR_NOACACHE | EATTR_NOECACHE | EATTR_NODATACACHE)))) {
		return SAUNAFS_ERROR_EINVAL;
	}
	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	FSNode *p;
	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kAny, MODE_MASK_EMPTY,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	uint32_t si = 0;
	uint32_t nci = 0;
	uint32_t nsi = 0;
	sassert(context.hasUidGidData());
	fsnodes_seteattr_recursive(p, context.ts(), context.uid(), eattr, smode, &si, &nci, &nsi);
	if (context.isPersonalityMaster()) {
		if ((smode & SMODE_RMASK) == 0 && nsi > 0 && si == 0 && nci == 0) {
			return SAUNAFS_ERROR_EPERM;
		}
		*sinodes = si;
		*ncinodes = nci;
		*nsinodes = nsi;
		fs_changelog(context.ts(), "SETEATTR(%" PRIu32 ",%" PRIu32 ",%" PRIu8 ",%" PRIu8
		                           "):%" PRIu32 ",%" PRIu32 ",%" PRIu32,
		             p->id, context.uid(), eattr, smode, si, nci, nsi);
	} else {
		gMetadata->metaversion++;
		if ((*sinodes != si) || (*ncinodes != nci) || (*nsinodes != nsi)) {
			return SAUNAFS_ERROR_MISMATCH;
		}
	}
	return SAUNAFS_STATUS_OK;
}

#ifndef METARESTORE

uint8_t fs_listxattr_leng(const FsContext &context, uint32_t inode, uint8_t opened,
			void **xanode, uint32_t *xasize) {
	FSNode *p;

	uint8_t status = verify_session(context, OperationMode::kReadOnly, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kAny,
	                                        opened == 0 ? MODE_MASK_R : MODE_MASK_EMPTY, inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	*xasize = sizeof(kAclXattrs);
	return xattr_listattr_leng(p->id, xanode, xasize);
}

void fs_listxattr_data(void *xanode, uint8_t *xabuff) {
	memcpy(xabuff, kAclXattrs, sizeof(kAclXattrs));
	xattr_listattr_data(xanode, xabuff + sizeof(kAclXattrs));
}

uint8_t fs_setxattr(const FsContext &context, uint32_t inode, uint8_t opened,
		uint8_t anleng, const uint8_t *attrname,
		uint32_t avleng, const uint8_t *attrvalue, uint8_t mode) {
	uint32_t ts = eventloop_time();
	ChecksumUpdater cu(ts);
	FSNode *p;
	uint8_t status;

	status = verify_session(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kAny,
	                                        opened == 0 ? MODE_MASK_W : MODE_MASK_EMPTY, inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	if (xattr_namecheck(anleng, attrname) < 0) {
		return SAUNAFS_ERROR_EINVAL;
	}
	if (mode > XATTR_SMODE_REMOVE) {
		return SAUNAFS_ERROR_EINVAL;
	}
	status = xattr_setattr(p->id, anleng, attrname, avleng, attrvalue, mode);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	fsnodes_update_ctime(p, ts);
	fsnodes_update_checksum(p);
	fs_changelog(ts, "SETXATTR(%" PRIu32 ",%s,%s,%" PRIu8 ")", p->id,
	             fsnodes_escape_name(std::string((const char*)attrname, anleng)).c_str(),
	             fsnodes_escape_name(std::string((const char*)attrvalue, avleng)).c_str(),
	             mode);
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_getxattr(const FsContext &context, uint32_t inode, uint8_t opened,
		uint8_t anleng, const uint8_t *attrname,
		uint32_t *avleng, uint8_t **attrvalue) {
	FSNode *p;

	uint8_t status = verify_session(context, OperationMode::kReadOnly, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kAny,
	                                        opened == 0 ? MODE_MASK_R : MODE_MASK_EMPTY, inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	if (xattr_namecheck(anleng, attrname) < 0) {
		return SAUNAFS_ERROR_EINVAL;
	}
	return xattr_getattr(p->id, anleng, attrname, avleng, attrvalue);
}

#endif /* #ifndef METARESTORE */

uint8_t fs_apply_setxattr(uint32_t ts, uint32_t inode, uint32_t anleng, const uint8_t *attrname,
			uint32_t avleng, const uint8_t *attrvalue, uint32_t mode) {
	FSNode *p;
	uint8_t status;
	if (anleng == 0 || anleng > SFS_XATTR_NAME_MAX || avleng > SFS_XATTR_SIZE_MAX ||
	    mode > XATTR_SMODE_REMOVE) {
		return SAUNAFS_ERROR_EINVAL;
	}
	p = fsnodes_id_to_node(inode);
	if (!p) {
		return SAUNAFS_ERROR_ENOENT;
	}
	status = xattr_setattr(inode, anleng, attrname, avleng, attrvalue, mode);

	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	fsnodes_update_ctime(p, ts);
	gMetadata->metaversion++;
	fsnodes_update_checksum(p);
	return status;
}

uint8_t fs_deleteacl(const FsContext &context, uint32_t inode, AclType type) {
	ChecksumUpdater cu(context.ts());
	FSNode *p;
	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kAny, MODE_MASK_EMPTY,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	status = fsnodes_deleteacl(p, type, context.ts());
	if (context.isPersonalityMaster()) {
		if (status == SAUNAFS_STATUS_OK) {
			static char acl_type[3] = {'a', 'd', 'r'};

			static_assert((int)AclType::kAccess == 0, "fix acl_type table");
			static_assert((int)AclType::kDefault == 1, "fix acl_type table");
			static_assert((int)AclType::kRichACL == 2, "fix acl_type table");

			fs_changelog(context.ts(), "DELETEACL(%" PRIu32 ",%c)", p->id, acl_type[std::min(3, (int)type)]);
		}
	} else {
		gMetadata->metaversion++;
	}
	return status;
}

#ifndef METARESTORE

uint8_t fs_setacl(const FsContext &context, uint32_t inode, const RichACL &acl) {
	ChecksumUpdater cu(context.ts());
	FSNode *p;
	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kAny, MODE_MASK_EMPTY,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	std::string acl_string = acl.toString();
	status = fsnodes_setacl(p, acl, context.ts());
	if (context.isPersonalityMaster()) {
		if (status == SAUNAFS_STATUS_OK) {
			fs_changelog(context.ts(), "SETRICHACL(%" PRIu32 ",%s)", p->id, acl_string.c_str());
		}
	} else {
		gMetadata->metaversion++;
	}
	return status;
}

uint8_t fs_setacl(const FsContext &context, uint32_t inode, AclType type, const AccessControlList &acl) {
	ChecksumUpdater cu(context.ts());
	FSNode *p;
	uint8_t status = verify_session(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kAny, MODE_MASK_EMPTY,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	std::string acl_string = acl.toString();
	status = fsnodes_setacl(p, type, acl, context.ts());
	if (context.isPersonalityMaster()) {
		if (status == SAUNAFS_STATUS_OK) {
			fs_changelog(context.ts(), "SETACL(%" PRIu32 ",%c,%s)", p->id,
						 (type == AclType::kAccess ? 'a' : 'd'), acl_string.c_str());
		}
	} else {
		gMetadata->metaversion++;
	}
	return status;

	return SAUNAFS_ERROR_EINVAL;
}

uint8_t fs_getacl(const FsContext &context, uint32_t inode, RichACL &acl) {
	FSNode *p;
	uint8_t status = verify_session(context, OperationMode::kReadOnly, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kAny, MODE_MASK_EMPTY,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	return fsnodes_getacl(p, acl);
}

#endif /* #ifndef METARESTORE */

uint8_t fs_apply_setacl(uint32_t ts, uint32_t inode, char aclType, const char *aclString) {
	AccessControlList acl;
	try {
		acl = AccessControlList::fromString(aclString);
	} catch (Exception &) {
		return SAUNAFS_ERROR_EINVAL;
	}
	FSNode *p = fsnodes_id_to_node(inode);
	if (!p) {
		return SAUNAFS_ERROR_ENOENT;
	}
	AclType aclTypeEnum;
	if (!decodeChar("da", {AclType::kDefault, AclType::kAccess}, aclType, aclTypeEnum)) {
		return SAUNAFS_ERROR_EINVAL;
	}
	uint8_t status = fsnodes_setacl(p, aclTypeEnum, std::move(acl), ts);
	if (status == SAUNAFS_STATUS_OK) {
		gMetadata->metaversion++;
	}
	return status;
}

uint8_t fs_apply_setrichacl(uint32_t ts, uint32_t inode, const std::string &acl_string) {
	RichACL acl;
	try {
		acl = RichACL::fromString(acl_string);
	} catch (Exception &) {
		return SAUNAFS_ERROR_EINVAL;
	}
	FSNode *p = fsnodes_id_to_node(inode);
	if (!p) {
		return SAUNAFS_ERROR_ENOENT;
	}
	uint8_t status = fsnodes_setacl(p, std::move(acl), ts);
	if (status == SAUNAFS_STATUS_OK) {
		gMetadata->metaversion++;
	}
	return status;
}

#ifndef METARESTORE
uint32_t fs_getdirpath_size(uint32_t inode) {
	FSNode *node;
	node = fsnodes_id_to_node(inode);
	if (node) {
		if (node->type != FSNode::kDirectory) {
			return 15;  // "(not directory)"
		} else {
			FSNodeDirectory *parent = nullptr;
			if (!node->parent.empty()) {
				parent = fsnodes_id_to_node_verify<FSNodeDirectory>(
				    node->parent[0].first);
			}
			return 1 + fsnodes_getpath_size(parent, node);
		}
	} else {
		return 11;  // "(not found)"
	}
	return 0;  // unreachable
}

void fs_getdirpath_data(uint32_t inode, uint8_t *buff, uint32_t size) {
	FSNode *node;
	node = fsnodes_id_to_node(inode);
	if (node) {
		if (node->type != FSNode::kDirectory) {
			if (size >= 15) {
				memcpy(buff, "(not directory)", 15);
				return;
			}
		} else {
			if (size > 0) {
				FSNodeDirectory *parent = nullptr;
				if (!node->parent.empty()) {
					parent = fsnodes_id_to_node_verify<FSNodeDirectory>(
					    node->parent[0].first);
				}

				buff[0] = '/';
				fsnodes_getpath_data(parent, node, buff + 1, size - 1);
				return;
			}
		}
	} else {
		if (size >= 11) {
			memcpy(buff, "(not found)", 11);
			return;
		}
	}
}

uint8_t fs_get_dir_stats(const FsContext &context, uint32_t inode,
                         uint32_t *inodes, uint32_t *dirs, uint32_t *files,
                         uint32_t *links, uint32_t *chunks, uint64_t *length,
                         uint64_t *size, uint64_t *rsize) {
	FSNode *p;
	statsrecord sr;

	uint8_t status = verify_session(context, OperationMode::kReadOnly, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kFileOrDirectory, MODE_MASK_EMPTY,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	fsnodes_get_stats(p, &sr);
	*inodes = sr.inodes;
	*dirs = sr.dirs;
	*files = sr.files;
	*links = sr.links;
	*chunks = sr.chunks;
	*length = sr.length;
	*size = sr.size;
	*rsize = sr.realsize;
	//      syslog(LOG_NOTICE,"using fast stats");
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_get_chunkid(const FsContext &context, uint32_t inode, uint32_t index,
			uint64_t *chunkid) {
	FSNode *p;
	uint8_t status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kFile,
	                                                MODE_MASK_EMPTY, inode, &p);
	FSNodeFile *node_file = static_cast<FSNodeFile*>(p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	if (index > MAX_INDEX) {
		return SAUNAFS_ERROR_INDEXTOOBIG;
	}
	if (index < node_file->chunks.size()) {
		*chunkid = node_file->chunks[index];
	} else {
		*chunkid = 0;
	}
	return SAUNAFS_STATUS_OK;
}
#endif

void fs_add_files_to_chunks() {
	FSNode *f;
	for (uint32_t i = 0; i < NODEHASHSIZE; i++) {
		for (f = gMetadata->nodehash[i]; f; f = f->next) {
			if (f->type == FSNode::kFile || f->type == FSNode::kTrash ||
			    f->type == FSNode::kReserved) {
				for (const auto &chunkid : static_cast<FSNodeFile*>(f)->chunks) {
					if (chunkid > 0) {
						chunk_add_file(chunkid, f->goal);
					}
				}
			}
		}
	}
}

uint64_t fs_getversion() {
	if (!gMetadata) {
		throw NoMetadataException();
	}
	return gMetadata->metaversion;
}

#ifndef METARESTORE
const std::map<int, Goal> &fs_get_goal_definitions() {
	return gGoalDefinitions;
}

const Goal &fs_get_goal_definition(uint8_t goalId) {
	return gGoalDefinitions[goalId];
}

std::vector<JobInfo> fs_get_current_tasks_info() {
	return gMetadata->task_manager.getCurrentJobsInfo();
}

uint8_t fs_cancel_job(uint32_t job_id) {
	if (gMetadata->task_manager.cancelJob(job_id)) {
		return SAUNAFS_STATUS_OK;
	} else {
		return SAUNAFS_ERROR_EINVAL;
	}
}

uint32_t fs_reserve_job_id() {
	return gMetadata->task_manager.reserveJobId();
}

uint8_t fs_getchunksinfo(const FsContext& context, uint32_t current_ip, uint32_t inode,
		uint32_t chunk_index, uint32_t chunk_count, std::vector<ChunkWithAddressAndLabel> &chunks) {
	static constexpr int kMaxNumberOfChunkCopies = 100;

	FSNode *p;

	uint8_t status = verify_session(context, OperationMode::kReadOnly, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	status = fsnodes_get_node_for_operation(context, ExpectedNodeType::kFile, MODE_MASK_R,
	                                        inode, &p);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	if (chunk_index > MAX_INDEX) {
		return SAUNAFS_ERROR_INDEXTOOBIG;
	}

	FSNodeFile *file_node = static_cast<FSNodeFile *>(p);

	std::vector<ChunkPartWithAddressAndLabel> chunk_parts;

	if (chunk_count == 0) {
		chunk_count = file_node->chunks.size();
	}

	chunks.clear();
	while(chunk_index < file_node->chunks.size() && chunk_count > 0) {
		uint64_t chunk_id = file_node->chunks[chunk_index];
		uint32_t chunk_version;

		chunk_parts.clear();
		chunk_version = 0;
		if (chunk_id > 0) {
			status = chunk_getversionandlocations(chunk_id, current_ip, chunk_version, kMaxNumberOfChunkCopies, chunk_parts);
			if (status != SAUNAFS_STATUS_OK) {
				return status;
			}
		}

		chunks.emplace_back(std::move(chunk_id), std::move(chunk_version), std::move(chunk_parts));
		chunk_index++;
		chunk_count--;
	}

	return SAUNAFS_STATUS_OK;
}

#endif
