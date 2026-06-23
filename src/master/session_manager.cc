/*
   Copyright 2005-2017 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2017 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

   This file is part of SaunaFS.

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

#include "master/session_manager.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "common/cwrap.h"
#include "common/datapack.h"
#include "common/event_loop.h"
#include "common/massert.h"
#include "master/filesystem_operation_context.h"
#include "master/filesystem_operations_interface.h"
#include "master/metadata_backend_common.h"
#include "protocol/SFSCommunication.h"
#include "slogger/slogger.h"

Session *SessionManagerBase::addSession(std::unique_ptr<Session> sessionPtr) {
	passert(sessionPtr.get());
	sessions_.push_back(std::move(sessionPtr));
	return sessions_.back().get();
}

Session *SessionManagerBase::findSessionEntry(uint32_t sessionId) {
	if (sessionId == 0) { return nullptr; }

	for (const auto &sessionPtr : sessions_) {
		if (sessionPtr->sessionId == sessionId) { return sessionPtr.get(); }
	}

	return nullptr;
}

const Session *SessionManagerBase::findSessionEntry(uint32_t sessionId) const {
	if (sessionId == 0) { return nullptr; }

	for (const auto &sessionPtr : sessions_) {
		if (sessionPtr->sessionId == sessionId) { return sessionPtr.get(); }
	}

	return nullptr;
}

Session *SessionManagerBase::newSession(uint8_t newSession, uint8_t noNewId) {
	auto sessionPtr = std::make_unique<Session>();
	auto newSessionIdNotNeeded = (newSession == 0 && noNewId);
	if (newSessionIdNotNeeded) {
		sessionPtr->sessionId = 0;
	} else {
		sessionPtr->sessionId = gFSOperations->newSessionId();
		if (sessionPtr->sessionId == 0) {
			safs::log_err("Refusing to register client: session id allocation failed");
			return nullptr;
		}
	}
	sessionPtr->newSession = newSession;
	sessionPtr->connections = 1;
	return addSession(std::move(sessionPtr));
}

Session *SessionManagerBase::findSession(uint32_t sessionId) {
	auto *session = findSessionEntry(sessionId);
	if (session == nullptr) { return nullptr; }

	if (session->newSession >= 2) { session->newSession -= 2; }

	session->connections++;
	session->disconnectedTimestamp = 0;
	return session;
}

void SessionManagerBase::closeSession(uint32_t sessionId) {
	auto *session = findSessionEntry(sessionId);
	if (session == nullptr) { return; }

	if (session->connections == 1 && session->newSession < 2) { session->newSession += 2; }
}

int SessionManagerBase::insertOpenFile(const FilesystemOperationContext &fsOpContext,
                                       Session *currentSession, inode_t inode) {
	if (currentSession->openFilesSet.contains(inode)) {
		return SAUNAFS_STATUS_OK;  // file already acquired - nothing to do
	}

	int status = gFSOperations->acquire(FsContext::getForMaster(eventloop_time()), fsOpContext,
	                                    inode, currentSession->sessionId);
	if (status == SAUNAFS_STATUS_OK) { currentSession->openFilesSet.insert(inode); }

	return status;
}

int SessionManagerBase::acquireOpenFilePersist(const FilesystemOperationContext &fsOpContext,
                                               Session *currentSession, inode_t inode) {
	if (currentSession->openFilesSet.contains(inode)) {
		return SAUNAFS_STATUS_OK;  // already acquired in memory - nothing to persist
	}

	// Persist only; the in-memory openFilesSet insert is deferred to commit success
	// by the caller so a commit retry replays this acquire on a fresh transaction.
	return gFSOperations->acquire(FsContext::getForMaster(eventloop_time()), fsOpContext, inode,
	                              currentSession->sessionId);
}

void SessionManagerBase::recordOpenFile(Session *currentSession, inode_t inode) {
	currentSession->openFilesSet.insert(inode);
}

void SessionManagerBase::addOpenFile(uint32_t sessionId, inode_t inode) {
	auto *session = findSessionEntry(sessionId);
	if (session != nullptr) {
		session->openFilesSet.insert(inode);
		return;
	}

	auto syntheticSession = std::make_unique<Session>();
	syntheticSession->sessionId = sessionId;
	// Session created by filesystem - only for old clients (pre 1.5.13).
	syntheticSession->disconnectedTimestamp = eventloop_time();
	syntheticSession->openFilesSet.insert(inode);
	addSession(std::move(syntheticSession));
}

void SessionManagerBase::removeOpenFile(uint32_t sessionId, inode_t inode) {
	auto *session = findSessionEntry(sessionId);
	if (session == nullptr) {
		safs::log_warn("{}: session {} not found for inode {}", __func__, sessionId, inode);
		return;
	}

	if (session->openFilesSet.contains(inode)) { session->openFilesSet.erase(inode); }
}

void SessionManagerBase::resetSessionTimeouts() {
	uint32_t now = eventloop_time();

	for (auto &sessionPtr : sessions_) { sessionPtr->disconnectedTimestamp = now; }
}

void SessionManagerBase::rotateStats() {
	for (auto &sessionPtr : sessions_) {
		sessionPtr->prevHourOperationsStats = sessionPtr->currHourOperationsStats;
		sessionPtr->currHourOperationsStats.fill(0);
	}

	storeSessions();
}

void SessionManagerBase::persistSession([[maybe_unused]] const Session &session) {
	storeSessions();
}

void SessionManagerBase::sessionDisconnected([[maybe_unused]] const Session &session) {}

void SessionManagerBase::unload() {
	for (const auto &sessionPtr : sessions_) { sessionPtr->openFilesSet.clear(); }
	sessions_.clear();
}

void SessionManagerBase::forEachSession(const std::function<void(const Session &)> &visitor) const {
	for (const auto &sessionPtr : sessions_) { visitor(*sessionPtr); }
}

std::vector<SessionFiles> SessionManagerBase::listSessions() const {
	std::vector<SessionFiles> summaries;

	for (const auto &sessionPtr : sessions_) {
		if (sessionPtr->connections == 0) { continue; }

		SessionFiles summary;
		summary.sessionId = sessionPtr->sessionId;
		summary.peerIp = sessionPtr->peerIpAddress;
		summary.filesNumber = static_cast<uint32_t>(sessionPtr->openFilesSet.size());
		summaries.push_back(summary);
	}

	return summaries;
}

bool SessionManagerBase::usesBackendSessionList() const { return false; }

void SessionManagerBase::removeTimedOutSessions(uint32_t now, uint32_t sessionSustainTime,
                                                const std::function<bool(Session *)> &onTimedOut) {
	for (auto sessionIt = sessions_.begin(); sessionIt != sessions_.end();) {
		auto &sessionPtr = *sessionIt;
		if (!isTimedOut(*sessionPtr, now, sessionSustainTime)) {
			++sessionIt;
			continue;
		}

		if (!onTimedOut(sessionPtr.get())) {
			++sessionIt;
			continue;
		}

		if (!onSessionRemoved(*sessionPtr)) {
			++sessionIt;
			continue;
		}

		sessionIt = sessions_.erase(sessionIt);
	}
}

bool SessionManagerBase::onSessionRemoved([[maybe_unused]] const Session &session) { return true; }

bool SessionManagerBase::isTimedOut(const Session &session, uint32_t now,
                                    uint32_t sessionSustainTime) {
	if (session.connections != 0) { return false; }

	return ((session.newSession > 1 && session.disconnectedTimestamp < now) ||
	        (session.newSession == 1 && session.disconnectedTimestamp + sessionSustainTime < now) ||
	        (session.newSession == 0 && session.disconnectedTimestamp + 7200 < now));
}

int SessionManagerFile::initialize() {
	unload();

	switch (loadSessions()) {
	case 0:
		safs::log_warn(
		    "sessions file {}/{} not found; if it is not a fresh installation you have to restart all active mounts",
		    fs::getCurrentWorkingDirectoryNoThrow(), kSessionsFilename);
		storeSessions();
		break;
	case 1:
		safs::log_info("initialized sessions from file {}/{}",
		               fs::getCurrentWorkingDirectoryNoThrow(), kSessionsFilename);
		break;
	default:
		safs::log_err("due to missing sessions ({}/{}) you have to restart all active mounts",
		              fs::getCurrentWorkingDirectoryNoThrow(), kSessionsFilename);
		break;
	}

	return 0;
}

void SessionManagerFile::storeSessions() {
	uint32_t sessionInfoLength;
	constexpr uint32_t kSessionSerializedSize =
	    sizeof(Session::sessionId) + sizeof(sessionInfoLength) + sizeof(Session::peerIpAddress) +
	    sizeof(Session::rootInode) + sizeof(Session::flags) + sizeof(Session::minGoal) +
	    sizeof(Session::maxGoal) + sizeof(Session::minTrashTime) + sizeof(Session::maxTrashTime) +
	    sizeof(Session::rootUid) + sizeof(Session::rootGid) + sizeof(Session::mapAllUid) +
	    sizeof(Session::mapAllGid);
	constexpr uint32_t kBufferSize = kSessionSerializedSize + (SESSION_STATS * 8);
	std::vector<uint8_t> fsesrecord(kBufferSize);

	FILE *fd = fopen(kSessionsTmpFilename, "w");
	if (fd == nullptr) {
		safs_silent_errlog(LOG_WARNING, "can't store sessions, open error");
		return;
	}

	memcpy(fsesrecord.data(), SFSSIGNATURE "S \001\006\004", 8);
	uint8_t *ptr = fsesrecord.data() + 8;
	put16bit(&ptr, SESSION_STATS);

	if (fwrite(fsesrecord.data(), 10, 1, fd) != 1) {
		safs_pretty_syslog(LOG_WARNING, "can't store sessions, fwrite error");
		fclose(fd);
		return;
	}

	for (const auto &sessionPtr : sessions_) {
		if (sessionPtr->newSession != 1) { continue; }

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

		for (int i = 0; i < SESSION_STATS; ++i) {
			put32bit(&ptr, sessionPtr->currHourOperationsStats[i]);
		}

		for (int i = 0; i < SESSION_STATS; ++i) {
			put32bit(&ptr, sessionPtr->prevHourOperationsStats[i]);
		}

		if (fwrite(fsesrecord.data(), kBufferSize, 1, fd) != 1) {
			safs::log_warn("can't store sessions, fwrite error");
			fclose(fd);
			return;
		}

		if (sessionInfoLength > 0 &&
		    fwrite(sessionPtr->info.data(), sessionInfoLength, 1, fd) != 1) {
			safs::log_warn("can't store sessions, fwrite error");
			fclose(fd);
			return;
		}
	}

	if (fclose(fd) != 0) {
		safs_silent_errlog(LOG_WARNING, "can't store sessions, fclose error");
		return;
	}

	if (rename(kSessionsTmpFilename, kSessionsFilename) < 0) {
		safs_silent_errlog(LOG_WARNING, "can't store sessions, rename error");
	}
}

int SessionManagerFile::loadSessions() {
	uint32_t sessionInfoLength;
	uint8_t headerBuffer[8];
	std::vector<uint8_t> sessionBuffer;
	const uint8_t *ptr;
	uint32_t statsInFile;
	int bytesRead;

	FILE *fd = fopen(kSessionsFilename, "r");
	if (fd == nullptr) {
		safs_silent_errlog(LOG_WARNING, "can't load sessions, fopen error");
		if (errno == ENOENT) { return 0; }
		return -1;
	}

	const size_t kSessionsHeaderSize = strlen(SFSSIGNATURE) + 5;
	if (fread(headerBuffer, kSessionsHeaderSize, 1, fd) != 1) {
		safs::log_warn("can't load sessions, fread error");
		fclose(fd);
		return -1;
	}

	if (memcmp(headerBuffer, SFSSIGNATURE "S \001\006\004", kSessionsHeaderSize) != 0) {
		safs::log_warn("can't load sessions, bad header");
		fclose(fd);
		return -1;
	}

	if (fread(headerBuffer, sizeof(uint16_t), 1, fd) != 1) {
		safs::log_warn("can't load sessions, fread error");
		fclose(fd);
		return -1;
	}
	ptr = headerBuffer;
	statsInFile = get16bit(&ptr);

	constexpr uint8_t kStatEntrySize =
	    sizeof(std::remove_extent<decltype(Session::currHourOperationsStats)>::type::value_type) +
	    sizeof(std::remove_extent<decltype(Session::prevHourOperationsStats)>::type::value_type);
	constexpr uint32_t kSessionSerializedSize =
	    sizeof(Session::sessionId) + sizeof(sessionInfoLength) + sizeof(Session::peerIpAddress) +
	    sizeof(Session::rootInode) + sizeof(Session::flags) +
	    sizeof(Session::minGoal) + sizeof(Session::maxGoal) + sizeof(Session::minTrashTime) +
	    sizeof(Session::maxTrashTime) + sizeof(Session::rootUid) + sizeof(Session::rootGid) +
	    sizeof(Session::mapAllUid) + sizeof(Session::mapAllGid);
	const uint32_t kStatsSize = statsInFile * kStatEntrySize;
	sessionBuffer.resize(kSessionSerializedSize + kStatsSize);

	while (!feof(fd)) {
		bytesRead = fread(sessionBuffer.data(), sessionBuffer.size(), 1, fd);
		if (bytesRead == 1) {
			ptr = sessionBuffer.data();
			auto sessionPtr = std::make_unique<Session>();
			get32bit(&ptr, sessionPtr->sessionId);
			get32bit(&ptr, sessionInfoLength);
			get32bit(&ptr, sessionPtr->peerIpAddress);
			getINode(&ptr, sessionPtr->rootInode);
			sessionPtr->flags = get8bit(&ptr);
			sessionPtr->minGoal = get8bit(&ptr);
			sessionPtr->maxGoal = get8bit(&ptr);
			get32bit(&ptr, sessionPtr->minTrashTime);
			get32bit(&ptr, sessionPtr->maxTrashTime);
			get32bit(&ptr, sessionPtr->rootUid);
			get32bit(&ptr, sessionPtr->rootGid);
			get32bit(&ptr, sessionPtr->mapAllUid);
			get32bit(&ptr, sessionPtr->mapAllGid);

			sessionPtr->newSession = 1;
			sessionPtr->disconnectedTimestamp = eventloop_time();
			for (uint32_t i = 0; i < SESSION_STATS; ++i) {
				if (i < statsInFile) {
					get32bit(&ptr, sessionPtr->currHourOperationsStats[i]);
				} else {
					sessionPtr->currHourOperationsStats[i] = 0;
				}
			}

			if (statsInFile > SESSION_STATS) { ptr += 4 * (statsInFile - SESSION_STATS); }

			for (uint32_t i = 0; i < SESSION_STATS; ++i) {
				if (i < statsInFile) {
					get32bit(&ptr, sessionPtr->prevHourOperationsStats[i]);
				} else {
					sessionPtr->prevHourOperationsStats[i] = 0;
				}
			}

			if (sessionInfoLength > 0) {
				sessionPtr->info.resize(sessionInfoLength);
				if (fread(sessionPtr->info.data(), sessionInfoLength, 1, fd) != 1) {
					safs::log_warn("can't load sessions, fread error");
					fclose(fd);
					return -1;
				}
			}

			addSession(std::move(sessionPtr));
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
