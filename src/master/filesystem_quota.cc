/*
   Copyright 2013-2016 Skytechnology sp. z o.o.
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

#include <master/filesystem_quota.h>

#include <cassert>

#include "common/event_loop.h"
#include "common/quota_database.h"
#include "master/filesystem_checksum_updater.h"
#include "master/filesystem_metadata.h"
#include "master/filesystem_operations_interface.h"

#ifndef METARESTORE
/*! \brief Remove entries that are not descendants of \param root_inode. */
static void fs_remove_invisible_quota_entries(inode_t root_inode, std::vector<QuotaEntry> &results) {
	if (root_inode == SPECIAL_INODE_ROOT) {
		return;
	}

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);

	FSNodeDirectory *root_node =
	    gFSOperations->nodeOperations()->idToNodeVerify<FSNodeDirectory>(fsOpContext, root_inode);

	auto it = std::remove_if(
	    results.begin(), results.end(), [&fsOpContext, root_node](const QuotaEntry &entry) {
		    if (entry.entryKey.owner.ownerType == QuotaOwnerType::kInode) {
			    FSNode *node = gFSOperations->nodeOperations()->idToNode(
			        fsOpContext, entry.entryKey.owner.ownerId);
			    if (!node) { return true; }
			    if (root_node->id == entry.entryKey.owner.ownerId) { return false; }
			    return !gFSOperations->nodeOperations()->isAncestor(fsOpContext, root_node, node);
		    }
		    return false;
	    });
	results.erase(it, results.end());
}

static void fsnodes_getpath(const FilesystemOperationContext &fsOpContext, inode_t root_inode,
                            FSNode *node, std::string &ret) {
	std::string::size_type size;
	FSNode *p;

	if (node->id == root_inode) {
		ret = "/";
		return;
	}

	p = node;
	size = 0;
	while (p != gMetadata->root && !p->parents.empty() && p->id != root_inode) {
		// get first parent
		auto *parent = gFSOperations->nodeOperations()->idToNodeVerify<FSNodeDirectory>(
		    fsOpContext, p->parents[0].first);
		size += parent->getChildName(p).length() + 1;
		p = parent;
	}
	if (size > 65535) {
		safs_pretty_syslog(LOG_WARNING, "path too long !!! - truncate");
		size = 65535;
	}

	ret.resize(size);

	p = node;
	while (p != gMetadata->root && !p->parents.empty()) {
		auto *parent = gFSOperations->nodeOperations()->idToNodeVerify<FSNodeDirectory>(
		    fsOpContext, p->parents[0].first);
		std::string name = parent->getChildName(p);
		if (size >= name.length()) {
			size -= name.length();
			std::copy(name.begin() , name.end(), ret.begin() + size);
		} else {
			if (size > 0) {
				std::copy(name.begin() + (name.length() - size), name.end(), ret.begin());
				size = 0;
			}
		}
		if (size > 0) {
			ret[--size] = '/';
		}
		p = parent;
	}
}

namespace quotas {
uint8_t fs_quota_get_all(const FsContext &context, std::vector<QuotaEntry> &results) {
	if (context.uid() != 0 && !(context.sesflags() & SESFLAG_ALLCANCHANGEQUOTA)) {
		return SAUNAFS_ERROR_EPERM;
	}
	results = gMetadata->quotaDatabase.getEntriesWithStats();

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);

	for (auto &entry : results) {
		if (entry.entryKey.owner.ownerType != QuotaOwnerType::kInode ||
		    entry.entryKey.rigor != QuotaRigor::kUsed) {
			continue;
		}

		FSNodeDirectory *node = gFSOperations->nodeOperations()->idToNode<FSNodeDirectory>(
		    fsOpContext, entry.entryKey.owner.ownerId);
		if (!node || node->type != FSNodeType::kDirectory) { continue; }

		switch (entry.entryKey.resource) {
		case QuotaResource::kSize:
			entry.limit = node->stats.size;
			break;
		case QuotaResource::kInodes:
			entry.limit = node->stats.inodes;
			break;
		}
	}

	fs_remove_invisible_quota_entries(context.rootinode(), results);

	return SAUNAFS_STATUS_OK;
}

uint8_t fs_quota_get(const FsContext &context, const std::vector<QuotaOwner> &owners,
                     std::vector<QuotaEntry> &results) {
	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);
	std::vector<QuotaEntry> tmp;
	FSNodeDirectory *node;
	for (const QuotaOwner &owner : owners) {
		if (context.uid() != 0 && !(context.sesflags() & SESFLAG_ALLCANCHANGEQUOTA)) {
			switch (owner.ownerType) {
			case QuotaOwnerType::kUser:
				if (context.uid() != owner.ownerId) { return SAUNAFS_ERROR_EPERM; }
				break;
			case QuotaOwnerType::kGroup:
				if (context.gid() != owner.ownerId && !(context.sesflags() & SESFLAG_IGNOREGID)) {
					return SAUNAFS_ERROR_EPERM;
				}
				break;
			case QuotaOwnerType::kInode:
				node = gFSOperations->nodeOperations()->idToNode<FSNodeDirectory>(fsOpContext,
				                                                                  owner.ownerId);
				if (!node || node->type != FSNodeType::kDirectory) { return SAUNAFS_ERROR_EINVAL; }
				if (node->uid != context.uid() ||
				    (node->gid != context.gid() && !(context.sesflags() & SESFLAG_IGNOREGID))) {
					return SAUNAFS_ERROR_EPERM;
				}
				break;
			default:
				return SAUNAFS_ERROR_EINVAL;
			}
		}
		auto result = gMetadata->quotaDatabase.get(owner.ownerType, owner.ownerId);
		if (result) {
			for (auto rigor : {QuotaRigor::kSoft, QuotaRigor::kHard, QuotaRigor::kUsed}) {
				if (owner.ownerType == QuotaOwnerType::kInode && rigor == QuotaRigor::kUsed) {
					node = gFSOperations->nodeOperations()->idToNode<FSNodeDirectory>(
					    fsOpContext, owner.ownerId);
					assert(node);
					tmp.push_back(
					    {{owner, rigor, QuotaResource::kInodes}, (uint64_t)node->stats.inodes});
					tmp.push_back(
					    {{owner, rigor, QuotaResource::kSize}, (uint64_t)node->stats.size});
					continue;
				}
				for (auto resource : {QuotaResource::kInodes, QuotaResource::kSize}) {
					tmp.push_back({{owner, rigor, resource}, (*result)[(int)rigor][(int)resource]});
				}
			}
		}
	}

	fs_remove_invisible_quota_entries(context.rootinode(), tmp);
	results = std::move(tmp);

	return SAUNAFS_STATUS_OK;
}

uint8_t fs_quota_get_info(const FsContext &context, const std::vector<QuotaEntry> &entries,
		std::vector<std::string> &result) {
	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);
	std::string info;

	result.clear();
	for (const auto &entry : entries) {
		info.clear();
		if (entry.entryKey.owner.ownerType == QuotaOwnerType::kInode) {
			FSNode *node = gFSOperations->nodeOperations()->idToNode(fsOpContext,
			                                                         entry.entryKey.owner.ownerId);
			if (node) { fsnodes_getpath(fsOpContext, context.rootinode(), node, info); }
		}
		result.push_back(info);
	}
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_quota_set(const FsContext &context, const FilesystemOperationContext &fsOpContext,
                     const std::vector<QuotaEntry> &entries) {
	static const char rigor_name[3] = {'S', 'H', 'U'};
	static const char resource_name[2] = {'I', 'S'};
	static const char owner_name[3] = {'U', 'G', 'I'};

	uint32_t ts = eventloop_time();
	ChecksumUpdater cu(ts);
	if (context.sesflags() & SESFLAG_READONLY) {
		return SAUNAFS_ERROR_EROFS;
	}
	if (context.uid() != 0 && !(context.sesflags() & SESFLAG_ALLCANCHANGEQUOTA)) {
		return SAUNAFS_ERROR_EPERM;
	}
	for (const QuotaEntry &entry : entries) {
		if(entry.entryKey.owner.ownerType != QuotaOwnerType::kInode) {
			continue;
		}
		FSNode *node =
		    gFSOperations->nodeOperations()->idToNode(fsOpContext, entry.entryKey.owner.ownerId);
		if(!node) {
			return SAUNAFS_ERROR_EINVAL;
		}
	}

	for (const QuotaEntry &entry : entries) {
		const QuotaOwner &owner = entry.entryKey.owner;
		gMetadata->quotaDatabase.set(owner.ownerType, owner.ownerId, entry.entryKey.rigor,
		                              entry.entryKey.resource, entry.limit);
		gMetadata->quotaDatabase.removeEmpty(owner.ownerType, owner.ownerId);
		gMetadata->quotaChecksum = gMetadata->quotaDatabase.checksum();
		gFSOperations->changeLog(
		    fsOpContext, ts, "SETQUOTA(%c,%c,%c,%" PRIiNode ",%" PRIu64 ")",
		    rigor_name[(int)entry.entryKey.rigor], resource_name[(int)entry.entryKey.resource],
		    owner_name[(int)owner.ownerType], inode_t{owner.ownerId}, uint64_t{entry.limit});
		// Notify durable backends so the owner's limit rows are persisted
		gQuotaChangedSignal.emit(owner.ownerType, owner.ownerId);
	}
	return SAUNAFS_STATUS_OK;
}
}  // namespace quotas

#endif  // METARESTORE

namespace quotas {
uint8_t fs_apply_setquota(char rigor, char resource, char owner_type, inode_t owner_id,
                          uint64_t limit) {
	QuotaRigor quotaRigor = QuotaRigor::kSoft;
	QuotaResource quotaResource = QuotaResource::kSize;
	QuotaOwnerType quotaOwnerType = QuotaOwnerType::kUser;
	bool valid = true;
	valid &= decodeChar("SH", {QuotaRigor::kSoft, QuotaRigor::kHard}, rigor, quotaRigor);
	valid &=
	    decodeChar("SI", {QuotaResource::kSize, QuotaResource::kInodes}, resource, quotaResource);
	valid &=
	    decodeChar("UGI", {QuotaOwnerType::kUser, QuotaOwnerType::kGroup, QuotaOwnerType::kInode},
	               owner_type, quotaOwnerType);
	if (!valid) {
		return SAUNAFS_ERROR_EINVAL;
	}
	gMetadata->metadataVersion++;
	gMetadata->quotaDatabase.set(quotaOwnerType, owner_id, quotaRigor, quotaResource, limit);
	gMetadata->quotaDatabase.removeEmpty(quotaOwnerType, owner_id);
	gMetadata->quotaChecksum = gMetadata->quotaDatabase.checksum();
	// Notify durable backends so the owner's limit rows are persisted
	gQuotaChangedSignal.emit(quotaOwnerType, owner_id);
	return SAUNAFS_STATUS_OK;
}
}  // namespace quotas

static int fsnodes_find_depth(const FilesystemOperationContext &fsOpContext, FSNodeDirectory *a) {
	assert(a);

	int depth = 1;
	while (!a->parents.empty()) {
		a = gFSOperations->nodeOperations()->idToNodeVerify<FSNodeDirectory>(fsOpContext,
		                                                                     a->parents[0].first);
		++depth;
	}

	return depth;
}

/*! \brief Find common ancestor.
 *
 * Only path starting from first parent is used to find
 * common ancestor.
 * If the nodes are files with many hard links,
 * then it's possible that this function will fail.
 *
 * \param fsOpContext Filesystem operation context with a potential transaction.
 *  \return Pointer to common ancestor.
 */
static FSNode *fsnodes_find_common_ancestor(const FilesystemOperationContext &fsOpContext,
                                            FSNodeDirectory *a, FSNodeDirectory *b) {
	if (!a || !b) {
		return nullptr;
	}

	int depth_a = fsnodes_find_depth(fsOpContext, a);
	int depth_b = fsnodes_find_depth(fsOpContext, b);

	if (depth_a > depth_b) {
		for(;depth_a > depth_b;--depth_a) {
			assert(a && !a->parents.empty());
			a = gFSOperations->nodeOperations()->idToNodeVerify<FSNodeDirectory>(
			    fsOpContext, a->parents[0].first);
		}
	} else if (depth_b > depth_a) {
		for(;depth_b > depth_a;--depth_b) {
			assert(b && !b->parents.empty());
			b = gFSOperations->nodeOperations()->idToNodeVerify<FSNodeDirectory>(
			    fsOpContext, b->parents[0].first);
		}
	}

	if (a == b) {
		return a;
	}

	while(!a->parents.empty()) {
		assert(!b->parents.empty());

		a = gFSOperations->nodeOperations()->idToNodeVerify<FSNodeDirectory>(fsOpContext,
		                                                                     a->parents[0].first);
		b = gFSOperations->nodeOperations()->idToNodeVerify<FSNodeDirectory>(fsOpContext,
		                                                                     b->parents[0].first);

		if (a == b) {
			return a;
		}
	}

	return nullptr;
}

static bool fsnodes_test_dir_quota_noparents(FSNode *node,
		const std::initializer_list<std::pair<QuotaResource, int64_t>> &resource_list) {
	if (!node || node->type != FSNodeType::kDirectory) {
		return false;
	}

	const QuotaDatabase::Limits *entry =
	    gMetadata->quotaDatabase.get(QuotaOwnerType::kInode, node->id);
	if (!entry) {
		return false;
	}

	const StatsRecord &stats = static_cast<FSNodeDirectory*>(node)->stats;
	uint64_t limit;

	for (const auto &resource : resource_list) {
		limit = (*entry)[(int)QuotaRigor::kHard][(int)resource.first];
		if (limit <= 0) {
			continue;
		}

		switch (resource.first) {
		case QuotaResource::kInodes:
			if (((uint64_t)stats.inodes + resource.second) > limit) {
				return true;
			}
			break;
		case QuotaResource::kSize:
			if ((stats.size + resource.second) > limit) {
				return true;
			}
			break;
		}
	}

	return false;
}

bool fsnodes_quota_exceeded_ug(uint32_t uid, uint32_t gid,
		const std::initializer_list<std::pair<QuotaResource, int64_t>> &resource_list) {
	return gMetadata->quotaDatabase.exceeds(QuotaOwnerType::kUser, uid, QuotaRigor::kHard, resource_list) ||
	       gMetadata->quotaDatabase.exceeds(QuotaOwnerType::kGroup, gid, QuotaRigor::kHard, resource_list);
}

bool fsnodes_quota_exceeded_ug(FSNode *node,
		const std::initializer_list<std::pair<QuotaResource, int64_t>> &resource_list) {
	if (!node) {
		return false;
	}

	return fsnodes_quota_exceeded_ug(node->uid, node->gid, resource_list);
}

bool fsnodes_quota_exceeded_dir(
    const FilesystemOperationContext &fsOpContext, FSNode *node,
    const std::initializer_list<std::pair<QuotaResource, int64_t>> &resource_list) {
	if (!node) {
		return false;
	}

	if (fsnodes_test_dir_quota_noparents(node, resource_list)) {
		return true;
	}

	if (node->type == FSNodeType::kDirectory) {
		// Directory can have only one parent, so we get rid of recursion.
		while(!node->parents.empty()) {
			auto *parent = gFSOperations->nodeOperations()->idToNodeVerify<FSNodeDirectory>(
			    fsOpContext, node->parents[0].first);
			if (fsnodes_test_dir_quota_noparents(parent, resource_list)) {
				return true;
			}
			node = parent;
		}
	} else {
		for (const auto &[parentId, _] : node->parents) {
			auto *parent = gFSOperations->nodeOperations()->idToNodeVerify<FSNodeDirectory>(
			    fsOpContext, parentId);
			if (fsnodes_quota_exceeded_dir(fsOpContext, parent, resource_list)) { return true; }
		}
	}

	return false;
}

bool fsnodes_quota_exceeded_dir(
    const FilesystemOperationContext &fsOpContext, FSNodeDirectory *node,
    FSNodeDirectory *prev_node,
    const std::initializer_list<std::pair<QuotaResource, int64_t>> &resource_list) {
	// Because nodes are directories fsnodes_find_common_ancestor
	// is guaranteed to work properly.
	FSNode *common = fsnodes_find_common_ancestor(fsOpContext, prev_node, node);
	if (node == common) {
		return false;
	}

	if (fsnodes_test_dir_quota_noparents(node, resource_list)) {
		return true;
	}

	// node is directory so it has only one parent.
	while(!node->parents.empty()) {
		auto *parent = gFSOperations->nodeOperations()->idToNode<FSNodeDirectory>(
		    fsOpContext, node->parents[0].first);

		if (parent == common) {
			return false;
		}

		if (fsnodes_test_dir_quota_noparents(parent, resource_list)) {
			return true;
		}

		node = parent;
	}

	return false;
}

bool fsnodes_quota_exceeded(
    const FilesystemOperationContext &fsOpContext, FSNode *node,
    const std::initializer_list<std::pair<QuotaResource, int64_t>> &resource_list) {
	return fsnodes_quota_exceeded_ug(node, resource_list) ||
	       fsnodes_quota_exceeded_dir(fsOpContext, node, resource_list);
}

void fsnodes_quota_update(FSNode *node,
		const std::initializer_list<std::pair<QuotaResource, int64_t>> &resource_list) {
	for (const auto &resource : resource_list) {
		if (resource.second == 0) {
			continue;
		}
		gMetadata->quotaDatabase.update(QuotaOwnerType::kUser, node->uid, QuotaRigor::kUsed,
		                                 resource.first, resource.second);
		gMetadata->quotaDatabase.update(QuotaOwnerType::kGroup, node->gid, QuotaRigor::kUsed,
		                                 resource.first, resource.second);
	}
}

void fsnodes_quota_remove(QuotaOwnerType owner_type, inode_t owner_id) {
	gMetadata->quotaDatabase.remove(owner_type, owner_id);
	gMetadata->quotaChecksum = gMetadata->quotaDatabase.checksum();
	// Notify durable backends so the owner's limit rows are removed
	gQuotaChangedSignal.emit(owner_type, owner_id);
}

void fsnodes_quota_adjust_space(FSNode * /*node*/, uint64_t & /*total_space*/,
		uint64_t & /*available_space*/) {
}
