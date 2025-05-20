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
#include <limits>
#include <new>

/// 64B (512b) sufficient for AVX-512 and most cache line sizes.
static constexpr size_t kDefaultAlignment = 64;

/**
 * Returns aligned pointers when allocations are requested.
 * Default alignment is 64B = 512b.
 *
 * @tparam ALIGNMENT_IN_BYTES Must be a positive power of 2.
 */
template <typename ElementType,
          std::size_t ALIGNMENT_IN_BYTES = kDefaultAlignment>
class AlignedAllocator {
private:
	static_assert(
	    ALIGNMENT_IN_BYTES >= alignof(ElementType),
	    "Beware that types like int have minimum alignment requirements "
	    "or access will result in crashes.");

public:
	using value_type = ElementType;
	static std::align_val_t constexpr ALIGNMENT{ALIGNMENT_IN_BYTES};

	/**
	 * This is only necessary because AlignedAllocator has a second template
	 * argument for the alignment that will make the default
	 * std::allocator_traits implementation fail during compilation.
	 */
	template <class OtherElementType>
	struct rebind {
		using other = AlignedAllocator<OtherElementType, ALIGNMENT_IN_BYTES>;
	};

	constexpr AlignedAllocator() noexcept = default;
	constexpr AlignedAllocator(const AlignedAllocator &) noexcept = default;

	template <typename U>
	explicit constexpr AlignedAllocator(
	    AlignedAllocator<U, ALIGNMENT_IN_BYTES> const & /*unused*/) noexcept {}

	~AlignedAllocator() noexcept = default;

	AlignedAllocator &operator=(const AlignedAllocator &) noexcept = delete;
	AlignedAllocator(AlignedAllocator &&) noexcept = delete;
	AlignedAllocator &operator=(AlignedAllocator &&) noexcept = delete;

	[[nodiscard]] ElementType *allocate(std::size_t nElementsToAllocate) {
		if (nElementsToAllocate >
		    std::numeric_limits<std::size_t>::max() / sizeof(ElementType)) {
			throw std::bad_array_new_length();
		}

		auto const nBytesToAllocate = nElementsToAllocate * sizeof(ElementType);
		return static_cast<ElementType *>(
		    ::operator new[](nBytesToAllocate, ALIGNMENT));
	}

	void deallocate(ElementType *allocatedPointer,
	                [[maybe_unused]] std::size_t nBytesAllocated) {
		/* According to the C++20 draft n4868 § 17.6.3.3, the delete operator
		 * must be called with the same alignment argument as the new
		 * expression. The size argument can be omitted but if present must also
		 * be equal to the one used in new. */
		::operator delete[](allocatedPointer, ALIGNMENT);
	}
};
