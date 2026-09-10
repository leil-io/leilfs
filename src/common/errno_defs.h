/*


   Copyright 2015 Skytechnology sp. z o.o.
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

#ifdef _WIN32
#ifdef ENODATA
#undef ENODATA
#endif
#define ENODATA    61  /* No data available */
#define ENOTBLK    15  /* Block device required */
#define EDQUOT     122 /* Quota exceeded */
#ifdef ETXTBSY
#undef ETXTBSY
#endif
#define ETXTBSY    26  /* Text file busy */
#endif
