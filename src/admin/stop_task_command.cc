/*

   Copyright 2017 Skytechnology sp. z o.o.
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
#include "admin/stop_task_command.h"

#include <iostream>

#include "admin/registered_admin_connection.h"
#include "common/job_info.h"
#include "protocol/cltoma.h"
#include "protocol/matocl.h"

std::string StopTaskCommand::name() const {
	return "stop-task";
}

SaunaFsAdminCommand::SupportedOptions StopTaskCommand::supportedOptions() const {
	return {{kTlsMode, kTlsModeDescription}};
}

void StopTaskCommand::usage() const {
	std::cerr << name() << " <master ip> <master port> <task id>" << std::endl;
	std::cerr << "    Stop execution of task with the given id" << std::endl;
}

void StopTaskCommand::run(const Options& options) const {
	if (options.arguments().size() != 3) {
		throw WrongUsageException("Expected <master ip> <master port> <task id> for " + name());
	}

	uint32_t msgid = 0, task_id = 0;
	try {
		task_id = std::stoi(options.argument(2), nullptr, 0);
	} catch (std::invalid_argument &e) {
		std::cout << "Expected <task_id> as integer number" << std::endl;
		return;
	}

	auto tlsCfg =
	    options.getValue<std::string>("--tlsconfigfile", std::string(TlsSession::kNoFile));

	auto connection =
	    RegisteredAdminConnection::create(options.argument(0), options.argument(1), tlsCfg);
	auto request = cltoma::stopTask::build(msgid, task_id);
	auto response = connection->sendAndReceive(request, SAU_MATOCL_STOP_TASK);
	uint8_t status;
	matocl::stopTask::deserialize(response, msgid, status);
	if (status == SAUNAFS_STATUS_OK) {
		std::cout << "Task (id: 0x" << std::hex << task_id << ") has been cancelled" << std::endl;
	} else {
		std::cout << "Given task_id does not identify any currently running task" << std::endl;
	}
	std::cout << std::dec;
}
