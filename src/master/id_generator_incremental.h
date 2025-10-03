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
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "common/platform.h"

#include <master/id_generator_interface.h>
#include <slogger/slogger.h>

/// Implements IIdGenerator to provide incremental IDs.
/// Provides similar behavior to the previous nextChunkId variable, but promotes extensibility.
///
/// This generator is used for chunk IDs which are simple monotonically increasing values.
/// The timestamp-based getNextId() variant is provided for interface compatibility
/// but delegates to the simple version since chunk IDs don't need detainer logic.
/// For inode IDs with reuse/detainer requirements, use IdGeneratorWithDetainer instead.
template<std::unsigned_integral T>
class IdGeneratorIncremental : public IIdGeneratorWithState<T> {
public:
	/// Default constructor
	IdGeneratorIncremental() = default;

	/// This implementation does not need custom initialization.
	bool initialize() override { return true; }

	/// Compatibility method for interface that supports timestamp-based generation.
	/// This implementation ignores parameters and delegates to parameterless version.
	/// Used by inode generators with detainer logic; chunk IDs use simple getNextId().
	/// @param timeStamp Ignored in incremental implementation
	/// @param requestedId Ignored in incremental implementation
	/// @return Next available ID
	T getNextId(uint32_t /*timeStamp*/, T /*requestedId*/) override { return getNextId(); }

	/// Gets next free id and prepares/obtains the next one.
	/// Increases the internal counter by one.
	/// @return Next free id to use.
	T getNextId() override { return id_++; }

	/// Set the current id to a specific value, if greater than the current one.
	/// Used when loading from metadata and in some special scenarios like:
	/// - Chunk damaged (see function chunk_damaged).
	/// - Metarestore (see function do_nextchunkid).
	/// - A chunkserver reports an nonexistent chunk (see function chunk_server_has_chunk).
	/// @param value  New current id value
	/// @return true if the value was set, false if the value was not set (because it was lower than
	/// current)
	bool setCurrentId(T value) override {
		if (value >= id_) {
			id_ = value;
			return true;
		}

		safs::log_err("IdGeneratorIncremental::setCurrentId: cannot decrease id from {} to {}", id_,
		              value);
		return false;
	}

	/// Get the current id value (the next to be returned) without incrementing it.
	/// Used for instance to save the value to metadata during dumping.
	/// @return Current id value
	T getCurrentId() const override {
		return id_;
	}

	/// This implementation is strictly monotonic.
	bool isStrictlyMonotonic() const override { return true; }

private:
	T id_ = 1;  ///< Next id to be returned
};
