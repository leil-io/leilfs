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
#include <sys/resource.h>
#include <sys/time.h>
#include <syslog.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>
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
#include "slogger/slogger.h"

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

#define CHARTS_NUMBER 31

const unsigned long kLinuxMaxrssSize = 1024UL;

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

static const uint32_t calcdefs[]=CALCDEFS
static const statdef statdefs[]=STATDEFS
static const estatdef estatdefs[]=ESTATDEFS

static struct itimerval it_set;

// Signal handler that prevents process termination from timer signals
static void timerSignalHandler(int /*signal*/) {
	// Reset both timers to prevent future signals from killing the process
	setitimer(ITIMER_PROF, &it_set, nullptr);
	setitimer(ITIMER_VIRTUAL, &it_set, nullptr);
}

inline uint32_t toMicroSeconds(struct itimerval &itimer) {
    return itimer.it_value.tv_sec * 1000000 + itimer.it_value.tv_usec;
}

// NOLINTNEXTLINE(misc-use-anonymous-namespace)
static uint64_t GetMemUsage() {
	struct rusage resUse{};
	int err = getrusage(RUSAGE_SELF, &resUse);
	if (err != -1) {
#ifdef __APPLE__
		return resUse.ru_maxrss;
#else
		// NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
		return resUse.ru_maxrss * kLinuxMaxrssSize;
#endif
	} else {
		safs::log_error_code(errno, "could not get memory usage for chartsdata");
		return 0;
	}
}

void chartsdata_refresh(void) {
	uint64_t data[CHARTS_NUMBER];
	uint64_t bytesIn, bytesOut, totalBytesRead, totalBytesWrite;
	uint32_t opsRead, opsWrite, totalOpsRead, totalOpsWrite, replications = 0;
	uint32_t opsCreate, opsDelete, opsUpdateVersion, opsDuplicate, opsTruncate;
	uint32_t opsDupTrunc, opsTest;
	uint32_t maxChunkServerJobsCount, maxMasterJobsCount;

	// Timer runs only when the process is executing.
	struct itimerval userTime;

	// Timer runs when the process is executing and when
	// the system is executing on behalf of the process.
	struct itimerval procTime;

	uint32_t userTimeMicroSeconds, procTimeMicroSeconds;

	for (auto i = 0; i < CHARTS_NUMBER; ++i) {
		data[i] = 0;
	}

	setitimer(ITIMER_VIRTUAL, &it_set, &userTime); // user time
	setitimer(ITIMER_PROF, &it_set, &procTime);    // user time + system time

	// on fucken linux timers can go backward !!!
	if (userTime.it_value.tv_sec <= 999) {
		userTime.it_value.tv_sec = 999 - userTime.it_value.tv_sec;
		userTime.it_value.tv_usec = 999999 - userTime.it_value.tv_usec;
	} else {
		userTime.it_value.tv_sec = 0;
		userTime.it_value.tv_usec = 0;
	}

	// as abowe - who the hell has invented this stupid os !!!
	if (procTime.it_value.tv_sec <= 999) {
		procTime.it_value.tv_sec = 999 - procTime.it_value.tv_sec;
		procTime.it_value.tv_usec = 999999 - procTime.it_value.tv_usec;
	} else {
		procTime.it_value.tv_sec = 0;
		procTime.it_value.tv_usec = 0;
	}

	userTimeMicroSeconds = toMicroSeconds(userTime);
	procTimeMicroSeconds = toMicroSeconds(procTime);

	if (procTimeMicroSeconds > userTimeMicroSeconds) {
		procTimeMicroSeconds -= userTimeMicroSeconds;
	} else {
		procTimeMicroSeconds = 0;
	}

	data[CHARTS_MEMORY] = GetMemUsage();

	data[CHARTS_UCPU] = userTimeMicroSeconds;
	data[CHARTS_SCPU] = procTimeMicroSeconds;

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

	HddStats::operationStats(&opsCreate, &opsDelete, &opsUpdateVersion,
	                         &opsDuplicate, &opsTruncate, &opsDupTrunc,
	                         &opsTest);
	data[CHARTS_CREATE] = opsCreate;
	data[CHARTS_DELETE] = opsDelete;
	data[CHARTS_VERSION] = opsUpdateVersion;
	data[CHARTS_DUPLICATE] = opsDuplicate;
	data[CHARTS_TRUNCATE] = opsTruncate;
	data[CHARTS_DUPTRUNC] = opsDupTrunc;
	data[CHARTS_TEST] = opsTest;

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

int chartsdata_init(void) {
	struct itimerval userTime, procTime;

	it_set.it_interval.tv_sec = 0;
	it_set.it_interval.tv_usec = 0;
	it_set.it_value.tv_sec = 999;
	it_set.it_value.tv_usec = 999999;

	// Install timer signal handlers for SIGVTALRM and SIGPROF
	if (initializeTimerSignalHandlers(timerSignalHandler) != 0) {
		safs::log_err("{} failed to initialize timer signal handlers", __func__);
		return -1;
	}

	setitimer(ITIMER_VIRTUAL, &it_set, &userTime); // user time
	setitimer(ITIMER_PROF, &it_set, &procTime);    // user time + system time

	eventloop_timeregister(TIMEMODE_RUN_LATE, SECONDS_IN_ONE_MINUTE, 0,
	                       chartsdata_refresh);
	eventloop_timeregister(TIMEMODE_RUN_LATE, SECONDS_IN_ONE_HOUR, 0,
	                       chartsdata_store);
	eventloop_destructregister(chartsdata_term);
	return charts_init(calcdefs, statdefs, estatdefs, CHARTS_FILENAME);
}
