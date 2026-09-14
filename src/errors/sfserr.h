/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include <cstdint>

/* Workaround for Debian/kFreeBSD which does not define ENODATA */
#if defined(__FreeBSD_kernel__) || defined(__FreeBSD__)
#ifndef ENODATA
#define ENODATA ENOATTR
#endif
#endif

/// returns errno string representation
const char* strerr(int error);

/// converts sfs error to errno
int saunafs_error_conv(uint8_t status);
