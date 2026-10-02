/*

   Copyright 2026 Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/platform.h"

#include <cstring>

#include "mount/acl_xattr_names.h"

bool isAclXattrName(const char *name) {
	return strcmp(name, "system.posix_acl_access") == 0 ||
			strcmp(name, "system.posix_acl_default") == 0 ||
			strcmp(name, "system.nfs4_acl") == 0 ||
			strcmp(name, "system.richacl") == 0
#ifdef __APPLE__
			|| strcmp(name, "com.apple.system.Security") == 0
#endif
			;
}

std::vector<uint8_t> filterAclXattrNames(const uint8_t *names, uint32_t length) {
	std::vector<uint8_t> filtered;
	if (names == nullptr || length == 0) {
		return filtered;
	}
	const uint8_t *current = names;
	const uint8_t *end = names + length;
	while (current < end) {
		const uint8_t *terminator = (const uint8_t*)memchr(current, '\0', end - current);
		if (!terminator) {
			break;
		}
		if (!isAclXattrName((const char*)current)) {
			filtered.insert(filtered.end(), current, terminator + 1);
		}
		current = terminator + 1;
	}
	return filtered;
}
