/*
   Copyright 2025      Leil Storage OÜ

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

#pragma once

#include "master/metadata_backend_interface.h"
#include "master/task_manager.h"

/// Task for deferred metadata dumping after failover
/// This allows chunkservers to connect immediately while metadata dump happens in background
class DeferredMetadataDumpTask : public TaskManager::Task {
public:
	DeferredMetadataDumpTask(IMetadataBackend *backend);

	int execute(uint32_t /*timeStamp*/, intrusive_list<Task> &/*subTasks*/) override;
	bool isFinished() const override;

	static std::string generateDescription() { return "Deferred metadata dump (post-failover)"; }

private:
	IMetadataBackend *backend_ = nullptr;
};
