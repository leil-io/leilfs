/*
   LeilFS is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once
#include "common/platform.h"

#include "admin/saunafs_admin_command.h"

class ListSessionsCommand : public SaunaFsAdminCommand {
public:
	std::string name() const final;
	SupportedOptions supportedOptions() const final;
	void usage() const final;
	void run(const Options& options) const final;
};
