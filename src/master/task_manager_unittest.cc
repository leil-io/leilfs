/*
   Copyright 2026      Leil Storage OÜ

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

#include <gtest/gtest.h>

#include "errors/sfserr.h"
#include "kv/ifuture.h"
#include "master/task_manager.h"

namespace {

class ThrowingTask : public TaskManager::Task {
public:
	int execute(uint32_t /*ts*/, intrusive_list<Task> & /*work_queue*/) override {
		throw kv::RetryableTransactionError(1007, "transaction too old");
	}
	bool isFinished() const override { return false; }
};

}  // namespace

TEST(TaskManagerTests, RetryableBackendErrorFailsJobClean) {
	// A retryable backend error escaping a task used to unwind past finalizeTask,
	// leaving the job queued to re-execute (and fail again) forever without ever
	// answering the client. It must surface as a clean job failure instead.
	TaskManager manager;
	const int status = manager.submitTask(/*ts=*/0, /*initial_batch_size=*/1, new ThrowingTask(),
	                                      "throwing task", [](int /*code*/) {});
	EXPECT_EQ(SAUNAFS_ERROR_IO, status);
}
