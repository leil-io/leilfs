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

/// Interface for ID generation strategies.
///
/// Supports multiple implementation strategies:
/// - IdGeneratorIncremental: Simple monotonic counter (used for chunk IDs in Master)
/// - IdGeneratorWithDetainer: Timestamp-based with ID reuse (used for inode IDs in Master)
/// - IdGeneratorKVWithRanges: Distributed range-based (used in MDS with FoundationDB)
///
/// Not all implementations use all methods. Implementations may ignore parameters
/// that don't apply to their strategy (e.g., incremental generators ignore timestamp).
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

	/// Gets next free id and prepares/obtains the next one.
	/// The way to obtain it is implementation-specific.
	virtual T getNextId() = 0;
};

/// Extended interface for ID generators that maintain state.
template <std::unsigned_integral T>
class IIdGeneratorWithState : public IIdGenerator<T> {
public:
	IIdGeneratorWithState() = default;

	/// Virtual destructor
	virtual ~IIdGeneratorWithState() = default;

	// Not needed copy/move constructors/assignments
	IIdGeneratorWithState(const IIdGeneratorWithState &) = delete;
	IIdGeneratorWithState &operator=(const IIdGeneratorWithState &) = delete;
	IIdGeneratorWithState(IIdGeneratorWithState &&) = delete;
	IIdGeneratorWithState &operator=(IIdGeneratorWithState &&) = delete;

	/// Get the current id value (the next to be returned) without incrementing it.
	/// @return Current id value
	virtual T getCurrentId() const = 0;

	/// Set the current id to a specific value.
	/// Used in some backends when loading from metadata or database or in special scenarios.
	/// @see IdGeneratorIncremental for specific scenarios.
	/// @param value  New current id value
	/// @return true if successful, false if the value is considered invalid in a concrete
	/// implementation (e.g., lower than current value in incremental generators).
	virtual bool setCurrentId(T value) = 0;

	/// Check if the generator is strictly monotonic (can not go backwards under any circumstances).
	/// E.g., the current behavior of IdGeneratorIncremental, a chunk appearing with higher id than
	/// the current one would be considered an error.
	/// Counter-example: A distributed generator (other nodes may allocate ids in parallel).
	/// @return true if strictly monotonic, false otherwise.
	virtual bool isStrictlyMonotonic() const = 0;
};
