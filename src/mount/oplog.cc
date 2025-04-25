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
#include "mount/oplog.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#ifdef _WIN32
#define localtime_r(T, Tm) localtime_s(Tm, T)
#endif

#include "common/user_groups.h"

#define OPBUFFSIZE 0x1000000
#define LINELENG 1000
#define MAXHISTORYSIZE 0xF00000
#define TIMEDATAPREFFIXSIZE 34

typedef struct _fhentry {
	unsigned long fh;
	uint64_t readpos;
	uint32_t refcount;
	struct _fhentry *next;
} fhentry;

static unsigned long nextfh=1;
static fhentry *fhhead=NULL;

static uint8_t opbuff[OPBUFFSIZE];
static uint64_t writepos=0;
static uint8_t waiting=0;
static std::mutex opbufflock;
static std::condition_variable noData;

static time_t gConvTmHour = std::numeric_limits<time_t>::max(); // enforce update on first read
static struct tm gConvTm;
static std::mutex timelock;
#ifdef _WIN32
static int debug_mode_;

void set_debug_mode(int debug_mode) { debug_mode_ = debug_mode; }
#endif

static inline void oplog_put(uint8_t *buff,uint32_t leng) {
#ifdef _WIN32
	if (debug_mode_) {
		char str_buff[LINELENG];
		for (uint32_t i = TIMEDATAPREFFIXSIZE; i < leng - 1; i++) {
			str_buff[i - TIMEDATAPREFFIXSIZE] = buff[i];
		}
		str_buff[leng - TIMEDATAPREFFIXSIZE - 1] = '\0';
		safs::log_debug("{}", str_buff);
	}
#endif
	uint32_t bpos;
	if (leng>OPBUFFSIZE) {  // just in case
		buff+=leng-OPBUFFSIZE;
		leng=OPBUFFSIZE;
	}
	std::lock_guard lock(opbufflock);
	bpos = writepos%OPBUFFSIZE;
	writepos+=leng;
	if (bpos+leng>OPBUFFSIZE) {
		memcpy(opbuff+bpos,buff,OPBUFFSIZE-bpos);
		buff+=OPBUFFSIZE-bpos;
		leng-=OPBUFFSIZE-bpos;
		bpos = 0;
	}
	memcpy(opbuff+bpos,buff,leng);
	if (waiting) {
		noData.notify_all();
		waiting=0;
	}
}

static void get_time(timeval &tv, tm &ltime) {
	gettimeofday(&tv, nullptr);
	static constexpr time_t secs_per_hour = 60 * 60;
	time_t hour = tv.tv_sec / secs_per_hour;
	unsigned secs_this_hour = tv.tv_sec % secs_per_hour;

	std::unique_lock lock(timelock);
	if (hour != gConvTmHour) {
		gConvTmHour = hour;
		time_t convts = hour * secs_per_hour;
		localtime_r(&convts, &gConvTm);
	}
	ltime = gConvTm;
	lock.unlock();

	assert(ltime.tm_sec == 0);
	assert(ltime.tm_min == 0);

	ltime.tm_sec = secs_this_hour % 60;
	ltime.tm_min = secs_this_hour / 60;
}

void oplog_printf(const struct SaunaClient::Context &ctx,const char *format,...) {
	struct timeval tv;
	struct tm ltime;
	va_list ap;
	int r, leng = 0;
	char buff[LINELENG];

	auto groupId = ctx.gid;

	if (user_groups::isGroupCacheId(groupId)) {
		groupId = ctx.gids.at(user_groups::kPrimaryGroupPosition);
	}

	get_time(tv, ltime);
	// Update TIMEDATAPREFFIXSIZE constant if changing the time format
	r  = snprintf(buff, LINELENG, "%llu %02u.%02u %02u:%02u:%02u.%06u: uid:%u gid:%u pid:%u cmd:",
		(unsigned long long)tv.tv_sec, ltime.tm_mon + 1, ltime.tm_mday, ltime.tm_hour, ltime.tm_min, ltime.tm_sec, (unsigned)tv.tv_usec,
		(unsigned)ctx.uid, groupId, (unsigned)ctx.pid);
	if (r < 0) {
		return;
	}
	leng = std::min(LINELENG - 1, r);

	va_start(ap, format);
	r = vsnprintf(buff + leng, LINELENG - leng, format, ap);
	va_end(ap);
	if (r < 0) {
		return;
	}
	leng += r;

	leng = std::min(LINELENG - 1, leng);
	buff[leng++] = '\n';
	oplog_put((uint8_t*)buff, leng);
}

void oplog_printf(const char *format, ...) {
	struct timeval tv;
	struct tm ltime;
	va_list ap;
	int r, leng = 0;
	char buff[LINELENG];

	get_time(tv, ltime);
	// Update TIMEDATAPREFFIXSIZE constant if changing the time format
	r = snprintf(buff, LINELENG, "%llu %02u.%02u %02u:%02u:%02u.%06u: cmd:",
		(unsigned long long)tv.tv_sec, ltime.tm_mon + 1, ltime.tm_mday, ltime.tm_hour, ltime.tm_min, ltime.tm_sec, (unsigned)tv.tv_usec);
	if (r < 0) {
		return;
	}
	leng = std::min(LINELENG - 1, r);

	va_start(ap, format);
	r = vsnprintf(buff + leng, LINELENG - leng, format, ap);
	va_end(ap);
	if (r < 0) {
		return;
	}
	leng += r;

	leng = std::min(LINELENG - 1, leng);
	buff[leng++] = '\n';
	oplog_put((uint8_t*)buff, leng);
}

unsigned long oplog_newhandle(int hflag) {
	fhentry *fhptr;
	uint32_t bpos;

	std::lock_guard lock(opbufflock);
	fhptr = (fhentry*) malloc(sizeof(fhentry));
	fhptr->fh = nextfh++;
	fhptr->refcount = 1;
	if (hflag) {
		if (writepos<MAXHISTORYSIZE) {
			fhptr->readpos = 0;
		} else {
			fhptr->readpos = writepos - MAXHISTORYSIZE;
			bpos = fhptr->readpos%OPBUFFSIZE;
			while (fhptr->readpos < writepos) {
				if (opbuff[bpos]=='\n') {
					break;
				}
				bpos++;
				bpos%=OPBUFFSIZE;
				fhptr->readpos++;
			}
			if (fhptr->readpos<writepos) {
				fhptr->readpos++;
			}
		}
	} else {
		fhptr->readpos = writepos;
	}
	fhptr->next = fhhead;
	fhhead = fhptr;
	return fhptr->fh;
}

void oplog_releasedata(unsigned long fh) {
	fhentry **fhpptr, *fhptr;
	fhpptr = &fhhead;
	while ((fhptr = *fhpptr)) {
		if (fhptr->fh == fh) {
			fhptr->refcount--;
			if (!fhptr->refcount) {
				*fhpptr = fhptr->next;
				free(fhptr);
			} else {
				fhpptr = &(fhptr->next);
			}
		} else {
			fhpptr = &(fhptr->next);
		}
	}
}

void oplog_releasehandle(unsigned long fh) {
	std::lock_guard lock(opbufflock);
	oplog_releasedata(fh);
}

void oplog_getdata(unsigned long fh,uint8_t **buff,uint32_t *leng,uint32_t maxleng) {
	fhentry *fhptr;
	uint32_t bpos;

	std::unique_lock lock(opbufflock);
	for (fhptr=fhhead ; fhptr && fhptr->fh != fh ; fhptr=fhptr->next) {
	}
	if (fhptr==NULL) {
		*buff = NULL;
		*leng = 0;
		return;
	}
	fhptr->refcount++;
	while (fhptr->readpos>=writepos) {
		waiting=1;
		if (noData.wait_for(lock, std::chrono::seconds(1)) == std::cv_status::timeout) {
			*buff = (uint8_t*)"#\n";
			*leng = 2;
			return;
		}
	}
	bpos = fhptr->readpos%OPBUFFSIZE;
	*leng = (writepos-(fhptr->readpos));
	*buff = opbuff+bpos;
	if ((*leng)>(OPBUFFSIZE-bpos)) {
		(*leng) = (OPBUFFSIZE-bpos);
	}
	if ((*leng)>maxleng) {
		(*leng) = maxleng;
	}
	fhptr->readpos+=(*leng);

	oplog_releasedata(fh);
}
