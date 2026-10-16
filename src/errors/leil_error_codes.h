/*


   Copyright 2017 Skytechnology sp. z o.o.
   Copyright 2023 Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <stdint.h>

enum saunafs_error_code {
	SAUNAFS_STATUS_OK                     =  0,    // OK
	SAUNAFS_ERROR_EPERM                   =  1,    // Operation not permitted
	SAUNAFS_ERROR_ENOTDIR                 =  2,    // Not a directory
	SAUNAFS_ERROR_ENOENT                  =  3,    // No such file or directory
	SAUNAFS_ERROR_EACCES                  =  4,    // Permission denied
	SAUNAFS_ERROR_EEXIST                  =  5,    // File exists
	SAUNAFS_ERROR_EINVAL                  =  6,    // Invalid argument
	SAUNAFS_ERROR_ENOTEMPTY               =  7,    // Directory not empty
	SAUNAFS_ERROR_CHUNKLOST               =  8,    // Chunk lost
	SAUNAFS_ERROR_OUTOFMEMORY             =  9,    // Out of memory
	SAUNAFS_ERROR_INDEXTOOBIG             = 10,    // Index too big
	SAUNAFS_ERROR_LOCKED                  = 11,    // Chunk locked
	SAUNAFS_ERROR_NOCHUNKSERVERS          = 12,    // No chunk servers
	SAUNAFS_ERROR_NOCHUNK                 = 13,    // No such chunk
	SAUNAFS_ERROR_CHUNKBUSY               = 14,    // Chunk is busy
	SAUNAFS_ERROR_REGISTER                = 15,    // Incorrect register BLOB
	SAUNAFS_ERROR_NOTDONE                 = 16,    // Requested operation not completed
	SAUNAFS_ERROR_GROUPNOTREGISTERED      = 17,    // Group info is not registered in master server
	SAUNAFS_ERROR_NOTSTARTED              = 18,    // Write not started
	SAUNAFS_ERROR_WRONGVERSION            = 19,    // Wrong chunk version
	SAUNAFS_ERROR_CHUNKEXIST              = 20,    // Chunk already exists
	SAUNAFS_ERROR_NOSPACE                 = 21,    // No space left
	SAUNAFS_ERROR_IO                      = 22,    // IO error
	SAUNAFS_ERROR_BNUMTOOBIG              = 23,    // Incorrect block number
	SAUNAFS_ERROR_WRONGSIZE               = 24,    // Incorrect size
	SAUNAFS_ERROR_WRONGOFFSET             = 25,    // Incorrect offset
	SAUNAFS_ERROR_CANTCONNECT             = 26,    // Can't connect
	SAUNAFS_ERROR_WRONGCHUNKID            = 27,    // Incorrect chunk id
	SAUNAFS_ERROR_DISCONNECTED            = 28,    // Disconnected
	SAUNAFS_ERROR_CRC                     = 29,    // CRC error
	SAUNAFS_ERROR_DELAYED                 = 30,    // Operation delayed
	SAUNAFS_ERROR_CANTCREATEPATH          = 31,    // Can't create path
	SAUNAFS_ERROR_MISMATCH                = 32,    // Data mismatch
	SAUNAFS_ERROR_EROFS                   = 33,    // Read-only file system
	SAUNAFS_ERROR_QUOTA                   = 34,    // Quota exceeded
	SAUNAFS_ERROR_BADSESSIONID            = 35,    // Bad session id
	SAUNAFS_ERROR_NOPASSWORD              = 36,    // Password is needed
	SAUNAFS_ERROR_BADPASSWORD             = 37,    // Incorrect password
	SAUNAFS_ERROR_ENOATTR                 = 38,    // Attribute not found
	SAUNAFS_ERROR_ENOTSUP                 = 39,    // Operation not supported
	SAUNAFS_ERROR_ERANGE                  = 40,    // Result too large
	SAUNAFS_ERROR_TIMEOUT                 = 41,    // Timeout
	SAUNAFS_ERROR_BADMETADATACHECKSUM     = 42,    // Metadata checksum not matching
	SAUNAFS_ERROR_CHANGELOGINCONSISTENT   = 43,    // Changelog inconsistent
	SAUNAFS_ERROR_PARSE                   = 44,    // Parsing unsuccessful
	SAUNAFS_ERROR_METADATAVERSIONMISMATCH = 45,    // Metadata version mismatch
	SAUNAFS_ERROR_NOTLOCKED               = 46,    // No such lock
	SAUNAFS_ERROR_WRONGLOCKID             = 47,    // Wrong lock id
	SAUNAFS_ERROR_NOTPOSSIBLE             = 48,    // It's not possible to perform operation in this way
	SAUNAFS_ERROR_TEMP_NOTPOSSIBLE        = 49,    // Operation temporarily not possible
	SAUNAFS_ERROR_WAITING                 = 50,    // Waiting for operation completion
	SAUNAFS_ERROR_UNKNOWN                 = 51,    // Unknown error
	SAUNAFS_ERROR_ENAMETOOLONG            = 52,    // Name too long
	SAUNAFS_ERROR_EFBIG                   = 53,    // File too large
	SAUNAFS_ERROR_EBADF                   = 54,    // Bad file number
	SAUNAFS_ERROR_ENODATA                 = 55,    // No data available
	SAUNAFS_ERROR_E2BIG                   = 56,    // Argument list too long
	SAUNAFS_ERROR_MAX                     = 57
};

const char *saunafs_error_string(uint8_t status);
