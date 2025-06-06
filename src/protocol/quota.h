/*
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

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

#include "common/platform.h"

#include "common/serialization_macros.h"

SAUNAFS_DEFINE_SERIALIZABLE_ENUM_CLASS(QuotaRigor, kSoft, kHard, kUsed)
SAUNAFS_DEFINE_SERIALIZABLE_ENUM_CLASS(QuotaResource, kInodes, kSize)
SAUNAFS_DEFINE_SERIALIZABLE_ENUM_CLASS(QuotaOwnerType, kUser, kGroup, kInode)

SAUNAFS_DEFINE_SERIALIZABLE_CLASS(QuotaOwner,
		QuotaOwnerType, ownerType,
		inode_t       , ownerId);

SAUNAFS_DEFINE_SERIALIZABLE_CLASS(QuotaEntryKey,
		QuotaOwner   , owner,
		QuotaRigor   , rigor,
		QuotaResource, resource);

SAUNAFS_DEFINE_SERIALIZABLE_CLASS(QuotaEntry,
		QuotaEntryKey, entryKey,
		uint64_t     , limit);
