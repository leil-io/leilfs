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

#include "config/cfg.h"
#include "common/event_loop.h"
#if defined(SAUNAFS_HAVE_64BIT_JUDY) && !defined(DISABLE_JUDY_FOR_DEFECTIVENODESMAP)
#  include "common/judy_map.h"
#else
#  include "common/flat_map.h"
#endif
#include "common/loop_watchdog.h"
#include "master/chunks.h"
#include "master/filesystem_checksum.h"
#include "master/filesystem_checksum_updater.h"
#include "master/filesystem_metadata.h"
#include "master/filesystem_node.h"
#include "master/filesystem_operations_interface.h"
#include "master/matoclserv.h"

#define MSGBUFFSIZE 1000000
#define ERRORS_LOG_MAX 500
#define FILETESTSMINLOOPTIME 1
#define FILETESTSMAXLOOPTIME 7200

#ifndef METARESTORE

static inode_t fsinfo_files = 0;
static inode_t fsinfo_ugfiles = 0;
static inode_t fsinfo_mfiles = 0;
static uint32_t fsinfo_chunks = 0;
static uint32_t fsinfo_ugchunks = 0;
static uint32_t fsinfo_mchunks = 0;
static uint32_t fsinfo_loopstart = 0;
static uint32_t fsinfo_loopend = 0;
static uint32_t fsinfo_notfoundchunks = 0;
static uint32_t fsinfo_unavailchunks = 0;
static inode_t fsinfo_unavailfiles = 0;
static inode_t fsinfo_unavailtrashfiles = 0;
static inode_t fsinfo_unavailreservedfiles = 0;

static int gTasksBatchSize = 1000;

static int gFileTestLoopTime = 300;
static int gFileTestLoopIndex = 0;
static unsigned gFileTestLoopBucketLimit = 0;

enum NodeErrorFlag {
	kChunkUnavailable = 1,
	kChunkUnderGoal   = 2,
	kStructureError   = 4,
	kAllNodeErrors    = 7
};

#if defined(SAUNAFS_HAVE_64BIT_JUDY) && !defined(DISABLE_JUDY_FOR_DEFECTIVENODESMAP)
	using DefectiveNodesMap = judy_map<inode_t, uint8_t>;
#else
	using DefectiveNodesMap = flat_map<inode_t, uint8_t>;
#endif

static const size_t kMaxNodeEntries = 1000000;
static DefectiveNodesMap gDefectiveNodes;

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

static std::string get_node_info(FSNode *node) {
	std::string name;
	if (node == nullptr) {
		return name;
	}
	if (node->type == FSNodeType::kTrash) {
		name = "file in trash " + std::to_string(node->id) + ": " +
		       (std::string)gMetadata->trash.at(TrashPathKey(node));
	} else if (node->type == FSNodeType::kReserved) {
		name = "reserved file " + std::to_string(node->id) + ": " +
		       (std::string)gMetadata->reserved.at(node->id);
	} else if (node->type == FSNodeType::kFile) {
		name = "file " + std::to_string(node->id) + ": ";
		bool first = true;
		for (const auto &[parentId, _] : node->parents) {
			std::string path;
			auto *parent = fsnodes_id_to_node_verify<FSNodeDirectory>(parentId);
			fsnodes_getpath(parent, node, path);
			if (!first) {
				name += "|" + path;
			} else {
				name += path;
			}
			first = false;
		}
	} else if (node->type == FSNodeType::kDirectory) {
		name = "directory " + std::to_string(node->id) + ": ";
		std::string path;
		FSNodeDirectory *parent = nullptr;
		if (!node->parents.empty()) {
			parent = fsnodes_id_to_node_verify<FSNodeDirectory>(node->parents.front().first);
		}
		fsnodes_getpath(parent, node, path);
		name += path;
	}

	return fsnodes_escape_name(name);
}

std::vector<DefectiveFileInfo> fs_get_defective_nodes_info(uint8_t requested_flags, uint64_t max_entries,
	                                                   uint64_t &entry_index) {
	FSNode *node;
	std::vector<DefectiveFileInfo> defective_nodes_info;
	ActiveLoopWatchdog watchdog;
	defective_nodes_info.reserve(max_entries);
	auto it = gDefectiveNodes.find_nth(entry_index);
	watchdog.start();
	for (uint64_t i = 0; i < max_entries && it != gDefectiveNodes.end(); ++it) {
		if (((*it).second & requested_flags) != 0) {
			node = fsnodes_id_to_node<FSNode>((*it).first);
			std::string info = get_node_info(node);
			defective_nodes_info.emplace_back(std::move(info), (*it).second);
			++i;
		}
		++entry_index;
		if (watchdog.expired()) {
			return defective_nodes_info;
		}
	}
	entry_index = 0;
	return defective_nodes_info;
}

void fs_test_getdata(uint32_t &loopstart, uint32_t &loopend, inode_t &files, inode_t &ugfiles,
                     inode_t &mfiles, uint32_t &chunks, uint32_t &ugchunks, uint32_t &mchunks,
                     std::string &result) {
	std::stringstream report;
	int errors = 0;

	for (const auto &entry : gDefectiveNodes) {
		if (errors >= ERRORS_LOG_MAX) {
			break;
		}

		FSNode *node = fsnodes_id_to_node<FSNode>(entry.first);
		if (!node) {
			report << "Structure error in defective list, entry " << std::to_string(entry.first) << "\n";
			errors++;
			continue;
		}

		if (node->type == FSNodeType::kFile || node->type == FSNodeType::kTrash ||
		    node->type == FSNodeType::kReserved) {
			FSNodeFile *file_node = static_cast<FSNodeFile *>(node);
			for (std::size_t j = 0; j < file_node->chunks.size(); ++j) {
				auto chunkid = file_node->chunks[j];
				if (chunkid == 0) {
					continue;
				}

				uint8_t vc;
				if (chunk_get_fullcopies(chunkid, &vc) != SAUNAFS_STATUS_OK) {
					report << "structure error - chunk " << chunkid
					       << " not found (inode: " << file_node->id
					       << " ; index: " << j << ")\n";
					errors++;
				} else if (vc == 0) {
					report << "currently unavailable chunk " << chunkid
					       << " (inode: " << file_node->id << " ; index: " << j
					       << ")\n";
					errors++;
				}
			}
		}

		if (errors >= ERRORS_LOG_MAX) {
			break;
		}

		if (entry.second & kChunkUnavailable) {
			assert(node->type == FSNodeType::kFile || node->type == FSNodeType::kTrash ||
			       node->type == FSNodeType::kReserved);
			std::string name = get_node_info(node);
			if (node->type == FSNodeType::kTrash) {
				report << "-";
			} else if (node->type == FSNodeType::kReserved) {
				report << "+";
			} else {
				report << "*";
			}
			report << " currently unavailable " << name << "\n";
			errors++;
		}

		if (errors >= ERRORS_LOG_MAX) {
			break;
		}

		if (entry.second & kStructureError) {
			std::string name = get_node_info(node);
			report << "Structure error in " << name << "\n";
			errors++;
		}

		if (errors >= ERRORS_LOG_MAX) {
			break;
		}
	}

	if (errors >= ERRORS_LOG_MAX) {
		report << "only first " << errors
		       << " errors (unavailable chunks/files) were logged\n";
	}
	if (fsinfo_notfoundchunks > 0) {
		report << "unknown chunks: " << fsinfo_notfoundchunks << "\n";
	}
	if (fsinfo_unavailchunks > 0) {
		report << "unavailable chunks: " << fsinfo_unavailchunks << "\n";
	}
	if (fsinfo_unavailtrashfiles > 0) {
		report << "unavailable trash files: " << fsinfo_unavailtrashfiles << "\n";
	}
	if (fsinfo_unavailreservedfiles > 0) {
		report << "unavailable reserved files: " << fsinfo_unavailreservedfiles << "\n";
	}
	if (fsinfo_unavailfiles > 0) {
		report << "unavailable files: " << fsinfo_unavailfiles << "\n";
	}
	result = report.str();

	files = fsinfo_files;
	ugfiles = fsinfo_ugfiles;
	mfiles = fsinfo_mfiles;
	chunks = fsinfo_chunks;
	ugchunks = fsinfo_ugchunks;
	mchunks = fsinfo_mchunks;
	loopstart = fsinfo_loopstart;
	loopend = fsinfo_loopend;
}

void fs_background_checksum_recalculation_a_bit() {
	uint32_t recalculated = 0;

	switch (gChecksumBackgroundUpdater.getStep()) {
	case ChecksumRecalculatingStep::kNone:  // Recalculation not in progress.
		return;
	case ChecksumRecalculatingStep::kNodes:
		// Nodes are in a hashtable, therefore they can be recalculated in multiple steps.
		while (gChecksumBackgroundUpdater.getPosition() < NODEHASHSIZE) {
			auto checkSumPosition = gChecksumBackgroundUpdater.getPosition();
			for (const auto &node : gMetadata->nodeHash[checkSumPosition]) {
				fsnodes_checksum_add_to_background(node);
				++recalculated;
			}
			gChecksumBackgroundUpdater.incPosition();
			if (recalculated >= gChecksumBackgroundUpdater.getSpeedLimit()) {
				break;
			}
		}
		if (gChecksumBackgroundUpdater.getPosition() == NODEHASHSIZE) {
			gChecksumBackgroundUpdater.incStep();
		}
		break;
	case ChecksumRecalculatingStep::kXattrs:
		// Xattrs are in a hashtable, therefore they can be recalculated in multiple steps.
		while (gChecksumBackgroundUpdater.getPosition() < XATTR_DATA_HASH_SIZE) {
			auto checksumPosition = gChecksumBackgroundUpdater.getPosition();
			for (const auto &xde : gMetadata->xattrDataHash[checksumPosition]) {
				xattr_checksum_add_to_background(xde.get());
				++recalculated;
			}
			gChecksumBackgroundUpdater.incPosition();
			if (recalculated >= gChecksumBackgroundUpdater.getSpeedLimit()) {
				break;
			}
		}
		if (gChecksumBackgroundUpdater.getPosition() == XATTR_DATA_HASH_SIZE) {
			gChecksumBackgroundUpdater.incStep();
		}
		break;
	case ChecksumRecalculatingStep::kChunks:
		// Chunks can be processed in multiple steps.
		if (chunks_update_checksum_a_bit(gChecksumBackgroundUpdater.getSpeedLimit()) ==
		    ChecksumRecalculationStatus::kDone) {
			gChecksumBackgroundUpdater.incStep();
		}
		break;
	case ChecksumRecalculatingStep::kDone:
		gChecksumBackgroundUpdater.end();
		matoclserv_broadcast_metadata_checksum_recalculated(SAUNAFS_STATUS_OK);
		return;
	}
	eventloop_make_next_poll_nonblocking();
}

void fs_process_file_test() {
	uint32_t k;
	uint8_t vc, node_error_flag;
	ActiveLoopWatchdog watchdog;

	static inode_t files = 0;
	static inode_t ugfiles = 0;
	static inode_t mfiles = 0;
	static uint32_t chunks = 0;
	static uint32_t ugchunks = 0;
	static uint32_t mchunks = 0;
	static uint32_t notfoundchunks = 0;
	static uint32_t unavailchunks = 0;
	static inode_t unavailfiles = 0;
	static inode_t unavailtrashfiles = 0;
	static inode_t unavailreservedfiles = 0;

	if (gFileTestLoopIndex == 0) {
		if (unavailfiles > 0) {
			safs::log_err("Currently unavailable files: {}", unavailfiles);
		}
		if (unavailchunks > 0) {
			safs::log_err("Currently unavailable chunks: {}", unavailchunks);
		}
		if (unavailreservedfiles > 0) {
			safs::log_err("Currently unavailable reserved files: {}", unavailreservedfiles);
		}
		if (unavailtrashfiles > 0) {
			safs::log_warn("Currently unavailable trash files: {}", unavailtrashfiles);
		}

		fsinfo_files = files;
		fsinfo_ugfiles = ugfiles;
		fsinfo_mfiles = mfiles;
		fsinfo_chunks = chunks;
		fsinfo_ugchunks = ugchunks;
		fsinfo_mchunks = mchunks;
		fsinfo_loopstart = fsinfo_loopend;
		fsinfo_loopend = eventloop_time();
		fsinfo_notfoundchunks = notfoundchunks;
		fsinfo_unavailchunks = unavailchunks;
		fsinfo_unavailfiles = unavailfiles;
		fsinfo_unavailtrashfiles = unavailtrashfiles;
		fsinfo_unavailreservedfiles = unavailreservedfiles;

		files = 0;
		ugfiles = 0;
		mfiles = 0;
		chunks = 0;
		ugchunks = 0;
		mchunks = 0;
		notfoundchunks = 0;
		unavailchunks = 0;
		unavailfiles = 0;
		unavailtrashfiles = 0;
		unavailreservedfiles = 0;
	}

	watchdog.start();
	for (k = 0; k < gFileTestLoopBucketLimit && gFileTestLoopIndex < NODEHASHSIZE;
	     k++, gFileTestLoopIndex++) {
		if (k > 0 && watchdog.expired()) {
			gFileTestLoopBucketLimit -= k;
			return;
		}

		for (const auto &node : gMetadata->nodeHash[gFileTestLoopIndex]) {
			node_error_flag = 0;

			if (node->type == FSNodeType::kFile || node->type == FSNodeType::kTrash ||
			    node->type == FSNodeType::kReserved) {
				for (const auto &chunkid : static_cast<FSNodeFile *>(node)->chunks) {
					if (chunkid == 0) {
						continue;
					}

					if (chunk_get_fullcopies(chunkid, &vc) !=
					    SAUNAFS_STATUS_OK) {
						node_error_flag |=
						        static_cast<int>(kChunkUnavailable);
						notfoundchunks++;
						mchunks++;
					} else if (vc == 0) {
						node_error_flag |=
						        static_cast<int>(kChunkUnavailable);
						unavailchunks++;
						mchunks++;
					} else {
						int recover, remove;
						chunk_get_partstomodify(chunkid, recover, remove);
						if (recover > 0) {
							node_error_flag |=
							        static_cast<int>(kChunkUnderGoal);
							ugchunks++;
						}
					}
					chunks++;
				}
			}

			if (node->type == FSNodeType::kDirectory) {
				for (const auto &entry : static_cast<FSNodeDirectory *>(node)->entries) {
					FSNode *childNode = entry.second;

					if (!childNode) {
						// the node points to invalid memory
						node_error_flag |= static_cast<int>(kStructureError);
					} else {
						auto parentInChildPtr = std::find_if(
						    childNode->parents.begin(), childNode->parents.end(),
						    [node](const std::pair<inode_t, const hstorage::Handle *> &p) {
							    return p.first == node->id;
						    });
						// the node doesn't have a parent entry pointing to the current directory
						if (parentInChildPtr == childNode->parents.end()) {
							node_error_flag |= static_cast<int>(kStructureError);
						}
					}
				}
			}

			if (node_error_flag == 0) {
				auto it = gDefectiveNodes.find(node->id);
				if (it != gDefectiveNodes.end()) {
					gDefectiveNodes.erase(it);
				}
				continue;
			}

			if (node_error_flag & kChunkUnavailable) {
				if (node->type == FSNodeType::kTrash) {
					unavailtrashfiles++;
				} else if (node->type == FSNodeType::kReserved) {
					unavailreservedfiles++;
				} else {
					unavailfiles += node->parents.size();
				}

				auto it = gDefectiveNodes.find(node->id);
				if (it == gDefectiveNodes.end()) {
					std::string name = get_node_info(node);
					safs::log_trace("Chunks unavailable in {}",
					                   name);
				}
			}
			if (node_error_flag & kChunkUnderGoal) {
				ugfiles++;
			}
			if (node_error_flag & kStructureError) {
				auto it = gDefectiveNodes.find(node->id);
				if (it == gDefectiveNodes.end()) {
					std::string name = get_node_info(node);
					safs_pretty_syslog(LOG_ERR, "Structure error in %s",
					                   name.c_str());
				}
			}

			if (gDefectiveNodes.size() < kMaxNodeEntries) {
				gDefectiveNodes[node->id] = node_error_flag;
			} else {
				auto it = gDefectiveNodes.find(node->id);
				if (it != gDefectiveNodes.end()) {
					(*it).second = node_error_flag;
				}
			}
		}
	}

	gFileTestLoopBucketLimit -= k;
	if (gFileTestLoopIndex >= NODEHASHSIZE) {
		gFileTestLoopIndex = 0;
	}
}

void fs_periodic_file_test() {
	if (eventloop_time() <= gTestStartTime) {
		gFileTestLoopBucketLimit = 0;
		return;
	}

	if (gFileTestLoopBucketLimit == 0) {
		gFileTestLoopBucketLimit = NODEHASHSIZE / gFileTestLoopTime;
		fs_process_file_test();
	}
}

void fs_background_file_test(void) {
	if (gFileTestLoopBucketLimit > 0) {
		fs_process_file_test();
		if (gFileTestLoopBucketLimit > 0) {
			eventloop_make_next_poll_nonblocking();
		}
	}
}

void fsnodes_periodic_remove(inode_t inode) {
	auto it = gDefectiveNodes.find(inode);
	if (it != gDefectiveNodes.end()) {
		gDefectiveNodes.erase(it);
	}
}
#endif

struct InodeInfo {
	inode_t free;
	inode_t reserved;
};

#ifndef METARESTORE
static void fs_do_emptytrash(uint32_t ts) {
	SignalLoopWatchdog watchdog;

	auto it = gMetadata->trash.begin();
	watchdog.start();
	while (it != gMetadata->trash.end() && ((*it).first.timestamp < ts)) {
		FSNodeFile *node = fsnodes_id_to_node_verify<FSNodeFile>((*it).first.id);

		if (!node) {
			std::string pathName = (*it).second.get();
			removeTrashEntry(gMetadata->trash, gMetadata->trashHandlesIndex,
			                 gMetadata->trashReservedToId, node);
			it = gMetadata->trash.begin();
			continue;
		}

		assert(node->type == FSNodeType::kTrash);

		auto node_id = node->id;
		fsnodes_purge(ts, node);

		// Purge operation should be performed anyway - if it fails, inode will be reserved
		gFSOperations->changeLog(ts, "PURGE(%" PRIiNode ")", node_id);

		it = gMetadata->trash.begin();

		if (watchdog.expired()) {
			break;
		}
	}
}

void fs_periodic_emptytrash(void) {
	uint32_t ts = eventloop_time();
	fs_do_emptytrash(ts);
}

static void fs_do_emptyreserved(uint32_t ts) {
	SignalLoopWatchdog watchdog;

	auto it = gMetadata->reserved.begin();
	watchdog.start();
	while (it != gMetadata->reserved.end()) {
		FSNodeFile *node = fsnodes_id_to_node_verify<FSNodeFile>((*it).first);

		if (!node) {
			removeReservedEntry(gMetadata->reserved, gMetadata->reservedHandlesIndex,
			                    gMetadata->trashReservedToId, (*it).first);
			it = gMetadata->reserved.begin();
			continue;
		}

		assert(node->type == FSNodeType::kReserved);

		auto node_id = node->id;
		fsnodes_purge(ts, node);

		// Purge operation should be performed anyway
		gFSOperations->changeLog(ts, "PURGE(%" PRIiNode ")", node_id);

		it = gMetadata->reserved.begin();

		if (watchdog.expired()) {
			break;
		}
	}
}

void fs_periodic_emptyreserved(void) {
	uint32_t ts = eventloop_time();
	fs_do_emptyreserved(ts);
}

void fs_read_periodic_config_file() {
	gFileTestLoopTime = cfg_get_minmaxvalue<uint32_t>("FILE_TEST_LOOP_MIN_TIME", 3600, FILETESTSMINLOOPTIME, FILETESTSMAXLOOPTIME);
}

void fs_periodic_master_init() {
	eventloop_timeregister(TIMEMODE_RUN_LATE, 1, 0, fs_periodic_file_test);
	eventloop_eachloopregister(fs_background_checksum_recalculation_a_bit);
	eventloop_eachloopregister(fs_background_task_manager_work);
	eventloop_eachloopregister(fs_background_file_test);
	eventloop_timeregister_ms(100, fs_periodic_emptytrash);
	eventloop_timeregister_ms(gEmptyReservedFilesPeriod, fs_periodic_emptyreserved);
}
#endif
