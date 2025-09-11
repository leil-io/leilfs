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

#include "master/deferred_metadata_dump_task.h"

#include "errors/saunafs_error_codes.h"
#include "master/metadata_dumper_interface.h"
#include "slogger/slogger.h"

DeferredMetadataDumpTask::DeferredMetadataDumpTask(IMetadataBackend *backend) : backend_(backend) {}

int DeferredMetadataDumpTask::execute(uint32_t /*timeStamp*/, intrusive_list<Task> & /*subTasks*/) {
	safs::log_info("Executing deferred metadata dump");

	try {
		uint8_t result = backend_->fs_storeall(DumpType::kBackgroundDump);

		if (result == SAUNAFS_ERROR_TEMP_NOTPOSSIBLE) {
			safs::log_info(
			    "Deferred metadata dump skipped due to periodic dump in progress - "
			    "metadata will be captured in next periodic dump");
			return SAUNAFS_STATUS_OK;  // Treat as success
		}

		if (result == SAUNAFS_STATUS_OK) {
			safs::log_info("Deferred metadata dump completed successfully");
			return SAUNAFS_STATUS_OK;
		}

		safs::log_err("Deferred metadata dump failed with result: {}", result);
		return result;
	} catch (const std::exception &e) {
		safs::log_err("Deferred metadata dump failed with exception: {}", e.what());
		return SAUNAFS_ERROR_IO;
	}
}

bool DeferredMetadataDumpTask::isFinished() const {
	return true;  // Single execution task
}
