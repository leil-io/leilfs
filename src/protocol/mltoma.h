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

#include <map>
#include <string>

#include "common/serialization_macros.h"

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		mltoma, registerShadow, SAU_MLTOMA_REGISTER_SHADOW, 0,
		uint32_t, version,
		uint32_t, timeout_ms,
		uint64_t, metadataVersion)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		mltoma, changelogApplyError, SAU_MLTOMA_CHANGELOG_APPLY_ERROR, 0,
		uint8_t, status)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		mltoma, matoclport, SAU_MLTOMA_CLTOMA_PORT, 0,
		uint16_t, port)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		mltoma, dumpConfiguration, SAU_MLTOMA_DUMP_CONFIG, 0,
		std::string, config)

SAUNAFS_DEFINE_PACKET_SERIALIZATION(
		mltoma, startTls, SAU_MLTOMA_STARTTLS, 0)
