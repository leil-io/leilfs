/*

   Copyright 2016 Skytechnology sp. z o.o.
   Copyright 2023 Leil Storage OÜ

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

#include "master/setgoal_task.h"

#include "kv/ifuture.h"
#include "master/filesystem_checksum.h"
#include "master/filesystem_metadata.h"
#include "master/filesystem_operations_interface.h"
#include "slogger/slogger.h"

int SetGoalTask::execute(uint32_t ts, intrusive_list<Task> &work_queue) {
	assert(current_inode_ != inode_list_.end());

	inode_t inode = *current_inode_;

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	FSNode *node = gFSOperations->nodeOperations()->idToNode(fsOpContext, inode);

	if (!node) { return SAUNAFS_ERROR_EINVAL; }

	uint8_t result;
	try {
		result = setGoal(fsOpContext, node, ts);
	} catch (const kv::TransactionError &e) {
		safs::log_warn("setgoal: backend error: {}", e.what());
		return SAUNAFS_ERROR_IO;
	}

	if (result == kBackendWaiting) {
		if (fsOpContext.hasReadWriteTransaction() &&
		    fsOpContext.getReadWriteTransaction()->mutationCount() > 0 &&
		    !fsOpContext.commitTransaction()) {
			safs::log_err("{}: transaction failed to start setgoal: inode {}, goal {}, smode {}",
			              __func__, inode, static_cast<uint32_t>(goal_),
			              static_cast<uint32_t>(smode_));
			return SAUNAFS_ERROR_IO;
		}
		waitingForChange_ = true;
		return SAUNAFS_STATUS_OK;
	}

	const bool backendPublished = waitingForChange_ && result == kNotChanged && node->goal == goal_;
	if (backendPublished) { result = kChanged; }
	waitingForChange_ = false;
	++current_inode_;

	// A file with an active bulk operation keeps one goal for its owned range.
	// The client retries once the operation finishes and clears its fence.
	// A recursive job walks on (parity with setgoalRecursive: the file counts as
	// unchanged) so one fenced file cannot abort the whole tree.
	if (result == kTemporarilyLocked || result == kChunkTableError) {
		if (smode_ & SMODE_RMASK) {
			(*stats_)[kNotChanged] += 1;
			return SAUNAFS_STATUS_OK;
		}
		return result == kTemporarilyLocked ? SAUNAFS_ERROR_LOCKED : SAUNAFS_ERROR_IO;
	}

	if (result != kNoAction) {
		if (node->type == FSNodeType::kDirectory && (smode_ & SMODE_RMASK)) {
			auto inode_list = gFSOperations->nodeOperations()->getDirectoryChildInodes(
			    fsOpContext, static_cast<const FSNodeDirectory *>(node));
			if (!inode_list.empty()) {
				auto *task = new SetGoalTask(std::move(inode_list), uid_, goal_, smode_, stats_);
				work_queue.push_front(*task);
			}
		}

		if ((smode_ & SMODE_RMASK) == 0 && result == kNotPermitted) {
			return SAUNAFS_ERROR_EPERM;
		}
		(*stats_)[result] += 1;
		if (result == kChanged && !backendPublished) {
			gFSOperations->changeLog(fsOpContext, ts,
			                         "SETGOAL(%" PRIiNode ",%" PRIu32 ",%" PRIu8 ",%" PRIu8 ")",
			                         inode, uid_, goal_, smode_);
		}
	}

	if (result == kChanged && fsOpContext.hasReadWriteTransaction() &&
	    fsOpContext.getReadWriteTransaction()->mutationCount() > 0) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err("{}: transaction failed to commit: inode {}, goal {}, smode {}", __func__,
			              inode, static_cast<uint32_t>(goal_), static_cast<uint32_t>(smode_));
			return SAUNAFS_ERROR_IO;
		}
	}

	return SAUNAFS_STATUS_OK;
}

bool SetGoalTask::isFinished() const {
	return current_inode_ == inode_list_.end();
}

uint8_t SetGoalTask::setGoal(const FilesystemOperationContext &fsOpContext, FSNode *node,
                             uint32_t ts) {
	if (node->type == FSNodeType::kFile || node->type == FSNodeType::kDirectory ||
	    node->type == FSNodeType::kTrash || node->type == FSNodeType::kReserved) {
		if ((node->mode & (EATTR_NOOWNER << EATTR_BIT_OFFSET)) == 0 && uid_ != 0 &&
		    node->uid != uid_) {
			return SetGoalTask::kNotPermitted;
		} else {
			if ((smode_ & SMODE_TMASK) == SMODE_SET && node->goal != goal_) {
				if (node->type != FSNodeType::kDirectory) {
					// changeFileGoal reports LOCKED while a bulk operation owns the
					// chunk table, and IO when a row is unreadable.
					const uint8_t changeStatus = gFSOperations->nodeOperations()->changeFileGoal(
					    fsOpContext, static_cast<FSNodeFile *>(node), goal_,
					    {.timestamp = ts, .uid = uid_, .mode = smode_, .emitChangelog = true});
					if (changeStatus == SAUNAFS_ERROR_LOCKED) {
						return SetGoalTask::kTemporarilyLocked;
					}
					if (changeStatus == SAUNAFS_ERROR_WAITING) {
						return SetGoalTask::kBackendWaiting;
					}
					if (changeStatus != SAUNAFS_STATUS_OK) { return SetGoalTask::kChunkTableError; }
				} else {
					node->goal = goal_;
				}
				gFSOperations->nodeOperations()->updateCTime(fsOpContext, node, ts);
				fsnodes_update_checksum(node);

				// Make goal updates persistent for KV backends.
				gFSOperations->nodeOperations()->updateNode(fsOpContext, node);
				return SetGoalTask::kChanged;
			} else {
				return SetGoalTask::kNotChanged;
			}
		}
	}
	return SetGoalTask::kNoAction;
}
