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

#include "master/filesystem_checksum.h"
#include "master/filesystem_metadata.h"
#include "master/filesystem_operations_interface.h"

int SetGoalTask::execute(uint32_t ts, intrusive_list<Task> &work_queue) {
	assert(current_inode_ != inode_list_.end());

	inode_t inode = *current_inode_;
	++current_inode_;
	FSNode *node = gFSOperations->nodeOperations()->idToNode(inode);
	if (!node) {
		return SAUNAFS_ERROR_EINVAL;
	}

	uint8_t result = setGoal(node, ts);

	if (result != kNoAction) {
		if (node->type == FSNodeType::kDirectory && (smode_ & SMODE_RMASK) &&
		    !static_cast<const FSNodeDirectory *>(node)->entries.empty()) {
			std::vector<inode_t> inode_list;
			inode_list.reserve(static_cast<const FSNodeDirectory *>(node)->entries.size());
			for (const auto &entry : static_cast<const FSNodeDirectory *>(node)->entries) {
				inode_list.push_back(entry.second->id);
			}
			auto task = new SetGoalTask(std::move(inode_list), uid_, goal_, smode_, stats_);
			work_queue.push_front(*task);
		}

		if ((smode_ & SMODE_RMASK) == 0 && result == kNotPermitted) {
			return SAUNAFS_ERROR_EPERM;
		}
		(*stats_)[result] += 1;
		if (result == kChanged) {
			gFSOperations->changeLog(ts, "SETGOAL(%" PRIiNode ",%" PRIu32 ",%" PRIu8 ",%" PRIu8 ")",
			                         inode, uid_, goal_, smode_);
		}
	}

	return SAUNAFS_STATUS_OK;
}

bool SetGoalTask::isFinished() const {
	return current_inode_ == inode_list_.end();
}

uint8_t SetGoalTask::setGoal(FSNode *node, uint32_t ts) {
	if (node->type == FSNodeType::kFile || node->type == FSNodeType::kDirectory ||
	    node->type == FSNodeType::kTrash || node->type == FSNodeType::kReserved) {
		if ((node->mode & (EATTR_NOOWNER << 12)) == 0 && uid_ != 0 && node->uid != uid_) {
			return SetGoalTask::kNotPermitted;
		} else {
			if ((smode_ & SMODE_TMASK) == SMODE_SET && node->goal != goal_) {
				if (node->type != FSNodeType::kDirectory) {
					gFSOperations->nodeOperations()->changeFileGoal(static_cast<FSNodeFile *>(node),
					                                                goal_);
				} else {
					node->goal = goal_;
					gMetadata->nodeChangedSignal.emit(node);
				}
				gFSOperations->nodeOperations()->updateCTime(node, ts);
				fsnodes_update_checksum(node);
				return SetGoalTask::kChanged;
			} else {
				return SetGoalTask::kNotChanged;
			}
		}
	}
	return SetGoalTask::kNoAction;
}
