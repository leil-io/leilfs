/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
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
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include "master/filesystem_periodic.h"

#include <cstdint>
#include <string>
#include <utility>

#include "common/event_loop.h"
#include "config/cfg.h"
#include "master/filesystem_checksum_updater.h"
#include "master/filesystem_metadata.h"
#include "master/filesystem_operations.h"
#include "master/filesystem_operations_interface.h"

#define FILETESTSMINLOOPTIME 1
#define FILETESTSMAXLOOPTIME 7200

#ifndef METARESTORE

static int gTasksBatchSize = 1000;

void fs_background_task_manager_work() {
	if (gMetadata->taskManager.workAvailable()) {
		uint32_t ts = eventloop_time();
		ChecksumUpdater cu(ts);
		gMetadata->taskManager.processJobs(ts, gTasksBatchSize);
		if (gMetadata->taskManager.workAvailable()) {
			eventloop_make_next_poll_nonblocking();
		}
	}
}

std::vector<DefectiveFileInfo> fs_get_defective_nodes_info(uint8_t requested_flags,
                                                           uint64_t max_entries,
                                                           uint64_t &entry_index) {
	return gFSOperations->fsTestGetDefectiveNodes(requested_flags, max_entries, entry_index);
}

void fs_test_getdata(uint32_t &loopstart, uint32_t &loopend, inode_t &files, inode_t &ugfiles,
                     inode_t &mfiles, uint32_t &chunks, uint32_t &ugchunks, uint32_t &mchunks,
                     std::string &result) {
	FsTestReport report;
	gFSOperations->fsTestGetData(report);

	loopstart = report.loopStart;
	loopend = report.loopEnd;
	files = report.files;
	ugfiles = report.underGoalFiles;
	mfiles = report.missingFiles;
	chunks = report.chunks;
	ugchunks = report.underGoalChunks;
	mchunks = report.missingChunks;
	result = std::move(report.report);
}

void fs_background_checksum_recalculation_a_bit() { gFSOperations->backgroundChecksumStep(); }

void fs_periodic_file_test() { gFSOperations->fsTestPeriodicTick(eventloop_time()); }

void fs_background_file_test(void) { gFSOperations->fsTestBackgroundStep(); }

void fsnodes_periodic_remove(const FilesystemOperationContext &fsOpContext, inode_t inode) {
	gFSOperations->fsTestOnNodeRemoved(fsOpContext, inode);
}

void fs_periodic_emptytrash(void) {
	gFSOperations->doEmptyTrash(eventloop_time());
}

void fs_periodic_emptyreserved(void) {
	gFSOperations->doEmptyReserved(eventloop_time());
}

void fs_periodic_file_operations(void) {
	gFSOperations->drainPendingFileOperations(eventloop_time());
}

void fs_read_periodic_config_file() {
	FilesystemOperationsBase::setFileTestLoopTime(cfg_get_minmaxvalue<uint32_t>(
	    "FILE_TEST_LOOP_MIN_TIME", 3600, FILETESTSMINLOOPTIME, FILETESTSMAXLOOPTIME));
	gFSOperations->readBackendPeriodicConfig();
}

void fs_periodic_master_init() {
	eventloop_timeregister(TIMEMODE_RUN_LATE, 1, 0, fs_periodic_file_test);
	eventloop_eachloopregister(fs_background_checksum_recalculation_a_bit);
	eventloop_eachloopregister(fs_background_task_manager_work);
	eventloop_eachloopregister(fs_background_file_test);
	eventloop_timeregister_ms(100, fs_periodic_emptytrash);
	eventloop_timeregister_ms(gEmptyReservedFilesPeriod, fs_periodic_emptyreserved);
	eventloop_timeregister_ms(100, fs_periodic_file_operations);
}
#endif
