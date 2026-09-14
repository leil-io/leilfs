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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "slogger/slogger.h"
#include "errors/sfserr.h"
#ifndef _WIN32
#include <sys/syslog.h>
#endif

#ifdef THROW_INSTEAD_OF_ABORT
#  include <stdexcept>
#  include <string>
#  define ABORT_OR_THROW() throw std::runtime_error(\
		std::string(__FILE__ ":") + std::to_string(__LINE__))
#else
#  define ABORT_OR_THROW() abort()
#endif

#define massert(e, msg) do { if (!(e)) { \
				safs_pretty_syslog(LOG_ERR, "failed assertion '%s' : %s", #e, (msg)); \
				ABORT_OR_THROW(); \
		} } while (false)

#define passert(ptr) do { if ((ptr) == NULL) { \
				safs_pretty_syslog(LOG_ERR, "out of memory: %s is NULL", #ptr); \
				ABORT_OR_THROW(); \
		} } while (false)

#define sassert(e) do { if (!(e)) { \
				safs_pretty_syslog(LOG_ERR, "failed assertion '%s'", #e); \
				ABORT_OR_THROW(); \
		} } while (false)

#define eassert(e) do { if (!(e)) { \
			const char *_sfs_errorstring = strerr(errno); \
			safs_pretty_syslog(LOG_ERR, "failed assertion '%s', error: %s", #e, _sfs_errorstring); \
			ABORT_OR_THROW(); \
		} } while(false)

#define zassert(e) do { if ((e) != 0) { \
			const char *_sfs_errorstring = strerr(errno); \
			safs_pretty_syslog(LOG_ERR, "unexpected status, '%s' returned: %s", #e, _sfs_errorstring); \
			ABORT_OR_THROW(); \
		} } while(false)

#define mabort(msg) do { \
			safs_pretty_syslog(LOG_ERR, "abort '%s'", msg); \
			ABORT_OR_THROW(); \
		} while (false)
