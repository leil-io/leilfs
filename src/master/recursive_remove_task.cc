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

#include "master/recursive_remove_task.h"

#include "master/filesystem_operations_interface.h"
#include "master/filesystem_stats.h"

bool RemoveTask::isFinished() const {
	return current_subtask_ == subtask_.end();
}

int RemoveTask::retrieveNodes(FSNodeDirectory *&wd, FSNode *&child) {
	FSNode *wd_tmp = gFSOperations->nodeOperations()->fsnodes_id_to_node(parent_);
	if (!wd_tmp) {
		return SAUNAFS_ERROR_ENOENT;
	}
	if (!gFSOperations->nodeOperations()->fsnodes_access(*context_, wd_tmp, MODE_MASK_W)) {
		return SAUNAFS_ERROR_EACCES;
	}
	wd = static_cast<FSNodeDirectory*>(wd_tmp);
	child = gFSOperations->nodeOperations()->fsnodes_lookup(wd, *current_subtask_);
	if (!child) {
		return SAUNAFS_ERROR_ENOENT;
	}
	if (!gFSOperations->nodeOperations()->fsnodes_sticky_access(wd, child, context_->uid())) {
		return SAUNAFS_ERROR_EPERM;
	}
	return SAUNAFS_STATUS_OK;
}

void RemoveTask::doUnlink(uint32_t ts, FSNodeDirectory *wd, FSNode *child) {
	gFSOperations->changeLog(
	    ts, "UNLINK(%" PRIiNode ",%s):%" PRIiNode, parent_,
	    gFSOperations->nodeOperations()->fsnodes_escape_name(*current_subtask_).c_str(), child->id);
	gFSOperations->nodeOperations()->fsnodes_unlink(ts, wd, *current_subtask_, child);
}

int RemoveTask::execute(uint32_t ts, intrusive_list<Task> &work_queue) {
	FSNodeDirectory *wd = nullptr;
	FSNode *child = nullptr;
	int status = retrieveNodes(wd, child);
	if (status != SAUNAFS_STATUS_OK) {
		return status;
	}
	if (child->type == FSNodeType::kDirectory &&
	    !static_cast<FSNodeDirectory *>(child)->entries.empty()) {
		SubtaskContainer subtasks;
		subtasks.reserve(
			  static_cast<const FSNodeDirectory*>(child)->entries.size());
		for (const auto &entry :
				static_cast<FSNodeDirectory*>(child)->entries) {
			subtasks.push_back(static_cast<HString>(*entry.first));
		}
		auto *task = new RemoveTask(std::move(subtasks), child->id, context_);
		work_queue.push_front(*task);
		if (++repeat_counter_ >= kMaxRepeatCounter) {
			// something keeps adding files to a folder which is being deleted
			return SAUNAFS_ERROR_ENOTEMPTY;
		}
	} else {
		incrementFSStat(child->type == FSNodeType::kDirectory ? FsStats::Rmdir : FsStats::Unlink);
		doUnlink(ts, wd, child);
		++current_subtask_;
		repeat_counter_ = 0;
	}
	return SAUNAFS_STATUS_OK;
}
