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

#include "common/platform.h"

#if defined(_WIN32)
#define LOG_EMERG 0   /* system is unusable */
#define LOG_ALERT 1   /* action must be taken immediately */
#define LOG_CRIT 2    /* critical conditions */
#define LOG_ERR 3     /* error conditions */
#define LOG_WARNING 4 /* warning conditions */
#define LOG_NOTICE 5  /* normal but significant condition */
#define LOG_INFO 6    /* informational */
#define LOG_DEBUG 7   /* debug-level messages */
#else
#include <syslog.h>
#endif

/**
 * This header contains only the C-style logging function declarations.
 * It's designed to be included by modules that need to call logging
 * functions but shouldn't depend on the full slogger implementation.
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * function names may contain following words:
 *   "pretty" -> write pretty prefix to stderr
 *   "silent" -> do not write anything to stderr
 *   "errlog" -> append strerr(errno) to printed message
 *   "attempt" -> instead of pretty prefix based on priority, write prefix suggesting
 *      that something is starting
 */

void safs_pretty_syslog(int priority, const char *format, ...)
    __attribute__((__format__(__printf__, 2, 3)));

void safs_pretty_syslog_attempt(int priority, const char *format, ...)
    __attribute__((__format__(__printf__, 2, 3)));

void safs_pretty_errlog(int priority, const char *format, ...)
    __attribute__((__format__(__printf__, 2, 3)));

void safs_silent_syslog(int priority, const char *format, ...)
    __attribute__((__format__(__printf__, 2, 3)));

void safs_silent_errlog(int priority, const char *format, ...)
    __attribute__((__format__(__printf__, 2, 3)));

#ifdef __cplusplus
}  // extern "C"
#endif
