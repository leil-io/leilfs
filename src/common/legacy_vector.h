/*
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include "common/serialization.h"

// This class behaves just as std::vector, with the exception that it is
// serialized differently. MooseFS did not send array length when serializing
// it, SaunaFS does. Some older parts of SaunaFS still use MooseFS
// serialization, so this class is still needed
template <class T>
class LegacyVector : public std::vector<T> {
public:
	// Gcc 4.6 which we support don't support inherited constructors,
	// so a workaround was needed:
	template<typename... Args>
	LegacyVector(Args&&... args) : std::vector<T>(std::forward<Args>(args)...) {
	}

	uint32_t serializedSize() const {
		uint32_t ret = 0;
		for (const auto& element : *this) {
			ret += ::serializedSize(element);
		}
		return ret;
	}

	void serialize(uint8_t** destination) const {
		for (const T& t : *this) {
			::serialize(destination, t);
		}
	}

	void deserialize(const uint8_t** source, uint32_t& bytesLeftInBuffer) {
		sassert(this->empty());
		while (bytesLeftInBuffer > 0) {
			uint32_t prevBytesLeftInBuffer = bytesLeftInBuffer;
			this->emplace_back();
			::deserialize(source, bytesLeftInBuffer, this->back());
			sassert(bytesLeftInBuffer < prevBytesLeftInBuffer);
		}
	}
};
