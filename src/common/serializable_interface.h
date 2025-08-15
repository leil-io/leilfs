/*
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

#include <cstddef>
#include <cstdint>

/// Interface for serializable objects.
class ISerializable {
public:
	// Default constructor
	ISerializable() = default;

	// virtual destructor for polymorphic deletion
	virtual ~ISerializable() = default;

	ISerializable(const ISerializable &) = delete;
	ISerializable &operator=(const ISerializable &) = delete;
	ISerializable(ISerializable &&) = delete;
	ISerializable &operator=(ISerializable &&) = delete;

	/// Returns the size of the serialized object in bytes.
	virtual size_t serializedSize() const = 0;

	/// Serializes the object into the provided destination buffer.
	/// The function assumes that the destination buffer has enough space allocated.
	/// Use serializedSize in the caller to ensure sufficient space.
	virtual void serialize(uint8_t **destination) const = 0;

	/// Deserializes the object from the provided source buffer.
	virtual void deserialize(const uint8_t **source) = 0;
};
