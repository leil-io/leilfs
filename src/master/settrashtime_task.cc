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

#include "master/settrashtime_task.h"

#include "master/filesystem_checksum.h"
#include "master/filesystem_node.h"
#include "master/filesystem_operations_interface.h"

int SetTrashtimeTask::execute(uint32_t ts, intrusive_list<Task> &work_queue) {
	assert(current_inode_ != inode_list_.end());

	inode_t inode = *current_inode_;
	++current_inode_;
	FSNode *node = fsnodes_id_to_node(inode);
	if (!node) {
		return SAUNAFS_ERROR_EINVAL;
	}

	uint8_t result = setTrashtime(node, ts);

	if (result != kNoAction) {
		if (node->type == FSNodeType::kDirectory && (smode_ & SMODE_RMASK) &&
		    !static_cast<const FSNodeDirectory *>(node)->entries.empty()) {
			std::vector<inode_t> inode_list;
			inode_list.reserve(static_cast<const FSNodeDirectory *>(node)->entries.size());
			for (const auto &entry : static_cast<const FSNodeDirectory *>(node)->entries) {
				inode_list.push_back(entry.second->id);
			}
			auto task = new SetTrashtimeTask(std::move(inode_list), uid_,
			                                              trashtime_, smode_, stats_);
			work_queue.push_front(*task);
		}

		if ((smode_ & SMODE_RMASK) == 0 && result == kNotPermitted) {
			return SAUNAFS_ERROR_EPERM;
		}
		(*stats_)[result] += 1;
		if (result == kChanged) {
			gFSOperations->changeLog(
			    ts, "SETTRASHTIME(%" PRIiNode ",%" PRIu32 ",%" PRIu32 ",%" PRIu8 ")", inode, uid_,
			    trashtime_, smode_);
		}
	}
	return SAUNAFS_STATUS_OK;
}

bool SetTrashtimeTask::isFinished() const {
	return current_inode_ == inode_list_.end();
}

uint8_t SetTrashtimeTask::setTrashtime(FSNode *node, uint32_t ts) {
	uint8_t set;

	if (node->type == FSNodeType::kFile || node->type == FSNodeType::kDirectory ||
	    node->type == FSNodeType::kTrash || node->type == FSNodeType::kReserved) {
		if ((node->mode & (EATTR_NOOWNER << 12)) == 0 && uid_ != 0 && node->uid != uid_) {
			return SetTrashtimeTask::kNotPermitted;
		} else {
			set = 0;
			auto old_trash_key = TrashPathKey(node);
			switch (smode_ & SMODE_TMASK) {
			case SMODE_SET:
				if (node->trashtime != trashtime_) {
					node->trashtime = trashtime_;
					set = 1;
				}
				break;
			case SMODE_INCREASE:
				if (node->trashtime < trashtime_) {
					node->trashtime = trashtime_;
					set = 1;
				}
				break;
			case SMODE_DECREASE:
				if (node->trashtime > trashtime_) {
					node->trashtime = trashtime_;
					set = 1;
				}
				break;
			}
			if (set) {
				node->ctime = ts;
				if (node->type == FSNodeType::kTrash) {
					updateTrashFromOldEntry(gMetadata->trash, node, old_trash_key);
				}
				fsnodes_update_checksum(node);
				return SetTrashtimeTask::kChanged;
			} else {
				return SetTrashtimeTask::kNotChanged;
			}
		}
	}
	return SetTrashtimeTask::kNoAction;
}
