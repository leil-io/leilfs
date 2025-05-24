/*
   Copyright 2023 Leil Storage

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

#include <array>
#include <string>

#include "common/massert.h"

/// Subfolder is a helper class to obtain information about folders used in
/// operations like create paths and subfolders and scan of disks.
class Subfolder {
public:
	static constexpr uint16_t kNumberOfSubfolders = 256;
	static constexpr uint8_t kSubfolderOffset = 16;
	static constexpr uint8_t kSubfolderMask = 0xFF;

	inline static uint8_t getSubfolderNumber(uint64_t chunkId) {
		return (chunkId >> kSubfolderOffset) & kSubfolderMask;
	}

	inline static std::string getSubfolderNameGivenNumber(
	    uint32_t subfolderNumber) {
		sassert(subfolderNumber < kNumberOfSubfolders);
		constexpr size_t kMaxNameSize = 16;
		std::array<char, kMaxNameSize> buffer{};
		sprintf(buffer.data(), "chunks%02X",
		        static_cast<unsigned>(subfolderNumber));
		return {buffer.data()};
	}

	inline static std::string getSubfolderNameGivenChunkId(uint64_t chunkId) {
		return getSubfolderNameGivenNumber(getSubfolderNumber(chunkId));
	}
};
