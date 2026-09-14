/*
   Copyright 2023 Leil Storage OÜ

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

#include <algorithm>
#include <array>
#include <boost/container/small_vector.hpp>
#include <cassert>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <vector>

// This class is a wrapper around boost::container::small_vector that adds
// constructors for initializer lists and iterators. This is useful because
// boost::container::small_vector does not have these constructors yet.
template <typename T, size_t N = 8>
class small_vector : public boost::container::small_vector<T, N> {
public:
	using base = boost::container::small_vector<T, N>;
	using size_type = base::size_type;
	using value_type = base::value_type;

	small_vector() : base() {}

	explicit small_vector(size_type n) : base(n) {}

	small_vector(const size_type n, const T &value) {
		base::reserve(N);
		base::insert(base::end(), n, value);
	}

	small_vector(std::initializer_list<T> initializerList) {
		base::reserve(std::max(N, initializerList.size()));
		base::insert(base::end(), initializerList);
	}

	template <
	    typename InputIterator,
	    typename = typename std::enable_if<std::is_convertible<
	        typename std::iterator_traits<InputIterator>::iterator_category,
	        std::input_iterator_tag>::value>::type>
	small_vector(InputIterator first, InputIterator last) : base() {
		base::reserve(std::max<size_type>(N, std::distance(first, last)));
		base::insert(base::end(), first, last);
	}
};
