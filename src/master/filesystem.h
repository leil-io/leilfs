/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2017 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include <cstdint>

#include "common/exception.h"
#include "common/type_defs.h"
#include "master/checksum.h"
#include "master/fs_context.h"
#include "protocol/quota.h"

SAUNAFS_CREATE_EXCEPTION_CLASS_MSG(NoMetadataException, Exception, "no metadata");

inline std::string gClusterId;  ///< Unique cluster identifier

uint8_t fs_apply_freeinodes(uint32_t timestamp, inode_t freeinodes);

uint8_t fs_quota_get_all(const FsContext &context, std::vector<QuotaEntry> &results);
uint8_t fs_quota_get(const FsContext &context, const std::vector<QuotaOwner> &owners,
                     std::vector<QuotaEntry> &results);
uint8_t fs_quota_set(const FsContext &context, const std::vector<QuotaEntry> &entries);
uint8_t fs_quota_get_info(const FsContext &context, const std::vector<QuotaEntry> &entries,
                          std::vector<std::string> &result);

uint8_t fs_apply_setquota(char rigor, char resource, char ownerType, inode_t ownerId,
                          uint64_t limit);

/// Returns checksum of the loaded metadata.
uint64_t fs_checksum(ChecksumMode mode);

/// Starts recalculating metadata checksum in background.
/// \return SAUNAFS_STATUS_OK iff dump started successfully, otherwise cause of the failure.
uint8_t fs_start_checksum_recalculation();

/// Load whole filesystem information.
int fs_loadall(bool isFromInit);

/// Unloads metadata.
/// This should be called in the shadow master each time it needs to download
/// metadata file from the active metadata server again.
void fs_unload();

/// Removes metadata lock leaving working directory in a clean state
void fs_unlock();

// Number of changelog file versions
const uint32_t kDefaultStoredPreviousBackMetaCopies = 1;
const uint32_t kMaxStoredPreviousBackMetaCopies = 99;

#ifdef METARESTORE

void fs_dump(void);
void fs_term(const char *fname, bool noLock);
int fs_init(const char *fname, int ignoreflag, bool noLock);
void fs_disable_checksum_verification(bool value);

#else

void fs_cs_disconnected(void);

// Disable saving metadata on exit
void fs_disable_metadata_dump_on_exit();

/// Erases a message from metadata lockfile.
/// This function should be called before the first operation which may change
/// files in data dir (e.g., rotation of logs, creating new metadata file, ...)
void fs_erase_message_from_lockfile();

int fs_init(void);
int fs_init(bool force);

#endif
