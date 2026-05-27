/*
   Copyright 2016-2017 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/platform.h"

#include "master/snapshot_task.h"

#include "master/chunk_operations_interface.h"
#include "master/chunks.h"
#include "master/filesystem_checksum.h"
#include "master/filesystem_metadata.h"
#include "master/filesystem_operation_context.h"
#include "master/filesystem_operations_interface.h"

int SnapshotTask::cloneNodeTest(const FilesystemOperationContext &fsOpContext, FSNode *src_node,
                                FSNode *dst_node, FSNodeDirectory *dst_parent) {
	if (gFSOperations->quotaExceededUg(fsOpContext, src_node->uid, src_node->gid,
	                                   {{QuotaResource::kInodes, 1}}) ||
	    gFSOperations->quotaExceededDir(fsOpContext, dst_parent,
	                                    {{QuotaResource::kInodes, 1}})) {
		return SAUNAFS_ERROR_QUOTA;
	}
	if (src_node->type == FSNodeType::kFile &&
	    (gFSOperations->quotaExceededUg(fsOpContext, src_node->uid, src_node->gid,
	                                    {{QuotaResource::kSize, 1}}) ||
	     gFSOperations->quotaExceededDir(fsOpContext, dst_parent,
	                                    {{QuotaResource::kSize, 1}}))) {
		return SAUNAFS_ERROR_QUOTA;
	}

	if (dst_node) {
		if (orig_inode_ != 0 && dst_node->id == orig_inode_) {
			return SAUNAFS_ERROR_EINVAL;
		}
		if (dst_node->type != src_node->type) {
			return SAUNAFS_ERROR_EPERM;
		}
		if (src_node->type != FSNodeType::kDirectory && !can_overwrite_) {
			return SAUNAFS_ERROR_EEXIST;
		}
	}
	return SAUNAFS_STATUS_OK;
}

FSNode *SnapshotTask::cloneToExistingNode(const FilesystemOperationContext &fsOpContext,
                                          uint32_t ts, FSNode *src_node,
                                          FSNodeDirectory *dst_parent, FSNode *dst_node) {
	assert(src_node->type == dst_node->type);

	switch (src_node->type) {
	case FSNodeType::kDirectory:
		cloneDirectoryData(static_cast<const FSNodeDirectory *>(src_node),
		                   static_cast<FSNodeDirectory *>(dst_node));
		break;
	case FSNodeType::kFile:
		dst_node = cloneToExistingFileNode(fsOpContext, ts, static_cast<FSNodeFile *>(src_node),
		                                   dst_parent, static_cast<FSNodeFile *>(dst_node));
		break;
	case FSNodeType::kSymlink:
		cloneSymlinkData(fsOpContext, static_cast<FSNodeSymlink *>(src_node),
		                 static_cast<FSNodeSymlink *>(dst_node), dst_parent);
		break;
	case FSNodeType::kBlockDev:
	case FSNodeType::kCharDev:
		static_cast<FSNodeDevice *>(dst_node)->rdev = static_cast<FSNodeDevice *>(src_node)->rdev;
		break;
	default:
		// No additional data to clone for these types
		break;
	}

	dst_node->mode = src_node->mode;
	dst_node->uid = src_node->uid;
	dst_node->gid = src_node->gid;
	dst_node->atime = src_node->atime;
	dst_node->mtime = src_node->mtime;
	dst_node->ctime = ts;

	return dst_node;
}

FSNode *SnapshotTask::cloneToNewNode(const FilesystemOperationContext &fsOpContext, uint32_t ts,
                                     FSNode *src_node, FSNodeDirectory *dst_parent) {
	FSNode *dst_node = gFSOperations->nodeOperations()->createNode(fsOpContext,
	    ts, dst_parent, current_subtask_->second, src_node->type, src_node->mode, 0, src_node->uid,
	    src_node->gid, 0, AclInheritance::kDontInheritAcl, dst_inode_);

	dst_node->goal = src_node->goal;
	dst_node->trashtime = src_node->trashtime;
	dst_node->mode = src_node->mode;
	dst_node->atime = src_node->atime;
	dst_node->mtime = src_node->mtime;

	switch (src_node->type) {
	case FSNodeType::kDirectory:
		cloneDirectoryData(static_cast<const FSNodeDirectory *>(src_node),
		                   static_cast<FSNodeDirectory *>(dst_node));
		break;
	case FSNodeType::kFile:
		cloneChunkData(fsOpContext, static_cast<FSNodeFile *>(src_node),
		               static_cast<FSNodeFile *>(dst_node), dst_parent);
		break;
	case FSNodeType::kSymlink:
		cloneSymlinkData(fsOpContext, static_cast<FSNodeSymlink *>(src_node),
		                 static_cast<FSNodeSymlink *>(dst_node), dst_parent);
		break;
	case FSNodeType::kBlockDev:
	case FSNodeType::kCharDev:
		static_cast<FSNodeDevice *>(dst_node)->rdev = static_cast<FSNodeDevice *>(src_node)->rdev;
		break;
	default:
		// No additional data to clone for these types
		break;
	}

	return dst_node;
}

FSNodeFile *SnapshotTask::cloneToExistingFileNode(const FilesystemOperationContext &fsOpContext,
                                                  uint32_t ts, FSNodeFile *src_node,
                                                  FSNodeDirectory *dst_parent,
                                                  FSNodeFile *dst_node) {
	bool same = dst_node->length == src_node->length && dst_node->chunks == src_node->chunks;

	if (same) {
		return dst_node;
	}

	gFSOperations->nodeOperations()->unlink(fsOpContext, ts, dst_parent, current_subtask_->second,
	                                        dst_node);

	dst_node = static_cast<FSNodeFile *>(gFSOperations->nodeOperations()->createNode(
	    fsOpContext, ts, dst_parent, current_subtask_->second, FSNodeType::kFile, src_node->mode, 0,
	    src_node->uid, src_node->gid, 0, AclInheritance::kDontInheritAcl, dst_inode_));

	cloneChunkData(fsOpContext, src_node, dst_node, dst_parent);

	return dst_node;
}

void SnapshotTask::cloneChunkData(const FilesystemOperationContext &fsOpContext,
                                  const FSNodeFile *src_node, FSNodeFile *dst_node,
                                  FSNodeDirectory *dst_parent) {
	StatsRecord psr, nsr;

	gFSOperations->nodeOperations()->getStats(fsOpContext, dst_node, &psr);

	dst_node->goal = src_node->goal;
	dst_node->trashtime = src_node->trashtime;
	dst_node->chunks = src_node->chunks;
	dst_node->length = src_node->length;
	for (uint32_t i = 0; i < src_node->chunks.size(); ++i) {
		auto chunkid = src_node->chunks[i];
		if (chunkid > 0) {
			if (gChunkOperations->addFile(fsOpContext, chunkid, dst_node->goal) !=
			    SAUNAFS_STATUS_OK) {
				safs_pretty_syslog(LOG_ERR,
				       "structure error - chunk %016" PRIX64
				       " not found (inode: %" PRIiNode " ; index: %" PRIu32 ")",
				       chunkid, src_node->id, i);
			}
		}
	}

	gFSOperations->nodeOperations()->getStats(fsOpContext, dst_node, &nsr);
	gFSOperations->nodeOperations()->addSubStats(fsOpContext, dst_parent, &nsr, &psr);
	gFSOperations->quotaUpdate(fsOpContext, dst_node,
	                           {{QuotaResource::kSize, nsr.size - psr.size}});
}

void SnapshotTask::cloneDirectoryData(const FSNodeDirectory *src_node, FSNodeDirectory *dst_node) {
	if (!enqueue_work_) {
		return;
	}
	SubtaskContainer data;
	data.reserve(src_node->entries.size());
	for (const auto &entry : src_node->entries) {
		auto local_id = entry.second->id;
		data.emplace_back(std::move(local_id), (HString)(*entry.first));
	}
	if (!data.empty()) {
		auto task = new SnapshotTask(std::move(data), orig_inode_, dst_node->id, 0, can_overwrite_,
		                             ignore_missing_src_, emit_changelog_, enqueue_work_);
		local_tasks_.push_back(*task);
	}
}

void SnapshotTask::cloneSymlinkData(const FilesystemOperationContext &fsOpContext,
                                    FSNodeSymlink *src_node, FSNodeSymlink *dst_node,
                                    FSNodeDirectory *dst_parent) {
	StatsRecord psr, nsr;

	gFSOperations->nodeOperations()->getStats(fsOpContext, dst_node, &psr);

	dst_node->path = src_node->path;
	dst_node->path_length = src_node->path_length;

	gFSOperations->nodeOperations()->getStats(fsOpContext, dst_node, &nsr);
	gFSOperations->nodeOperations()->addSubStats(fsOpContext, dst_parent, &nsr, &psr);
}

void SnapshotTask::emitChangelog(const FilesystemOperationContext &fsOpContext, uint32_t ts,
                                 inode_t dst_inode) {
	if (!emit_changelog_) {
		gFSOperations->increaseMetadataVersion(fsOpContext);
		return;
	}

	gFSOperations->changeLog(
	    fsOpContext, ts, "CLONE(%" PRIiNode ",%" PRIiNode ",%" PRIiNode ",%s,%" PRIu8 ")",
	    current_subtask_->first, dst_parent_inode_, dst_inode,
	    gFSOperations->nodeOperations()->escapeName(current_subtask_->second).c_str(),
	    can_overwrite_);
}

int SnapshotTask::cloneNode(uint32_t ts) {
	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);
	FSNode *src_node =
	    gFSOperations->nodeOperations()->idToNode(fsOpContext, current_subtask_->first);
	auto *dst_parent =
	    gFSOperations->nodeOperations()->idToNode<FSNodeDirectory>(fsOpContext, dst_parent_inode_);

	if (!src_node || src_node->type == FSNodeType::kTrash ||
	    src_node->type == FSNodeType::kReserved) {
		return SAUNAFS_ERROR_ENOENT;
	}
	if (!dst_parent || dst_parent->type != FSNodeType::kDirectory) {
		return SAUNAFS_ERROR_EINVAL;
	}

	FSNode *dst_node =
	    gFSOperations->nodeOperations()->lookup(fsOpContext, dst_parent, current_subtask_->second);

	int status = cloneNodeTest(fsOpContext, src_node, dst_node, dst_parent);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}

	if (dst_node) {
		dst_node = cloneToExistingNode(fsOpContext, ts, src_node, dst_parent, dst_node);
	} else {
		dst_node = cloneToNewNode(fsOpContext, ts, src_node, dst_parent);
	}

	assert(dst_node);
	fsnodes_update_checksum(dst_node);
	fsnodes_update_checksum(dst_parent);

	// Persist post-createNode in-memory mutations on KV backends.
	if (fsOpContext.hasReadWriteTransaction()) {
		gFSOperations->nodeOperations()->updateNode(fsOpContext, dst_node);
		gFSOperations->nodeOperations()->updateNode(fsOpContext, dst_parent);
	}

	emitChangelog(fsOpContext, ts, dst_node->id);

	if (dst_inode_ != 0 && dst_inode_ != dst_node->id) {
		return SAUNAFS_ERROR_MISMATCH;
	}

	if (fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.getReadWriteTransaction()->commit()) {
			safs::log_err(
			    "{}: transaction failed to commit: source inode {}, destination parent inode {}, name {}",
			    __func__, current_subtask_->first, dst_parent_inode_, current_subtask_->second);
			return SAUNAFS_ERROR_IO;
		}
	}

	return SAUNAFS_STATUS_OK;
}

int SnapshotTask::execute(uint32_t ts, intrusive_list<Task> &work_queue) {
	assert(current_subtask_ != subtask_.end());

	int status = cloneNode(ts);
	++current_subtask_;

	if (ignore_missing_src_ && status == SAUNAFS_ERROR_ENOENT) {
		return SAUNAFS_STATUS_OK;
	}
	if (status == SAUNAFS_STATUS_OK) {
		work_queue.splice(work_queue.end(), local_tasks_);
	}

	return status;
}
