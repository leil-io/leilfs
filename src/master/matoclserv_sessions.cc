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

#include "common/event_loop.h"
#include "common/massert.h"
#include "config/cfg.h"
#include "master/session_manager.h"
#include "slogger/slogger.h"

namespace {

std::unique_ptr<ISessionManager> gSessionManager;

}  // namespace

void matoclserv_configure_session_sustain_time() {
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
}

void matoclserv_set_session_manager(std::unique_ptr<ISessionManager> manager) {
	sassert(manager != nullptr);
	if (gSessionManager != nullptr) { gSessionManager->unload(); }
	gSessionManager = std::move(manager);
}

void matoclserv_store_sessions() {
	sassert(gSessionManager != nullptr);
	gSessionManager->storeSessions();
}

void matoclserv_persist_session(Session *session) {
	sassert(gSessionManager != nullptr);
	if (session == nullptr) { return; }
	gSessionManager->persistSession(*session);
}

void matoclserv_session_disconnected(Session *session) {
	sassert(gSessionManager != nullptr);
	if (session == nullptr) { return; }
	gSessionManager->sessionDisconnected(*session);
}

int matoclserv_load_sessions() {
	sassert(gSessionManager != nullptr);
	return gSessionManager->loadSessions();
}

Session *matoclserv_new_session(uint8_t newSession, uint8_t noNewId) {
	sassert(gSessionManager != nullptr);
	return gSessionManager->newSession(newSession, noNewId);
}

Session *matoclserv_find_session(uint32_t sessionId) {
	sassert(gSessionManager != nullptr);
	return gSessionManager->findSession(sessionId);
}

void matoclserv_close_session(uint32_t sessionId) {
	sassert(gSessionManager != nullptr);
	gSessionManager->closeSession(sessionId);
}

void matoclserv_reset_session_timeouts() {
	sassert(gSessionManager != nullptr);
	gSessionManager->resetSessionTimeouts();
}

void matoclserv_for_each_session(const std::function<void(const Session &)> &visitor) {
	sassert(gSessionManager != nullptr);
	gSessionManager->forEachSession(visitor);
}

std::vector<SessionFiles> matoclserv_list_sessions() {
	sassert(gSessionManager != nullptr);
	return gSessionManager->listSessions();
}

bool matoclserv_uses_backend_session_list() {
	sassert(gSessionManager != nullptr);
	return gSessionManager->usesBackendSessionList();
}

void matoclserv_remove_timed_out_sessions(const std::function<bool(Session *)> &onTimedOut) {
	sassert(gSessionManager != nullptr);
	gSessionManager->removeTimedOutSessions(eventloop_time(), gSessionSustainTime, onTimedOut);
}

int matoclserv_insert_open_file(const FilesystemOperationContext &fsOpContext,
                                Session *currentSession, inode_t inode) {
	sassert(gSessionManager != nullptr);
	return gSessionManager->insertOpenFile(fsOpContext, currentSession, inode);
}

int matoclserv_acquire_open_file_persist(const FilesystemOperationContext &fsOpContext,
                                         Session *currentSession, inode_t inode) {
	sassert(gSessionManager != nullptr);
	return gSessionManager->acquireOpenFilePersist(fsOpContext, currentSession, inode);
}

void matoclserv_add_open_file(uint32_t sessionId, inode_t inode) {
	sassert(gSessionManager != nullptr);
	gSessionManager->addOpenFile(sessionId, inode);
}

void matoclserv_record_open_file(Session *currentSession, inode_t inode) {
	sassert(gSessionManager != nullptr);
	gSessionManager->recordOpenFile(currentSession, inode);
}

void matoclserv_remove_open_file(uint32_t sessionId, inode_t inode) {
	sassert(gSessionManager != nullptr);
	gSessionManager->removeOpenFile(sessionId, inode);
}

uint32_t session_number_of_files(Session *currentSession) {
	return currentSession->openFilesSet.size();
}

void matocl_session_stats_rotate() {
	sassert(gSessionManager != nullptr);
	gSessionManager->rotateStats();
}

int matoclserv_sessions_init() {
	sassert(gSessionManager != nullptr);
	int status = gSessionManager->initialize();
	matoclserv_configure_session_sustain_time();
	return status;
}

void matoclserv_session_unload() {
	if (gSessionManager != nullptr) { gSessionManager->unload(); }
}
