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

#include "master/changelog.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>
#include <cstdio>

#include "common/cwrap.h"
#include "common/event_loop.h"
#include "common/exceptions.h"
#include "common/rotate_files.h"
#include "config/cfg.h"
#include "master/metadata_backend_common.h"
#include "slogger/slogger.h"

#if !defined(METARESTORE) && !defined(METALOGGER)
#include <fstream>

#include "common/setup.h"
#include "master/exceptions.h"
#include "master/filesystem.h"
#include "master/filesystem_metadata.h"
#include "master/restore.h"
#endif  // #if !defined(METARESTORE) && !defined(METALOGGER)

Signal<ChangelogEvent> &getChangelogSignal() {
	static Signal<ChangelogEvent> gChangelogSignal;
	return gChangelogSignal;
}

/// Base name of a changelog file.
/// Something like "changelog.sfs" or "changelog_ml.sfs"
static std::string gChangelogFilename;

/// Minimal acceptable value of BACK_LOGS config entry.
static uint32_t gMinBackLogsNumber = 0;

/// Maximal acceptable value of BACK_LOGS config entry.
static uint32_t gMaxBackLogsNumber = 50;

static uint32_t gBackLogsNumber;
static FILE *fd = nullptr;
static bool gFlush = true;

void changelog_rotate() {
	if (fd) {
		fclose(fd);
		fd=NULL;
	}
	if (gBackLogsNumber > 0) {
		rotateFiles(gChangelogFilename, gBackLogsNumber);
	} else {
		unlink(gChangelogFilename.c_str());
	}
}

void changelog(uint64_t version, const char* entry) {
	if (fd==NULL) {
		fd = fopen(gChangelogFilename.c_str(), "a");
		if (!fd) {
			safs_pretty_syslog(LOG_NOTICE, "lost metadata change %" PRIu64 ": %s", version, entry);
		}
	}

	if (fd) {
		fprintf(fd,"%" PRIu64 ": %s\n", version, entry);
		if (gFlush) {
			fflush(fd);
		}
	}
}

static void changelog_reload(void) {
	gBackLogsNumber = cfg_get_minmaxvalue<uint32_t>("BACK_LOGS", 50,
			gMinBackLogsNumber, gMaxBackLogsNumber);
}

void changelog_init(std::string changelogFilename,
		uint32_t minBackLogsNumber, uint32_t maxBackLogsNumber) {
	gChangelogFilename = std::move(changelogFilename);
	gMinBackLogsNumber = minBackLogsNumber;
	gMaxBackLogsNumber = maxBackLogsNumber;
	gBackLogsNumber = cfg_getuint32("BACK_LOGS", 50);
	if (gBackLogsNumber > gMaxBackLogsNumber) {
		throw InitializeException(cfg_filename() + ": BACK_LOGS value too big, "
				"maximum allowed is " + std::to_string(gMaxBackLogsNumber));
	}
	if (gBackLogsNumber < gMinBackLogsNumber) {
		throw InitializeException(cfg_filename() + ": BACK_LOGS value too low, "
				"minimum allowed is " + std::to_string(gMinBackLogsNumber));
	}
	eventloop_reloadregister(changelog_reload);
}

uint32_t changelog_get_back_logs_config_value() {
	return gBackLogsNumber;
}

void changelog_flush(void) {
	if (fd) {
		fflush(fd);
	}
}

void changelog_disable_flush(void) {
	gFlush = false;
}

void changelog_enable_flush(void) {
	gFlush = true;
	changelog_flush();
}

#ifndef METALOGGER

uint64_t changelogGetFirstLogVersion(const std::string &fname) {
	uint8_t buff[50];
	int32_t s, p;
	uint64_t fv;
	int fd;

	fd = open(fname.c_str(), O_RDONLY);
	if (fd < 0) { return 0; }
	s = read(fd, buff, 50);
	close(fd);
	if (s <= 0) { return 0; }
	fv = 0;
	p = 0;
	while (p < s && buff[p] >= '0' && buff[p] <= '9') {
		fv *= 10;
		fv += buff[p] - '0';
		p++;
	}
	if (p >= s || buff[p] != ':') { return 0; }
	return fv;
}

uint64_t changelogGetLastLogVersion(const std::string &fname) {
	struct stat st;

	FileDescriptor fd(open(fname.c_str(), O_RDONLY));
	if (fd.get() < 0) {
		throw FilesystemException("open " + fname + " failed: " + errorString(errno));
	}

	::fstat(fd.get(), &st);

	size_t fileSize = st.st_size;
	if (fileSize == 0) { return 0; }

	const char *fileContent =
	    (const char *)mmap(NULL, fileSize, PROT_READ, MAP_PRIVATE, fd.get(), 0);
	if (fileContent == MAP_FAILED) {
		throw FilesystemException("mmap(" + fname + ") failed: " + errorString(errno));
	}
	uint64_t lastLogVersion = 0;
	// first LF is (should be) the last byte of the file
	if (fileSize == 0 || fileContent[fileSize - 1] != '\n') {
		throw ParseException("truncated changelog " + fname +
		                     " (no LF at the end of the last line)");
	} else {
		size_t pos = fileSize - 1;
		while (pos > 0) {
			--pos;
			if (fileContent[pos] == '\n') { break; }
		}
		char *endPtr = NULL;
		lastLogVersion = strtoull(fileContent + pos, &endPtr, 10);
		if (*endPtr != ':') {
			throw ParseException("malformed changelog " + fname +
			                     " (expected colon after change number)");
		}
	}
	if (munmap((void *)fileContent, fileSize)) {
		safs_pretty_errlog(LOG_WARNING, "munmap(%s) failed", fname.c_str());
	}
	return lastLogVersion;
}

#else  // #ifndef METALOGGER

uint64_t findLastLogVersion() {
	struct stat st;
	uint8_t buff[32800];  // 32800 = 32768 + 32
	uint64_t size;
	uint32_t buffpos;
	uint64_t lastnewline;
	int fd;
	uint64_t lastlogversion = 0;

	if ((stat(kMetadataMlFilename, &st) < 0) || (st.st_size == 0) ||
	    ((st.st_mode & S_IFMT) != S_IFREG)) {
		return 0;
	}

	fd = open(kChangelogMlFilename, O_RDWR);
	if (fd < 0) { return 0; }

	::fstat(fd, &st);
	size = st.st_size;
	memset(buff, 0, 32);
	lastnewline = 0;

	while (size > 0 && size + 200000 > (uint64_t)(st.st_size)) {
		if (size > 32768) {
			memcpy(buff + 32768, buff, 32);
			size -= 32768;
			lseek(fd, size, SEEK_SET);
			if (read(fd, buff, 32768) != 32768) {
				lastlogversion = 0;
				close(fd);
				return lastlogversion;
			}
			buffpos = 32768;
		} else {
			memmove(buff + size, buff, 32);
			lseek(fd, 0, SEEK_SET);
			if (read(fd, buff, size) != (ssize_t)size) {
				lastlogversion = 0;
				close(fd);
				return lastlogversion;
			}
			buffpos = size;
			size = 0;
		}
		// size = position in file of first byte in buff
		// buffpos = position of last byte in buff to search
		while (buffpos > 0) {
			buffpos--;
			if (buff[buffpos] == '\n') {
				if (lastnewline == 0) {
					lastnewline = size + buffpos;
				} else {
					if (lastnewline + 1 !=
					    (uint64_t)(st.st_size)) {  // garbage at the end of file - truncate
						if (ftruncate(fd, lastnewline + 1) < 0) {
							lastlogversion = 0;
							close(fd);
							return lastlogversion;
						}
					}
					buffpos++;
					while (buffpos < 32800 && buff[buffpos] >= '0' && buff[buffpos] <= '9') {
						lastlogversion *= 10;
						lastlogversion += buff[buffpos] - '0';
						buffpos++;
					}
					if (buffpos == 32800 || buff[buffpos] != ':') { lastlogversion = 0; }
					close(fd);
					return lastlogversion;
				}
			}
		}
	}

	close(fd);

	return lastlogversion;
}

#endif  // #ifndef METALOGGER

#if !defined(METARESTORE) && !defined(METALOGGER)

void load_changelogs() {
	/*
	 * We need to load 3 changelog files in extreme case.
	 * If we are being run as Shadow we need to download two
	 * changelog files:
	 * 1 - current changelog => "changelog.sfs.1"
	 * 2 - previous changelog in case Shadow connects during metadata dump,
	 *     that is "changelog.sfs.2"
	 * Beside this we received changelog lines that we stored in
	 * yet another changelog file => "changelog.sfs"
	 *
	 * If we are master we only really care for:
	 * "changelog.sfs.1" and "changelog.sfs" files.
	 */
	static const std::string changelogs[]{
	    std::string(kChangelogFilename) + ".2",
	    std::string(kChangelogFilename) + ".1", kChangelogFilename};
	restore_setverblevel(gVerbosity);
	bool oldExists = false;
	try {
		for (const std::string &s : changelogs) {
			std::string fullFileName = fs::getCurrentWorkingDirectoryNoThrow() + "/" + s;
			if (fs::exists(s)) {
				oldExists = true;
				uint64_t first = changelogGetFirstLogVersion(s);
				uint64_t last = changelogGetLastLogVersion(s);
				if (last >= first) {
					if (last >= fs_getversion()) {
						load_changelog(s);
					}
				} else {
					throw InitializeException(
					    "changelog " + fullFileName +
					    " inconsistent, "
					    "use sfsmetarestore to recover the filesystem; "
					    "current fs version: " +
					    std::to_string(fs_getversion()) +
					    ", first change in the file: " + std::to_string(first));
				}
			} else if (oldExists && s != kChangelogFilename) {
				safs_pretty_syslog(LOG_WARNING, "changelog `%s' missing",
				                   fullFileName.c_str());
			}
		}
	} catch (const FilesystemException &ex) {
		throw FilesystemException("error loading changelogs: " + ex.message());
	}
}

void load_changelog(const std::string &path) {
	std::string fullFileName = fs::getCurrentWorkingDirectoryNoThrow() + "/" + path;
	std::ifstream changelog(path);
	std::string line;
	size_t end = 0;
	sassert(gMetadata->metadataVersion > 0);

	uint64_t first = 0;
	uint64_t id = 0;
	uint64_t skippedEntries = 0;
	uint64_t appliedEntries = 0;
	while (std::getline(changelog, line).good()) {
		id = std::stoull(line, &end);
		if (id < fs_getversion()) {
			++skippedEntries;
			continue;
		} else if (!first) {
			first = id;
		}
		++appliedEntries;
		uint8_t status =
		    restore(path.c_str(), id, line.c_str() + end, RestoreRigor::kIgnoreParseErrors);
		if (status != SAUNAFS_STATUS_OK) {
			throw MetadataConsistencyException("can't apply changelog " + fullFileName, status);
		}
	}
	if (appliedEntries > 0) {
		safs_pretty_syslog_attempt(LOG_NOTICE,
		                           "%s: %" PRIu64 " changes applied (%" PRIu64 " to %" PRIu64
		                           "), %" PRIu64 " skipped",
		                           fullFileName.c_str(), appliedEntries, first, id, skippedEntries);
	} else if (skippedEntries > 0) {
		safs_pretty_syslog_attempt(LOG_NOTICE, "%s: skipped all %" PRIu64 " entries",
		                           fullFileName.c_str(), skippedEntries);
	} else {
		safs_pretty_syslog_attempt(LOG_NOTICE, "%s: file empty (ignored)", fullFileName.c_str());
	}
}

#endif  // !defined(METARESTORE) && !defined(METALOGGER)
