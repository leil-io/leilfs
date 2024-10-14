/*
   Copyright 2023-2024  Leil Storage OÜ

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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#include <filesystem>
#pragma GCC diagnostic pop
#include <string>

class ChunkTrashManager {
public:
	constexpr static const std::string_view kTrashDirname = ".trash.bin";
	static std::string getDeletionTimeString();
	static int moveToTrash(const std::filesystem::path &filePath,
	                       const std::filesystem::path &diskPath,
	                       const std::string &deletionTime);
};
