/*
   Copyright 2005-2017 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2017 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include "master/matoclserv_sessions.h"

#include "common/cwrap.h"
#include "common/datapack.h"
#include "common/event_loop.h"
#include "common/massert.h"
#include "common/type_defs.h"
#include "config/cfg.h"
#include "master/filesystem_operations_interface.h"
#include "master/metadata_backend_common.h"
#include "protocol/SFSCommunication.h"
#include "slogger/slogger.h"

Session *matoclserv_new_session(uint8_t newSession, uint8_t noNewId) {
	auto sessionPtr = std::make_unique<Session>();
	passert(sessionPtr.get());

	auto newSessionIdNotNeeded = (newSession == 0 && noNewId);
	sessionPtr->sessionId = (newSessionIdNotNeeded) ? 0 : gFSOperations->fs_newsessionid();
	sessionPtr->newSession = newSession;
	sessionPtr->connections = 1;
	gSessionsVector.push_back(std::move(sessionPtr));
	return gSessionsVector.back().get();
}

Session* matoclserv_find_session(uint32_t sessionId) {
	if (sessionId == 0) { return nullptr; }

	for (const auto& sessionPtr : gSessionsVector) {
		if (sessionPtr->sessionId == sessionId) {
			if (sessionPtr->newSession >= 2) {
				sessionPtr->newSession -= 2;
			}
			sessionPtr->connections++;
			sessionPtr->disconnectedTimestamp = 0;
			return sessionPtr.get();
		}
	}
	return nullptr;
}

void matoclserv_close_session(uint32_t sessionId) {
	if (sessionId == 0) { return; }

	for (const auto& sessionPtr : gSessionsVector) {
		if (sessionPtr->sessionId == sessionId) {
			if (sessionPtr->connections == 1 && sessionPtr->newSession < 2) {
				sessionPtr->newSession += 2;
			}
		}
	}
}

void matoclserv_store_sessions() {
	uint32_t sessionInfoLength;
	constexpr uint32_t kSessionSerializedSize =
	    sizeof(Session::sessionId) + sizeof(sessionInfoLength) + sizeof(Session::peerIpAddress) +
	    sizeof(Session::rootInode) + sizeof(Session::flags) + sizeof(Session::minGoal) +
	    sizeof(Session::maxGoal) + sizeof(Session::minTrashTime) + sizeof(Session::maxTrashTime) +
	    sizeof(Session::rootUid) + sizeof(Session::rootGid) + sizeof(Session::mapAllUid) +
	    sizeof(Session::mapAllGid);
	constexpr uint32_t kBufferSize = kSessionSerializedSize + (SESSION_STATS * 8);
	std::vector<uint8_t> fsesrecord(kBufferSize); // 4+4+4+4+1+1+1+4+4+4+4+4+4+SESSION_STATS*4+SESSION_STATS*4

	FILE *fd = fopen(kSessionsTmpFilename, "w");
	if (fd == nullptr) {
		safs_silent_errlog(LOG_WARNING,"can't store sessions, open error");
		return;
	}

	memcpy(fsesrecord.data(), SFSSIGNATURE "S \001\006\004", 8);
	uint8_t *ptr = fsesrecord.data() + 8;
	put16bit(&ptr,SESSION_STATS);

	if (fwrite(fsesrecord.data(), 10, 1, fd) != 1) {
		safs_pretty_syslog(LOG_WARNING,"can't store sessions, fwrite error");
		fclose(fd);
		return;
	}

	for (const auto& sessionPtr : gSessionsVector) {
		if (sessionPtr->newSession == 1) {
			ptr = fsesrecord.data();
			sessionInfoLength = sessionPtr->info.size();

			put32bit(&ptr, sessionPtr->sessionId);
			put32bit(&ptr, sessionInfoLength);
			put32bit(&ptr, sessionPtr->peerIpAddress);
			putINode(&ptr, sessionPtr->rootInode);
			put8bit(&ptr, sessionPtr->flags);
			put8bit(&ptr, sessionPtr->minGoal);
			put8bit(&ptr, sessionPtr->maxGoal);
			put32bit(&ptr, sessionPtr->minTrashTime);
			put32bit(&ptr, sessionPtr->maxTrashTime);
			put32bit(&ptr, sessionPtr->rootUid);
			put32bit(&ptr, sessionPtr->rootGid);
			put32bit(&ptr, sessionPtr->mapAllUid);
			put32bit(&ptr, sessionPtr->mapAllGid);

			for (auto i = 0; i < SESSION_STATS; i++) {
				put32bit(&ptr, sessionPtr->currHourOperationsStats[i]);
			}

			for (auto i = 0; i < SESSION_STATS; i++) {
				put32bit(&ptr, sessionPtr->prevHourOperationsStats[i]);
			}

			if (fwrite(fsesrecord.data(), kBufferSize, 1, fd) != 1) {
				safs::log_warn("can't store sessions, fwrite error");
				fclose(fd);
				return;
			}

			if (sessionInfoLength > 0) {
				if (fwrite(sessionPtr->info.data(), sessionInfoLength, 1, fd) != 1) {
					safs::log_warn("can't store sessions, fwrite error");
					fclose(fd);
					return;
				}
			}
		}
	}

	if (fclose(fd) != 0) {
		safs_silent_errlog(LOG_WARNING,"can't store sessions, fclose error");
		return;
	}

	if (rename(kSessionsTmpFilename, kSessionsFilename) < 0) {
		safs_silent_errlog(LOG_WARNING, "can't store sessions, rename error");
	}
}

#define MFSSIGNATURE "MFS"

int matoclserv_load_sessions() {
	uint32_t sessionInfoLength;
	uint8_t headerBuffer[8];  // for signature and version. e.g. "SFS" " S 1.5"
	std::vector<uint8_t> sessionBuffer;
	const uint8_t *ptr;
	uint8_t mapAllData;
	uint8_t goalTrashData;
	uint32_t statsInFile;
	int bytesRead;

	FILE *fd = fopen(kSessionsFilename, "r");

	if (fd == nullptr) {
		safs_silent_errlog(LOG_WARNING, "can't load sessions, fopen error");
		if (errno == ENOENT) {  // it's ok if file does not exist
			return 0;
		}

		return -1;
	}

	const size_t kSessionsHeaderSize = strlen(SFSSIGNATURE) + 5;

	if (fread(headerBuffer, kSessionsHeaderSize, 1, fd) != 1) {
		safs::log_warn("can't load sessions, fread error");
		fclose(fd);
		return -1;
	}

	// Guillex: Only "S \001\006\004" (last option) is expected
	if (memcmp(headerBuffer, SFSSIGNATURE "S 1.5", kSessionsHeaderSize) == 0 ||
	    memcmp(headerBuffer, MFSSIGNATURE "S 1.5", kSessionsHeaderSize) == 0) {
		mapAllData = 0;
		goalTrashData = 0;
		statsInFile = 16;
	} else if (memcmp(headerBuffer, SFSSIGNATURE "S \001\006\001", kSessionsHeaderSize) == 0 ||
	           memcmp(headerBuffer, MFSSIGNATURE "S \001\006\001", kSessionsHeaderSize) == 0) {
		mapAllData = 1;
		goalTrashData = 0;
		statsInFile = 16;
	} else if (memcmp(headerBuffer, SFSSIGNATURE "S \001\006\002", kSessionsHeaderSize) == 0 ||
	           memcmp(headerBuffer, MFSSIGNATURE "S \001\006\002", kSessionsHeaderSize) == 0) {
		mapAllData = 1;
		goalTrashData = 0;
		statsInFile = 21;
	} else if (memcmp(headerBuffer, SFSSIGNATURE "S \001\006\003", kSessionsHeaderSize) == 0 ||
	           memcmp(headerBuffer, MFSSIGNATURE "S \001\006\003", kSessionsHeaderSize) == 0) {
		mapAllData = 1;
		goalTrashData = 0;
		if (fread(headerBuffer, 2, 1, fd) != 1) {
			safs::log_warn("can't load sessions, fread error");
			fclose(fd);
			return -1;
		}
		ptr = headerBuffer;
		statsInFile = get16bit(&ptr);
	} else if (memcmp(headerBuffer, SFSSIGNATURE "S \001\006\004", kSessionsHeaderSize) == 0 ||
	           memcmp(headerBuffer, MFSSIGNATURE "S \001\006\004", kSessionsHeaderSize) == 0) {
		mapAllData = 1;
		goalTrashData = 1;
		if (fread(headerBuffer, sizeof(uint16_t), 1, fd) != 1) {
			safs::log_warn("can't load sessions, fread error");
			fclose(fd);
			return -1;
		}
		ptr = headerBuffer;
		statsInFile = get16bit(&ptr);
	} else {
		safs::log_warn("can't load sessions, bad header");
		fclose(fd);
		return -1;
	}

	// Compile time constants
	constexpr uint8_t kStatEntrySize =
	    sizeof(std::remove_extent<decltype(Session::currHourOperationsStats)>::type::value_type) +
	    sizeof(std::remove_extent<decltype(Session::prevHourOperationsStats)>::type::value_type);

	constexpr uint32_t kCommonSize = sizeof(Session::sessionId) + sizeof(sessionInfoLength) +
	                                 sizeof(Session::peerIpAddress) + sizeof(Session::rootInode) +
	                                 sizeof(Session::flags) + sizeof(Session::rootUid) +
	                                 sizeof(Session::rootGid);
	constexpr uint32_t kExtraSizeWithMapAll =
	    sizeof(Session::mapAllUid) + sizeof(Session::mapAllGid);
	constexpr uint32_t kExtraSizeWithGoalTrash =
	    sizeof(Session::minGoal) + sizeof(Session::maxGoal) + sizeof(Session::minTrashTime) +
	    sizeof(Session::maxTrashTime);

	// statsInFile is unknown at compile time, we need to use a runtime constant
	const uint32_t kStatsSize = statsInFile * kStatEntrySize;

	if (mapAllData == 0) {
		sessionBuffer.resize(kCommonSize + kStatsSize);
	} else if (goalTrashData == 0) {
		sessionBuffer.resize(kCommonSize + kExtraSizeWithMapAll + kStatsSize);
	} else {
		sessionBuffer.resize(kCommonSize + kExtraSizeWithMapAll + kExtraSizeWithGoalTrash +
		                  kStatsSize);
	}

	while (!feof(fd)) {
		bytesRead = fread(sessionBuffer.data(), sessionBuffer.size(), 1, fd);

		if (bytesRead == 1) {
			ptr = sessionBuffer.data();
			auto sessionPtr = std::make_unique<Session>();
			passert(sessionPtr);
			get32bit(&ptr, sessionPtr->sessionId);
			get32bit(&ptr, sessionInfoLength);
			get32bit(&ptr, sessionPtr->peerIpAddress);
			getINode(&ptr, sessionPtr->rootInode);
			sessionPtr->flags = get8bit(&ptr);
			if (goalTrashData) {
				sessionPtr->minGoal = get8bit(&ptr);
				sessionPtr->maxGoal = get8bit(&ptr);
				get32bit(&ptr, sessionPtr->minTrashTime);
				get32bit(&ptr, sessionPtr->maxTrashTime);
			}
			get32bit(&ptr, sessionPtr->rootUid);
			get32bit(&ptr, sessionPtr->rootGid);
			if (mapAllData) {
				get32bit(&ptr, sessionPtr->mapAllUid);
				get32bit(&ptr, sessionPtr->mapAllGid);
			}
			sessionPtr->newSession = 1;
			sessionPtr->disconnectedTimestamp = eventloop_time();
			for (uint32_t i = 0; i < SESSION_STATS; i++) {
				if (i < statsInFile) {
					get32bit(&ptr, sessionPtr->currHourOperationsStats[i]);
				} else {
					sessionPtr->currHourOperationsStats[i] = 0;
				}
			}

			if (statsInFile > SESSION_STATS) {
				ptr += 4 * (statsInFile - SESSION_STATS);
			}

			for (uint32_t i = 0; i < SESSION_STATS; i++) {
				if (i < statsInFile) {
					get32bit(&ptr, sessionPtr->prevHourOperationsStats[i]);
				} else {
					sessionPtr->prevHourOperationsStats[i] = 0;
				}
			}

			if (sessionInfoLength > 0) {
				sessionPtr->info.resize(sessionInfoLength);
				if (fread(sessionPtr->info.data(), sessionInfoLength, 1, fd) != 1) {
					sessionPtr.reset();
					safs::log_warn("can't load sessions, fread error");
					fclose(fd);
					return -1;
				}
			}

			gSessionsVector.push_back(std::move(sessionPtr));
		}

		if (ferror(fd)) {
			safs::log_warn("can't load sessions, fread error");
			fclose(fd);
			return -1;
		}
	}

	safs::log_info("sessions have been loaded");
	fclose(fd);
	return 1;
}
#undef MFSSIGNATURE

int matoclserv_insert_open_file(Session *currentSession, inode_t inode) {
	if (currentSession->openFilesSet.contains(inode)) {
		return SAUNAFS_STATUS_OK;  // file already acquired - nothing to do
	}

	int status = gFSOperations->fs_acquire(FsContext::getForMaster(eventloop_time()), inode,
	                                       currentSession->sessionId);

	if (status == SAUNAFS_STATUS_OK) { currentSession->openFilesSet.insert(inode); }

	return status;
}

void matoclserv_add_open_file(uint32_t sessionId, inode_t inode) {
	for (const auto& sessionPtr : gSessionsVector) {
		if (sessionPtr->sessionId == sessionId) {
			if (!sessionPtr->openFilesSet.contains(inode)) {
				sessionPtr->openFilesSet.insert(inode);
			}
			return;
		}
	}

	// If session does not exist, create a new one
	auto sessionPtr = std::make_unique<Session>();
	passert(sessionPtr.get());
	sessionPtr->sessionId = sessionId;
	/* session created by filesystem - only for old clients (pre 1.5.13) */
	sessionPtr->disconnectedTimestamp = eventloop_time();
	sessionPtr->openFilesSet.insert(inode);
	gSessionsVector.push_back(std::move(sessionPtr));
}

void matoclserv_remove_open_file(uint32_t sessionId, inode_t inode) {
	for (const auto& sessionPtr : gSessionsVector) {
		if (sessionPtr->sessionId == sessionId) {
			if (sessionPtr->openFilesSet.contains(inode)) {
				sessionPtr->openFilesSet.erase(inode);
			}
			return;
		}
	}

	safs::log_err("sessions file is corrupted");
}

/// Resets the session timeouts for all sessions.
void matoclserv_reset_session_timeouts() {
	uint32_t now = eventloop_time();

	for (auto& sessionPtr : gSessionsVector) {
		sessionPtr->disconnectedTimestamp = now;
	}
}

uint32_t session_number_of_files(Session *currentSession) {
	return currentSession->openFilesSet.size();
}

void matocl_session_stats_rotate() {
	for (auto& sessionPtr : gSessionsVector) {
		sessionPtr->prevHourOperationsStats = sessionPtr->currHourOperationsStats;
		sessionPtr->currHourOperationsStats.fill(0);
	}
	matoclserv_store_sessions();
}

int matoclserv_sessions_init() {
	gSessionsVector.clear();

	switch (matoclserv_load_sessions()) {
		case 0: // no file
		    safs::log_warn(
		        "sessions file {}/{} not found; if it is not a fresh installation "
		        "you have to restart all active mounts",
		        fs::getCurrentWorkingDirectoryNoThrow().c_str(), kSessionsFilename);
		    matoclserv_store_sessions();
			break;
		case 1: // file loaded
		    safs::log_info("initialized sessions from file {}/{}",
		                   fs::getCurrentWorkingDirectoryNoThrow().c_str(), kSessionsFilename);
		    break;
		default:
		    safs::log_err("due to missing sessions ({}/{}) you have to restart all active mounts",
		                  fs::getCurrentWorkingDirectoryNoThrow().c_str(), kSessionsFilename);
		    break;
	}

	gSessionSustainTime = cfg_getuint32("SESSION_SUSTAIN_TIME", 86400);

	if (gSessionSustainTime > 7 * 86400) {
		gSessionSustainTime = 7 * 86400;
		safs::log_warn(
		    "SESSION_SUSTAIN_TIME too big (more than week) - setting this value to one week");
	}

	if (gSessionSustainTime < 60) {
		gSessionSustainTime = 60;
		safs::log_warn(
		    "SESSION_SUSTAIN_TIME too low (less than minute) - setting this value to one minute");
	}

	return 0;
}

void matoclserv_session_unload() {
	for (const auto& sessionPtr : gSessionsVector) {
		sessionPtr->openFilesSet.clear();
	}

	gSessionsVector.clear();
}
