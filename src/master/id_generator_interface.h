/*
   Copyright 2023      Leil Storage OÜ

   This file is part of SaunaFS.

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

#include <concepts>
#include <cstdint>

/// Generic interface for id generators
template<std::unsigned_integral T>
class IIdGenerator {
public:
	/// Default constructor
	IIdGenerator() = default;

	/// Virtual destructor
	virtual ~IIdGenerator() = default;

	// Not needed copy/move constructors/assignments
	IIdGenerator(const IIdGenerator &) = delete;
	IIdGenerator &operator=(const IIdGenerator &) = delete;
	IIdGenerator(IIdGenerator &&) = delete;
	IIdGenerator &operator=(IIdGenerator &&) = delete;

	/// Overload to implement custom initialization if needed for concrete generators
	virtual bool initialize() = 0;

	/// Get next free inode number.
	///
	/// @param timeStamp    Current time stamp (backward compatibility)
	/// @param requestedId  Requested id: >0 - specific id, 0 - get any free id
	///
	/// @return 0 - no more free ids, >0 - allocated id (may differ from requested if already taken)
	virtual T getNextId(uint32_t timeStamp, T requestedId) = 0;
};
