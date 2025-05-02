/*
   Copyright 2024      Leil Storage OÜ

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

#include <string>

namespace common {

inline std::string version() {
	return "Version: " \
	SAUNAFS_PACKAGE_VERSION "\n"
	"Build time: " BUILD_TIME "\n"
		"Git commit: " GIT_COMMIT "\n"
		"Git branch: " GIT_BRANCH
	;
}

} // namespace common
