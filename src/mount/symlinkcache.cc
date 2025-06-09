/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2019 Skytechnology sp. z o.o.
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
#include "mount/symlinkcache.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <ctime>
#include <limits>
#include <mutex>
#include <vector>

#include "common/type_defs.h"
#include "protocol/SFSCommunication.h"
#include "mount/stats.h"

#define HASH_FUNCTIONS 4
#define HASH_BUCKET_SIZE 16
#define HASH_BUCKETS 6257

static uint32_t kCacheTimeInSeconds;

// entries in cache = HASH_FUNCTIONS*HASH_BUCKET_SIZE*HASH_BUCKETS
// 4 * 16 * 6257 = 400448
// Symlink cache capacity can be easily changed by altering HASH_BUCKETS value.
// Any number should work but it is better to use prime numbers here.

typedef struct _hashbucket {
	inode_t inode[HASH_BUCKET_SIZE];
	uint32_t time[HASH_BUCKET_SIZE];
	uint8_t* path[HASH_BUCKET_SIZE];
} hashbucket;

static hashbucket *symlinkhash = NULL;
static std::mutex slcachelock;

enum {
	INSERTS = 0,
	SEARCH_HITS,
	SEARCH_MISSES,
	LINKS,
	STATNODES
};

static std::vector<uint64_t *> statsptr(STATNODES);

static inline void symlink_cache_statsptr_init(void) {
	statsnode *s;
	s = stats_get_subnode(NULL,"symlink_cache",0);
	statsptr[INSERTS] = stats_get_counterptr(stats_get_subnode(s,"inserts",0));
	statsptr[SEARCH_HITS] = stats_get_counterptr(stats_get_subnode(s,"search_hits",0));
	statsptr[SEARCH_MISSES] = stats_get_counterptr(stats_get_subnode(s,"search_misses",0));
	statsptr[LINKS] = stats_get_counterptr(stats_get_subnode(s,"#links",1));
}

void symlink_cache_insert(inode_t inode,const uint8_t *path) {
	uint32_t primes[HASH_FUNCTIONS] = {1072573589U,3465827623U,2848548977U,748191707U};
	hashbucket *hb,*fhb;
	uint8_t h,i,fi;
	uint32_t now;
	uint32_t mints;

	now = time(NULL);
	mints = std::numeric_limits<uint32_t>::max();
	fi = 0;
	fhb = NULL;

	stats_inc(INSERTS, statsptr);
	std::lock_guard lock(slcachelock);
	for (h=0 ; h<HASH_FUNCTIONS ; h++) {
		hb = symlinkhash + ((inode*primes[h])%HASH_BUCKETS);
		for (i=0 ; i<HASH_BUCKET_SIZE ; i++) {
			if (hb->inode[i]==inode) {
				if (hb->path[i]) {
					free(hb->path[i]);
				}
				hb->path[i]=(uint8_t*)strdup((const char *)path);
				hb->time[i]=now;
				return;
			}
			if (hb->time[i]<mints) {
				fhb = hb;
				fi = i;
				mints = hb->time[i];
			}
		}
	}
	if (fhb) {      // just sanity check
		if (fhb->time[fi]==0) {
			stats_inc(LINKS, statsptr);
		}
		if (fhb->path[fi]) {
			free(fhb->path[fi]);
		}
		fhb->inode[fi]=inode;
		fhb->path[fi]=(uint8_t*)strdup((const char *)path);
		fhb->time[fi]=now;
	}
}

int symlink_cache_search(inode_t inode,const uint8_t **path) {
	uint32_t primes[HASH_FUNCTIONS] = {1072573589U,3465827623U,2848548977U,748191707U};
	hashbucket *hb;
	uint8_t h,i;
	uint32_t now;

	now = time(NULL);

	std::unique_lock lock(slcachelock);
	for (h=0 ; h<HASH_FUNCTIONS ; h++) {
		hb = symlinkhash + ((inode*primes[h])%HASH_BUCKETS);
		for (i=0 ; i<HASH_BUCKET_SIZE ; i++) {
			if (hb->inode[i]==inode) {
				if (hb->time[i] + kCacheTimeInSeconds < now) {
					if (hb->path[i]) {
						free(hb->path[i]);
						hb->path[i]=NULL;
					}
					hb->time[i]=0;
					hb->inode[i]=0;
					lock.unlock();
					stats_dec(LINKS, statsptr);
					stats_inc(SEARCH_MISSES, statsptr);
					return 0;
				}
				*path = hb->path[i];
				lock.unlock();
				stats_inc(SEARCH_HITS, statsptr);
				return 1;
			}
		}
	}
	lock.unlock();
	stats_inc(SEARCH_MISSES, statsptr);
	return 0;
}

void symlink_cache_init(uint32_t cache_time) {
	symlinkhash = (hashbucket*) malloc(sizeof(hashbucket)*HASH_BUCKETS);
	memset(symlinkhash,0,sizeof(hashbucket)*HASH_BUCKETS);
	kCacheTimeInSeconds = cache_time;
	symlink_cache_statsptr_init();
}

void symlink_cache_term(void) {
	hashbucket *hb;
	uint8_t i;
	uint32_t hi;

	std::lock_guard lock(slcachelock);
	for (hi=0 ; hi<HASH_BUCKETS ; hi++) {
		hb = symlinkhash + hi;
		for (i=0 ; i<HASH_BUCKET_SIZE ; i++) {
			if (hb->path[i]) {
				free(hb->path[i]);
			}
		}
	}
	free(symlinkhash);
}
