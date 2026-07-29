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

#include "master/task_manager.h"

#include "common/loop_watchdog.h"
#include "errors/sfserr.h"
#include "kv/ifuture.h"
#include "slogger/slogger.h"

void TaskManager::Job::finalize(int status) {
	if (finish_callback_) {
		finish_callback_(status);
	}
	tasks_.clear_and_dispose([](Task *ptr) { delete ptr; });
}

void TaskManager::Job::finalizeTask(TaskIterator itask, int status) {
	if (status || (itask->isFinished() && tasks_.size() <= 1)) {
		finalize(status);
	} else if (itask->isFinished()) {
		tasks_.erase_and_dispose(itask, [](Task *ptr) { delete ptr; });
	}
}

void TaskManager::Job::processTask(uint32_t ts) {
	if (!tasks_.empty()) {
		auto i_front = tasks_.begin();
		int status;
		try {
			status = i_front->execute(ts, tasks_);
		} catch (const kv::TransactionError &e) {
			// Without this boundary the job stays at the front of the queue and
			// re-executes (and fails again) every loop iteration, never answering
			// the client. Fail the whole job cleanly instead.
			safs::log_warn("task manager: backend error, failing job '{}': {}", description_,
			               e.what());
			status = SAUNAFS_ERROR_IO;
		}
		finalizeTask(i_front, status);
	}
}

JobInfo TaskManager::Job::getInfo() const {
	return { id_, description_ };
}

int TaskManager::submitTask(uint32_t taskid, uint32_t ts, int initial_batch_size, Task *task,
	                    const std::string &description, const std::function<void(int)> &callback) {
	Job new_job(taskid, description);

	int done = 0;
	int status = SAUNAFS_STATUS_OK;

	new_job.setFinishCallback([&done, &status](int code) {
		status = code;
		done = 1;
	});

	new_job.addTask(task);
	for (int i = 0; i < initial_batch_size; i++) {
		new_job.processTask(ts);
		if (new_job.isFinished()) {
			break;
		}
	}

	if (done) {
		assert(new_job.isFinished());
		return status;
	}

	new_job.setFinishCallback(callback);
	job_list_.push_back(std::move(new_job));

	return SAUNAFS_ERROR_WAITING;
}

int TaskManager::submitTask(uint32_t ts, int initial_batch_size, Task *task,
	                    const std::string &description, const std::function<void(int)> &callback) {
	return submitTask(reserveJobId(), ts, initial_batch_size, task, description, callback);
}

void TaskManager::processJobs(uint32_t ts, int number_of_tasks) {
	SignalLoopWatchdog watchdog;
	JobIterator it = job_list_.begin();
	watchdog.start();
	for (int i = 0; i < number_of_tasks; ++i) {
		if (it == job_list_.end() || watchdog.expired()) {
			break;
		}
		if (it->isFinished()) {
			it = job_list_.erase(it);
		} else {
			it->processTask(ts);
			++it;
		}
		if (it == job_list_.end()) {
			it = job_list_.begin();
		}
	}
}

TaskManager::JobsInfoContainer TaskManager::getCurrentJobsInfo() const {
	JobsInfoContainer info;
	info.reserve(job_list_.size());
	for (const Job &j : job_list_) {
		info.push_back(j.getInfo());
	}
	return info;
}

bool TaskManager::cancelJob(uint32_t job_id) {
	for (auto it = job_list_.begin(); it != job_list_.end(); ++it) {
		if (it->getId() == job_id) {
			it->finalize(SAUNAFS_ERROR_NOTDONE);
			return true;
		}
	}
	return false;
}
