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

#include "common/platform.h"

#include "chunkserver/chartsdata.h"

#include <fcntl.h>
#include <syslog.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "chunkserver-common/hdd_stats.h"
#include "chunkserver/chunk_replicator.h"
#include "chunkserver/masterconn.h"
#include "chunkserver/network_stats.h"
#include "common/charts.h"
#include "common/event_loop.h"
#include "common/time_utils.h"
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

#define CHARTS_FILENAME "csstats.sfs"

#define CHARTS_UCPU 0
#define CHARTS_SCPU 1
#define CHARTS_MASTERIN 2
#define CHARTS_MASTEROUT 3
#define CHARTS_CSCONNIN 4
#define CHARTS_CSCONNOUT 5
#define CHARTS_CSSERVIN 6
#define CHARTS_CSSERVOUT 7
#define CHARTS_OVERHEAD_BYTESR 8
#define CHARTS_OVERHEAD_BYTESW 9
#define CHARTS_OVERHEAD_LLOPR 10
#define CHARTS_OVERHEAD_LLOPW 11
#define CHARTS_TOTAL_BYTESR 12
#define CHARTS_TOTAL_BYTESW 13
#define CHARTS_TOTAL_LLOPR 14
#define CHARTS_TOTAL_LLOPW 15
#define CHARTS_HLOPR 16
#define CHARTS_HLOPW 17
#define CHARTS_TOTAL_RTIME 18
#define CHARTS_TOTAL_WTIME 19
#define CHARTS_REPL 20
#define CHARTS_CREATE 21
#define CHARTS_DELETE 22
#define CHARTS_VERSION 23
#define CHARTS_DUPLICATE 24
#define CHARTS_TRUNCATE 25
#define CHARTS_DUPTRUNC 26
#define CHARTS_TEST 27
#define CHARTS_CHUNKIOJOBS 28
#define CHARTS_CHUNKOPJOBS 29
#define CHARTS_MEMORY 30
#define CHARTS_GC_PURGE 31
#define CHARTS_SPACE_GROWTH 32
#define CHARTS_SPACE_RECLAIMED 33

#define CHARTS_NUMBER 34

/* name , join mode , percent , scale , multiplier , divisor */
#define STATDEFS { \
	{"ucpu"             ,CHARTS_MODE_ADD,1,CHARTS_SCALE_MICRO, 100,60}, \
	{"scpu"             ,CHARTS_MODE_ADD,1,CHARTS_SCALE_MICRO, 100,60}, \
	{"masterin"         ,CHARTS_MODE_ADD,0,CHARTS_SCALE_MILI ,8000,60}, \
	{"masterout"        ,CHARTS_MODE_ADD,0,CHARTS_SCALE_MILI ,8000,60}, \
	{"csconnin"         ,CHARTS_MODE_ADD,0,CHARTS_SCALE_MILI ,8000,60}, \
	{"csconnout"        ,CHARTS_MODE_ADD,0,CHARTS_SCALE_MILI ,8000,60}, \
	{"csservin"         ,CHARTS_MODE_ADD,0,CHARTS_SCALE_MILI ,8000,60}, \
	{"csservout"        ,CHARTS_MODE_ADD,0,CHARTS_SCALE_MILI ,8000,60}, \
	{"overhead_bytesr"  ,CHARTS_MODE_ADD,0,CHARTS_SCALE_MILI ,1000,60}, \
	{"overhead_bytesw"  ,CHARTS_MODE_ADD,0,CHARTS_SCALE_MILI ,1000,60}, \
	{"overhead_llopr"   ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"overhead_llopw"   ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"total_bytesr"     ,CHARTS_MODE_ADD,0,CHARTS_SCALE_MILI ,1000,60}, \
	{"total_bytesw"     ,CHARTS_MODE_ADD,0,CHARTS_SCALE_MILI ,1000,60}, \
	{"total_llopr"      ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"total_llopw"      ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"hlopr"            ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"hlopw"            ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"total_rtime"      ,CHARTS_MODE_ADD,0,CHARTS_SCALE_MICRO,   1,60}, \
	{"total_wtime"      ,CHARTS_MODE_ADD,0,CHARTS_SCALE_MICRO,   1,60}, \
	{"repl"             ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"create"           ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"delete"           ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"version"          ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"duplicate"        ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"truncate"         ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"duptrunc"         ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"test"             ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"chunkiojobs"      ,CHARTS_MODE_MAX,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"chunkopjobs"      ,CHARTS_MODE_MAX,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"memory"           ,CHARTS_MODE_MAX,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"gcpurge"          ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{"spacegrowth"      ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1,60}, \
	{"spacereclaimed"   ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1,60}, \
	{NULL               ,0              ,0,0                 ,   0, 0}  \
};

#define CALCDEFS { \
	CHARTS_DEFS_END \
};

/* c1_def , c2_def , c3_def , join mode , percent , scale , multiplier , divisor */
#define ESTATDEFS { \
	{CHARTS_DIRECT(CHARTS_UCPU)             ,CHARTS_DIRECT(CHARTS_SCPU)             ,CHARTS_NONE           ,CHARTS_MODE_ADD,1,CHARTS_SCALE_MICRO, 100,60}, \
	{CHARTS_DIRECT(CHARTS_CSSERVIN)         ,CHARTS_DIRECT(CHARTS_CSCONNIN)         ,CHARTS_NONE           ,CHARTS_MODE_ADD,0,CHARTS_SCALE_MILI ,8000,60}, \
	{CHARTS_DIRECT(CHARTS_CSSERVOUT)        ,CHARTS_DIRECT(CHARTS_CSCONNOUT)        ,CHARTS_NONE           ,CHARTS_MODE_ADD,0,CHARTS_SCALE_MILI ,8000,60}, \
	{CHARTS_DIRECT(CHARTS_TOTAL_BYTESR)     ,CHARTS_DIRECT(CHARTS_OVERHEAD_BYTESR)  ,CHARTS_NONE           ,CHARTS_MODE_ADD,0,CHARTS_SCALE_MILI ,1000,60}, \
	{CHARTS_DIRECT(CHARTS_TOTAL_BYTESW)     ,CHARTS_DIRECT(CHARTS_OVERHEAD_BYTESW)  ,CHARTS_NONE           ,CHARTS_MODE_ADD,0,CHARTS_SCALE_MILI ,1000,60}, \
	{CHARTS_DIRECT(CHARTS_TOTAL_LLOPR)      ,CHARTS_DIRECT(CHARTS_OVERHEAD_LLOPR)   ,CHARTS_NONE           ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{CHARTS_DIRECT(CHARTS_TOTAL_LLOPW)      ,CHARTS_DIRECT(CHARTS_OVERHEAD_LLOPW)   ,CHARTS_NONE           ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{CHARTS_DIRECT(CHARTS_CHUNKOPJOBS)      ,CHARTS_DIRECT(CHARTS_CHUNKIOJOBS)      ,CHARTS_NONE           ,CHARTS_MODE_ADD,0,CHARTS_SCALE_NONE ,   1, 1}, \
	{CHARTS_NONE                            ,CHARTS_NONE                            ,CHARTS_NONE           ,0              ,0,0                 ,   0, 0}  \
};

static const uint32_t calcdefs[] = CALCDEFS;
static const statdef statdefs[] = STATDEFS;
static const estatdef estatdefs[] = ESTATDEFS;

#ifdef CPU_USAGE
static struct rusage prev_rusage;
static bool prevRusageValid = false;
#endif

// NOLINTNEXTLINE(misc-use-anonymous-namespace)
#ifdef MEMORY_USAGE
static uint64_t GetMemUsage(const struct rusage &resUse) {
#ifdef __APPLE__
	return resUse.ru_maxrss;
#else
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
	return static_cast<uint64_t>(resUse.ru_maxrss) * UINT64_C(1024);
#endif
}
#endif

void chartsdata_refresh(void) {
	uint64_t data[CHARTS_NUMBER];
	uint64_t bytesIn, bytesOut, totalBytesRead, totalBytesWrite;
	uint32_t opsRead, opsWrite, totalOpsRead, totalOpsWrite, replications = 0;
	uint32_t opsCreate, opsDelete, opsUpdateVersion, opsDuplicate, opsTruncate;
	uint32_t opsDupTrunc, opsTest, opsGCPurge;
	uint32_t maxChunkServerJobsCount, maxMasterJobsCount;
	int64_t usedSpaceDelta;
#ifdef MEMORY_USAGE
	struct rusage ru{};
	bool haveRusage = false;
#endif

	for (auto i = 0; i < CHARTS_NUMBER; ++i) { data[i] = CHARTS_NODATA; }

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

#ifdef MEMORY_USAGE
	if (!haveRusage && getrusage(RUSAGE_SELF, &ru) == -1) {
		safs::log_error_code(errno, "could not get memory usage for chartsdata");
	} else {
		data[CHARTS_MEMORY] = GetMemUsage(ru);
	}
#endif

	masterconn_stats(&bytesIn, &bytesOut, &maxMasterJobsCount);
	data[CHARTS_MASTERIN] = bytesIn;
	data[CHARTS_MASTEROUT] = bytesOut;
	data[CHARTS_CHUNKOPJOBS] = maxMasterJobsCount;
	data[CHARTS_CSCONNIN] = 0;
	data[CHARTS_CSCONNOUT] = 0;

	networkStats(&bytesIn, &bytesOut, &opsRead, &opsWrite,
	             &maxChunkServerJobsCount);
	data[CHARTS_CSSERVIN] = bytesIn;
	data[CHARTS_CSSERVOUT] = bytesOut;
	data[CHARTS_CHUNKIOJOBS] = maxChunkServerJobsCount;
	data[CHARTS_HLOPR] = opsRead;
	data[CHARTS_HLOPW] = opsWrite;

	HddStats::stats(HddStats::statsReport(
	    &bytesIn, &bytesOut, &opsRead, &opsWrite, &totalBytesRead,
	    &totalBytesWrite, &totalOpsRead, &totalOpsWrite,
	    data + CHARTS_TOTAL_RTIME, data + CHARTS_TOTAL_WTIME));
	data[CHARTS_OVERHEAD_BYTESR] = bytesIn;
	data[CHARTS_OVERHEAD_BYTESW] = bytesOut;
	data[CHARTS_OVERHEAD_LLOPR] = opsRead;
	data[CHARTS_OVERHEAD_LLOPW] = opsWrite;
	data[CHARTS_TOTAL_BYTESR] = totalBytesRead;
	data[CHARTS_TOTAL_BYTESW] = totalBytesWrite;
	data[CHARTS_TOTAL_LLOPR] = totalOpsRead;
	data[CHARTS_TOTAL_LLOPW] = totalOpsWrite;
	data[CHARTS_REPL] = replications + gReplicator.getStats();

	HddStats::operationStats(&opsCreate, &opsDelete, &opsUpdateVersion, &opsDuplicate, &opsTruncate,
	                         &opsDupTrunc, &opsTest, &opsGCPurge);
	data[CHARTS_CREATE] = opsCreate;
	data[CHARTS_DELETE] = opsDelete;
	data[CHARTS_VERSION] = opsUpdateVersion;
	data[CHARTS_DUPLICATE] = opsDuplicate;
	data[CHARTS_TRUNCATE] = opsTruncate;
	data[CHARTS_DUPTRUNC] = opsDupTrunc;
	data[CHARTS_TEST] = opsTest;
	data[CHARTS_GC_PURGE] = opsGCPurge;

	HddStats::getSpaceDeltaStats(&usedSpaceDelta);

	data[CHARTS_SPACE_GROWTH] = std::max(usedSpaceDelta, int64_t(0));
	data[CHARTS_SPACE_RECLAIMED] = std::max(-usedSpaceDelta, int64_t(0));

	charts_add(data, eventloop_time() - SECONDS_IN_ONE_MINUTE);
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
#ifdef CPU_USAGE
	if (getrusage(RUSAGE_SELF, &prev_rusage) == -1) {
		safs::log_error_code(errno, "could not initialize cpu usage for chartsdata");
	} else {
		prevRusageValid = true;
	}
#endif

	eventloop_timeregister(TIMEMODE_RUN_LATE, SECONDS_IN_ONE_MINUTE, 0, chartsdata_refresh);
	eventloop_timeregister(TIMEMODE_RUN_LATE, SECONDS_IN_ONE_HOUR, 0, chartsdata_store);
	eventloop_destructregister(chartsdata_term);
	return charts_init(calcdefs, statdefs, estatdefs, CHARTS_FILENAME);
}
