/*

   Copyright 2016 Skytechnology sp. z o.o.
   Copyright 2023 Leil Storage OÜ

   This file is part of SaunaFS.

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

#include <unistd.h>

#include "common/event_loop.h"
#include "config/cfg.h"
#include "master/hstring_memstorage.h"
#include "master/metadata_backend_common.h"
#ifdef SAUNAFS_HAVE_DB
  #include "master/hstring_bdbstorage.h"
#endif

static int gUseBDBStorage;
static std::string gBDBStoragePath;
static uint64_t gBDBStorageCacheSize;

static std::string resolveDataPath() {
	std::string path = cfg_getstring("DATA_PATH", DATA_PATH);
	if (path == DATA_PATH && !hasKnownMetadata(DATA_PATH) &&
	    hasKnownMetadata(DATA_PATH_LEGACY)) {
		safs_pretty_syslog(LOG_WARNING,
		    "using legacy data directory %s because default directory %s has no metadata",
		    DATA_PATH_LEGACY, DATA_PATH);
		path = DATA_PATH_LEGACY;
	}
	return path;
}

void hstorage_reload() {
	int use_bdb = cfg_getuint8("USE_BDB_FOR_NAME_STORAGE", 0);
	std::string bdb_path = resolveDataPath();
	uint64_t cache_size = cfg_getuint32("BDB_NAME_STORAGE_CACHE_SIZE", 10);

	if (use_bdb != gUseBDBStorage) {
		safs_pretty_syslog(
		    LOG_ERR, "Changing USE_BDB_FOR_NAME_STORAGE requires restart.");
	}

	if (gUseBDBStorage && bdb_path != gBDBStoragePath) {
		safs_pretty_syslog(LOG_ERR,
		                   "Changing DATA_PATH with enabled BDB name storage requires restart.");
	}

	if (cache_size != gBDBStorageCacheSize) {
		safs_pretty_syslog(LOG_ERR, "Changing BDB_NAME_STORAGE_CACHE_SIZE requires restart.");
	}
}

void hstorage_term(void) {
	hstorage::Storage::reset();
}

int hstorage_init() {
	gUseBDBStorage = cfg_getuint8("USE_BDB_FOR_NAME_STORAGE", 0);
	gBDBStoragePath = resolveDataPath();
	gBDBStorageCacheSize = cfg_getuint32("BDB_NAME_STORAGE_CACHE_SIZE", 10);

	if (gUseBDBStorage) {
#ifdef SAUNAFS_HAVE_DB
		hstorage::Storage::reset(new hstorage::BDBStorage(gBDBStoragePath + "/name_storage.db",
		                                                  gBDBStorageCacheSize * 1024 * 1024, 1));
#else
		safs_pretty_syslog(LOG_ERR, "Berkeley DB was not enabled during compilation. Falling back to default name storage.");
		hstorage::Storage::reset(new hstorage::MemStorage());
#endif
	} else {
		hstorage::Storage::reset(new hstorage::MemStorage());
	}

	eventloop_reloadregister(hstorage_reload);
	eventloop_destructregister(hstorage_term);

	return 0;
}
