/*
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
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
#include "config.h"

/* Workaround for Debian/kFreeBSD which does not define ENODATA */
#if defined(__FreeBSD_kernel__) || defined(__FreeBSD__)
#ifndef ENODATA
#define ENODATA ENOATTR
#endif
#endif

/* Workaround for musl libc (e.g. Alpine Linux), which doesn't define the
   BSD/glibc S_BLKSIZE extension. Its value has always been 512 wherever
   it is defined, so this is a behavior-preserving fallback, not a guess. */
#include <sys/stat.h>
#ifndef S_BLKSIZE
#define S_BLKSIZE 512
#endif

#ifndef SAUNAFS_HAVE_STD_TO_STRING

#include <sstream>
#include <string>

namespace std {

template<typename T>
inline std::string to_string(const T val) {
	std::stringstream ss;
	ss << val;
	return ss.str();
}

}

#endif /* #ifndef SAUNAFS_HAVE_STD_TO_STRING */

#ifndef SAUNAFS_HAVE_STD_STOULL

#include <cstdlib>
#include <string>

namespace std {

inline unsigned long long stoull(const std::string& s, std::size_t* pos = 0, int base = 10) {
	char* end = nullptr;
	unsigned long long val = strtoull(s.c_str(), &end, base);
	if (pos != nullptr) {
		*pos = end - s.c_str();
	}
	return val;
}

}

#endif /* #ifndef SAUNAFS_HAVE_STD_STOULL */

#if defined(SAUNAFS_HAVE_JUDY) && (__WORDSIZE == 64 || _WIN64 || __x86_64__)
#  define SAUNAFS_HAVE_64BIT_JUDY
#endif

// thread_local hack for old GCC
#ifndef SAUNAFS_HAVE_THREAD_LOCAL
#define thread_local __thread
#endif
