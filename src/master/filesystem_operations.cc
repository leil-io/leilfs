/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
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
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include "master/filesystem_operations.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <sstream>
#include <string_view>

#include "common/attributes.h"
#include "common/event_loop.h"
#if defined(SAUNAFS_HAVE_64BIT_JUDY) && !defined(DISABLE_JUDY_FOR_DEFECTIVENODESMAP)
#include "common/judy_map.h"
#else
#include "common/flat_map.h"
#endif
#include "common/loop_watchdog.h"
#include "errors/saunafs_error_codes.h"
#include "master/changelog.h"
#include "master/chunk_operations_interface.h"
#include "master/chunks.h"
#include "master/filesystem.h"
#include "master/filesystem_checksum.h"
#include "master/filesystem_checksum_updater.h"
#include "master/filesystem_metadata.h"
#include "master/filesystem_node_types.h"
#include "master/filesystem_operation_context.h"
#include "master/filesystem_operations_interface.h"
#include "master/filesystem_quota.h"
#include "master/filesystem_stats.h"
#include "master/fs_context.h"
#include "master/locks.h"
#include "master/matoclserv.h"
#include "master/matoclserv_sessions.h"
#include "master/matocsserv.h"
#include "master/matomlserv.h"
#include "master/matontserv.h"
#include "master/recursive_remove_task.h"
#include "master/task_manager.h"
#include "metrics/metrics.h"
#include "slogger/slogger.h"

inline bool isDepletedSpace() {
	uint64_t totalSpace = 0;
	uint64_t availableSpace = 0;
	matocsserv_getspace(&totalSpace, &availableSpace);
	return (totalSpace < SFSCHUNKSIZE || availableSpace < SFSCHUNKSIZE);
}

static const int kInitialTaskBatchSize = 1000;

FilesystemOperationsBase::FilesystemOperationsBase(
    std::unique_ptr<IFilesystemNodeOperations> _nodeOps)
    : nodeOperations_(std::move(_nodeOps)) {}

void FilesystemOperationsBase::changeLog(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, [[maybe_unused]] uint32_t ts,
    [[maybe_unused]] const char *format, ...) {
#ifndef METARESTORE
	const uint32_t kMaxTimestampSize = 20;
	const uint32_t kMaxEntrySize = kMaxLogLineSize - kMaxTimestampSize;
	static char entry[kMaxLogLineSize];

	// First, put "<timestamp>|" in the buffer
	int timeStampLength = snprintf(entry, kMaxTimestampSize, "%" PRIu32 "|", ts);

	// Then append the entry to the buffer
	va_list argList;
	va_start(argList, format);
	uint32_t entryLength = vsnprintf(entry + timeStampLength, kMaxEntrySize, format, argList);
	va_end(argList);

	if (entryLength >= kMaxEntrySize) {
		entry[timeStampLength + kMaxEntrySize - 1] = '\0';
		entryLength = kMaxEntrySize;
	} else {
		entryLength++;
	}

	uint64_t version = increaseMetadataVersion(fsOpContext);
	changelog(version, entry);

	if (!getChangelogSignal().empty()) {
		getChangelogSignal().emit({.version = version, .entry = entry});
	}

	matomlserv_broadcast_logstring(version, (uint8_t *)entry, timeStampLength + entryLength);
	matontserv_broadcast_message(version, std::string_view(entry, timeStampLength + entryLength));
#endif
}

#ifndef METARESTORE

// ---------------------------------------------------------------------------
// Periodic trash/reserved cleanup (in-memory backend)
// ---------------------------------------------------------------------------

void FilesystemOperationsBase::doEmptyTrash(uint32_t timeStamp) {
	SignalLoopWatchdog watchdog;

	auto trashIter = gMetadata->trash.begin();

	// This function is reimplemented in KV backends, so fsOpContext is here only to satisfy the
	// interface. It does not need to be committed.
	auto fsOpContext =
	    createFilesystemOperationContext(FilesystemOperationContext::TransactionType::kReadWrite);

	watchdog.start();

	while (trashIter != gMetadata->trash.end() && ((*trashIter).first.timestamp < timeStamp)) {
		auto *node =
		    nodeOperations()->idToNodeVerify<FSNodeFile>(fsOpContext, (*trashIter).first.id);

		if (node == kNodeNotFound) {
			const TrashPathKey trashKey = (*trashIter).first;
			removeTrashEntryByKey(gMetadata->trash, gMetadata->trashHandlesIndex,
			                      gMetadata->trashReservedToId, trashKey);
			trashIter = gMetadata->trash.begin();
			continue;
		}

		assert(node->type == FSNodeType::kTrash);

		auto nodeId = node->id;
		nodeOperations()->purge(fsOpContext, timeStamp, node);

		// Purge operation should be performed anyway - if it fails, inode will be reserved
		changeLog(fsOpContext, timeStamp, "PURGE(%" PRIiNode ")", nodeId);

		trashIter = gMetadata->trash.begin();

		if (watchdog.expired()) { break; }
	}
}

void FilesystemOperationsBase::doEmptyReserved(uint32_t timeStamp) {
	SignalLoopWatchdog watchdog;

	auto reservedIter = gMetadata->reserved.begin();

	// This function is reimplemented in KV backends, so fsOpContext is here only to satisfy the
	// interface. It does not need to be committed.
	auto fsOpContext =
	    createFilesystemOperationContext(FilesystemOperationContext::TransactionType::kReadWrite);

	watchdog.start();

	while (reservedIter != gMetadata->reserved.end()) {
		if (watchdog.expired()) { break; }

		auto *node =
		    nodeOperations()->idToNodeVerify<FSNodeFile>(fsOpContext, (*reservedIter).first);

		if (node == kNodeNotFound) {
			removeReservedEntry(gMetadata->reserved, gMetadata->reservedHandlesIndex,
			                    gMetadata->trashReservedToId, (*reservedIter).first);
			reservedIter = gMetadata->reserved.begin();
			continue;
		}

		assert(node->type == FSNodeType::kReserved);

		auto nodeId = node->id;
		FsContext context = FsContext::getForMaster(timeStamp);

		assert(!node->sessionIds.empty());

		auto sessionIds = node->sessionIds;

		if (!sessionIds.empty()) {
			for (auto &sessionId : sessionIds) {
				uint8_t status = release(context, fsOpContext, nodeId, sessionId);
				if (status != SAUNAFS_STATUS_OK) {
					safs::log_err(
					    "Failed to release from periodic cleaning reserved file: {}, session: {}, status: {}",
					    nodeId, sessionId, status);
				}
			}
		} else {
			safs::log_critical(
			    "Failed to release from periodic cleaning reserved file: {}, no session associated with the file",
			    nodeId);
		}

		reservedIter = gMetadata->reserved.begin();
	}
}

// ---------------------------------------------------------------------------
// Periodic file-test scanner helpers (in-memory backend)
// ---------------------------------------------------------------------------

namespace {

constexpr int kDefaultFileTestLoopTime = 300;
constexpr int kErrorsLogMax = 500;

static inode_t gFileTestPublishedFiles = 0;
static inode_t gFileTestPublishedUnderGoalFiles = 0;
static inode_t gFileTestPublishedMissingFiles = 0;
static uint32_t gFileTestPublishedChunks = 0;
static uint32_t gFileTestPublishedUnderGoalChunks = 0;
static uint32_t gFileTestPublishedMissingChunks = 0;
static uint32_t gFileTestPublishedLoopStart = 0;
static uint32_t gFileTestPublishedLoopEnd = 0;
static uint32_t gFileTestPublishedUnknownChunks = 0;
static uint32_t gFileTestPublishedUnavailableChunks = 0;
static inode_t gFileTestPublishedUnavailableFiles = 0;
static inode_t gFileTestPublishedUnavailableTrashFiles = 0;
static inode_t gFileTestPublishedUnavailableReservedFiles = 0;

static uint32_t gFileTestLoopTime = kDefaultFileTestLoopTime;
static int gFileTestLoopIndex = 0;
static unsigned gFileTestLoopBucketLimit = 0;

#if defined(SAUNAFS_HAVE_64BIT_JUDY) && !defined(DISABLE_JUDY_FOR_DEFECTIVENODESMAP)
using DefectiveNodesMap = judy_map<inode_t, uint8_t>;
#else
using DefectiveNodesMap = flat_map<inode_t, uint8_t>;
#endif

constexpr size_t kMaxNodeEntries = 1000000;
static DefectiveNodesMap gDefectiveNodes;

void addNodeErrorFlag(uint8_t &errorFlags, NodeErrorFlag flag) {
	errorFlags |= static_cast<uint8_t>(flag);
}

bool hasNodeErrorFlag(uint8_t errorFlags, NodeErrorFlag flag) {
	return (errorFlags & static_cast<uint8_t>(flag)) != 0;
}

bool hasAnyNodeErrorFlag(uint8_t errorFlags, uint8_t requestedFlags) {
	return (errorFlags & requestedFlags) != 0;
}

std::string getDetachedNodePath(IFilesystemOperations &operations,
                                const FilesystemOperationContext &fsOpContext, const FSNode *node) {
	return operations.getDetachedPath(fsOpContext, node).value_or("");
}

std::string getNodeInfo(IFilesystemOperations &operations,
                        const FilesystemOperationContext &fsOpContext, FSNode *node) {
	std::string name;
	if (node == nullptr) { return name; }

	if (node->type == FSNodeType::kTrash) {
		name = "file in trash " + std::to_string(node->id) + ": " +
		       getDetachedNodePath(operations, fsOpContext, node);
	} else if (node->type == FSNodeType::kReserved) {
		name = "reserved file " + std::to_string(node->id) + ": " +
		       getDetachedNodePath(operations, fsOpContext, node);
	} else if (node->type == FSNodeType::kFile) {
		name = "file " + std::to_string(node->id) + ": ";
		bool first = true;
		for (const auto &[parentId, _] : node->parents) {
			std::string path;
			auto *parent =
			    operations.nodeOperations()->idToNodeVerify<FSNodeDirectory>(fsOpContext, parentId);
			operations.nodeOperations()->getPath(fsOpContext, parent, node, path);
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
			parent = operations.nodeOperations()->idToNodeVerify<FSNodeDirectory>(
			    fsOpContext, node->parents.front().first);
		}
		operations.nodeOperations()->getPath(fsOpContext, parent, node, path);
		name += path;
	}

	return operations.nodeOperations()->escapeName(name);
}

void processFileTest(FilesystemOperationsBase &operations) {
	ActiveLoopWatchdog watchdog;
	auto fsOpContext = operations.createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);

	static inode_t currentScanFiles = 0;
	static inode_t currentScanUnderGoalFiles = 0;
	static inode_t currentScanMissingFiles = 0;
	static uint32_t currentScanChunks = 0;
	static uint32_t currentScanUnderGoalChunks = 0;
	static uint32_t currentScanMissingChunks = 0;
	static uint32_t currentScanUnknownChunks = 0;
	static uint32_t currentScanUnavailableChunks = 0;
	static inode_t currentScanUnavailableFiles = 0;
	static inode_t currentScanUnavailableTrashFiles = 0;
	static inode_t currentScanUnavailableReservedFiles = 0;

	if (gFileTestLoopIndex == 0) {
		if (currentScanUnavailableFiles > 0) {
			safs::log_err("Currently unavailable files: {}", currentScanUnavailableFiles);
		}
		if (currentScanUnavailableChunks > 0) {
			safs::log_err("Currently unavailable chunks: {}", currentScanUnavailableChunks);
		}
		if (currentScanUnavailableReservedFiles > 0) {
			safs::log_err("Currently unavailable reserved files: {}",
			              currentScanUnavailableReservedFiles);
		}
		if (currentScanUnavailableTrashFiles > 0) {
			safs::log_warn("Currently unavailable trash files: {}",
			               currentScanUnavailableTrashFiles);
		}

		gFileTestPublishedFiles = currentScanFiles;
		gFileTestPublishedUnderGoalFiles = currentScanUnderGoalFiles;
		gFileTestPublishedMissingFiles = currentScanMissingFiles;
		gFileTestPublishedChunks = currentScanChunks;
		gFileTestPublishedUnderGoalChunks = currentScanUnderGoalChunks;
		gFileTestPublishedMissingChunks = currentScanMissingChunks;
		gFileTestPublishedLoopStart = gFileTestPublishedLoopEnd;
		gFileTestPublishedLoopEnd = eventloop_time();
		gFileTestPublishedUnknownChunks = currentScanUnknownChunks;
		gFileTestPublishedUnavailableChunks = currentScanUnavailableChunks;
		gFileTestPublishedUnavailableFiles = currentScanUnavailableFiles;
		gFileTestPublishedUnavailableTrashFiles = currentScanUnavailableTrashFiles;
		gFileTestPublishedUnavailableReservedFiles = currentScanUnavailableReservedFiles;

		currentScanFiles = 0;
		currentScanUnderGoalFiles = 0;
		currentScanMissingFiles = 0;
		currentScanChunks = 0;
		currentScanUnderGoalChunks = 0;
		currentScanMissingChunks = 0;
		currentScanUnknownChunks = 0;
		currentScanUnavailableChunks = 0;
		currentScanUnavailableFiles = 0;
		currentScanUnavailableTrashFiles = 0;
		currentScanUnavailableReservedFiles = 0;
	}

	watchdog.start();
	uint32_t scannedBuckets = 0;
	for (; scannedBuckets < gFileTestLoopBucketLimit && gFileTestLoopIndex < NODEHASHSIZE;
	     scannedBuckets++, gFileTestLoopIndex++) {
		if (scannedBuckets > 0 && watchdog.expired()) {
			gFileTestLoopBucketLimit -= scannedBuckets;
			return;
		}

		for (const auto &node : gMetadata->nodeHash[gFileTestLoopIndex]) {
			uint8_t nodeErrorFlags = 0;

			if (node->type == FSNodeType::kFile || node->type == FSNodeType::kTrash ||
			    node->type == FSNodeType::kReserved) {
				for (const auto &chunkId : static_cast<FSNodeFile *>(node)->chunks) {
					if (chunkId == 0) { continue; }

					uint8_t fullCopies = 0;
					if (gChunkOperations->getFullCopies(chunkId, &fullCopies) !=
					    SAUNAFS_STATUS_OK) {
						addNodeErrorFlag(nodeErrorFlags, kChunkUnavailable);
						currentScanUnknownChunks++;
						currentScanMissingChunks++;
					} else if (fullCopies == 0) {
						addNodeErrorFlag(nodeErrorFlags, kChunkUnavailable);
						currentScanUnavailableChunks++;
						currentScanMissingChunks++;
					} else {
						int recoverParts = 0;
						int removeParts = 0;
						gChunkOperations->getPartsToModify(chunkId, recoverParts, removeParts);
						if (recoverParts > 0) {
							addNodeErrorFlag(nodeErrorFlags, kChunkUnderGoal);
							currentScanUnderGoalChunks++;
						}
					}
					currentScanChunks++;
				}
			}

			if (node->type == FSNodeType::kDirectory) {
				for (const auto &entry : static_cast<FSNodeDirectory *>(node)->entries) {
					auto *childNode = entry.second;

					if (childNode == kNodeNotFound) {
						addNodeErrorFlag(nodeErrorFlags, kStructureError);
					} else {
						auto parentIter = std::find_if(
						    childNode->parents.begin(), childNode->parents.end(),
						    [node](const std::pair<inode_t, const hstorage::Handle *> &p) {
							    return p.first == node->id;
						    });
						if (parentIter == childNode->parents.end()) {
							addNodeErrorFlag(nodeErrorFlags, kStructureError);
						}
					}
				}
			}

			if (nodeErrorFlags == 0) {
				auto defectiveNodeIter = gDefectiveNodes.find(node->id);
				if (defectiveNodeIter != gDefectiveNodes.end()) {
					gDefectiveNodes.erase(defectiveNodeIter);
				}
				continue;
			}

			if (hasNodeErrorFlag(nodeErrorFlags, kChunkUnavailable)) {
				if (node->type == FSNodeType::kTrash) {
					currentScanUnavailableTrashFiles++;
				} else if (node->type == FSNodeType::kReserved) {
					currentScanUnavailableReservedFiles++;
				} else {
					currentScanUnavailableFiles += node->parents.size();
				}

				auto defectiveNodeIter = gDefectiveNodes.find(node->id);
				if (defectiveNodeIter == gDefectiveNodes.end()) {
					std::string name = getNodeInfo(operations, fsOpContext, node);
					safs::log_trace("Chunks unavailable in {}", name);
				}
			}
			if (hasNodeErrorFlag(nodeErrorFlags, kChunkUnderGoal)) { currentScanUnderGoalFiles++; }
			if (hasNodeErrorFlag(nodeErrorFlags, kStructureError)) {
				auto defectiveNodeIter = gDefectiveNodes.find(node->id);
				if (defectiveNodeIter == gDefectiveNodes.end()) {
					std::string name = getNodeInfo(operations, fsOpContext, node);
					safs_pretty_syslog(LOG_ERR, "Structure error in %s", name.c_str());
				}
			}

			if (gDefectiveNodes.size() < kMaxNodeEntries) {
				gDefectiveNodes[node->id] = nodeErrorFlags;
			} else {
				auto defectiveNodeIter = gDefectiveNodes.find(node->id);
				if (defectiveNodeIter != gDefectiveNodes.end()) {
					(*defectiveNodeIter).second = nodeErrorFlags;
				}
			}
		}
	}

	gFileTestLoopBucketLimit -= scannedBuckets;
	if (gFileTestLoopIndex >= NODEHASHSIZE) { gFileTestLoopIndex = 0; }
}

}  // namespace

// ---------------------------------------------------------------------------
// Periodic file-test scanner hooks (in-memory backend)
// ---------------------------------------------------------------------------

void FilesystemOperationsBase::setFileTestLoopTime(uint32_t loopTime) {
	gFileTestLoopTime = loopTime;
}

uint32_t FilesystemOperationsBase::fileTestLoopTime() { return gFileTestLoopTime; }

void FilesystemOperationsBase::fsTestPeriodicTick(uint32_t timeStamp) {
	if (timeStamp <= gTestStartTime) {
		gFileTestLoopBucketLimit = 0;
		return;
	}

	if (gFileTestLoopBucketLimit == 0) {
		gFileTestLoopBucketLimit = NODEHASHSIZE / gFileTestLoopTime;
		processFileTest(*this);
	}
}

void FilesystemOperationsBase::fsTestBackgroundStep() {
	if (gFileTestLoopBucketLimit > 0) {
		processFileTest(*this);
		if (gFileTestLoopBucketLimit > 0) { eventloop_make_next_poll_nonblocking(); }
	}
}

void FilesystemOperationsBase::fsTestGetData(FsTestReport &out) {
	auto fsOpContext =
	    createFilesystemOperationContext(FilesystemOperationContext::TransactionType::kReadOnly);
	std::stringstream report;
	int errorCount = 0;

	for (const auto &defectiveNodeEntry : gDefectiveNodes) {
		if (errorCount >= kErrorsLogMax) { break; }

		auto *node = nodeOperations()->idToNode<FSNode>(fsOpContext, defectiveNodeEntry.first);
		if (node == kNodeNotFound) {
			report << "Structure error in defective list, entry "
			       << std::to_string(defectiveNodeEntry.first) << "\n";
			errorCount++;
			continue;
		}

		if (node->type == FSNodeType::kFile || node->type == FSNodeType::kTrash ||
		    node->type == FSNodeType::kReserved) {
			auto *fileNode = static_cast<FSNodeFile *>(node);
			for (std::size_t chunkIndex = 0; chunkIndex < fileNode->chunks.size(); ++chunkIndex) {
				auto chunkId = fileNode->chunks[chunkIndex];
				if (chunkId == 0) { continue; }

				uint8_t fullCopies = 0;
				if (gChunkOperations->getFullCopies(chunkId, &fullCopies) != SAUNAFS_STATUS_OK) {
					report << "structure error - chunk " << chunkId
					       << " not found (inode: " << fileNode->id << " ; index: " << chunkIndex
					       << ")\n";
					errorCount++;
				} else if (fullCopies == 0) {
					report << "currently unavailable chunk " << chunkId
					       << " (inode: " << fileNode->id << " ; index: " << chunkIndex << ")\n";
					errorCount++;
				}
			}
		}

		if (errorCount >= kErrorsLogMax) { break; }

		if (hasNodeErrorFlag(defectiveNodeEntry.second, kChunkUnavailable)) {
			assert(node->type == FSNodeType::kFile || node->type == FSNodeType::kTrash ||
			       node->type == FSNodeType::kReserved);
			std::string name = getNodeInfo(*this, fsOpContext, node);
			if (node->type == FSNodeType::kTrash) {
				report << "-";
			} else if (node->type == FSNodeType::kReserved) {
				report << "+";
			} else {
				report << "*";
			}
			report << " currently unavailable " << name << "\n";
			errorCount++;
		}

		if (errorCount >= kErrorsLogMax) { break; }

		if (hasNodeErrorFlag(defectiveNodeEntry.second, kStructureError)) {
			std::string name = getNodeInfo(*this, fsOpContext, node);
			report << "Structure error in " << name << "\n";
			errorCount++;
		}

		if (errorCount >= kErrorsLogMax) { break; }
	}

	if (errorCount >= kErrorsLogMax) {
		report << "only first " << errorCount << " errors (unavailable chunks/files) were logged\n";
	}

	if (gFileTestPublishedUnknownChunks > 0) {
		report << "unknown chunks: " << gFileTestPublishedUnknownChunks << "\n";
	}

	if (gFileTestPublishedUnavailableChunks > 0) {
		report << "unavailable chunks: " << gFileTestPublishedUnavailableChunks << "\n";
	}

	if (gFileTestPublishedUnavailableTrashFiles > 0) {
		report << "unavailable trash files: " << gFileTestPublishedUnavailableTrashFiles << "\n";
	}

	if (gFileTestPublishedUnavailableReservedFiles > 0) {
		report << "unavailable reserved files: " << gFileTestPublishedUnavailableReservedFiles
		       << "\n";
	}

	if (gFileTestPublishedUnavailableFiles > 0) {
		report << "unavailable files: " << gFileTestPublishedUnavailableFiles << "\n";
	}

	out.report = report.str();
	out.files = gFileTestPublishedFiles;
	out.underGoalFiles = gFileTestPublishedUnderGoalFiles;
	out.missingFiles = gFileTestPublishedMissingFiles;
	out.chunks = gFileTestPublishedChunks;
	out.underGoalChunks = gFileTestPublishedUnderGoalChunks;
	out.missingChunks = gFileTestPublishedMissingChunks;
	out.loopStart = gFileTestPublishedLoopStart;
	out.loopEnd = gFileTestPublishedLoopEnd;
}

std::vector<DefectiveFileInfo> FilesystemOperationsBase::fsTestGetDefectiveNodes(
    uint8_t requestedFlags, uint64_t maxEntries, uint64_t &cursor) {
	auto fsOpContext =
	    createFilesystemOperationContext(FilesystemOperationContext::TransactionType::kReadOnly);
	std::vector<DefectiveFileInfo> defectiveNodesInfo;
	ActiveLoopWatchdog watchdog;
	defectiveNodesInfo.reserve(maxEntries);
	auto defectiveNodeIter = gDefectiveNodes.find_nth(cursor);
	watchdog.start();
	for (uint64_t returnedEntries = 0;
	     returnedEntries < maxEntries && defectiveNodeIter != gDefectiveNodes.end();
	     ++defectiveNodeIter) {
		if (hasAnyNodeErrorFlag((*defectiveNodeIter).second, requestedFlags)) {
			auto *node =
			    nodeOperations()->idToNode<FSNode>(fsOpContext, (*defectiveNodeIter).first);
			std::string info = getNodeInfo(*this, fsOpContext, node);
			defectiveNodesInfo.emplace_back(std::move(info), (*defectiveNodeIter).second);
			++returnedEntries;
		}
		++cursor;
		if (watchdog.expired()) { return defectiveNodesInfo; }
	}
	cursor = 0;
	return defectiveNodesInfo;
}

void FilesystemOperationsBase::fsTestOnNodeRemoved(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, inode_t inode) {
	auto defectiveNodeIter = gDefectiveNodes.find(inode);
	if (defectiveNodeIter != gDefectiveNodes.end()) { gDefectiveNodes.erase(defectiveNodeIter); }
}

// ---------------------------------------------------------------------------
// Periodic checksum recalculation (in-memory backend)
// ---------------------------------------------------------------------------

void FilesystemOperationsBase::backgroundChecksumStep() {
	uint32_t recalculated = 0;

	switch (gChecksumBackgroundUpdater.getStep()) {
	case ChecksumRecalculatingStep::kNone:
		return;
	case ChecksumRecalculatingStep::kNodes:
		while (gChecksumBackgroundUpdater.getPosition() < NODEHASHSIZE) {
			auto checksumPosition = gChecksumBackgroundUpdater.getPosition();
			for (const auto &node : gMetadata->nodeHash[checksumPosition]) {
				fsnodes_checksum_add_to_background(node);
				++recalculated;
			}
			gChecksumBackgroundUpdater.incPosition();
			if (recalculated >= gChecksumBackgroundUpdater.getSpeedLimit()) { break; }
		}
		if (gChecksumBackgroundUpdater.getPosition() == NODEHASHSIZE) {
			gChecksumBackgroundUpdater.incStep();
		}
		break;
	case ChecksumRecalculatingStep::kXattrs:
		while (gChecksumBackgroundUpdater.getPosition() < XATTR_DATA_HASH_SIZE) {
			auto checksumPosition = gChecksumBackgroundUpdater.getPosition();
			for (const auto &xde : gMetadata->xattrDataHash[checksumPosition]) {
				xattr_checksum_add_to_background(xde.get());
				++recalculated;
			}
			gChecksumBackgroundUpdater.incPosition();
			if (recalculated >= gChecksumBackgroundUpdater.getSpeedLimit()) { break; }
		}
		if (gChecksumBackgroundUpdater.getPosition() == XATTR_DATA_HASH_SIZE) {
			gChecksumBackgroundUpdater.incStep();
		}
		break;
	case ChecksumRecalculatingStep::kChunks:
		if (gChunkOperations->updateChecksumABit(gChecksumBackgroundUpdater.getSpeedLimit()) ==
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

uint8_t FilesystemOperationsBase::readReservedSize(inode_t rootinode,
                                                   [[maybe_unused]] uint8_t sesflags,
                                                   uint32_t *dbuffsize) {
	if (rootinode != 0) { return SAUNAFS_ERROR_EPERM; }

	*dbuffsize = nodeOperations_->getDetachedSize(gMetadata->reserved);

	return SAUNAFS_STATUS_OK;
}

void FilesystemOperationsBase::readReservedData([[maybe_unused]] inode_t rootinode,
                                                [[maybe_unused]] uint8_t sesflags, uint8_t *dbuff) {
	nodeOperations_->getDetachedData(gMetadata->reserved, dbuff);
}

void FilesystemOperationsBase::readReserved(uint32_t off, uint32_t max_entries,
                                            std::vector<NamedInodeEntry> &entries) {
	nodeOperations_->getDetachedData(gMetadata->reserved, off, max_entries, entries);
}

void FilesystemOperationsBase::readReserved(const FilesystemOperationContext &fsOpContext,
                                            uint64_t handleOffset, uint32_t maxEntries,
                                            std::vector<HandleInodeEntry> &entries) {
	nodeOperations_->getDetachedData(fsOpContext, gMetadata->reservedHandlesIndex, handleOffset,
	                                 maxEntries, entries, false);
}

uint8_t FilesystemOperationsBase::readTrashSize(inode_t rootinode,
                                                [[maybe_unused]] uint8_t sesflags,
                                                uint32_t *dbuffsize) {
	if (rootinode != 0) { return SAUNAFS_ERROR_EPERM; }

	*dbuffsize = nodeOperations_->getDetachedSize(gMetadata->trash);

	return SAUNAFS_STATUS_OK;
}

void FilesystemOperationsBase::readTrashData([[maybe_unused]] inode_t rootinode,
                                             [[maybe_unused]] uint8_t sesflags, uint8_t *dbuff) {
	nodeOperations_->getDetachedData(gMetadata->trash, dbuff);
}

void FilesystemOperationsBase::readTrash(uint32_t off, uint32_t max_entries,
                                         std::vector<NamedInodeEntry> &entries) {
	nodeOperations_->getDetachedData(gMetadata->trash, off, max_entries, entries);
}

void FilesystemOperationsBase::readTrash(const FilesystemOperationContext &fsOpContext,
                                         uint64_t handleOffset, uint32_t maxEntries,
                                         std::vector<HandleInodeEntry> &entries) {
	nodeOperations_->getDetachedData(fsOpContext, gMetadata->trashHandlesIndex, handleOffset,
	                                 maxEntries, entries, true);
}

/* common procedure for trash and reserved files */
uint8_t FilesystemOperationsBase::getDetachedAttr(const FilesystemOperationContext &fsOpContext,
                                                  inode_t rootinode,
                                                  [[maybe_unused]] uint8_t sesflags, inode_t inode,
                                                  Attributes &attr, uint8_t dtype) {
	attr.fill(0);
	if (rootinode != 0) { return SAUNAFS_ERROR_EPERM; }
	if (!DTYPE_ISVALID(dtype)) { return SAUNAFS_ERROR_EINVAL; }

	FSNode *node = nodeOperations_->idToNode(fsOpContext, inode);
	if (node == kNodeNotFound) { return SAUNAFS_ERROR_ENOENT; }

	if (node->type != FSNodeType::kTrash && node->type != FSNodeType::kReserved) {
		return SAUNAFS_ERROR_ENOENT;
	}

	if (dtype == DTYPE_TRASH && node->type == FSNodeType::kReserved) {
		return SAUNAFS_ERROR_ENOENT;
	}

	if (dtype == DTYPE_RESERVED && node->type == FSNodeType::kTrash) {
		return SAUNAFS_ERROR_ENOENT;
	}

	nodeOperations_->fillAttr(fsOpContext, node, nullptr, node->uid, node->gid, node->uid,
	                          node->gid, sesflags, attr);

	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::getTrashPath(const FilesystemOperationContext &fsOpContext,
                                               inode_t rootinode, [[maybe_unused]] uint8_t sesflags,
                                               inode_t inode, std::string &path) {
	if (rootinode != 0) { return SAUNAFS_ERROR_EPERM; }

	FSNode *node = nodeOperations_->idToNode(fsOpContext, inode);
	if (node == kNodeNotFound) { return SAUNAFS_ERROR_ENOENT; }

	if (node->type != FSNodeType::kTrash) { return SAUNAFS_ERROR_ENOENT; }

	path = (std::string)gMetadata->trash.at(TrashPathKey(node));

	return SAUNAFS_STATUS_OK;
}

#endif

std::optional<std::string> FilesystemOperationsBase::getDetachedPath(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, const FSNode *node) {
	if (node == nullptr) { return std::nullopt; }

	if (node->type == FSNodeType::kTrash) {
		auto iter = gMetadata->trash.find(TrashPathKey(node));
		if (iter == gMetadata->trash.end()) { return std::nullopt; }
		return (std::string)(*iter).second;
	}

	if (node->type == FSNodeType::kReserved) {
		auto iter = gMetadata->reserved.find(node->id);
		if (iter == gMetadata->reserved.end()) { return std::nullopt; }
		return (std::string)(*iter).second;
	}

	return std::nullopt;
}

uint8_t FilesystemOperationsBase::setTrashPath(const FsContext &context, inode_t inode,
                                               const std::string &path) {
	ChecksumUpdater checksumUpdater(context.ts());
	FSNode *node;
	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kOnlyMeta);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);
	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kAny,
	                                              MODE_MASK_EMPTY, inode, &node);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	if (node->type != FSNodeType::kTrash) { return SAUNAFS_ERROR_ENOENT; }

	if (path.length() == 0) { return SAUNAFS_ERROR_EINVAL; }

	for (uint32_t i = 0; i < path.length(); i++) {
		if (path[i] == 0) { return SAUNAFS_ERROR_EINVAL; }
	}

	updateTrashNameEntry(gMetadata->trash, gMetadata->trashHandlesIndex,
	                     gMetadata->trashReservedToId, node, path);

	if (context.isPersonalityMaster()) {
		changeLog(fsOpContext, context.ts(), "SETPATH(%" PRIiNode ",%s)", node->id,
		          nodeOperations_->escapeName(path).c_str());
	} else {
		gMetadata->metadataVersion++;
	}
	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::undel(const FsContext &context, inode_t inode) {
	ChecksumUpdater checksumUpdater(context.ts());
	FSNode *node;
	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kOnlyMeta);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);
	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kAny,
	                                              MODE_MASK_EMPTY, inode, &node);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	if (node->type != FSNodeType::kTrash) { return SAUNAFS_ERROR_ENOENT; }

	status = nodeOperations_->undel(fsOpContext, context.ts(), static_cast<FSNodeFile *>(node));
	if (context.isPersonalityMaster()) {
		if (status == SAUNAFS_STATUS_OK) {
			changeLog(fsOpContext, context.ts(), "UNDEL(%" PRIiNode ")", node->id);
		}
	} else {
		gMetadata->metadataVersion++;
	}

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.getReadWriteTransaction()->commit()) {
			safs::log_err("undel: failed to commit transaction for inode {}", inode);
			status = SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

uint8_t FilesystemOperationsBase::purge(const FsContext &context,
                                        const FilesystemOperationContext &fsOpContext,
                                        inode_t inode) {
	ChecksumUpdater checksumUpdater(context.ts());
	FSNode *node;
	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kOnlyMeta);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kAny,
	                                              MODE_MASK_EMPTY, inode, &node);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	if (node->type != FSNodeType::kTrash) { return SAUNAFS_ERROR_ENOENT; }

	// This should be equal to inode, because `node` is not a directory
	inode_t purgedInode = node->id;
	nodeOperations_->purge(fsOpContext, context.ts(), node);

	if (context.isPersonalityMaster()) {
		changeLog(fsOpContext, context.ts(), "PURGE(%" PRIiNode ")", purgedInode);
	} else {
		gMetadata->metadataVersion++;
	}
	return SAUNAFS_STATUS_OK;
}

#ifndef METARESTORE
void FilesystemOperationsBase::getFSStats(uint64_t *totalSpace, uint64_t *availableSpace,
                                          uint64_t *trashSpace, inode_t *trashNodes,
                                          uint64_t *reservedSpace, inode_t *reservedNodes,
                                          inode_t *inodes, inode_t *directoryNodes,
                                          inode_t *fileNodes, inode_t *linkNodes) {
	matocsserv_getspace(totalSpace, availableSpace);
	*trashSpace = gMetadata->trashSpace;
	*trashNodes = gMetadata->trashNodes;
	*reservedSpace = gMetadata->reservedSpace;
	*reservedNodes = gMetadata->reservedNodes;
	*inodes = gMetadata->nodes;
	*directoryNodes = gMetadata->dirNodes;
	*fileNodes = gMetadata->fileNodes;
	*linkNodes = gMetadata->linkNodes;
}

uint8_t FilesystemOperationsBase::getRootInode(inode_t *rootinode, const uint8_t *path) {
	HString hname;
	uint32_t nameLength;
	const uint8_t *name = path;

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);
	FSNodeDirectory *parent = nodeOperations_->getRootNode(fsOpContext);

	for (;;) {
		while (*name == '/') { name++; }
		if (*name == '\0') {
			*rootinode = parent->id;
			return SAUNAFS_STATUS_OK;
		}
		nameLength = 0;
		while (name[nameLength] && name[nameLength] != '/') { nameLength++; }
		hname = HString((const char *)name, nameLength);
		if (nodeOperations_->nameCheck(hname) < 0) { return SAUNAFS_ERROR_EINVAL; }

		FSNode *child = nodeOperations_->lookup(fsOpContext, parent, hname);
		if (child == kNodeNotFound) { return SAUNAFS_ERROR_ENOENT; }
		if (child->type != FSNodeType::kDirectory) { return SAUNAFS_ERROR_ENOTDIR; }

		parent = static_cast<FSNodeDirectory *>(child);
		name += nameLength;
	}
}

void FilesystemOperationsBase::statfs(const FsContext &context,
                                      const FilesystemOperationContext &fsOpContext,
                                      uint64_t *totalspace, uint64_t *availspace, uint64_t *trspace,
                                      uint64_t *respace, inode_t *inodes) {
	FSNode *rootNode;
	StatsRecord statsRecord;
	if (context.rootinode() == SPECIAL_INODE_ROOT) {
		*trspace = gMetadata->trashSpace;
		*respace = gMetadata->reservedSpace;
		rootNode = nodeOperations_->getRootNode(fsOpContext);
	} else {
		*trspace = 0;
		*respace = 0;
		rootNode = nodeOperations_->idToNode(fsOpContext, context.rootinode());
	}
	if (!rootNode || rootNode->type != FSNodeType::kDirectory) {
		*totalspace = 0;
		*availspace = 0;
		*inodes = 0;
	} else {
		matocsserv_getspace(totalspace, availspace);
		fsnodes_quota_adjust_space(rootNode, *totalspace, *availspace);
		nodeOperations_->getStats(fsOpContext, rootNode, &statsRecord);
		*inodes = statsRecord.inodes;
	}
	incrementFSStat(FsStats::Statfs);
	metrics::Counter::increment(metrics::Counter::Master::FS_STATFS);
}
#endif /* #ifndef METARESTORE */

uint8_t FilesystemOperationsBase::applyChecksum(const std::string &version, uint64_t checksum) {
	std::string versionString = saunafsVersionToString(SAUNAFS_VERSHEX);
	uint64_t computedChecksum = metadataChecksum(ChecksumMode::kGetCurrent);
	gMetadata->metadataVersion++;
	if (!gDisableChecksumVerification && (version == versionString)) {
		if (checksum != computedChecksum) { return SAUNAFS_ERROR_BADMETADATACHECKSUM; }
	}
	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::applyAccess(const FilesystemOperationContext &fsOpContext,
                                              uint32_t timestamp, inode_t inode) {
	FSNode *node = nodeOperations_->idToNode(fsOpContext, inode);
	if (node == kNodeNotFound) { return SAUNAFS_ERROR_ENOENT; }

	node->atime = timestamp;
	fsnodes_update_checksum(node);
	gMetadata->metadataVersion++;

	// Make the change persistent for KV backends
	if (fsOpContext.hasReadWriteTransaction()) { nodeOperations_->updateNode(fsOpContext, node); }

	return SAUNAFS_STATUS_OK;
}

#ifndef METARESTORE
uint8_t FilesystemOperationsBase::access(const FsContext &context, inode_t inode, int modemask) {
	FSNode *node;

	uint8_t status = nodeOperations_->verifySession(
	    context, (modemask & MODE_MASK_W) ? OperationMode::kReadWrite : OperationMode::kReadOnly,
	    SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);

	return nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kAny,
	                                            modemask, inode, &node);
}

uint8_t FilesystemOperationsBase::lookup(const FsContext &context,
                                         const FilesystemOperationContext &fsOpContext,
                                         inode_t parent, const HString &name, inode_t *inode,
                                         Attributes &attr) {
	FSNode *workDir;
	FSNodeDirectory *effectiveRootDir;

	*inode = 0;
	attr.fill(0);

	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadOnly, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	status =
	    nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kDirectory,
	                                         MODE_MASK_X, parent, &workDir, &effectiveRootDir);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	if (!name.empty() && name[0] == '.') {
		if (name.length() == 1) {  // self
			if (workDir->id == context.rootinode()) {
				*inode = SPECIAL_INODE_ROOT;
			} else {
				*inode = workDir->id;
			}
			nodeOperations_->fillAttr(fsOpContext, workDir, workDir, context.uid(), context.gid(),
			                          context.auid(), context.agid(), context.sesflags(), attr);
			incrementFSStat(FsStats::Lookup);
			metrics::Counter::increment(metrics::Counter::Master::FS_LOOKUP);
			return SAUNAFS_STATUS_OK;
		}

		if (name.length() == 2 && name[1] == '.') {  // parent
			if (workDir->id == context.rootinode()) {
				*inode = SPECIAL_INODE_ROOT;
				nodeOperations_->fillAttr(fsOpContext, workDir, workDir, context.uid(),
				                          context.gid(), context.auid(), context.agid(),
				                          context.sesflags(), attr);
			} else {
				inode_t firstParentId = nodeOperations_->getFirstParentId(fsOpContext, workDir);
				if (firstParentId != 0) {
					FSNode *parentNode = nodeOperations_->idToNode(fsOpContext, firstParentId);
					if (parentNode == kNodeNotFound) { return SAUNAFS_ERROR_ENOENT; }
					*inode =
					    (firstParentId == context.rootinode()) ? SPECIAL_INODE_ROOT : firstParentId;
					nodeOperations_->fillAttr(fsOpContext, parentNode, workDir, context.uid(),
					                          context.gid(), context.auid(), context.agid(),
					                          context.sesflags(), attr);
				} else {
					*inode = SPECIAL_INODE_ROOT;  // rn->id;
					nodeOperations_->fillAttr(fsOpContext, effectiveRootDir, workDir, context.uid(),
					                          context.gid(), context.auid(), context.agid(),
					                          context.sesflags(), attr);
				}
			}
			incrementFSStat(FsStats::Lookup);
			metrics::Counter::increment(metrics::Counter::Master::FS_LOOKUP);
			return SAUNAFS_STATUS_OK;
		}
	}

	if (nodeOperations_->nameCheck(name) < 0) { return SAUNAFS_ERROR_EINVAL; }

	FSNode *child = nodeOperations_->lookup(fsOpContext, static_cast<FSNodeDirectory *>(workDir),
	                                        name, context.isCaseInsensitive());
	if (child == kNodeNotFound) { return SAUNAFS_ERROR_ENOENT; }

	*inode = child->id;
	nodeOperations_->fillAttr(fsOpContext, child, workDir, context.uid(), context.gid(),
	                          context.auid(), context.agid(), context.sesflags(), attr);

	incrementFSStat(FsStats::Lookup);
	metrics::Counter::increment(metrics::Counter::Master::FS_LOOKUP);
	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::wholePathLookup(const FsContext &context, inode_t parent,
                                                  const std::string &path, inode_t *found_inode,
                                                  Attributes &attr) {
	uint8_t status;
	inode_t tmpInode = context.rootinode();

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);

	auto currentIt = path.begin();
	while (currentIt != path.end()) {
		auto delimIt = std::find(currentIt, path.end(), '/');
		if (currentIt != delimIt) {
			HString pathComponent(currentIt, delimIt);
			status = lookup(context, fsOpContext, parent, pathComponent, &tmpInode, attr);
			if (status != SAUNAFS_STATUS_OK) { return status; }
			parent = tmpInode;
		}
		if (delimIt == path.end()) { break; }
		currentIt = std::next(delimIt);
	}

	*found_inode = tmpInode;
	if (tmpInode == context.rootinode()) {
		return getAttr(context, fsOpContext, SPECIAL_INODE_ROOT, attr);
	}
	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::fullPathByInode(const FsContext &context, inode_t initial_inode,
                                                  std::string &fullPath) {
	inode_t currentInode = initial_inode;
	FSNode *parentNode;
	FSNode *currentNode;
	std::string currentName;

	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadOnly, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	FilesystemOperationContext fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);

	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kAny,
	                                              MODE_MASK_R, initial_inode, &currentNode);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	if (currentInode == SPECIAL_INODE_ROOT) {
		fullPath = "";
		return SAUNAFS_STATUS_OK;
	}

	while (currentInode != context.rootinode()) {
		inode_t parentId = currentNode != kNodeNotFound
		                       ? nodeOperations_->getFirstParentId(fsOpContext, currentNode)
		                       : 0;
		if (currentNode == kNodeNotFound || parentId == 0) {
			if (currentNode != kNodeNotFound && parentId == 0 &&
			    (currentNode->type == FSNodeType::kReserved ||
			     currentNode->type == FSNodeType::kTrash)) {
				auto detachedPath = getDetachedPath(fsOpContext, currentNode);
				if (!detachedPath.has_value()) { return SAUNAFS_ERROR_ENOENT; }
				currentName =
				    *detachedPath +
				    (currentNode->type == FSNodeType::kTrash ? " (trash)" : " (reserved)");
				fullPath = currentName;
				return SAUNAFS_STATUS_OK;
			}

			return SAUNAFS_ERROR_ENOENT;
		}

		status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kAny,
		                                              MODE_MASK_R, parentId, &parentNode);
		if (status != SAUNAFS_STATUS_OK) { return status; }

		currentName = nodeOperations_->getChildNameByParentId(fsOpContext, parentId, currentNode);
		if (currentName.empty()) { return SAUNAFS_ERROR_ENOENT; }

		fullPath = currentInode == initial_inode ? currentName : currentName + "/" + fullPath;

		currentInode = parentId;
		currentNode = parentNode;
	}

	return SAUNAFS_STATUS_OK;
}

std::string FilesystemOperationsBase::fullPathByInode(const FilesystemOperationContext &fsOpContext,
                                                      inode_t initialInode) {
	std::string fullPath;
	inode_t currentInode = initialInode;
	FSNode *currentNode = nodeOperations_->idToNode(fsOpContext, currentInode);
	std::string currentName;

	while (currentInode != SPECIAL_INODE_ROOT) {
		if (currentNode == kNodeNotFound) { return ""; }
		inode_t parent = nodeOperations_->getFirstParentId(fsOpContext, currentNode);

		if (parent == 0) {
			if (currentNode->type == FSNodeType::kReserved ||
			    currentNode->type == FSNodeType::kTrash) {
				auto detachedPath = getDetachedPath(fsOpContext, currentNode);
				if (!detachedPath.has_value()) { return ""; }
				return "/" + *detachedPath +
				       (currentNode->type == FSNodeType::kTrash ? " (trash)" : " (reserved)");
			}
			break;
		}

		auto *parentNode = nodeOperations_->idToNode<FSNodeDirectory>(fsOpContext, parent);
		if (parentNode == kNodeNotFound) { return ""; }

		currentName = nodeOperations_->getChildNameByParentId(fsOpContext, parent, currentNode);
		if (currentName.empty()) { return ""; }

		fullPath = currentInode == initialInode ? currentName : currentName + "/" + fullPath;
		currentInode = parent;
		currentNode = parentNode;
	}

	fullPath = "/" + fullPath;
	return fullPath;
}

uint8_t FilesystemOperationsBase::getAttr(const FsContext &context,
                                          const FilesystemOperationContext &fsOpContext,
                                          inode_t inode, Attributes &attr) {
	FSNode *node;

	attr.fill(0);

	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadOnly, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kAny,
	                                              MODE_MASK_EMPTY, inode, &node);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	nodeOperations_->fillAttr(fsOpContext, node, nullptr, context.uid(), context.gid(),
	                          context.auid(), context.agid(), context.sesflags(), attr);

	incrementFSStat(FsStats::Getattr);
	metrics::Counter::increment(metrics::Counter::Master::FS_GETATTR);

	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::trySetLength(const FsContext &context,
                                               const FilesystemOperationContext &fsOpContext,
                                               inode_t inode, uint8_t opened, uint64_t length,
                                               bool denyTruncatingParity, uint32_t lockId,
                                               Attributes &attr, uint64_t *chunkid) {
	uint32_t timeStamp = eventloop_time();
	ChecksumUpdater checksumUpdater(timeStamp);
	FSNode *node;
	attr.fill(0);

	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kFile,
	                                              opened == 0 ? MODE_MASK_W : MODE_MASK_EMPTY,
	                                              inode, &node);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	auto *fileNode = static_cast<FSNodeFile *>(node);

	if (length & SFSCHUNKMASK) {
		uint32_t chunkIndex = (length >> SFSCHUNKBITS);
		if (chunkIndex < fileNode->chunks.size()) {
			uint64_t oldChunkId = fileNode->chunks[chunkIndex];
			if (oldChunkId > 0) {
				uint8_t status;
				uint64_t newChunkId;
				// We deny truncating parity only if truncating down
				denyTruncatingParity = denyTruncatingParity && (length < fileNode->length);
				const auto quotaCheck =
				    quotaExceeded(fsOpContext, node, {{QuotaResource::kSize, 1}});
				if (quotaCheck.status != SAUNAFS_STATUS_OK) { return quotaCheck.status; }
				status = gChunkOperations->multiTruncate(
				    fsOpContext, oldChunkId, lockId, (length & SFSCHUNKMASK), node->goal,
				    denyTruncatingParity, quotaCheck.exceeded, &newChunkId);
				if (status != SAUNAFS_STATUS_OK) { return status; }
				fileNode->chunks[chunkIndex] = newChunkId;
				*chunkid = newChunkId;
				changeLog(fsOpContext, timeStamp,
				          "TRUNC(%" PRIiNode ",%" PRIu32 ",%" PRIu32 "):%" PRIu64, node->id,
				          chunkIndex, lockId, newChunkId);
				fsnodes_update_checksum(node);

				// Make the change persistent for KV backends
				if (fsOpContext.hasReadWriteTransaction()) {
					nodeOperations_->updateNode(fsOpContext, node);
				}

				return SAUNAFS_ERROR_DELAYED;
			}
		}
	}
	nodeOperations_->fillAttr(fsOpContext, node, nullptr, context.uid(), context.gid(),
	                          context.auid(), context.agid(), context.sesflags(), attr);
	incrementFSStat(FsStats::Setattr);
	metrics::Counter::increment(metrics::Counter::Master::FS_SETATTR);
	return SAUNAFS_STATUS_OK;
}
#endif

uint8_t FilesystemOperationsBase::getCanonicalPath(const FsContext &context,
                                                   const FilesystemOperationContext &fsOpContext,
                                                   const std::string &inputPath,
                                                   std::string &canonicalPath) {
	bool caseInsensitiveFS = context.isCaseInsensitive();
	FSNode *currentNode = nodeOperations_->idToNode(fsOpContext, context.rootinode());
	std::string resultPath;

	if (!currentNode || currentNode->type != FSNodeType::kDirectory) {
		return SAUNAFS_ERROR_ENOTDIR;
	}

	auto pathIter = inputPath.begin();
	while (pathIter != inputPath.end()) {
		auto delim = std::find(pathIter, inputPath.end(), '/');
		if (pathIter != delim) {
			HString name(pathIter, delim);
			if (currentNode->type != FSNodeType::kDirectory) { return SAUNAFS_ERROR_ENOTDIR; }

			auto *dir = static_cast<FSNodeDirectory *>(currentNode);
			if (dir->caseInsensitive != caseInsensitiveFS) {
				dir->caseInsensitive = caseInsensitiveFS;
				dir->updateLowerCaseEntries();
			}

			// Case-insensitive: resolve canonical casing using getBaseStoredChildName
			std::string baseName;
			if (caseInsensitiveFS) {
				baseName = dir->getBaseStoredChildName(name);
				if (baseName.empty()) { return SAUNAFS_ERROR_ENOENT; }
			} else {
				baseName = name;
			}

			FSNode *child = nodeOperations_->lookup(fsOpContext, dir, HString(baseName));
			if (child == kNodeNotFound) { return SAUNAFS_ERROR_ENOENT; }

			if (!resultPath.empty()) { resultPath += "/"; }
			resultPath += baseName;
			currentNode = child;
		}

		if (delim == inputPath.end()) { break; }
		pathIter = std::next(delim);
	}

	canonicalPath = resultPath;
	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::applyTrunc(const FilesystemOperationContext &fsOpContext,
                                             uint32_t timestamp, inode_t inode, uint32_t indx,
                                             uint64_t chunkid, uint32_t lockid) {
	uint64_t oldChunkId;
	uint64_t newChunkId;
	uint8_t status;
	auto *nodeFile = nodeOperations_->idToNode<FSNodeFile>(fsOpContext, inode);

	if (nodeFile == kNodeNotFound) { return SAUNAFS_ERROR_ENOENT; }

	if (nodeFile->type != FSNodeType::kFile && nodeFile->type != FSNodeType::kTrash &&
	    nodeFile->type != FSNodeType::kReserved) {
		return SAUNAFS_ERROR_EINVAL;
	}

	if (indx > kMaxChunkIndex) { return SAUNAFS_ERROR_INDEXTOOBIG; }

	if (indx >= nodeFile->chunks.size()) { return SAUNAFS_ERROR_EINVAL; }

	oldChunkId = nodeFile->chunks[indx];

	if (oldChunkId == 0) {
		safs::log_err("fs_apply_trunc: node does not have a chunk at index {} chunks, inode {}",
		              indx, inode);
		return SAUNAFS_ERROR_NOCHUNK;
	}

	status = gChunkOperations->applyModification(timestamp, oldChunkId, lockid, nodeFile->goal,
	                                             true, &newChunkId);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	if (chunkid != newChunkId) { return SAUNAFS_ERROR_MISMATCH; }

	nodeFile->chunks[indx] = newChunkId;
	gMetadata->metadataVersion++;
	fsnodes_update_checksum(nodeFile);

	// Make the change persistent for KV backends
	if (fsOpContext.hasReadWriteTransaction()) {
		nodeOperations_->updateNode(fsOpContext, nodeFile);
	}

	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::setNextChunkId(const FsContext &context, uint64_t nextChunkId) {
	ChecksumUpdater checksumUpdater(context.ts());
	uint8_t status = gChunkOperations->setNextChunkId(nextChunkId);
	if (context.isPersonalityMaster()) {
		if (status == SAUNAFS_STATUS_OK) {
			// changeLog requires a FilesystemOperationContext, but this function is only reachable
			// when gChunkIdGenerator->isStrictlyMonotonic() (non-KV), so no transaction commit is
			// needed.
			auto fsOpContext = createFilesystemOperationContext(
			    FilesystemOperationContext::TransactionType::kReadWrite);

			changeLog(fsOpContext, context.ts(), "NEXTCHUNKID(%" PRIu64 ")", nextChunkId);
		}
	} else {
		gMetadata->metadataVersion++;
	}
	return status;
}

#ifndef METARESTORE
uint8_t FilesystemOperationsBase::endSetLength(const FilesystemOperationContext &fsOpContext,
                                               uint64_t chunkid) {
	uint32_t timeStamp = eventloop_time();
	ChecksumUpdater checksumUpdater(timeStamp);
	changeLog(fsOpContext, timeStamp, "UNLOCK(%" PRIu64 ")", chunkid);
	return gChunkOperations->unlock(chunkid);
}
#endif

uint8_t FilesystemOperationsBase::applyUnlock(uint64_t chunkid) {
	gMetadata->metadataVersion++;
	return gChunkOperations->unlock(chunkid);
}

#ifndef METARESTORE
uint8_t FilesystemOperationsBase::doSetLength(const FsContext &context,
                                              const FilesystemOperationContext &fsOpContext,
                                              inode_t inode, uint64_t length, Attributes &attr) {
	uint32_t timeStamp = eventloop_time();
	ChecksumUpdater checksumUpdater(timeStamp);
	FSNode *node{nullptr};

	attr.fill(0);

	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kFile,
	                                              MODE_MASK_EMPTY, inode, &node);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	// This function is called only when the file is being truncated, in
	// matoclserv_chunk_status and in matoclserv_fuse_truncate. Therefore,
	// eraseFurtherChunks should be set to true because we are setting the
	// length of the file and we should erase further chunks.
	bool eraseFurtherChunks = true;
	nodeOperations_->setLength(fsOpContext, static_cast<FSNodeFile *>(node), length,
	                           eraseFurtherChunks);
	changeLog(fsOpContext, timeStamp, "LENGTH(%" PRIiNode ",%" PRIu64 ",%" PRIu32 ")", inode,
	          static_cast<FSNodeFile *>(node)->length, static_cast<uint32_t>(eraseFurtherChunks));
	node->mtime = timeStamp;
	nodeOperations_->updateCTime(fsOpContext, node, timeStamp);
	fsnodes_update_checksum(node);
	nodeOperations_->fillAttr(fsOpContext, node, nullptr, context.uid(), context.gid(),
	                          context.auid(), context.agid(), context.sesflags(), attr);

	// Make the change persistent for KV backends
	if (fsOpContext.hasReadWriteTransaction()) { nodeOperations_->updateNode(fsOpContext, node); }

	incrementFSStat(FsStats::Setattr);
	metrics::Counter::increment(metrics::Counter::Master::FS_SETATTR);

	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::setAttr(const FsContext &context,
                                          const FilesystemOperationContext &fsOpContext,
                                          inode_t inode, uint8_t setmask, uint16_t attrmode,
                                          uint32_t attruid, uint32_t attrgid, uint32_t attratime,
                                          uint32_t attrmtime, SugidClearMode sugidclearmode,
                                          Attributes &attr) {
	uint32_t timeStamp = eventloop_time();
	ChecksumUpdater checksumUpdater(timeStamp);
	FSNode *node{nullptr};

	attr.fill(0);

	auto status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kAny,
	                                              MODE_MASK_EMPTY, inode, &node);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	if (context.uid() != 0 && (context.sesflags() & SESFLAG_MAPALL) &&
	    (setmask & (SET_UID_FLAG | SET_GID_FLAG))) {
		return SAUNAFS_ERROR_EPERM;
	}
	if ((node->mode & (EATTR_NOOWNER << EATTR_BIT_OFFSET)) == 0 && context.uid() != 0 &&
	    context.uid() != node->uid) {
		if (setmask & (SET_MODE_FLAG | SET_UID_FLAG | SET_GID_FLAG)) { return SAUNAFS_ERROR_EPERM; }
		if ((setmask & SET_ATIME_FLAG) && !(setmask & SET_ATIME_NOW_FLAG)) {
			return SAUNAFS_ERROR_EPERM;
		}
		if ((setmask & SET_MTIME_FLAG) && !(setmask & SET_MTIME_NOW_FLAG)) {
			return SAUNAFS_ERROR_EPERM;
		}
		if ((setmask & (SET_ATIME_NOW_FLAG | SET_MTIME_NOW_FLAG)) &&
		    !nodeOperations_->access(context, fsOpContext, node, MODE_MASK_W)) {
			return SAUNAFS_ERROR_EACCES;
		}
	}
	if (context.uid() != 0 && context.uid() != attruid && (setmask & SET_UID_FLAG)) {
		return SAUNAFS_ERROR_EPERM;
	}
	if ((context.sesflags() & SESFLAG_IGNOREGID) == 0) {
		if (context.uid() != 0 && (setmask & SET_GID_FLAG) && !context.hasGroup(attrgid)) {
			return SAUNAFS_ERROR_EPERM;
		}
	}
	// first ignore sugid clears done by kernel
	if ((setmask & (SET_UID_FLAG | SET_GID_FLAG)) &&
	    (setmask & SET_MODE_FLAG)) {  // chown+chmod = chown with sugid clears
		attrmode |= (node->mode & 06000);
	}
	// then do it yourself
	if ((node->mode & 06000) &&
	    (setmask &
	     (SET_UID_FLAG | SET_GID_FLAG))) {  // this is "chown" operation and suid or sgid bit is set
		switch (sugidclearmode) {
		case SugidClearMode::kAlways:
			node->mode &= 0171777;  // safest approach - always delete both suid and sgid
			attrmode &= 01777;
			break;
		case SugidClearMode::kOsx:
			if (context.uid() != 0) {  // OSX+Solaris - every change done by unprivileged user
				                       // should clear suid and sgid
				node->mode &= 0171777;
				attrmode &= 01777;
			}
			break;
		case SugidClearMode::kBsd:
			if (context.uid() != 0 && (setmask & SET_GID_FLAG) &&
			    node->gid != attrgid) {  // *BSD - like in kOsx but only when something is
				                         // actually changed
				node->mode &= 0171777;
				attrmode &= 01777;
			}
			break;
		case SugidClearMode::kExt:
			if (node->type != FSNodeType::kDirectory) {
				if (node->mode & 010) {  // when group exec is set - clear both bits
					node->mode &= 0171777;
					attrmode &= 01777;
				} else {  // when group exec is not set - clear suid only
					node->mode &= 0173777;
					attrmode &= 03777;
				}
			}
			break;
		case SugidClearMode::kSfs:
			if (node->type != FSNodeType::kDirectory) {  // similar to EXT3, but unprivileged users
				                                         // also clear suid/sgid bits on
				                                         // directories
				if (node->mode & 010) {
					node->mode &= 0171777;
					attrmode &= 01777;
				} else {
					node->mode &= 0173777;
					attrmode &= 03777;
				}
			} else if (context.uid() != 0) {
				node->mode &= 0171777;
				attrmode &= 01777;
			}
			break;
		case SugidClearMode::kNever:
			break;
		}
	}
	if (setmask & SET_MODE_FLAG) {
		node->mode = (attrmode & 07777) | (node->mode & 0xF000);
		nodeOperations_->syncAclWithMode(fsOpContext, node);
	}
	if (setmask & (SET_UID_FLAG | SET_GID_FLAG)) {
		nodeOperations_->changeUidGid(fsOpContext, node,
		                              ((setmask & SET_UID_FLAG) ? attruid : node->uid),
		                              ((setmask & SET_GID_FLAG) ? attrgid : node->gid));
	}
	if (setmask & SET_ATIME_NOW_FLAG) {
		node->atime = timeStamp;
	} else if (setmask & SET_ATIME_FLAG) {
		node->atime = attratime;
	}
	if (setmask & SET_MTIME_NOW_FLAG) {
		node->mtime = timeStamp;
	} else if (setmask & SET_MTIME_FLAG) {
		node->mtime = attrmtime;
	}
	changeLog(fsOpContext, timeStamp,
	          "ATTR(%" PRIiNode ",%d,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ")", node->id,
	          node->mode & 07777, node->uid, node->gid, node->atime, node->mtime);
	nodeOperations_->updateCTime(fsOpContext, node, timeStamp);
	nodeOperations_->fillAttr(fsOpContext, node, nullptr, context.uid(), context.gid(),
	                          context.auid(), context.agid(), context.sesflags(), attr);
	fsnodes_update_checksum(node);

	// Make persistent the changes on KV backends
	if (fsOpContext.hasReadWriteTransaction()) { nodeOperations_->updateNode(fsOpContext, node); }

	incrementFSStat(FsStats::Setattr);
	metrics::Counter::increment(metrics::Counter::Master::FS_SETATTR);
	return SAUNAFS_STATUS_OK;
}
#endif

uint8_t FilesystemOperationsBase::applyAttr(const FilesystemOperationContext &fsOpContext,
                                            uint32_t timestamp, inode_t inode, uint32_t mode,
                                            uint32_t uid, uint32_t gid, uint32_t atime,
                                            uint32_t mtime) {
	FSNode *node = nodeOperations_->idToNode(fsOpContext, inode);
	if (node == kNodeNotFound) { return SAUNAFS_ERROR_ENOENT; }

	if (mode > 07777) { return SAUNAFS_ERROR_EINVAL; }

	node->mode = mode | (node->mode & 0xF000);
	nodeOperations_->syncAclWithMode(fsOpContext, node);
	if (node->uid != uid || node->gid != gid) {
		nodeOperations_->changeUidGid(fsOpContext, node, uid, gid);
	}
	node->atime = atime;
	node->mtime = mtime;
	nodeOperations_->updateCTime(fsOpContext, node, timestamp);
	fsnodes_update_checksum(node);
	gMetadata->metadataVersion++;

	// Make persistent the changes on KV backends
	if (fsOpContext.hasReadWriteTransaction()) { nodeOperations_->updateNode(fsOpContext, node); }

	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::applyLength(const FilesystemOperationContext &fsOpContext,
                                              uint32_t timestamp, inode_t inode, uint64_t length,
                                              bool eraseFurtherChunks) {
	FSNode *node = nodeOperations_->idToNode(fsOpContext, inode);
	if (node == kNodeNotFound) { return SAUNAFS_ERROR_ENOENT; }
	if (node->type != FSNodeType::kFile && node->type != FSNodeType::kTrash &&
	    node->type != FSNodeType::kReserved) {
		return SAUNAFS_ERROR_EINVAL;
	}

	nodeOperations_->setLength(fsOpContext, static_cast<FSNodeFile *>(node), length,
	                           eraseFurtherChunks);
	node->mtime = timestamp;
	nodeOperations_->updateCTime(fsOpContext, node, timestamp);
	fsnodes_update_checksum(node);
	gMetadata->metadataVersion++;

	// Make the change persistent for KV backends
	if (fsOpContext.hasReadWriteTransaction()) { nodeOperations_->updateNode(fsOpContext, node); }

	return SAUNAFS_STATUS_OK;
}

#ifndef METARESTORE

/// Update atime of the given node and generate a changelog entry.
/// Doesn't do anything if NO_ATIME=1 is set in the config file.
static inline void fs_update_atime(const FilesystemOperationContext &fsOpContext, FSNode *p,
                                   uint32_t ts) {
	if (!gAtimeDisabled && p->atime != ts) {
		p->atime = ts;
		fsnodes_update_checksum(p);
		gFSOperations->changeLog(fsOpContext, ts, "ACCESS(%" PRIiNode ")", p->id);

		// Schedule the node update for KV backends.
		if (fsOpContext.hasReadWriteTransaction()) {
			gFSOperations->nodeOperations()->updateNode(fsOpContext, p);
		}
	}
}

uint8_t FilesystemOperationsBase::readlink(const FsContext &context,
                                           const FilesystemOperationContext &fsOpContext,
                                           inode_t inode, std::string &path) {
	uint32_t timeStamp = eventloop_time();
	ChecksumUpdater checksumUpdater(timeStamp);
	FSNode *node{nullptr};

	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadOnly, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kAny,
	                                              MODE_MASK_EMPTY, inode, &node);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	if (node->type != FSNodeType::kSymlink) { return SAUNAFS_ERROR_EINVAL; }

	path = (std::string) static_cast<FSNodeSymlink *>(node)->path;
	fs_update_atime(fsOpContext, node, timeStamp);
	incrementFSStat(FsStats::Readlink);
	metrics::Counter::increment(metrics::Counter::Master::FS_READLINK);
	return SAUNAFS_STATUS_OK;
}
#endif

uint8_t FilesystemOperationsBase::symlink(const FsContext &context,
                                          const FilesystemOperationContext &fsOpContext,
                                          inode_t parent, const HString &name,
                                          const std::string &path, inode_t *inode,
                                          Attributes *attr) {
	ChecksumUpdater checksumUpdater(context.ts());
	std::string basePath;
	FSNode *workNode;

	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kNotMeta);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = nodeOperations_->getNodeForOperation(
	    context, fsOpContext, ExpectedNodeType::kDirectory, MODE_MASK_W, parent, &workNode);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	auto *workDir = static_cast<FSNodeDirectory *>(workNode);

	// If filesystem is case-insensitive, get the canonical path for the symlink target
	if (workDir->caseInsensitive) {
		status = getCanonicalPath(context, fsOpContext, path, basePath);
		if (status != SAUNAFS_STATUS_OK) {
			// Unix-style symlinks allow dangling links, so if the path cannot be resolved,
			// we just use the original path as-is
			basePath = path;
		}
	} else {
		basePath = path;
	}

	if (basePath.length() == 0) { return SAUNAFS_ERROR_EINVAL; }

	// Check for null bytes in the symlink path
	for (uint32_t i = 0; i < basePath.length(); i++) {
		if (basePath[i] == 0) { return SAUNAFS_ERROR_EINVAL; }
	}

	if (nodeOperations_->nameCheck(name) < 0) { return SAUNAFS_ERROR_EINVAL; }

	if (nodeOperations_->isNameUsed(fsOpContext, workDir, name, context.isCaseInsensitive())) {
		return SAUNAFS_ERROR_EEXIST;
	}

	if (context.isPersonalityMaster()) {
		uint8_t quotaStatus = statusFromQuotaCheck(quotaExceededUg(
		    fsOpContext, context.uid(), context.gid(), {{QuotaResource::kInodes, 1}}));
		if (quotaStatus != SAUNAFS_STATUS_OK) { return quotaStatus; }

		quotaStatus = statusFromQuotaCheck(
		    quotaExceededDir(fsOpContext, workDir, {{QuotaResource::kInodes, 1}}));
		if (quotaStatus != SAUNAFS_STATUS_OK) { return quotaStatus; }
	}

	auto *newNode = static_cast<FSNodeSymlink *>(nodeOperations_->createNode(
	    fsOpContext, context.ts(), workDir, name, FSNodeType::kSymlink, kStandardPermissionsMask, 0,
	    context.uid(), context.gid(), 0, AclInheritance::kDontInheritAcl, *inode));

	newNode->path = HString(basePath);
	newNode->path_length = basePath.length();

	fsnodes_update_checksum(newNode);

	StatsRecord statsRecord;
	memset(&statsRecord, 0, sizeof(StatsRecord));
	statsRecord.length = basePath.length();
	nodeOperations_->addStats(fsOpContext, workDir, &statsRecord);

	if (attr != nullptr) {
		nodeOperations_->fillAttr(context, fsOpContext, newNode, workDir, *attr);
	}

	if (context.isPersonalityMaster()) {
		assert(*inode == 0);
		*inode = newNode->id;
		changeLog(fsOpContext, context.ts(),
		          "SYMLINK(%" PRIiNode ",%s,%s,%" PRIu32 ",%" PRIu32 "):%" PRIiNode, workNode->id,
		          nodeOperations_->escapeName(name).c_str(),
		          nodeOperations_->escapeName(basePath).c_str(), context.uid(), context.gid(),
		          newNode->id);
	} else {
		if (*inode != newNode->id) { return SAUNAFS_ERROR_MISMATCH; }
		gMetadata->metadataVersion++;
	}

#ifndef METARESTORE
	incrementFSStat(FsStats::Symlink);
	metrics::Counter::increment(metrics::Counter::Master::FS_SYMLINK);
#endif /* #ifndef METARESTORE */

	return SAUNAFS_STATUS_OK;
}

#ifndef METARESTORE
uint8_t FilesystemOperationsBase::mknod(const FsContext &context,
                                        const FilesystemOperationContext &fsOpContext,
                                        inode_t parent, const HString &name, FSNodeType type,
                                        uint16_t mode, uint16_t umask, uint32_t rdev,
                                        inode_t *inode, Attributes &attr) {
	uint32_t timeStamp = eventloop_time();
	ChecksumUpdater checksumUpdater(timeStamp);
	FSNode *parentNode;
	FSNode *newNode;
	*inode = 0;
	attr.fill(0);

	// Session verification
	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kNotMeta);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	// Node type verification
	if (type != FSNodeType::kFile && type != FSNodeType::kSocket && type != FSNodeType::kFifo &&
	    type != FSNodeType::kBlockDev && type != FSNodeType::kCharDev) {
		return SAUNAFS_ERROR_EINVAL;
	}

	// Get parent node
	status = nodeOperations_->getNodeForOperation(
	    context, fsOpContext, ExpectedNodeType::kDirectory, MODE_MASK_W, parent, &parentNode);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	// Name verification
	if (nodeOperations_->nameCheck(name) < 0) { return SAUNAFS_ERROR_EINVAL; }

	// Check if name is already used in the parent directory (lookup)
	bool isCaseInsensitive = context.isCaseInsensitive();

	if (nodeOperations_->isNameUsed(fsOpContext, static_cast<FSNodeDirectory *>(parentNode), name,
	                                isCaseInsensitive)) {
		return SAUNAFS_ERROR_EEXIST;
	}

	// Quota verification
	uint8_t quotaStatus = statusFromQuotaCheck(
	    quotaExceededUg(fsOpContext, context.uid(), context.gid(), {{QuotaResource::kInodes, 1}}));
	if (quotaStatus != SAUNAFS_STATUS_OK) { return quotaStatus; }

	quotaStatus = statusFromQuotaCheck(
	    quotaExceededDir(fsOpContext, parentNode, {{QuotaResource::kInodes, 1}}));
	if (quotaStatus != SAUNAFS_STATUS_OK) { return quotaStatus; }

	static_cast<FSNodeDirectory *>(parentNode)->caseInsensitive = isCaseInsensitive;

	// Create node linked to parent directory
	newNode = nodeOperations_->createNode(
	    fsOpContext, timeStamp, static_cast<FSNodeDirectory *>(parentNode), name, type, mode, umask,
	    context.uid(), context.gid(), 0, AclInheritance::kInheritAcl);

	if (type == FSNodeType::kBlockDev || type == FSNodeType::kCharDev) {
		static_cast<FSNodeDevice *>(newNode)->rdev = rdev;
	}

	*inode = newNode->id;
	nodeOperations_->fillAttr(fsOpContext, newNode, parentNode, context.uid(), context.gid(),
	                          context.auid(), context.agid(), context.sesflags(), attr);

	changeLog(fsOpContext, timeStamp,
	          "CREATE(%" PRIiNode ",%s,%c,%d,%" PRIu32 ",%" PRIu32 ",%" PRIu32 "):%" PRIiNode,
	          parentNode->id, nodeOperations_->escapeName(name).c_str(), static_cast<char>(type),
	          newNode->mode & kPermissionsMask, context.uid(), context.gid(), rdev, newNode->id);

	incrementFSStat(FsStats::Mknod);
	metrics::Counter::increment(metrics::Counter::Master::FS_MKNOD);
	fsnodes_update_checksum(newNode);

	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::mkdir(const FsContext &context,
                                        const FilesystemOperationContext &fsOpContext,
                                        inode_t parent, const HString &name, uint16_t mode,
                                        uint16_t umask, uint8_t copysgid, inode_t *inode,
                                        Attributes &attr) {
	uint32_t timeStamp = eventloop_time();
	ChecksumUpdater checksumUpdater(timeStamp);
	FSNode *workNode;
	FSNode *newNode;
	*inode = 0;
	attr.fill(0);

	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kNotMeta);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = nodeOperations_->getNodeForOperation(
	    context, fsOpContext, ExpectedNodeType::kDirectory, MODE_MASK_W, parent, &workNode);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	if (nodeOperations_->nameCheck(name) < 0) { return SAUNAFS_ERROR_EINVAL; }

	auto *workDir = static_cast<FSNodeDirectory *>(workNode);

	bool isCaseInsensitive = context.isCaseInsensitive();

	if (nodeOperations_->isNameUsed(fsOpContext, workDir, name, isCaseInsensitive)) {
		return SAUNAFS_ERROR_EEXIST;
	}

	uint8_t quotaStatus = statusFromQuotaCheck(
	    quotaExceededUg(fsOpContext, context.uid(), context.gid(), {{QuotaResource::kInodes, 1}}));
	if (quotaStatus != SAUNAFS_STATUS_OK) { return quotaStatus; }

	quotaStatus = statusFromQuotaCheck(
	    quotaExceededDir(fsOpContext, workNode, {{QuotaResource::kInodes, 1}}));
	if (quotaStatus != SAUNAFS_STATUS_OK) { return quotaStatus; }

	if (gDisableEmptyFoldersMetadataOnFullDisk) {
		if (isDepletedSpace()) {
			safs::log_err("fs_mkdir: not enough space to create a folder");
			return SAUNAFS_ERROR_NOSPACE;
		}
	}

	workDir->caseInsensitive = isCaseInsensitive;

	newNode = nodeOperations_->createNode(fsOpContext, timeStamp, workDir, name,
	                                      FSNodeType::kDirectory, mode, umask, context.uid(),
	                                      context.gid(), copysgid, AclInheritance::kInheritAcl);
	*inode = newNode->id;
	nodeOperations_->fillAttr(fsOpContext, newNode, workDir, context.uid(), context.gid(),
	                          context.auid(), context.agid(), context.sesflags(), attr);

	changeLog(fsOpContext, timeStamp,
	          "CREATE(%" PRIiNode ",%s,%c,%d,%" PRIu32 ",%" PRIu32 ",%" PRIu32 "):%" PRIiNode,
	          workDir->id, nodeOperations_->escapeName(name).c_str(),
	          static_cast<char>(FSNodeType::kDirectory), newNode->mode & kPermissionsMask,
	          context.uid(), context.gid(), 0, newNode->id);

	incrementFSStat(FsStats::Mkdir);
	metrics::Counter::increment(metrics::Counter::Master::FS_MKDIR);

	return SAUNAFS_STATUS_OK;
}
#endif

uint8_t FilesystemOperationsBase::applyCreate(const FilesystemOperationContext &fsOpContext,
                                              uint32_t timestamp, inode_t parent,
                                              const HString &name, FSNodeType type, uint32_t mode,
                                              uint32_t uid, uint32_t gid, uint32_t rdev,
                                              inode_t inode) {
	FSNode *workDir;
	FSNode *node;

	if (type != FSNodeType::kFile && type != FSNodeType::kSocket && type != FSNodeType::kFifo &&
	    type != FSNodeType::kBlockDev && type != FSNodeType::kCharDev &&
	    type != FSNodeType::kDirectory) {
		return SAUNAFS_ERROR_EINVAL;
	}

	workDir = nodeOperations_->idToNode(fsOpContext, parent);
	if (workDir == kNodeNotFound) { return SAUNAFS_ERROR_ENOENT; }
	if (workDir->type != FSNodeType::kDirectory) { return SAUNAFS_ERROR_ENOTDIR; }

	// For metadata restore operations, use default case-sensitive behavior
	if (nodeOperations_->isNameUsed(fsOpContext, static_cast<FSNodeDirectory *>(workDir), name)) {
		return SAUNAFS_ERROR_EEXIST;
	}

	// we pass requested inode number here
	node = nodeOperations_->createNode(fsOpContext, timestamp,
	                                   static_cast<FSNodeDirectory *>(workDir), name, type, mode, 0,
	                                   uid, gid, 0, AclInheritance::kInheritAcl, inode);
	if (type == FSNodeType::kBlockDev || type == FSNodeType::kCharDev) {
		static_cast<FSNodeDevice *>(node)->rdev = rdev;
		fsnodes_update_checksum(node);
	}
	if (inode != node->id) {
		// if inode!=p->id then requested inode number was already acquired
		return SAUNAFS_ERROR_MISMATCH;
	}
	gMetadata->metadataVersion++;
	return SAUNAFS_STATUS_OK;
}

#ifndef METARESTORE
uint8_t FilesystemOperationsBase::unlink(const FsContext &context,
                                         const FilesystemOperationContext &fsOpContext,
                                         inode_t parent, const HString &name) {
	uint32_t timeStamp = eventloop_time();
	ChecksumUpdater checksumUpdater(timeStamp);
	FSNode *workNode;
	HString baseName = name;

	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kNotMeta);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = nodeOperations_->getNodeForOperation(
	    context, fsOpContext, ExpectedNodeType::kDirectory, MODE_MASK_W, parent, &workNode);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	auto *workDir = static_cast<FSNodeDirectory *>(workNode);

	if (workDir->caseInsensitive) {
		std::string resolvedName = workDir->getBaseStoredChildName(name);
		if (!resolvedName.empty()) { baseName = HString(resolvedName); }
	}

	if (nodeOperations_->nameCheck(baseName) < 0) { return SAUNAFS_ERROR_EINVAL; }

	FSNode *child = nodeOperations_->lookup(fsOpContext, workDir, baseName);
	if (child == kNodeNotFound) { return SAUNAFS_ERROR_ENOENT; }

	if (!nodeOperations_->stickyAccess(workDir, child, context.uid())) {
		return SAUNAFS_ERROR_EPERM;
	}

	if (child->type == FSNodeType::kDirectory) { return SAUNAFS_ERROR_EPERM; }

	changeLog(fsOpContext, timeStamp, "UNLINK(%" PRIiNode ",%s):%" PRIiNode, workDir->id,
	          nodeOperations_->escapeName(baseName).c_str(), child->id);

	nodeOperations_->unlink(fsOpContext, timeStamp, workDir, baseName, child);

	incrementFSStat(FsStats::Unlink);
	metrics::Counter::increment(metrics::Counter::Master::FS_UNLINK);

	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::recursiveRemove(const FsContext &context, inode_t parent,
                                                  const HString &name,
                                                  const std::function<void(int)> &callback,
                                                  uint32_t job_id) {
	ChecksumUpdater checksumUpdater(context.ts());
	FSNode *wdTmp;

	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	status = nodeOperations_->getNodeForOperation(
	    context, fsOpContext, ExpectedNodeType::kDirectory, MODE_MASK_W, parent, &wdTmp);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	FSNode *child =
	    nodeOperations_->lookup(fsOpContext, static_cast<FSNodeDirectory *>(wdTmp), name);
	if (child == kNodeNotFound) { return SAUNAFS_ERROR_ENOENT; }

	auto sharedContext = std::make_shared<FsContext>(context);
	auto *task = new RemoveTask({name}, wdTmp->id, sharedContext);

	std::string nodeName;

	nodeOperations_->getPath(fsOpContext, static_cast<FSNodeDirectory *>(wdTmp), child, nodeName);
	return gMetadata->taskManager.submitTask(job_id, context.ts(), kInitialTaskBatchSize, task,
	                                         RemoveTask::generateDescription(nodeName), callback);
}

uint8_t FilesystemOperationsBase::rmdir(const FsContext &context,
                                        const FilesystemOperationContext &fsOpContext,
                                        inode_t parent, const HString &name) {
	uint32_t timeStamp = eventloop_time();
	ChecksumUpdater checksumUpdater(timeStamp);
	FSNode *workNode;

	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kNotMeta);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = nodeOperations_->getNodeForOperation(
	    context, fsOpContext, ExpectedNodeType::kDirectory, MODE_MASK_W, parent, &workNode);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	auto *workDir = static_cast<FSNodeDirectory *>(workNode);

	if (nodeOperations_->nameCheck(name) < 0) { return SAUNAFS_ERROR_EINVAL; }

	FSNode *child = nodeOperations_->lookup(fsOpContext, workDir, name);

	if (child == kNodeNotFound) { return SAUNAFS_ERROR_ENOENT; }

	if (!nodeOperations_->stickyAccess(workDir, child, context.uid())) {
		return SAUNAFS_ERROR_EPERM;
	}

	if (child->type != FSNodeType::kDirectory) { return SAUNAFS_ERROR_ENOTDIR; }

	if (nodeOperations_->getNumberOfDirEntries(fsOpContext, static_cast<FSNodeDirectory *>(child)) >
	    0) {
		return SAUNAFS_ERROR_ENOTEMPTY;
	}

	changeLog(fsOpContext, timeStamp, "UNLINK(%" PRIiNode ",%s):%" PRIiNode, workNode->id,
	          nodeOperations_->escapeName(name).c_str(), child->id);

	nodeOperations_->unlink(fsOpContext, timeStamp, workDir, name, child);

	incrementFSStat(FsStats::Rmdir);
	metrics::Counter::increment(metrics::Counter::Master::FS_RMDIR);

	return SAUNAFS_STATUS_OK;
}
#endif

uint8_t FilesystemOperationsBase::applyUnlink(const FilesystemOperationContext &fsOpContext,
                                              uint32_t timestamp, inode_t parent,
                                              const HString &name, inode_t inode) {
	FSNode *workDir = nodeOperations_->idToNode(fsOpContext, parent);
	if (workDir == kNodeNotFound) { return SAUNAFS_ERROR_ENOENT; }

	if (workDir->type != FSNodeType::kDirectory) { return SAUNAFS_ERROR_ENOTDIR; }

	FSNode *child =
	    nodeOperations_->lookup(fsOpContext, static_cast<FSNodeDirectory *>(workDir), name);
	if (child == kNodeNotFound) { return SAUNAFS_ERROR_ENOENT; }

	if (child->id != inode) { return SAUNAFS_ERROR_MISMATCH; }

	if (child->type == FSNodeType::kDirectory &&
	    !static_cast<FSNodeDirectory *>(child)->entries.empty()) {
		return SAUNAFS_ERROR_ENOTEMPTY;
	}

	nodeOperations_->unlink(fsOpContext, timestamp, static_cast<FSNodeDirectory *>(workDir), name,
	                        child);

	// Commit the transaction
	if (fsOpContext.hasReadWriteTransaction() && !fsOpContext.getReadWriteTransaction()->commit()) {
		safs::log_err("applyUnlink: failed to commit transaction");

		return SAUNAFS_ERROR_IO;
	}

	gMetadata->metadataVersion++;

	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::rename(const FsContext &context,
                                         const FilesystemOperationContext &fsOpContext,
                                         inode_t parent_src, const HString &name_src,
                                         inode_t parent_dst, const HString &name_dst,
                                         inode_t *inode, Attributes *attr) {
	ChecksumUpdater checksumUpdater(context.ts());

	HString baseNameSrc = name_src;
	HString baseNameDst = name_dst;
	FSNode *sourceWorkNode;
	FSNode *destWorkNode;

	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kAny);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = nodeOperations_->getNodeForOperation(
	    context, fsOpContext, ExpectedNodeType::kDirectory, MODE_MASK_W, parent_dst, &destWorkNode);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	auto *destWorkDir = static_cast<FSNodeDirectory *>(destWorkNode);

	status =
	    nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kDirectory,
	                                         MODE_MASK_W, parent_src, &sourceWorkNode);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	auto *sourceWorkDir = static_cast<FSNodeDirectory *>(sourceWorkNode);

	if (sourceWorkDir->caseInsensitive) {
		std::string resolvedName = sourceWorkDir->getBaseStoredChildName(name_src);
		if (!resolvedName.empty()) { baseNameSrc = HString(resolvedName); }
	}

	if (destWorkDir->caseInsensitive) {
		std::string resolvedName = destWorkDir->getBaseStoredChildName(name_dst);
		if (!resolvedName.empty()) { baseNameDst = HString(resolvedName); }
	}

	if (nodeOperations_->nameCheck(baseNameSrc) < 0) { return SAUNAFS_ERROR_EINVAL; }

	FSNode *sourceChildNode = nodeOperations_->lookup(fsOpContext, sourceWorkDir, baseNameSrc);
	if (sourceChildNode == kNodeNotFound) { return SAUNAFS_ERROR_ENOENT; }

	if (context.canCheckPermissions() &&
	    !nodeOperations_->stickyAccess(sourceWorkDir, sourceChildNode, context.uid())) {
		return SAUNAFS_ERROR_EPERM;
	}

	if ((context.personality() != metadataserver::Personality::kMaster) &&
	    (sourceChildNode->id != *inode)) {
		return SAUNAFS_ERROR_MISMATCH;
	}

	*inode = sourceChildNode->id;

	std::array<int64_t, 2> quotaDelta = {{1, 1}};

	if (sourceChildNode->type == FSNodeType::kDirectory) {
		if (nodeOperations_->isAncestor(
		        fsOpContext, static_cast<FSNodeDirectory *>(sourceChildNode), destWorkDir)) {
			return SAUNAFS_ERROR_EINVAL;
		}
		const StatsRecord &stats = static_cast<FSNodeDirectory *>(sourceChildNode)->stats;
		quotaDelta = {{(int64_t)stats.inodes, (int64_t)stats.size}};
	} else if (sourceChildNode->type == FSNodeType::kFile) {
		quotaDelta[(int)QuotaResource::kSize] =
		    nodeOperations_->getSize(fsOpContext, sourceChildNode);
	}

	if (nodeOperations_->nameCheck(baseNameDst) < 0) { return SAUNAFS_ERROR_EINVAL; }

	FSNode *destinationChildNode = nodeOperations_->lookup(fsOpContext, destWorkDir, baseNameDst);

	if (destinationChildNode == sourceChildNode) { return SAUNAFS_STATUS_OK; }

	if (destinationChildNode != kNodeNotFound) {
		if (destinationChildNode->type == FSNodeType::kDirectory &&
		    !static_cast<FSNodeDirectory *>(destinationChildNode)->entries.empty()) {
			return SAUNAFS_ERROR_ENOTEMPTY;
		}

		if (context.canCheckPermissions() &&
		    !nodeOperations_->stickyAccess(destWorkNode, destinationChildNode, context.uid())) {
			return SAUNAFS_ERROR_EPERM;
		}

		if (destinationChildNode->type == FSNodeType::kDirectory) {
			const StatsRecord &stats = static_cast<FSNodeDirectory *>(destinationChildNode)->stats;
			quotaDelta[(int)QuotaResource::kInodes] -= stats.inodes;
			quotaDelta[(int)QuotaResource::kSize] -= stats.size;
		} else if (destinationChildNode->type == FSNodeType::kFile) {
			quotaDelta[(int)QuotaResource::kInodes] -= 1;
			quotaDelta[(int)QuotaResource::kSize] -= nodeOperations_->getSize(
			    fsOpContext, static_cast<FSNodeFile *>(destinationChildNode));
		} else {
			quotaDelta[(int)QuotaResource::kInodes] -= 1;
			quotaDelta[(int)QuotaResource::kSize] -= 1;
		}
	}

	uint8_t quotaStatus = statusFromQuotaCheck(
	    quotaExceededDirMove(fsOpContext, destWorkDir, sourceWorkDir,
	                         {{QuotaResource::kInodes, quotaDelta[(int)QuotaResource::kInodes]},
	                          {QuotaResource::kSize, quotaDelta[(int)QuotaResource::kSize]}}));
	if (quotaStatus != SAUNAFS_STATUS_OK) { return quotaStatus; }

	if (destinationChildNode != kNodeNotFound) {
		nodeOperations_->unlink(fsOpContext, context.ts(), destWorkDir, baseNameDst,
		                        destinationChildNode);
	}

	nodeOperations_->removeEdge(fsOpContext, context.ts(), sourceWorkDir, baseNameSrc,
	                            sourceChildNode);

	nodeOperations_->link(fsOpContext, context.ts(), destWorkDir, sourceChildNode, baseNameDst);

	if (attr) {
		nodeOperations_->fillAttr(context, fsOpContext, sourceChildNode, destWorkNode, *attr);
	}

	if (context.isPersonalityMaster()) {
		changeLog(fsOpContext, context.ts(), "MOVE(%" PRIiNode ",%s,%" PRIiNode ",%s):%" PRIiNode,
		          sourceWorkNode->id, nodeOperations_->escapeName(baseNameSrc).c_str(),
		          destWorkNode->id, nodeOperations_->escapeName(baseNameDst).c_str(),
		          sourceChildNode->id);
	} else {
		gMetadata->metadataVersion++;
	}

#ifndef METARESTORE
	incrementFSStat(FsStats::Rename);
	metrics::Counter::increment(metrics::Counter::Master::FS_RENAME);
#endif

	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::link(const FsContext &context,
                                       const FilesystemOperationContext &fsOpContext,
                                       inode_t inode_src, inode_t parent_dst,
                                       const HString &name_dst, inode_t *inode, Attributes *attr) {
	ChecksumUpdater checksumUpdater(context.ts());
	FSNode *sourceNode;
	FSNode *destinationDirNode;

	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kNotMeta);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	status =
	    nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kDirectory,
	                                         MODE_MASK_W, parent_dst, &destinationDirNode);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	status =
	    nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kNotDirectory,
	                                         MODE_MASK_EMPTY, inode_src, &sourceNode);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	if (sourceNode->type == FSNodeType::kTrash || sourceNode->type == FSNodeType::kReserved) {
		return SAUNAFS_ERROR_ENOENT;
	}

	if (nodeOperations_->nameCheck(name_dst) < 0) { return SAUNAFS_ERROR_EINVAL; }

	if (nodeOperations_->isNameUsed(fsOpContext, static_cast<FSNodeDirectory *>(destinationDirNode),
	                                name_dst, context.isCaseInsensitive())) {
		return SAUNAFS_ERROR_EEXIST;
	}

	nodeOperations_->link(fsOpContext, context.ts(),
	                      static_cast<FSNodeDirectory *>(destinationDirNode), sourceNode, name_dst);

	if (inode) { *inode = inode_src; }

	if (attr) {
		nodeOperations_->fillAttr(context, fsOpContext, sourceNode, destinationDirNode, *attr);
	}

	if (context.isPersonalityMaster()) {
		changeLog(fsOpContext, context.ts(), "LINK(%" PRIiNode ",%" PRIiNode ",%s)", sourceNode->id,
		          destinationDirNode->id, nodeOperations_->escapeName(name_dst).c_str());
	} else {
		gMetadata->metadataVersion++;
	}

#ifndef METARESTORE
	incrementFSStat(FsStats::Link);
	metrics::Counter::increment(metrics::Counter::Master::FS_LINK);
#endif
	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::append(const FsContext &context,
                                         const FilesystemOperationContext &fsOpContext,
                                         inode_t inode, inode_t inode_src) {
	ChecksumUpdater checksumUpdater(context.ts());
	FSNode *targetNode;
	FSNode *sourceNode;

	if (inode == inode_src) { return SAUNAFS_ERROR_EINVAL; }
	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kFile,
	                                              MODE_MASK_W, inode, &targetNode);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kFile,
	                                              MODE_MASK_R, inode_src, &sourceNode);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	if (context.isPersonalityMaster()) {
		uint8_t quotaStatus = statusFromQuotaCheck(
		    quotaExceeded(fsOpContext, targetNode, {{QuotaResource::kSize, 1}}));
		if (quotaStatus != SAUNAFS_STATUS_OK) { return quotaStatus; }
	}
	status = nodeOperations_->appendChunks(fsOpContext, context.ts(),
	                                       static_cast<FSNodeFile *>(targetNode),
	                                       static_cast<FSNodeFile *>(sourceNode));
	if (status != SAUNAFS_STATUS_OK) { return status; }

	// Make append changes persistent for KV backends.
	if (fsOpContext.hasReadWriteTransaction()) {
		nodeOperations_->updateNode(fsOpContext, targetNode);
		nodeOperations_->updateNode(fsOpContext, sourceNode);
	}

	if (context.isPersonalityMaster()) {
		changeLog(fsOpContext, context.ts(), "APPEND(%" PRIiNode ",%" PRIiNode ")", targetNode->id,
		          sourceNode->id);
	} else {
		gMetadata->metadataVersion++;
	}
	return status;
}

int FilesystemOperationsBase::checkLockPermissions(const FsContext &context,
                                                   const FilesystemOperationContext &fsOpContext,
                                                   inode_t inode, uint16_t op) {
	FSNode *dummy;
	uint8_t modemask = MODE_MASK_EMPTY;

	if (op == safs_locks::kExclusive) {
		modemask = MODE_MASK_W;
	} else if (op == safs_locks::kShared) {
		modemask = MODE_MASK_R;
	}

	return gFSOperations->nodeOperations()->getNodeForOperation(
	    context, fsOpContext, ExpectedNodeType::kAny, modemask, inode, &dummy);
}

int FilesystemOperationsBase::posixLockProbe(const FsContext &context,
                                             const FilesystemOperationContext &fsOpContext,
                                             inode_t inode, uint64_t start, uint64_t end,
                                             uint64_t owner, uint32_t sessionid, uint32_t reqid,
                                             uint32_t msgid, uint16_t oper,
                                             safs_locks::FlockWrapper &info) {
	if (oper != safs_locks::kShared && oper != safs_locks::kExclusive &&
	    oper != safs_locks::kUnlock) {
		return SAUNAFS_ERROR_EINVAL;
	}

	uint8_t status = checkLockPermissions(context, fsOpContext, inode, oper);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	FileLocks &locks = gMetadata->posixLocks;
	const FileLocks::Lock *collision =
	    locks.findCollision(inode, static_cast<FileLocks::Lock::Type>(oper), start, end,
	                        FileLocks::Owner{owner, sessionid, reqid, msgid});

	if (collision == nullptr) {
		info.l_type = safs_locks::kUnlock;
		return SAUNAFS_STATUS_OK;
	}

	info.l_type = static_cast<int>(collision->type);
	info.l_start = collision->start;
	info.l_len =
	    std::min<uint64_t>(collision->end - collision->start, std::numeric_limits<int64_t>::max());

	return SAUNAFS_ERROR_WAITING;
}

int FilesystemOperationsBase::lockOperation(
    const FsContext &context, const FilesystemOperationContext &fsOpContext, FileLocks &locks,
    inode_t inode, uint64_t start, uint64_t end, uint64_t owner, uint32_t sessionid, uint32_t reqid,
    uint32_t msgid, uint16_t oper, bool nonblocking, std::vector<FileLocks::Owner> &applied) {
	uint8_t status = checkLockPermissions(context, fsOpContext, inode, oper);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	FileLocks::LockQueue queue;
	bool success = false;

	switch (oper) {
	case safs_locks::kShared:
		success = locks.sharedLock(inode, start, end,
		                           FileLocks::Owner{owner, sessionid, reqid, msgid}, nonblocking);
		break;
	case safs_locks::kExclusive:
		success = locks.exclusiveLock(
		    inode, start, end, FileLocks::Owner{owner, sessionid, reqid, msgid}, nonblocking);
		break;
	case safs_locks::kRelease:
		locks.removePending(inode, [sessionid, owner](const FileLocks::Lock &lock) {
			const FileLocks::Lock::Owner &lockOwner = lock.owner();
			return lockOwner.sessionid == sessionid && lockOwner.owner == owner;
		});
		start = 0;
		end = std::numeric_limits<uint64_t>::max();
		/* fallthrough */
	case safs_locks::kUnlock:
		success = locks.unlock(inode, start, end, FileLocks::Owner{owner, sessionid, reqid, msgid});
		break;
	default:
		return SAUNAFS_ERROR_EINVAL;
	}
	status = success ? SAUNAFS_STATUS_OK : SAUNAFS_ERROR_WAITING;

	// If lock is exclusive, no further action is required
	// For shared locks it is required to gather candidates for lock.
	// The case when it is needed is when the owner had exclusive lock applied to a file range
	// and he issued shared lock for this same range. This converts exclusive lock
	// to shared lock. In the result we may need to apply other shared pending locks
	// for this range.
	if (oper == safs_locks::kExclusive) { return status; }

	locks.gatherCandidates(inode, start, end, queue);
	for (auto &candidate : queue) {
		if (locks.apply(inode, candidate)) {
			applied.insert(applied.end(), candidate.owners.begin(), candidate.owners.end());
		}
	}
	return status;
}

int FilesystemOperationsBase::flockOperation(const FsContext &context,
                                             const FilesystemOperationContext &fsOpContext,
                                             inode_t inode, uint64_t owner, uint32_t sessionid,
                                             uint32_t reqid, uint32_t msgid, uint16_t oper,
                                             bool nonblocking,
                                             std::vector<FileLocks::Owner> &applied) {
	ChecksumUpdater checksumUpdater(context.ts());
	int lockResult = lockOperation(context, fsOpContext, gMetadata->flockLocks, inode, 0, 1, owner,
	                               sessionid, reqid, msgid, oper, nonblocking, applied);
	if (context.isPersonalityMaster()) {
		changeLog(fsOpContext, context.ts(),
		          "FLCK(%" PRIu8 ",%" PRIiNode ",0,1,%" PRIu64 ",%" PRIu32 ",%" PRIu16 ")",
		          (uint8_t)safs_locks::Type::kFlock, inode, owner, sessionid, oper);
	} else {
		gMetadata->metadataVersion++;
	}

	return lockResult;
}

int FilesystemOperationsBase::posixLockOperation(const FsContext &context,
                                                 const FilesystemOperationContext &fsOpContext,
                                                 inode_t inode, uint64_t start, uint64_t end,
                                                 uint64_t owner, uint32_t sessionid, uint32_t reqid,
                                                 uint32_t msgid, uint16_t oper, bool nonblocking,
                                                 std::vector<FileLocks::Owner> &applied) {
	ChecksumUpdater checksumUpdater(context.ts());
	int lockResult = lockOperation(context, fsOpContext, gMetadata->posixLocks, inode, start, end,
	                               owner, sessionid, reqid, msgid, oper, nonblocking, applied);
	if (context.isPersonalityMaster()) {
		changeLog(fsOpContext, context.ts(),
		          "FLCK(%" PRIu8 ",%" PRIiNode ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu32
		          ",%" PRIu16 ")",
		          (uint8_t)safs_locks::Type::kPosix, inode, start, end, owner, sessionid, oper);
	} else {
		gMetadata->metadataVersion++;
	}
	return lockResult;
}

int FilesystemOperationsBase::locksClearSession(
    const FsContext &context, [[maybe_unused]] const FilesystemOperationContext &fsOpContext,
    uint8_t type, inode_t inode, uint32_t sessionid, std::vector<FileLocks::Owner> &applied) {
	if (type != (uint8_t)safs_locks::Type::kFlock && type != (uint8_t)safs_locks::Type::kPosix) {
		return SAUNAFS_ERROR_EINVAL;
	}

	ChecksumUpdater checksumUpdater(context.ts());

	FileLocks *locks =
	    type == (uint8_t)safs_locks::Type::kFlock ? &gMetadata->flockLocks : &gMetadata->posixLocks;

	locks->removePending(inode, [sessionid](const FileLocks::Lock &lock) {
		return lock.owner().sessionid == sessionid;
	});
	std::pair<uint64_t, uint64_t> range = locks->unlock(
	    inode,
	    [sessionid](const FileLocks::Lock::Owner &owner) { return owner.sessionid == sessionid; });

	if (range.first < range.second) {
		FileLocks::LockQueue queue;
		locks->gatherCandidates(inode, range.first, range.second, queue);
		for (auto &candidate : queue) {
			applied.insert(applied.end(), candidate.owners.begin(), candidate.owners.end());
		}
	}
	if (context.isPersonalityMaster()) {
		changeLog(fsOpContext, context.ts(), "CLRLCK(%" PRIu8 ",%" PRIiNode ",%" PRIu32 ")", type,
		          inode, sessionid);
	} else {
		gMetadata->metadataVersion++;
	}

	return SAUNAFS_STATUS_OK;
}

int FilesystemOperationsBase::locksListAll(
    [[maybe_unused]] const FsContext &context,
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, uint8_t type, bool pending,
    uint64_t start, uint64_t max, std::vector<safs_locks::Info> &outLocks) {
	FileLocks *locks;
	if (type == (uint8_t)safs_locks::Type::kFlock) {
		locks = &gMetadata->flockLocks;
	} else if (type == (uint8_t)safs_locks::Type::kPosix) {
		locks = &gMetadata->posixLocks;
	} else {
		return SAUNAFS_ERROR_EINVAL;
	}

	if (pending) {
		locks->copyPendingToVector(start, max, outLocks);
	} else {
		locks->copyActiveToVector(start, max, outLocks);
	}

	return SAUNAFS_STATUS_OK;
}

int FilesystemOperationsBase::locksListInode(
    [[maybe_unused]] const FsContext &context,
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, uint8_t type, bool pending,
    inode_t inode, uint64_t start, uint64_t max, std::vector<safs_locks::Info> &outLocks) {
	FileLocks *locks;

	if (type == (uint8_t)safs_locks::Type::kFlock) {
		locks = &gMetadata->flockLocks;
	} else if (type == (uint8_t)safs_locks::Type::kPosix) {
		locks = &gMetadata->posixLocks;
	} else {
		return SAUNAFS_ERROR_EINVAL;
	}

	if (pending) {
		locks->copyPendingToVector(inode, start, max, outLocks);
	} else {
		locks->copyActiveToVector(inode, start, max, outLocks);
	}

	return SAUNAFS_STATUS_OK;
}

void FilesystemOperationsBase::manageLockTryLockPending(FileLocks &locks, inode_t inode,
                                                        uint64_t start, uint64_t end,
                                                        std::vector<FileLocks::Owner> &applied) {
	FileLocks::LockQueue queue;
	locks.gatherCandidates(inode, start, end, queue);
	for (auto &candidate : queue) {
		if (locks.apply(inode, candidate)) {
			applied.insert(applied.end(), candidate.owners.begin(), candidate.owners.end());
		}
	}
}

int FilesystemOperationsBase::locksUnlockInode(
    const FsContext &context, [[maybe_unused]] const FilesystemOperationContext &fsOpContext,
    uint8_t type, inode_t inode, std::vector<FileLocks::Owner> &applied) {
	ChecksumUpdater checksumUpdater(context.ts());

	if (type == (uint8_t)safs_locks::Type::kFlock) {
		gMetadata->flockLocks.unlock(inode);
		manageLockTryLockPending(gMetadata->flockLocks, inode, 0, 1, applied);
	} else if (type == (uint8_t)safs_locks::Type::kPosix) {
		gMetadata->posixLocks.unlock(inode);
		manageLockTryLockPending(gMetadata->posixLocks, inode, 0,
		                         std::numeric_limits<uint64_t>::max(), applied);
	} else {
		return SAUNAFS_ERROR_EINVAL;
	}

	if (context.isPersonalityMaster()) {
		changeLog(fsOpContext, context.ts(), "FLCKINODE(%" PRIu8 ",%" PRIiNode ")", type, inode);
	} else {
		gMetadata->metadataVersion++;
	}

	return SAUNAFS_STATUS_OK;
}

int FilesystemOperationsBase::locksRemovePending(
    const FsContext &context, [[maybe_unused]] const FilesystemOperationContext &fsOpContext,
    uint8_t type, uint64_t ownerid, uint32_t sessionid, inode_t inode, uint64_t reqid) {
	ChecksumUpdater checksumUpdater(context.ts());

	FileLocks *locks;

	if (type == (uint8_t)safs_locks::Type::kFlock) {
		locks = &gMetadata->flockLocks;
	} else if (type == (uint8_t)safs_locks::Type::kPosix) {
		locks = &gMetadata->posixLocks;
	} else {
		return SAUNAFS_ERROR_EINVAL;
	}

	locks->removePending(inode, [ownerid, sessionid, reqid](const LockRange &range) {
		const LockRange::Owner &owner = range.owner();
		return owner.owner == ownerid && owner.sessionid == sessionid && owner.reqid == reqid;
	});

	if (context.isPersonalityMaster()) {
		changeLog(fsOpContext, context.ts(),
		          "RMPLOCK(%" PRIu8 ",%" PRIu64 ",%" PRIu32 ",%" PRIiNode ",%" PRIu64 ")", type,
		          ownerid, sessionid, inode, reqid);
	} else {
		gMetadata->metadataVersion++;
	}

	return SAUNAFS_STATUS_OK;
}

#ifndef METARESTORE

uint8_t FilesystemOperationsBase::readdirSize(const FsContext &context, inode_t inode,
                                              uint8_t flags, void **dnode, uint32_t *dbuffsize) {
	FSNode *node;
	*dnode = nullptr;
	*dbuffsize = 0;

	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadOnly, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);

	status = nodeOperations_->getNodeForOperation(
	    context, fsOpContext, ExpectedNodeType::kDirectory, MODE_MASK_R, inode, &node);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	*dnode = node;
	*dbuffsize = nodeOperations_->getDirSize(static_cast<FSNodeDirectory *>(node),
	                                         flags & GETDIR_FLAG_WITHATTR);
	return SAUNAFS_STATUS_OK;
}

void FilesystemOperationsBase::readdirData(const FsContext &context,
                                           const FilesystemOperationContext &fsOpContext,
                                           uint8_t flags, void *dnode, uint8_t *dbuff) {
	uint32_t timeStamp = eventloop_time();
	ChecksumUpdater checksumUpdater(timeStamp);
	FSNode *node = (FSNode *)dnode;

	fs_update_atime(fsOpContext, node, timeStamp);

	nodeOperations_->getDirData(fsOpContext, context.rootinode(), context.uid(), context.gid(),
	                            context.auid(), context.agid(), context.sesflags(),
	                            static_cast<FSNodeDirectory *>(node), dbuff,
	                            flags & GETDIR_FLAG_WITHATTR);
	incrementFSStat(FsStats::Readdir);
	metrics::Counter::increment(metrics::Counter::Master::FS_READDIR);
}

uint8_t FilesystemOperationsBase::readdir(const FsContext &context,
                                          const FilesystemOperationContext &fsOpContext,
                                          inode_t inode, uint64_t first_entry,
                                          uint64_t number_of_entries,
                                          std::vector<DirectoryEntry> &dir_entries) {
	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadOnly, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	FSNode *dir;
	status = nodeOperations_->getNodeForOperation(
	    context, fsOpContext, ExpectedNodeType::kDirectory, MODE_MASK_R, inode, &dir);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	uint32_t timeStamp = eventloop_time();
	ChecksumUpdater checksumUpdater(timeStamp);

	fs_update_atime(fsOpContext, dir, timeStamp);

	nodeOperations_->getDir(fsOpContext, context.rootinode(), context.uid(), context.gid(),
	                        context.auid(), context.agid(), context.sesflags(),
	                        static_cast<FSNodeDirectory *>(dir), first_entry, number_of_entries,
	                        dir_entries);

	incrementFSStat(FsStats::Readdir);
	metrics::Counter::increment(metrics::Counter::Master::FS_READDIR);

	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::checkFile(const FsContext &context, inode_t inode,
                                            ChunkCountArray &chunkCount) {
	FSNode *node;

	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadOnly, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);

	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kFile,
	                                              MODE_MASK_EMPTY, inode, &node);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	nodeOperations_->checkFile(static_cast<FSNodeFile *>(node), chunkCount);
	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::openCheck(const FsContext &context,
                                            const FilesystemOperationContext &fsOpContext,
                                            inode_t inode, uint8_t flags, Attributes &attr) {
	FSNode *node;

	uint8_t status = nodeOperations_->verifySession(
	    context, (flags & WANT_WRITE) ? OperationMode::kReadWrite : OperationMode::kReadOnly,
	    SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kFile,
	                                              MODE_MASK_EMPTY, inode, &node);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	if ((flags & AFTER_CREATE) == 0) {
		uint8_t modemask = 0;
		if (flags & WANT_READ) { modemask |= MODE_MASK_R; }
		if (flags & WANT_WRITE) { modemask |= MODE_MASK_W; }
		if (!nodeOperations_->access(context, fsOpContext, node, modemask)) {
			return SAUNAFS_ERROR_EACCES;
		}
	}
	nodeOperations_->fillAttr(fsOpContext, node, nullptr, context.uid(), context.gid(),
	                          context.auid(), context.agid(), context.sesflags(), attr);
	incrementFSStat(FsStats::Open);
	metrics::Counter::increment(metrics::Counter::Master::FS_OPEN);
	return SAUNAFS_STATUS_OK;
}
#endif

uint8_t FilesystemOperationsBase::acquire(const FsContext &context,
                                          const FilesystemOperationContext &fsOpContext,
                                          inode_t inode, uint32_t sessionid) {
	ChecksumUpdater checksumUpdater(context.ts());
#ifndef METARESTORE
	if (context.isPersonalityShadow()) { matoclserv_add_open_file(sessionid, inode); }
#endif /* #ifndef METARESTORE */
	auto *fileNode = nodeOperations_->idToNode<FSNodeFile>(fsOpContext, inode);
	if (fileNode == kNodeNotFound) { return SAUNAFS_ERROR_ENOENT; }

	if (fileNode->type != FSNodeType::kFile && fileNode->type != FSNodeType::kTrash &&
	    fileNode->type != FSNodeType::kReserved) {
		return SAUNAFS_ERROR_EPERM;
	}

	if (std::find(fileNode->sessionIds.begin(), fileNode->sessionIds.end(), sessionid) !=
	    fileNode->sessionIds.end()) {
		return SAUNAFS_ERROR_EINVAL;
	}

	fileNode->sessionIds.push_back(sessionid);

	fsnodes_update_checksum(fileNode);

	// Persist the changes in KV backends
	if (fsOpContext.hasReadWriteTransaction()) {
		nodeOperations_->updateNode(fsOpContext, fileNode);
	}

	if (context.isPersonalityMaster()) {
		changeLog(fsOpContext, context.ts(), "ACQUIRE(%" PRIiNode ",%" PRIu32 ")", inode,
		          sessionid);
	} else {
		gMetadata->metadataVersion++;
	}

	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::release(const FsContext &context,
                                          const FilesystemOperationContext &fsOpContext,
                                          inode_t inode, uint32_t sessionid) {
	ChecksumUpdater checksumUpdater(context.ts());

	auto *fileNode = nodeOperations_->idToNode<FSNodeFile>(fsOpContext, inode);
	if (fileNode == kNodeNotFound) { return SAUNAFS_ERROR_ENOENT; }

	if (fileNode->type != FSNodeType::kFile && fileNode->type != FSNodeType::kTrash &&
	    fileNode->type != FSNodeType::kReserved) {
		return SAUNAFS_ERROR_EPERM;
	}

	auto iter = std::find(fileNode->sessionIds.begin(), fileNode->sessionIds.end(), sessionid);

	if (iter != fileNode->sessionIds.end()) {
		fileNode->sessionIds.erase(iter);

		if (fileNode->type == FSNodeType::kReserved && fileNode->sessionIds.empty()) {
			nodeOperations_->purge(fsOpContext, context.ts(), fileNode);
		} else {
			fsnodes_update_checksum(fileNode);

			// Persist the changes in KV backends
			if (fsOpContext.hasReadWriteTransaction()) {
				nodeOperations_->updateNode(fsOpContext, fileNode);
			}
		}

#ifndef METARESTORE
		if (context.isPersonalityShadow()) { matoclserv_remove_open_file(sessionid, inode); }
#endif /* #ifndef METARESTORE */

		if (context.isPersonalityMaster()) {
			changeLog(fsOpContext, context.ts(), "RELEASE(%" PRIiNode ",%" PRIu32 ")", inode,
			          sessionid);
		} else {
			gMetadata->metadataVersion++;
		}

		return SAUNAFS_STATUS_OK;
	}

#ifndef METARESTORE
	safs::log_warn("{}: session {} not found for inode {}", __func__, sessionid, inode);
#endif

	return SAUNAFS_ERROR_EINVAL;
}

#ifndef METARESTORE
uint32_t FilesystemOperationsBase::newSessionId() {
	uint32_t timeStamp = eventloop_time();
	ChecksumUpdater checksumUpdater(timeStamp);

	const uint32_t current = gMetadata->nextSessionId().getValue();

	// Used so far only for the changeLog signature
	auto fsOpContext =
	    createFilesystemOperationContext(FilesystemOperationContext::TransactionType::kReadWrite);
	changeLog(fsOpContext, timeStamp, "SESSION():%" PRIu32, current);

	gMetadata->nextSessionId().increment();

	return current;
}
#endif

uint8_t FilesystemOperationsBase::applySession(uint32_t sessionid) {
	if (sessionid != gMetadata->nextSessionId().getValue()) { return SAUNAFS_ERROR_MISMATCH; }
	gMetadata->metadataVersion++;
	gMetadata->nextSessionId().increment();
	return SAUNAFS_STATUS_OK;
}

#ifndef METARESTORE
uint8_t fs_auto_repair_if_needed(FSNodeFile *p, uint32_t chunkIndex) {
	uint64_t chunkId = (chunkIndex < p->chunks.size() ? p->chunks[chunkIndex] : 0);
	if (chunkId != 0 && gChunkOperations->hasOnlyInvalidCopies(chunkId)) {
		uint32_t notchanged, erased, repaired;
		FsContext context =
		    FsContext::getForMasterWithSession(0, SPECIAL_INODE_ROOT, 0, 0, 0, 0, 0);
		gFSOperations->repair(context, p->id, 0, &notchanged, &erased, &repaired);
		safs_pretty_syslog(LOG_NOTICE,
		                   "auto repair inode %" PRIiNode ", chunk %016" PRIX64
		                   ": "
		                   "not changed: %" PRIu32 ", erased: %" PRIu32 ", repaired: %" PRIu32,
		                   p->id, chunkId, notchanged, erased, repaired);
		safs_silent_syslog(LOG_DEBUG, "master.fs.file_auto_repaired: %" PRIiNode " %" PRIu32, p->id,
		                   repaired);
	}
	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::readChunk(const FilesystemOperationContext &fsOpContext,
                                            inode_t inode, uint32_t indx, uint64_t *chunkid,
                                            uint64_t *length) {
	uint32_t timeStamp = eventloop_time();
	ChecksumUpdater checksumUpdater(timeStamp);

	*chunkid = 0;
	*length = 0;

	auto *nodeFile = nodeOperations_->idToNode<FSNodeFile>(fsOpContext, inode);
	if (nodeFile == kNodeNotFound) { return SAUNAFS_ERROR_ENOENT; }
	if (nodeFile->type != FSNodeType::kFile && nodeFile->type != FSNodeType::kTrash &&
	    nodeFile->type != FSNodeType::kReserved) {
		return SAUNAFS_ERROR_EPERM;
	}

	if (indx > kMaxChunkIndex) { return SAUNAFS_ERROR_INDEXTOOBIG; }
#ifndef METARESTORE
	if (gMagicAutoFileRepair) { fs_auto_repair_if_needed(nodeFile, indx); }
#endif
	if (indx < nodeFile->chunks.size()) { *chunkid = nodeFile->chunks[indx]; }
	*length = nodeFile->length;
	fs_update_atime(fsOpContext, nodeFile, timeStamp);
	incrementFSStat(FsStats::Read);
	metrics::Counter::increment(metrics::Counter::Master::FS_READ);
	return SAUNAFS_STATUS_OK;
}
#endif

uint8_t FilesystemOperationsBase::writeChunk(const FsContext &context,
                                             const FilesystemOperationContext &fsOpContext,
                                             inode_t inode, uint32_t index,
                                             /* inout */ uint32_t *lockid, uint64_t *chunkid,
                                             uint8_t *opflag, uint64_t *length,
                                             [[maybe_unused]] uint32_t min_server_version) {
	ChecksumUpdater checksumUpdater(context.ts());
	uint64_t oldChunkId;
	uint64_t newChunkId;
	FSNode *node;

	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kFile,
	                                              MODE_MASK_EMPTY, inode, &node);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	if (index > kMaxChunkIndex) { return SAUNAFS_ERROR_INDEXTOOBIG; }

	auto *fileNode = static_cast<FSNodeFile *>(node);

#ifndef METARESTORE
	if (gMagicAutoFileRepair && context.isPersonalityMaster()) {
		fs_auto_repair_if_needed(fileNode, index);
	}
#endif

	const auto quotaCheck = quotaExceeded(fsOpContext, fileNode, {{QuotaResource::kSize, 1}});
	if (quotaCheck.status != SAUNAFS_STATUS_OK) { return quotaCheck.status; }
	const bool isQuotaExceeded = quotaCheck.exceeded;

	// Cache original stats for quota and parent update
	StatsRecord originalStats;
	nodeOperations_->getStats(fsOpContext, fileNode, &originalStats);

	/* resize chunks structure */
	if (index >= fileNode->chunks.size()) {
		if (context.isPersonalityMaster() && isQuotaExceeded) { return SAUNAFS_ERROR_QUOTA; }
		uint32_t newSize;
		if (index < 8) {
			newSize = index + 1;
		} else if (index < 64) {
			newSize = (index & 0xFFFFFFF8) + 8;
		} else {
			newSize = (index & 0xFFFFFFC0) + 64;
		}
		assert(newSize > index);
		fileNode->chunks.resize(newSize, 0);
	}

	oldChunkId = fileNode->chunks[index];
	if (context.isPersonalityMaster()) {
#ifndef METARESTORE
		status =
		    gChunkOperations->multiModify(fsOpContext, oldChunkId, lockid, fileNode->goal,
		                                  isQuotaExceeded, opflag, &newChunkId, min_server_version);
#else
		// This will NEVER happen (metarestore doesn't call this in master context)
		mabort("bad code path: fs_writechunk");
#endif
	} else {
		bool increaseVersion = (*opflag != 0);
		status = gChunkOperations->applyModification(context.ts(), oldChunkId, *lockid,
		                                             fileNode->goal, increaseVersion, &newChunkId);
	}
	if (status != SAUNAFS_STATUS_OK) {
		if (status == SAUNAFS_ERROR_LOCKED) { *chunkid = newChunkId; }
		fsnodes_update_checksum(fileNode);
		return status;
	}
	if (context.isPersonalityShadow() && newChunkId != *chunkid) {
		fsnodes_update_checksum(fileNode);
		return SAUNAFS_ERROR_MISMATCH;
	}

	fileNode->chunks[index] = newChunkId;
	*chunkid = newChunkId;

	// Propagate size changes to parent directories
	StatsRecord newStats;
	nodeOperations_->getStats(fsOpContext, fileNode, &newStats);
	nodeOperations_->updateParentStatsForNode(fsOpContext, fileNode, &newStats, &originalStats);

	quotaUpdate(fsOpContext, fileNode,
	            {{QuotaResource::kSize, newStats.size - originalStats.size}});
	if (length) { *length = fileNode->length; }

	if (context.isPersonalityMaster()) {
		changeLog(fsOpContext, context.ts(),
		          "WRITE(%" PRIiNode ",%" PRIu32 ",%" PRIu8 ",%" PRIu32 "):%" PRIu64, inode, index,
		          should_increase_chunk_version_on_modification(*opflag), *lockid, newChunkId);
	} else {
		gMetadata->metadataVersion++;
	}

	fileNode->mtime = context.ts();
	nodeOperations_->updateCTime(fsOpContext, fileNode, context.ts());
	fsnodes_update_checksum(fileNode);

	// Make the change persistent for KV backends
	if (fsOpContext.hasReadWriteTransaction()) {
		nodeOperations_->updateNode(fsOpContext, fileNode);
	}

#ifndef METARESTORE
	incrementFSStat(FsStats::Write);
	metrics::Counter::increment(metrics::Counter::Master::FS_WRITE);
#endif
	return SAUNAFS_STATUS_OK;
}

#ifndef METARESTORE
uint8_t FilesystemOperationsBase::writeEnd(const FilesystemOperationContext &fsOpContext,
                                           inode_t inode, uint64_t length, uint64_t chunkid,
                                           uint32_t lockid) {
	uint32_t timeStamp = eventloop_time();
	ChecksumUpdater checksumUpdater(timeStamp);

	uint8_t status = gChunkOperations->canUnlock(chunkid, lockid);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	if (length > 0) {
		auto *nodeFile = nodeOperations_->idToNode<FSNodeFile>(fsOpContext, inode);
		if (nodeFile == kNodeNotFound) { return SAUNAFS_ERROR_ENOENT; }
		if (nodeFile->type != FSNodeType::kFile && nodeFile->type != FSNodeType::kTrash &&
		    nodeFile->type != FSNodeType::kReserved) {
			return SAUNAFS_ERROR_EPERM;
		}

		if (length > nodeFile->length) {
			// eraseFurtherChunks should be set to false because we don't want
			// to erase the further chunks while we are write operations. The
			// reason is that those might be done by other clients and we don't
			// want to erase the chunks other clients are writing.
			bool eraseFurtherChunks = false;
			nodeOperations_->setLength(fsOpContext, nodeFile, length, eraseFurtherChunks);
			nodeFile->mtime = timeStamp;
			nodeOperations_->updateCTime(fsOpContext, nodeFile, timeStamp);
			fsnodes_update_checksum(nodeFile);

			// Make the change persistent for KV backends
			if (fsOpContext.hasReadWriteTransaction()) {
				nodeOperations_->updateNode(fsOpContext, nodeFile);
			}

			changeLog(fsOpContext, timeStamp, "LENGTH(%" PRIiNode ",%" PRIu64 ",%" PRIu32 ")",
			          inode, length, static_cast<uint32_t>(eraseFurtherChunks));
		}
	}

	changeLog(fsOpContext, timeStamp, "UNLOCK(%" PRIu64 ")", chunkid);
	return gChunkOperations->unlock(chunkid);
}

void FilesystemOperationsBase::increaseChunkVersion(const FilesystemOperationContext &fsOpContext,
                                                    uint64_t chunkid) {
	uint32_t timeStamp = eventloop_time();
	ChecksumUpdater checksumUpdater(timeStamp);
	changeLog(fsOpContext, timeStamp, "INCVERSION(%" PRIu64 ")", chunkid);
	gChunkOperations->persistRecord(fsOpContext, chunkid);
}
#endif

uint8_t FilesystemOperationsBase::applyIncreaseChunkVersion(
    const FilesystemOperationContext &fsOpContext, uint64_t chunkid) {
	gMetadata->metadataVersion++;
	return gChunkOperations->increaseVersion(fsOpContext, chunkid);
}

#ifndef METARESTORE
uint8_t FilesystemOperationsBase::removeChunkFromFile(const FsContext &context,
                                                      const FilesystemOperationContext &fsOpContext,
                                                      inode_t inode, uint64_t chunkId) {
	uint32_t timeStamp = eventloop_time();
	ChecksumUpdater checksumUpdater(timeStamp);
	StatsRecord previousStats;
	StatsRecord newStats;
	FSNode *nodeFile;

	if (chunkId == 0) { return SAUNAFS_ERROR_NOCHUNK; }

	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kFile,
	                                              MODE_MASK_W, inode, &nodeFile);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	auto *fileNode = dynamic_cast<FSNodeFile *>(nodeFile);
	nodeOperations_->getStats(fsOpContext, nodeFile, &previousStats);
	auto chunkIter = std::ranges::find(fileNode->chunks, chunkId);
	uint32_t chunkIndex = (chunkIter == fileNode->chunks.end())
	                          ? fileNode->chunks.size()
	                          : std::distance(fileNode->chunks.begin(), chunkIter);

	// not found
	if (chunkIndex == fileNode->chunks.size()) { return SAUNAFS_ERROR_NOCHUNK; }

	status = gChunkOperations->deleteFile(fsOpContext, chunkId, nodeFile->goal);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	fileNode->chunks[chunkIndex] = 0;
	nodeFile->mtime = timeStamp;
	nodeOperations_->updateCTime(fsOpContext, nodeFile, timeStamp);

	// Log the repair operation with new version 0 (indicating deletion)
	uint32_t newVersion = 0;
	changeLog(fsOpContext, timeStamp, "REPAIR(%" PRIiNode ",%" PRIu32 "):%" PRIu32, inode,
	          chunkIndex, newVersion);

	nodeOperations_->getStats(fsOpContext, nodeFile, &newStats);
	nodeOperations_->updateParentStatsForNode(fsOpContext, nodeFile, &newStats, &previousStats);
	quotaUpdate(fsOpContext, nodeFile,
	            {{QuotaResource::kSize, newStats.size - previousStats.size}});
	fsnodes_update_checksum(nodeFile);
	return SAUNAFS_STATUS_OK;
}
#endif /* #ifndef METARESTORE */

#ifndef METARESTORE
uint8_t FilesystemOperationsBase::repair(const FsContext &context, inode_t inode,
                                         uint8_t correct_only, uint32_t *notchanged,
                                         uint32_t *erased, uint32_t *repaired) {
	uint32_t timeStamp = eventloop_time();
	ChecksumUpdater checksumUpdater(timeStamp);
	uint32_t newVersion;
	uint32_t chunkIndex;
	StatsRecord previousStats;
	StatsRecord newStats;
	FSNode *node;

	*notchanged = 0;
	*erased = 0;
	*repaired = 0;

	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kFile,
	                                              MODE_MASK_W, inode, &node);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	auto *fileNode = static_cast<FSNodeFile *>(node);
	nodeOperations_->getStats(fsOpContext, node, &previousStats);
	for (chunkIndex = 0; chunkIndex < fileNode->chunks.size(); chunkIndex++) {
		if (gChunkOperations->repair(fsOpContext, node->goal, fileNode->chunks[chunkIndex],
		                             &newVersion, correct_only)) {
			changeLog(fsOpContext, timeStamp, "REPAIR(%" PRIiNode ",%" PRIu32 "):%" PRIu32, inode,
			          chunkIndex, newVersion);
			node->mtime = timeStamp;
			nodeOperations_->updateCTime(fsOpContext, node, timeStamp);
			if (newVersion > 0) {
				(*repaired)++;
			} else {
				fileNode->chunks[chunkIndex] = 0;
				(*erased)++;
			}
		} else {
			(*notchanged)++;
		}
	}
	nodeOperations_->getStats(fsOpContext, node, &newStats);
	nodeOperations_->updateParentStatsForNode(fsOpContext, node, &newStats, &previousStats);
	quotaUpdate(fsOpContext, node, {{QuotaResource::kSize, newStats.size - previousStats.size}});
	fsnodes_update_checksum(node);

	if (fsOpContext.hasReadWriteTransaction() && !fsOpContext.getReadWriteTransaction()->commit()) {
		safs::log_err("{}: failed to commit transaction for inode {}", __func__, inode);
		return SAUNAFS_ERROR_IO;
	}

	return SAUNAFS_STATUS_OK;
}
#endif /* #ifndef METARESTORE */

uint8_t FilesystemOperationsBase::applyRepair(const FilesystemOperationContext &fsOpContext,
                                              uint32_t timestamp, inode_t inode, uint32_t indx,
                                              uint32_t nversion) {
	uint8_t status;
	StatsRecord previousStats;
	StatsRecord newStats;

	auto *nodeFile = nodeOperations_->idToNode<FSNodeFile>(fsOpContext, inode);
	if (nodeFile == kNodeNotFound) { return SAUNAFS_ERROR_ENOENT; }

	if (nodeFile->type != FSNodeType::kFile && nodeFile->type != FSNodeType::kTrash &&
	    nodeFile->type != FSNodeType::kReserved) {
		return SAUNAFS_ERROR_EPERM;
	}

	if (indx > kMaxChunkIndex) { return SAUNAFS_ERROR_INDEXTOOBIG; }

	if (indx >= nodeFile->chunks.size()) {
		safs::log_err("fs_apply_repair: indx {} is greater than number of chunks ({}), inode {}",
		              indx, nodeFile->chunks.size(), inode);
		return SAUNAFS_ERROR_NOCHUNK;
	}

	if (nodeFile->chunks[indx] == 0) {
		safs::log_err("fs_apply_repair: node chunks at index {} has no chunks, inode {}", indx,
		              inode);
		return SAUNAFS_ERROR_NOCHUNK;
	}

	nodeOperations_->getStats(fsOpContext, nodeFile, &previousStats);

	if (nversion == 0) {
		status = gChunkOperations->deleteFile(fsOpContext, nodeFile->chunks[indx], nodeFile->goal);
		nodeFile->chunks[indx] = 0;
	} else {
		status = gChunkOperations->setVersion(fsOpContext, nodeFile->chunks[indx], nversion);
	}

	nodeOperations_->getStats(fsOpContext, nodeFile, &newStats);

	nodeOperations_->updateParentStatsForNode(fsOpContext, nodeFile, &newStats, &previousStats);

	quotaUpdate(fsOpContext, nodeFile,
	            {{QuotaResource::kSize, newStats.size - previousStats.size}});

	gMetadata->metadataVersion++;
	nodeFile->mtime = timestamp;
	nodeOperations_->updateCTime(fsOpContext, nodeFile, timestamp);

	fsnodes_update_checksum(nodeFile);

	return status;
}

#ifndef METARESTORE
uint8_t FilesystemOperationsBase::getGoal(const FsContext &context,
                                          const FilesystemOperationContext &fsOpContext,
                                          inode_t inode, uint8_t gmode, GoalStatistics &fgtab,
                                          GoalStatistics &dgtab) {
	FSNode *node;

	if (!GMODE_ISVALID(gmode)) { return SAUNAFS_ERROR_EINVAL; }

	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadOnly, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = nodeOperations_->getNodeForOperation(
	    context, fsOpContext, ExpectedNodeType::kFileOrDirectory, 0, inode, &node);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	nodeOperations_->getGoalRecursive(fsOpContext, node, gmode, fgtab, dgtab);

	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::getTrashTimePrepare(const FsContext &context, inode_t inode,
                                                      uint8_t gmode, TrashtimeMap &fileTrashtimes,
                                                      TrashtimeMap &dirTrashtimes) {
	FSNode *node;

	if (!GMODE_ISVALID(gmode)) { return SAUNAFS_ERROR_EINVAL; }

	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadOnly, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);

	status = nodeOperations_->getNodeForOperation(
	    context, fsOpContext, ExpectedNodeType::kFileOrDirectory, 0, inode, &node);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	nodeOperations_->getTrashTimeRecursive(node, gmode, fileTrashtimes, dirTrashtimes);

	return SAUNAFS_STATUS_OK;
}

void FilesystemOperationsBase::getTrashTimeStore(TrashtimeMap &fileTrashtimes,
                                                 TrashtimeMap &dirTrashtimes, uint8_t *buff) {
	for (auto trashTimePair : fileTrashtimes) {
		put32bit(&buff, trashTimePair.first);
		put32bit(&buff, trashTimePair.second);
	}
	for (auto trashTimePair : dirTrashtimes) {
		put32bit(&buff, trashTimePair.first);
		put32bit(&buff, trashTimePair.second);
	}
}

uint8_t FilesystemOperationsBase::getExtraAttr(const FsContext &context, inode_t inode,
                                               uint8_t gmode, ExtraAttributesArray &fileEAttrTab,
                                               ExtraAttributesArray &dirEAttrTab) {
	FSNode *node;

	fileEAttrTab.fill(0);
	dirEAttrTab.fill(0);

	if (!GMODE_ISVALID(gmode)) { return SAUNAFS_ERROR_EINVAL; }

	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadOnly, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);

	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kAny, 0,
	                                              inode, &node);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	nodeOperations_->getExtraAttrRecursive(node, gmode, fileEAttrTab, dirEAttrTab);
	return SAUNAFS_STATUS_OK;
}

#endif

uint8_t FilesystemOperationsBase::setGoal(const FsContext &context, inode_t inode, uint8_t goal,
                                          uint8_t smode,
                                          std::shared_ptr<SetGoalTask::StatsArray> setgoal_stats,
                                          const std::function<void(int)> &callback) {
	ChecksumUpdater checksumUpdater(context.ts());
	if (!SMODE_ISVALID(smode) || !GoalId::isValid(goal) ||
	    (smode & (SMODE_INCREASE | SMODE_DECREASE))) {
		return SAUNAFS_ERROR_EINVAL;
	}
	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	FSNode *node;
	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kAny,
	                                              MODE_MASK_EMPTY, inode, &node);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	if (node->type != FSNodeType::kDirectory && node->type != FSNodeType::kFile &&
	    node->type != FSNodeType::kTrash && node->type != FSNodeType::kReserved) {
		return SAUNAFS_ERROR_EPERM;
	}
	sassert(context.hasUidGidData());
	(*setgoal_stats)[SetGoalTask::kChanged] = 0;       // - Number of inodes with changed goal
	(*setgoal_stats)[SetGoalTask::kNotChanged] = 0;    // - Number of inodes with not changed goal
	(*setgoal_stats)[SetGoalTask::kNotPermitted] = 0;  // - Number of inodes with permission denied

	auto *task = new SetGoalTask({node->id}, context.uid(), goal, smode, setgoal_stats);
	std::string nodeName;
	FSNodeDirectory *parent = nodeOperations_->getFirstParent(fsOpContext, node);
	nodeOperations_->getPath(fsOpContext, parent, node, nodeName);

	std::string goalName;
#ifndef METARESTORE
	goalName = gGoalDefinitions[goal].getName();
#else
	goalName = "goal id: " + std::to_string(goal);
#endif
	return gMetadata->taskManager.submitTask(context.ts(), kInitialTaskBatchSize, task,
	                                         SetGoalTask::generateDescription(nodeName, goalName),
	                                         callback);
}

// This function is only used by Shadow
uint8_t FilesystemOperationsBase::applySetGoal(const FsContext &context, inode_t inode,
                                               uint8_t goal, uint8_t smode,
                                               uint32_t master_result) {
	assert(context.isPersonalityShadow());
	ChecksumUpdater checksumUpdater(context.ts());
	if (!SMODE_ISVALID(smode) || !GoalId::isValid(goal) ||
	    (smode & (SMODE_INCREASE | SMODE_DECREASE))) {
		return SAUNAFS_ERROR_EINVAL;
	}
	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	FSNode *node;
	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kAny,
	                                              MODE_MASK_EMPTY, inode, &node);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	if (node->type != FSNodeType::kDirectory && node->type != FSNodeType::kFile &&
	    node->type != FSNodeType::kTrash && node->type != FSNodeType::kReserved) {
		return SAUNAFS_ERROR_EPERM;
	}
	sassert(context.hasUidGidData());

	SetGoalTask task(context.uid(), goal, smode);
	uint32_t myResult = task.setGoal(fsOpContext, node, context.ts());

	gMetadata->metadataVersion++;
	if (master_result != myResult) { return SAUNAFS_ERROR_MISMATCH; }

	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::setTrashTime(
    const FsContext &context, inode_t inode, uint32_t trashtime, uint8_t smode,
    std::shared_ptr<SetTrashtimeTask::StatsArray> settrashtime_stats,
    const std::function<void(int)> &callback) {
	ChecksumUpdater checksumUpdater(context.ts());
	if (!SMODE_ISVALID(smode)) { return SAUNAFS_ERROR_EINVAL; }
	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	FSNode *node;
	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kAny,
	                                              MODE_MASK_EMPTY, inode, &node);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	if (node->type != FSNodeType::kDirectory && node->type != FSNodeType::kFile &&
	    node->type != FSNodeType::kTrash && node->type != FSNodeType::kReserved) {
		return SAUNAFS_ERROR_EPERM;
	}
	sassert(context.hasUidGidData());
	(*settrashtime_stats)[SetTrashtimeTask::kChanged] =
	    0;  // - Number of inodes with changed trashtime
	(*settrashtime_stats)[SetTrashtimeTask::kNotChanged] =
	    0;  // - Number of inodes with not changed trashtime
	(*settrashtime_stats)[SetTrashtimeTask::kNotPermitted] =
	    0;  // - Number of inodes with permission denied

	auto *task =
	    new SetTrashtimeTask({node->id}, context.uid(), trashtime, smode, settrashtime_stats);
	std::string nodeName;
	FSNodeDirectory *parent = nodeOperations_->getFirstParent(fsOpContext, node);
	nodeOperations_->getPath(fsOpContext, parent, node, nodeName);
	return gMetadata->taskManager.submitTask(
	    context.ts(), kInitialTaskBatchSize, task,
	    SetTrashtimeTask::generateDescription(nodeName, trashtime), callback);
}

uint8_t FilesystemOperationsBase::applySetTrashTime(const FsContext &context, inode_t inode,
                                                    uint32_t trashtime, uint8_t smode,
                                                    uint32_t master_result) {
	assert(context.isPersonalityShadow());
	ChecksumUpdater checksumUpdater(context.ts());
	if (!SMODE_ISVALID(smode)) { return SAUNAFS_ERROR_EINVAL; }
	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	FSNode *node;
	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kAny,
	                                              MODE_MASK_EMPTY, inode, &node);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	if (node->type != FSNodeType::kDirectory && node->type != FSNodeType::kFile &&
	    node->type != FSNodeType::kTrash && node->type != FSNodeType::kReserved) {
		return SAUNAFS_ERROR_EPERM;
	}
	sassert(context.hasUidGidData());

	SetTrashtimeTask task(context.uid(), trashtime, smode);
	uint32_t myResult = task.setTrashtime(fsOpContext, node, context.ts());

	gMetadata->metadataVersion++;
	if (master_result != myResult) { return SAUNAFS_ERROR_MISMATCH; }
	if (myResult == SetTrashtimeTask::kChanged && fsOpContext.hasReadWriteTransaction()) {
		nodeOperations_->updateNode(fsOpContext, node);
		if (!fsOpContext.getReadWriteTransaction()->commit()) { return SAUNAFS_ERROR_IO; }
	}

	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::setExtraAttr(const FsContext &context,
                                               const FilesystemOperationContext &fsOpContext,
                                               inode_t inode, uint8_t eattr, uint8_t smode,
                                               inode_t *sinodes, inode_t *ncinodes,
                                               inode_t *nsinodes) {
	ChecksumUpdater checksumUpdater(context.ts());
	if (!SMODE_ISVALID(smode) ||
	    (eattr & (~(EATTR_NOOWNER | EATTR_NOACACHE | EATTR_NOECACHE | EATTR_NODATACACHE)))) {
		return SAUNAFS_ERROR_EINVAL;
	}
	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	FSNode *node;
	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kAny,
	                                              MODE_MASK_EMPTY, inode, &node);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	inode_t setInodesCount = 0;
	inode_t notChangedInodesCount = 0;
	inode_t notPermittedInodesCount = 0;
	sassert(context.hasUidGidData());
	nodeOperations_->setExtraAttrRecursive(fsOpContext, node, context.ts(), context.uid(), eattr,
	                                       smode, &setInodesCount, &notChangedInodesCount,
	                                       &notPermittedInodesCount);
	if (context.isPersonalityMaster()) {
		if ((smode & SMODE_RMASK) == 0 && notPermittedInodesCount > 0 && setInodesCount == 0 &&
		    notChangedInodesCount == 0) {
			return SAUNAFS_ERROR_EPERM;
		}
		*sinodes = setInodesCount;
		*ncinodes = notChangedInodesCount;
		*nsinodes = notPermittedInodesCount;
		changeLog(fsOpContext, context.ts(),
		          "SETEATTR(%" PRIiNode ",%" PRIu32 ",%" PRIu8 ",%" PRIu8 "):%" PRIiNode
		          ",%" PRIiNode ",%" PRIiNode,
		          node->id, context.uid(), eattr, smode, setInodesCount, notChangedInodesCount,
		          notPermittedInodesCount);
	} else {
		gMetadata->metadataVersion++;
		if ((*sinodes != setInodesCount) || (*ncinodes != notChangedInodesCount) ||
		    (*nsinodes != notPermittedInodesCount)) {
			return SAUNAFS_ERROR_MISMATCH;
		}
	}
	return SAUNAFS_STATUS_OK;
}

#ifndef METARESTORE

uint8_t FilesystemOperationsBase::listXAttr(const FsContext &context,
                                            const FilesystemOperationContext &fsOpContext,
                                            inode_t inode, uint8_t opened, XAttrListResult &result,
                                            uint32_t *xasize) {
	FSNode *node;

	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadOnly, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kAny,
	                                              opened == 0 ? MODE_MASK_R : MODE_MASK_EMPTY,
	                                              inode, &node);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	*xasize = sizeof(kAclXattrs);
	return doConcreteListXAttr(fsOpContext, node->id, result, xasize);
}

uint8_t FilesystemOperationsBase::getXAttr(const FsContext &context,
                                           const FilesystemOperationContext &fsOpContext,
                                           inode_t inode, uint8_t opened, uint8_t anleng,
                                           const uint8_t *attrname, XAttrGetResult &result) {
	FSNode *node;

	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadOnly, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kAny,
	                                              opened == 0 ? MODE_MASK_R : MODE_MASK_EMPTY,
	                                              inode, &node);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	if (xattr_namecheck(anleng, attrname) < 0) { return SAUNAFS_ERROR_EINVAL; }
	return doConcreteGetXAttr(fsOpContext, node->id, anleng, attrname, result);
}

uint8_t FilesystemOperationsBase::setXAttr(const FsContext &context,
                                           const FilesystemOperationContext &fsOpContext,
                                           inode_t inode, uint8_t opened, uint8_t anleng,
                                           const uint8_t *attrname, uint32_t avleng,
                                           const uint8_t *attrvalue, uint8_t mode) {
	uint32_t timeStamp = eventloop_time();
	ChecksumUpdater checksumUpdater(timeStamp);
	FSNode *node;
	uint8_t status;

	status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kAny,
	                                              opened == 0 ? MODE_MASK_W : MODE_MASK_EMPTY,
	                                              inode, &node);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	if (xattr_namecheck(anleng, attrname) < 0) { return SAUNAFS_ERROR_EINVAL; }
	if (mode > XATTR_SMODE_REMOVE) { return SAUNAFS_ERROR_EINVAL; }
	status = doConcreteSetXAttr(fsOpContext, node->id, anleng, attrname, avleng, attrvalue, mode);
	if (status != SAUNAFS_STATUS_OK) { return status; }
	nodeOperations_->updateCTime(fsOpContext, node, timeStamp);
	fsnodes_update_checksum(node);
	changeLog(fsOpContext, timeStamp, "SETXATTR(%" PRIiNode ",%s,%s,%" PRIu8 ")", node->id,
	          nodeOperations_->escapeName(std::string((const char *)attrname, anleng)).c_str(),
	          nodeOperations_->escapeName(std::string((const char *)attrvalue, avleng)).c_str(),
	          mode);
	return SAUNAFS_STATUS_OK;
}

#endif /* #ifndef METARESTORE */

uint8_t FilesystemOperationsBase::doConcreteGetXAttr(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, inode_t inode, uint8_t anleng,
    const uint8_t *attrname, XAttrGetResult &result) {
	uint32_t attrValueLength = 0;
	uint8_t *attrValue = nullptr;  // Assigned by xattr_getattr

	uint8_t status = xattr_getattr(inode, anleng, attrname, &attrValueLength, &attrValue);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	result.value.assign(attrValue, attrValue + attrValueLength);

	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::doConcreteListXAttr(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, inode_t inode,
    XAttrListResult &result, uint32_t *xasize) {
	void *xattrNode = nullptr;  // Assigned by get_xattrs_length_for_inode

	uint8_t status = get_xattrs_length_for_inode(inode, &xattrNode, xasize);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	uint32_t nameDataSize = *xasize - sizeof(kAclXattrs);

	if (nameDataSize > 0 && xattrNode != nullptr) {
		result.data.resize(nameDataSize);
		xattr_listattr_data(xattrNode, result.data.data());
	}

	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::doConcreteSetXAttr(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, inode_t inode, uint8_t anleng,
    const uint8_t *attrname, uint32_t avleng, const uint8_t *attrvalue, uint32_t mode) {
	return xattr_setattr(inode, anleng, attrname, avleng, attrvalue, mode);
}

uint8_t FilesystemOperationsBase::applySetXAttr(const FilesystemOperationContext &fsOpContext,
                                                uint32_t timestamp, inode_t inode, uint32_t anleng,
                                                const uint8_t *attrname, uint32_t avleng,
                                                const uint8_t *attrvalue, uint32_t mode) {
	FSNode *node;
	uint8_t status;
	if (anleng == 0 || anleng > SFS_XATTR_NAME_MAX || avleng > SFS_XATTR_SIZE_MAX ||
	    mode > XATTR_SMODE_REMOVE) {
		return SAUNAFS_ERROR_EINVAL;
	}

	node = nodeOperations_->idToNode(fsOpContext, inode);
	if (node == kNodeNotFound) { return SAUNAFS_ERROR_ENOENT; }

	status = doConcreteSetXAttr(fsOpContext, inode, static_cast<uint8_t>(anleng), attrname, avleng,
	                            attrvalue, mode);

	if (status != SAUNAFS_STATUS_OK) { return status; }
	nodeOperations_->updateCTime(fsOpContext, node, timestamp);
	gMetadata->metadataVersion++;
	fsnodes_update_checksum(node);
	return status;
}

uint8_t FilesystemOperationsBase::deleteAcl(const FsContext &context,
                                            const FilesystemOperationContext &fsOpContext,
                                            inode_t inode, AclType type) {
	ChecksumUpdater checksumUpdater(context.ts());
	FSNode *node;
	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kAny,
	                                              MODE_MASK_EMPTY, inode, &node);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = nodeOperations_->deleteAcl(fsOpContext, node, type, context.ts());

	if (context.isPersonalityMaster()) {
		if (status == SAUNAFS_STATUS_OK) {
			static char aclType[3] = {'a', 'd', 'r'};

			static_assert((int)AclType::kAccess == 0, "fix aclType table");
			static_assert((int)AclType::kDefault == 1, "fix aclType table");
			static_assert((int)AclType::kRichACL == 2, "fix aclType table");

			changeLog(fsOpContext, context.ts(), "DELETEACL(%" PRIiNode ",%c)", node->id,
			          aclType[std::min(3, (int)type)]);
		}
	} else {
		gMetadata->metadataVersion++;
	}
	return status;
}

#ifndef METARESTORE

uint8_t FilesystemOperationsBase::setAcl(const FsContext &context,
                                         const FilesystemOperationContext &fsOpContext,
                                         inode_t inode, const RichACL &acl) {
	ChecksumUpdater checksumUpdater(context.ts());
	FSNode *node;
	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kAny,
	                                              MODE_MASK_EMPTY, inode, &node);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	std::string aclString = acl.toString();
	status = nodeOperations_->setAcl(fsOpContext, node, acl, context.ts());

	if (context.isPersonalityMaster()) {
		if (status == SAUNAFS_STATUS_OK) {
			changeLog(fsOpContext, context.ts(), "SETRICHACL(%" PRIiNode ",%s)", node->id,
			          aclString.c_str());
		}
	} else {
		gMetadata->metadataVersion++;
	}
	return status;
}

uint8_t FilesystemOperationsBase::setAcl(const FsContext &context,
                                         const FilesystemOperationContext &fsOpContext,
                                         inode_t inode, AclType type,
                                         const AccessControlList &acl) {
	ChecksumUpdater checksumUpdater(context.ts());
	FSNode *node;
	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadWrite, SessionType::kNotMeta);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kAny,
	                                              MODE_MASK_EMPTY, inode, &node);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	std::string aclString = acl.toString();
	status = nodeOperations_->setAcl(fsOpContext, node, type, acl, context.ts());

	if (context.isPersonalityMaster()) {
		if (status == SAUNAFS_STATUS_OK) {
			changeLog(fsOpContext, context.ts(), "SETACL(%" PRIiNode ",%c,%s)", node->id,
			          (type == AclType::kAccess ? 'a' : 'd'), aclString.c_str());
		}
	} else {
		gMetadata->metadataVersion++;
	}
	return status;
}

uint8_t FilesystemOperationsBase::getAcl(const FsContext &context,
                                         const FilesystemOperationContext &fsOpContext,
                                         inode_t inode, RichACL &acl) {
	FSNode *node;
	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadOnly, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kAny,
	                                              MODE_MASK_EMPTY, inode, &node);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	return nodeOperations_->getAcl(fsOpContext, node, acl);
}

#endif /* #ifndef METARESTORE */

uint8_t FilesystemOperationsBase::applySetAcl(const FilesystemOperationContext &fsOpContext,
                                              uint32_t timestamp, inode_t inode, char aclType,
                                              const char *aclString) {
	AccessControlList acl;
	try {
		acl = AccessControlList::fromString(aclString);
	} catch (Exception &) { return SAUNAFS_ERROR_EINVAL; }

	FSNode *node = nodeOperations_->idToNode(fsOpContext, inode);
	if (node == kNodeNotFound) { return SAUNAFS_ERROR_ENOENT; }

	AclType aclTypeEnum;
	if (!decodeChar("da", {AclType::kDefault, AclType::kAccess}, aclType, aclTypeEnum)) {
		return SAUNAFS_ERROR_EINVAL;
	}

	uint8_t status = nodeOperations_->setAcl(fsOpContext, node, aclTypeEnum, acl, timestamp);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	gMetadata->metadataVersion++;
	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::applySetRichAcl(const FilesystemOperationContext &fsOpContext,
                                                  uint32_t timestamp, inode_t inode,
                                                  const std::string &acl_string) {
	RichACL acl;
	try {
		acl = RichACL::fromString(acl_string);
	} catch (Exception &) { return SAUNAFS_ERROR_EINVAL; }

	FSNode *node = nodeOperations_->idToNode(fsOpContext, inode);
	if (node == kNodeNotFound) { return SAUNAFS_ERROR_ENOENT; }

	uint8_t status = nodeOperations_->setAcl(fsOpContext, node, acl, timestamp);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	gMetadata->metadataVersion++;
	return SAUNAFS_STATUS_OK;
}

#ifndef METARESTORE
uint32_t FilesystemOperationsBase::getDirPathSize(const FilesystemOperationContext &fsOpContext,
                                                  inode_t inode) {
	FSNode *node = nodeOperations_->idToNode(fsOpContext, inode);

	if (node != kNodeNotFound) {
		if (node->type != FSNodeType::kDirectory) { return kDirPathNotDirectory.size(); }

		FSNodeDirectory *parent = nullptr;
		const inode_t parentId = nodeOperations_->getFirstParentId(fsOpContext, node);

		if (parentId != 0) {
			parent = nodeOperations_->idToNodeVerify<FSNodeDirectory>(fsOpContext, parentId);
		}

		return 1 + nodeOperations_->getPathSize(fsOpContext, parent, node);
	}

	return kDirPathNotFound.size();
}

void FilesystemOperationsBase::getDirPathData(const FilesystemOperationContext &fsOpContext,
                                              inode_t inode, uint8_t *buff, uint32_t size) {
	FSNode *node = nodeOperations_->idToNode(fsOpContext, inode);

	if (node != kNodeNotFound) {
		if (node->type != FSNodeType::kDirectory) {
			if (size >= kDirPathNotDirectory.size()) {
				memcpy(buff, kDirPathNotDirectory.data(), kDirPathNotDirectory.size());
				return;
			}
		} else {
			if (size > 0) {
				FSNodeDirectory *parent = nullptr;
				const inode_t parentId = nodeOperations_->getFirstParentId(fsOpContext, node);
				if (parentId != 0) {
					parent =
					    nodeOperations_->idToNodeVerify<FSNodeDirectory>(fsOpContext, parentId);
				}

				buff[0] = '/';
				nodeOperations_->getPathData(fsOpContext, parent, node, buff + 1, size - 1);
				return;
			}
		}
	} else {
		if (size >= kDirPathNotFound.size()) {
			memcpy(buff, kDirPathNotFound.data(), kDirPathNotFound.size());
			return;
		}
	}
}

uint8_t FilesystemOperationsBase::getDirStats(const FsContext &context, inode_t inode,
                                              inode_t *inodes, inode_t *dirs, inode_t *files,
                                              inode_t *links, uint32_t *chunks, uint64_t *length,
                                              uint64_t *size, uint64_t *rsize) {
	FSNode *node;
	StatsRecord statsRecord;

	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadOnly, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);

	status = nodeOperations_->getNodeForOperation(
	    context, fsOpContext, ExpectedNodeType::kFileOrDirectory, MODE_MASK_EMPTY, inode, &node);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	nodeOperations_->getStats(fsOpContext, node, &statsRecord);
	*inodes = statsRecord.inodes;
	*dirs = statsRecord.dirs;
	*files = statsRecord.files;
	*links = statsRecord.links;
	*chunks = statsRecord.chunks;
	*length = statsRecord.length;
	*size = statsRecord.size;
	*rsize = statsRecord.realsize;
	//      syslog(LOG_NOTICE,"using fast stats");
	return SAUNAFS_STATUS_OK;
}

uint8_t FilesystemOperationsBase::getChunkId(const FsContext &context,
                                             const FilesystemOperationContext &fsOpContext,
                                             inode_t inode, uint32_t index, uint64_t *chunkid) {
	FSNode *node;
	uint8_t status = nodeOperations_->getNodeForOperation(
	    context, fsOpContext, ExpectedNodeType::kFile, MODE_MASK_EMPTY, inode, &node);
	auto *fileNode = static_cast<FSNodeFile *>(node);
	if (status != SAUNAFS_STATUS_OK) { return status; }
	if (index > kMaxChunkIndex) { return SAUNAFS_ERROR_INDEXTOOBIG; }
	if (index < fileNode->chunks.size()) {
		*chunkid = fileNode->chunks[index];
	} else {
		*chunkid = 0;
	}
	return SAUNAFS_STATUS_OK;
}
#endif

void FilesystemOperationsBase::addFilesToChunks(bool isMetadataLoading) {
	// In-memory rebuild from the loaded node table; no KV transaction is involved,
	// so persisting backends receive an empty context and skip the CHNK_ write.
	const FilesystemOperationContext loadContext{};

	for (uint32_t i = 0; i < NODEHASHSIZE; i++) {
		for (const auto &node : gMetadata->nodeHash[i]) {
			if (node->type == FSNodeType::kFile || node->type == FSNodeType::kTrash ||
			    node->type == FSNodeType::kReserved) {
				for (const auto &chunkid : static_cast<FSNodeFile *>(node)->chunks) {
					if (chunkid > 0) {
						gChunkOperations->addFile(loadContext, chunkid, node->goal,
						                          isMetadataLoading);
					}
				}
			}
		}
	}
}

uint64_t FilesystemOperationsBase::getMetadataVersion() {
	if (!gMetadata) { throw NoMetadataException(); }
	return gMetadata->metadataVersion;
}

uint64_t FilesystemOperationsBase::increaseMetadataVersion(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext) {
	return gMetadata->metadataVersion++;
}

uint8_t FilesystemOperationsBase::applySetQuota(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, char rigor, char resource,
    char ownerType, inode_t ownerId, uint64_t limit) {
	return quotas::fs_apply_setquota(rigor, resource, ownerType, ownerId, limit);
}

uint64_t FilesystemOperationsBase::metadataChecksum(ChecksumMode mode) {
	return checksum::fs_checksum(mode);
}

#ifndef METARESTORE
const std::map<int, Goal> &FilesystemOperationsBase::getAllGoalDefinitions() const {
	return gGoalDefinitions;
}

const Goal &FilesystemOperationsBase::getGoalDefinition(uint8_t goalId) const {
	return gGoalDefinitions[goalId];
}

std::vector<JobInfo> FilesystemOperationsBase::getCurrentTasksInfo() {
	return gMetadata->taskManager.getCurrentJobsInfo();
}

uint8_t FilesystemOperationsBase::cancelJob(uint32_t job_id) {
	if (gMetadata->taskManager.cancelJob(job_id)) { return SAUNAFS_STATUS_OK; }
	return SAUNAFS_ERROR_EINVAL;
}

uint32_t FilesystemOperationsBase::reserveJobId() { return gMetadata->taskManager.reserveJobId(); }

uint8_t FilesystemOperationsBase::getChunksInfo(const FsContext &context, uint32_t current_ip,
                                                inode_t inode, uint32_t chunk_index,
                                                uint32_t chunk_count,
                                                std::vector<ChunkWithAddressAndLabel> &chunks) {
	static constexpr int kMaxNumberOfChunkCopies = 100;

	FSNode *node;

	uint8_t status =
	    nodeOperations_->verifySession(context, OperationMode::kReadOnly, SessionType::kAny);
	if (status != SAUNAFS_STATUS_OK) { return status; }

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);

	status = nodeOperations_->getNodeForOperation(context, fsOpContext, ExpectedNodeType::kFile,
	                                              MODE_MASK_R, inode, &node);

	if (status != SAUNAFS_STATUS_OK) { return status; }

	if (chunk_index > kMaxChunkIndex) { return SAUNAFS_ERROR_INDEXTOOBIG; }

	auto *fileNode = static_cast<FSNodeFile *>(node);

	std::vector<ChunkPartWithAddressAndLabel> chunkParts;

	if (chunk_count == 0) { chunk_count = fileNode->chunks.size(); }

	chunks.clear();
	while (chunk_index < fileNode->chunks.size() && chunk_count > 0) {
		uint64_t chunkId = fileNode->chunks[chunk_index];
		uint32_t chunkVersion = 0;
		chunkParts.clear();

		if (chunkId > 0) {
			status = gChunkOperations->getVersionAndLocations(chunkId, current_ip, chunkVersion,
			                                                  kMaxNumberOfChunkCopies, chunkParts);
			if (status != SAUNAFS_STATUS_OK) { return status; }
		}

		chunks.emplace_back(chunkId, chunkVersion, std::move(chunkParts));
		chunk_index++;
		chunk_count--;
	}

	return SAUNAFS_STATUS_OK;
}

#endif

QuotaCheckResult FilesystemOperationsBase::quotaExceededUg(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, uint32_t uid, uint32_t gid,
    const std::initializer_list<std::pair<QuotaResource, int64_t>> &resourceList) {
	return {.exceeded = fsnodes_quota_exceeded_ug(uid, gid, resourceList)};
}

QuotaCheckResult FilesystemOperationsBase::quotaExceededDir(
    const FilesystemOperationContext &fsOpContext, FSNode *node,
    const std::initializer_list<std::pair<QuotaResource, int64_t>> &resourceList) {
	return {.exceeded = fsnodes_quota_exceeded_dir(fsOpContext, node, resourceList)};
}

QuotaCheckResult FilesystemOperationsBase::quotaExceededDirMove(
    const FilesystemOperationContext &fsOpContext, FSNodeDirectory *node, FSNodeDirectory *prevNode,
    const std::initializer_list<std::pair<QuotaResource, int64_t>> &resourceList) {
	return {.exceeded = fsnodes_quota_exceeded_dir(fsOpContext, node, prevNode, resourceList)};
}

QuotaCheckResult FilesystemOperationsBase::quotaExceeded(
    const FilesystemOperationContext &fsOpContext, FSNode *node,
    const std::initializer_list<std::pair<QuotaResource, int64_t>> &resourceList) {
	return {.exceeded = fsnodes_quota_exceeded(fsOpContext, node, resourceList)};
}

void FilesystemOperationsBase::quotaUpdate(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, FSNode *node,
    const std::initializer_list<std::pair<QuotaResource, int64_t>> &resourceList) {
	fsnodes_quota_update(node, resourceList);
}

void FilesystemOperationsBase::quotaRemove(
    [[maybe_unused]] const FilesystemOperationContext &fsOpContext, QuotaOwnerType ownerType,
    inode_t ownerId) {
	fsnodes_quota_remove(ownerType, ownerId);
}

#ifndef METARESTORE
uint8_t FilesystemOperationsBase::quotaGetAll(const FsContext &context,
                                              std::vector<QuotaEntry> &results) {
	return quotas::fs_quota_get_all(context, results);
}

uint8_t FilesystemOperationsBase::quotaGet(const FsContext &context,
                                           const std::vector<QuotaOwner> &owners,
                                           std::vector<QuotaEntry> &results) {
	return quotas::fs_quota_get(context, owners, results);
}

uint8_t FilesystemOperationsBase::quotaSet(const FsContext &context,
                                           const FilesystemOperationContext &fsOpContext,
                                           const std::vector<QuotaEntry> &entries) {
	return quotas::fs_quota_set(context, fsOpContext, entries);
}

uint8_t FilesystemOperationsBase::quotaGetInfo(const FsContext &context,
                                               const std::vector<QuotaEntry> &entries,
                                               std::vector<std::string> &result) {
	return quotas::fs_quota_get_info(context, entries, result);
}
#endif

#ifndef METARESTORE
uint8_t FilesystemOperationsBase::startChecksumRecalculation() {
	return checksum::fs_start_checksum_recalculation();
}
#endif
