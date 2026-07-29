/*
   Copyright 2015-2017 Skytechnology sp. z o.o.
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

#include "config/cfg.h"
#include "master/filesystem_checksum_updater.h"
#include "master/filesystem_metadata.h"
#include "master/filesystem_operations_interface.h"
#include "master/snapshot_task.h"
#include "master/task_manager.h"

static uint32_t gInitialSnapshotTaskBatch;
static uint32_t gSnapshotTaskBatchLimit;

void fs_read_snapshot_config_file() {
	gInitialSnapshotTaskBatch = cfg_getuint32("SNAPSHOT_INITIAL_BATCH_SIZE", 1000);
	gSnapshotTaskBatchLimit = cfg_getuint32("SNAPSHOT_INITIAL_BATCH_SIZE_LIMIT", 10000);
}

uint8_t fs_snapshot(const FsContext &context, inode_t inode_src, inode_t parent_dst,
	            const HString &name_dst, uint8_t can_overwrite, uint8_t ignore_missing_src,
		    uint32_t initial_batch_size, const std::function<void(int)> &callback, uint32_t job_id) {
	ChecksumUpdater cu(context.ts());
	FSNode *src_node = nullptr;
	FSNode *dst_parent_node = nullptr;

	uint8_t status = gFSOperations->nodeOperations()->verifySession(
	    context, OperationMode::kReadWrite, SessionType::kNotMeta);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	FilesystemOperationContext fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);

	status = gFSOperations->nodeOperations()->getNodeForOperation(
	    context, fsOpContext, ExpectedNodeType::kDirectory, MODE_MASK_W, parent_dst,
	    &dst_parent_node);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = gFSOperations->nodeOperations()->getNodeForOperation(
	    context, fsOpContext, ExpectedNodeType::kAny, MODE_MASK_R, inode_src, &src_node);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	if (src_node->type == FSNodeType::kDirectory) {
		if (src_node == dst_parent_node ||
		    gFSOperations->nodeOperations()->isAncestor(
		        fsOpContext, static_cast<FSNodeDirectory *>(src_node), dst_parent_node)) {
			return SAUNAFS_ERROR_EINVAL;
		}
	}

	assert(context.isPersonalityMaster());

	auto *task =
	    gFSOperations->createSnapshotTask({{src_node->id, name_dst}}, src_node->id,
	                                      static_cast<FSNodeDirectory *>(dst_parent_node)->id, 0,
	                                      can_overwrite, ignore_missing_src, true, true);
	std::string src_path;
	FSNodeDirectory *parent =
	    gFSOperations->nodeOperations()->getFirstParent(fsOpContext, src_node);
	gFSOperations->nodeOperations()->getPath(fsOpContext, parent, src_node, src_path);

	std::string dst_path;
	FSNodeDirectory *grandparent =
	    gFSOperations->nodeOperations()->getFirstParent(fsOpContext, dst_parent_node);
	gFSOperations->nodeOperations()->getPath(fsOpContext, grandparent, dst_parent_node, dst_path);
	if (dst_path.size() > 1) {
		dst_path += "/";
	}
	dst_path += name_dst;
	if (initial_batch_size == 0) {
		initial_batch_size = gInitialSnapshotTaskBatch;
	}
	initial_batch_size = std::min(initial_batch_size, gSnapshotTaskBatchLimit);
	return gMetadata->taskManager.submitTask(job_id, context.ts(), initial_batch_size,
						  task, SnapshotTask::generateDescription(src_path, dst_path),
						  callback);
}

uint8_t fs_clone_node(const FsContext &context, inode_t inode_src, inode_t parent_dst,
			inode_t inode_dst, const HString &name_dst, uint8_t can_overwrite) {

	SnapshotTask task({{inode_src, name_dst}}, 0, parent_dst, inode_dst, can_overwrite,
			  0, false, false);

	return task.cloneNode(context.ts());
}
