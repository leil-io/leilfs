/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
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

#include "common/type_defs.h"
#include "master/id_generator_interface.h"

/// Inode id generator using the inode pool from gMetadata.
/// Provides the usual behavior but now implements the IIdGenerator interface to allow custom
/// polymorphic generators.
class IdGeneratorWithDetainer : public IIdGenerator<inode_t> {
public:
	IdGeneratorWithDetainer() = default;

	/// Nothing to initialize in this implementation
	bool initialize() override { return true; }

	/// Get next free inode number.
	///
	/// @param timeStamp    Current time stamp (backward compatibility)
	/// @param requestedId  Requested id: >0 - specific id, 0 - get any free id
	///
	/// @return 0 - no more free ids, >0 - allocated id (may differ from requested if already taken)
	/// @note This implementation uses the inode pool from gMetadata to provide the usual behavior.
	inode_t getNextId(uint32_t timeStamp, inode_t requestedId) override;

	/// Gets next free id and prepares/obtains the next one.
	/// Uses the inode pool detainer from gMetadata to provide the usual behavior.
	inode_t getNextId() override { return getNextId(0, 0); }
};
