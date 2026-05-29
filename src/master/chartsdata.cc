/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


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


/* DEPRECATED (master/chartsdata.h): Use src/metrics instead */

#include "common/platform.h"

#include "master/chartsdata.h"

#include <fcntl.h>
#include <syslog.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "common/charts.h"
#include "common/event_loop.h"
#include "common/time_utils.h"
#include "master/chunk_operations_interface.h"
#include "master/filesystem_stats.h"
#include "master/matoclserv.h"
#include "slogger/slogger.h"

#if defined(SAUNAFS_HAVE_GETRUSAGE)
#include <sys/types.h>
#ifdef SAUNAFS_HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif
#ifdef SAUNAFS_HAVE_SYS_RUSAGE_H
#include <sys/rusage.h>
#endif
#ifndef RUSAGE_SELF
#define RUSAGE_SELF 0
#endif
#define CPU_USAGE 1
#endif

#if defined(CPU_USAGE) && defined(SAUNAFS_HAVE_STRUCT_RUSAGE_RU_MAXRSS)
#define MEMORY_USAGE 1
#endif

#define CHARTS_FILENAME "stats.sfs"

#define CHARTS_UCPU 0
#define CHARTS_SCPU 1
#define CHARTS_DELCHUNK 2
#define CHARTS_REPLCHUNK 3
#define CHARTS_STATFS 4
#define CHARTS_GETATTR 5
#define CHARTS_SETATTR 6
#define CHARTS_LOOKUP 7
#define CHARTS_MKDIR 8
#define CHARTS_RMDIR 9
#define CHARTS_SYMLINK 10
#define CHARTS_READLINK 11
#define CHARTS_MKNOD 12
#define CHARTS_UNLINK 13
#define CHARTS_RENAME 14
#define CHARTS_LINK 15
#define CHARTS_READDIR 16
#define CHARTS_OPEN 17
#define CHARTS_READ 18
#define CHARTS_WRITE 19
#define CHARTS_MEMORY 20
#define CHARTS_PACKETSRCVD 21
#define CHARTS_PACKETSSENT 22
#define CHARTS_BYTESRCVD 23
#define CHARTS_BYTESSENT 24

#define CHARTS 25

/* name , join mode , percent , scale , multiplier , divisor */
#define STATDEFS { \
	{"ucpu"         ,CHARTS_MODE_ADD,1,CHARTS_SCALE_MICRO, 100,60}, \
	{"scpu"         ,CHARTS_MODE_ADD,1,CHARTS_SCALE_MICRO, 100,60}, \
	{"delete"       ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"replicate"    ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"statfs"       ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"getattr"      ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"setattr"      ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"lookup"       ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"mkdir"        ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"rmdir"        ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"symlink"      ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"readlink"     ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"mknod"        ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"unlink"       ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"rename"       ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"link"         ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"readdir"      ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"open"         ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"read"         ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"write"        ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"memory"       ,CHARTS_MODE_MAX,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"prcvd"        ,CHARTS_MODE_ADD,0,CHARTS_SCALE_MILI ,1000,60}, \
	{"psent"        ,CHARTS_MODE_ADD,0,CHARTS_SCALE_MILI ,1000,60}, \
	{"brcvd"        ,CHARTS_MODE_ADD,0,CHARTS_SCALE_MILI ,8000,60}, \
	{"bsent"        ,CHARTS_MODE_ADD,0,CHARTS_SCALE_MILI ,8000,60}, \
	{NULL           ,0              ,0,0                 ,   0, 0}  \
};

#define CALCDEFS { \
	CHARTS_DEFS_END \
};

/* c1_def , c2_def , c3_def , join mode , percent , scale , multiplier , divisor */
#define ESTATDEFS { \
	{CHARTS_DIRECT(CHARTS_UCPU)        ,CHARTS_DIRECT(CHARTS_SCPU)        ,CHARTS_NONE                       ,CHARTS_MODE_ADD,1,CHARTS_SCALE_MICRO, 100,60}, \
	{CHARTS_NONE                       ,CHARTS_NONE                       ,CHARTS_NONE                       ,0              ,0,0                 ,   0, 0}  \
};

static const uint32_t calcdefs[] = CALCDEFS;
static const statdef statdefs[] = STATDEFS;
static const estatdef estatdefs[] = ESTATDEFS;

#ifdef CPU_USAGE
static struct rusage prev_rusage;
static bool prevRusageValid = false;
#endif

#ifdef MEMORY_USAGE
static uint64_t memusage;
uint64_t chartsdata_memusage(void) {
	return memusage;
}
#else
uint64_t chartsdata_memusage(void) {
	return 0;
}
#endif

void chartsdata_refresh(void) {
	uint64_t data[CHARTS];
	FsStatsArray fsdata;
	uint32_t i,del,repl; //,bin,bout,opr,opw,dbr,dbw,dopr,dopw,repl;
#ifdef MEMORY_USAGE
	struct rusage ru{};
	bool haveRusage = false;
#endif

	for (i=0 ; i<CHARTS ; i++) {
		data[i]=CHARTS_NODATA;
	}

#ifdef CPU_USAGE
	// CPU usage via getrusage deltas
	struct rusage cur{};
	if (getrusage(RUSAGE_SELF, &cur) == -1) {
		safs::log_error_code(errno, "could not get cpu usage for chartsdata");
	} else {
		if (prevRusageValid) {
			data[CHARTS_UCPU] = timeDiffUsec(cur.ru_utime, prev_rusage.ru_utime);
			data[CHARTS_SCPU] = timeDiffUsec(cur.ru_stime, prev_rusage.ru_stime);
		}
		prev_rusage = cur;
		prevRusageValid = true;

#ifdef MEMORY_USAGE
		ru = cur;
		haveRusage = true;
#endif
	}
#endif

// memory usage
#ifdef MEMORY_USAGE
	if (!haveRusage && getrusage(RUSAGE_SELF, &ru) == -1) {
		safs::log_error_code(errno, "could not get memory usage for chartsdata");
	} else {
#  ifdef __APPLE__
		memusage = ru.ru_maxrss;
#  else
		memusage = ru.ru_maxrss * UINT64_C(1024);
#  endif
#  ifdef __linux__
		if (memusage == 0) {
			int fd = open("/proc/self/statm", O_RDONLY);
			char statbuff[1000];
			int l;
			if (fd >= 0) {
				l = read(fd, statbuff, 1000);
				if (l < 1000 && l > 0) {
					statbuff[l] = 0;
					memusage = strtoul(statbuff, NULL, 10) * getpagesize();
				}
				close(fd);
			}
		}
#  endif
	}
	if (memusage>0) {
		data[CHARTS_MEMORY] = memusage;
	}
#endif

	gChunkOperations->stats(&del, &repl);
	data[CHARTS_DELCHUNK]=del;
	data[CHARTS_REPLCHUNK]=repl;
	retrieveFSStats(fsdata);
	for (i = 0 ; i < kFsStatsSize; ++i) {
		data[CHARTS_STATFS + i] = fsdata[i];
	}
	matoclserv_stats(data+CHARTS_PACKETSRCVD);

	charts_add(data,eventloop_time()-60);
}

void chartsdata_term(void) {
	chartsdata_refresh();
	charts_store();
	charts_term();
}

void chartsdata_store(void) {
	charts_store();
}

int chartsdata_init() {
#ifdef MEMORY_USAGE
	struct rusage ru{};
	bool haveRusage = false;
#endif

#ifdef CPU_USAGE
	if (getrusage(RUSAGE_SELF, &prev_rusage) == -1) {
		safs::log_error_code(errno, "could not initialize cpu usage for chartsdata");
	} else {
		prevRusageValid = true;
#ifdef MEMORY_USAGE
		ru = prev_rusage;
		haveRusage = true;
#endif
	}
#endif
#ifdef MEMORY_USAGE
	if (!haveRusage && getrusage(RUSAGE_SELF, &ru) == -1) {
		safs::log_error_code(errno, "could not initialize memory usage for chartsdata");
	} else {
#  ifdef __APPLE__
		memusage = ru.ru_maxrss;
#  else
		memusage = static_cast<uint64_t>(ru.ru_maxrss) * UINT64_C(1024);
#  endif
	}
#endif

	eventloop_timeregister(TIMEMODE_RUN_LATE, 60, 0, chartsdata_refresh);
	eventloop_timeregister(TIMEMODE_RUN_LATE, 3600, 0, chartsdata_store);
	eventloop_destructregister(chartsdata_term);
	return charts_init(calcdefs, statdefs, estatdefs, CHARTS_FILENAME);
}
