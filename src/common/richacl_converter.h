/*


   Copyright 2017 Skytechnology sp. z o.o.
   Copyright 2023 Leil Storage OÜ

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

#include "common/richacl.h"

namespace richAclConverter {

	/*
	 * An exception of failed xattr/AccessControlList generation
	 */
	SAUNAFS_CREATE_EXCEPTION_CLASS(ConversionException, Exception);

	/*
	 * An exception of failure during extraction xattr to ACL object
	 */
	SAUNAFS_CREATE_EXCEPTION_CLASS(ExtractionException, Exception);

	RichACL extractObjectFromNFS(const uint8_t* buffer, uint32_t bufferSize);

	RichACL extractObjectFromRichACL(const uint8_t* buffer, uint32_t bufferSize);

	std::vector<uint8_t> objectToRichACLXattr(const RichACL& acl);

	std::vector<uint8_t> objectToNFSXattr(const RichACL& acl, uint32_t owner_id);
}
