/*
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

#include "master/metadata_backend_forkless.h"

#include <fcntl.h>  // for open and O_RDONLY
#include <sys/mman.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/datapack.h"
#include "common/event_loop.h"
#include "common/scoped_timer.h"
#include "common/serialization.h"
#include "common/time_utils.h"
#include "kv/itransaction.h"
#include "kv/kv_utils.h"
#include "master/changelog.h"
#include "master/chunk_operations_interface.h"
#include "master/chunks.h"
#include "master/filesystem.h"
#include "master/filesystem_metadata.h"
#include "master/filesystem_operations.h"
#include "master/filesystem_operations_interface.h"
#include "master/filesystem_quota.h"
#include "master/kv_common_keys.h"
#include "master/kv_connector_fdb.h"
#include "master/kv_connector_interface.h"
#include "master/matoclserv.h"
#include "master/matoclserv_sessions.h"
#include "master/matomlserv.h"
#include "master/metadata_backend_common.h"
#include "master/metadata_backend_interface.h"
#include "master/metadata_dumper_file.h"
#include "master/metadata_section_bootstrap_fdb.h"
#include "master/personality.h"
#include "protocol/SFSCommunication.h"
#include "slogger/slogger.h"

namespace {
constexpr uint32_t kMetadataFlushIntervalMs = 100;

MetadataBackendForkless *gForklessBackend = nullptr;

#ifndef METARESTORE
bool hasPersistedMetadataSectionData(kv::IKVEngine *kvEngine,
	                                 const std::vector<MetadataSectionFDB> &metadataSections) {
	if (kvEngine == nullptr) { return false; }

	// One transaction for all sections: cheaper, and gives a consistent point-in-time view.
	auto transaction = kvEngine->createReadOnlyTransaction();
	for (const auto &section : metadataSections) {
		kv::Key startKey = kv::toBytes(section.prefix);
		kv::Key endKey = kv::prefixEnd(startKey);
		auto page = transaction->getRange(kv::KeySelector(startKey, true, 0),
		                                  kv::KeySelector(endKey, true, 0), 1);

		if (!page.getPairs().empty()) { return true; }
	}

	return false;
}
#endif  // #ifndef METARESTORE

}

// Add a static callback function
static void flushMetadataCallback() {
	if (gForklessBackend != nullptr) { (void)gForklessBackend->flushPendingUpdates(false); }
}

// Called by the personality subsystem when this server is promoted from Shadow to Master.
// Must match the void(*)(void) signature required by registerFunctionCalledOnPromotion.
static void forklessBackendBecameMaster() {
	if (gForklessBackend != nullptr) { gForklessBackend->onPromotedToMaster(); }
}

// Eventloop destruct hook: runs while the time table is still alive (before
// eventloop_release_resources() clears it) so the backend's static destructor never
// unregisters an already-removed flush timer. Must match the void(*)(void) signature.
static void forklessFlushTimerTeardown() {
	if (gForklessBackend != nullptr) { gForklessBackend->onEventloopTeardown(); }
}

inline Signal initializeNewMetadataHeaderSignal;

MetadataBackendForkless::MetadataBackendForkless()
#if !defined(METARESTORE) && !defined(METALOGGER)
    : dumper_(std::make_unique<MetadataDumperFile>(kMetadataFilename, kMetadataTmpFilename))
#endif  // #if !defined(METARESTORE) && !defined(METALOGGER)
{
	initSections();

	// Set the global instance pointer
	gForklessBackend = this;

	safs::log_info("Metadata backend: {}", backendType());
}

MetadataBackendForkless::~MetadataBackendForkless() {
	// Unregister the periodic flush timer if it is still live. This matters when the backend
	// is destroyed while the eventloop is running (e.g. recreated in tests): otherwise stale
	// timers accumulate and keep flushing the newer instance through gForklessBackend. At
	// process shutdown the eventloop destruct hook (onEventloopTeardown) has already cleared
	// flushTimerHandle_, so this is a no-op and never touches the already-cleared time table
	// (eventloop_timeunregister maborts on an unknown handle).
	if (flushTimerHandle_ != nullptr) {
		eventloop_timeunregister(flushTimerHandle_);
		flushTimerHandle_ = nullptr;
	}

	// Static callbacks (periodic flush timer, promotion handler) dereference
	// gForklessBackend. Clear it on destruction so they never touch a deleted
	// instance. Guard on identity so destroying an old backend cannot clobber a
	// newer one that already claimed the pointer in its constructor.
	if (gForklessBackend == this) { gForklessBackend = nullptr; }
}

#if !defined(METARESTORE) && !defined(METALOGGER)

bool MetadataBackendForkless::commit_metadata_dump() {
	safs::log_warn("MetadataBackendForkless::commit_metadata_dump is not fully implemented");

	return true;
}

int MetadataBackendForkless::emergency_saves() {
	safs::log_warn("MetadataBackendForkless::emergency_saves is not fully implemented");

	return 0;
}

void MetadataBackendForkless::broadcast_metadata_saved(uint8_t status) {
	matomlserv_broadcast_metadata_saved(status);
	matoclserv_broadcast_metadata_saved(status);
}

uint8_t MetadataBackendForkless::fs_storeall(DumpType /*dumpType*/) {
	safs::log_info("MetadataBackendForkless::fs_storeall");

	if (gMetadata == nullptr) {
		// Periodic dump in shadow master or a request from saunafs-admin
		safs::log_info("Can't save metadata because no metadata is loaded");
		return SAUNAFS_ERROR_NOTPOSSIBLE;
	}

	// Checkpoint management and FDB persistence are master-only.
	// Shadows share the same FDB database and must not write checkpoint keys or
	// flush the writer queue — both conflict with the master's own storeall path.
	if (metadataserver::isMaster()) {
		auto checkpointDescriptor = buildCheckpointDescriptor();
		if (checkpointManager_ == nullptr ||
		    !checkpointManager_->beginCheckpoint(checkpointDescriptor)) {
			safs::log_err("Failed to begin metadata checkpoint sketch");
			broadcast_metadata_saved(SAUNAFS_ERROR_IO);
			return SAUNAFS_ERROR_IO;
		}

		// Flush ALL pending batched updates to FDB before saving metadata keys,
		// so restore-relevant keys reflect the fully persisted state.
		if (!flushPendingUpdates(true)) {
			safs::log_err("Failed to fully flush pending updates before saving metadata keys");
			broadcast_metadata_saved(SAUNAFS_ERROR_IO);
			return SAUNAFS_ERROR_IO;
		}

		// Seal the checkpoint descriptor after all pending live-key updates have been drained.
		if (checkpointManager_ == nullptr ||
		    !checkpointManager_->sealCheckpoint(checkpointDescriptor)) {
			safs::log_err("Failed to seal metadata checkpoint sketch");
			broadcast_metadata_saved(SAUNAFS_ERROR_IO);
			return SAUNAFS_ERROR_IO;
		}
	}

	// Changelog rotation and status broadcast still apply to both personalities.
	changelog_rotate();
	matomlserv_broadcast_logrotate();
	broadcast_metadata_saved(SAUNAFS_STATUS_OK);

	return SAUNAFS_STATUS_OK;
}

#endif  // #if !defined(METARESTORE) && !defined(METALOGGER)

int8_t MetadataBackendForkless::loadChunks(bool ignoreFlag) {
	(void)ignoreFlag;  // Unused parameter

	Timer timer;
	safs::log_info("Loading chunks from FoundationDB");

	uint64_t nextChunkId = loadedCheckpointDescriptor_.nextChunkId;
	auto status = chunk_set_next_chunkid(nextChunkId);

	if (status != SAUNAFS_STATUS_OK) {
		// Non-fatal: a missing or stale META_NEXT_CHUNK_ID (e.g. after an upgrade or partial
		// bootstrap) must not block startup. Match the file loader and continue; chunk ids seen
		// during the load below still establish the effective watermark.
		safs::log_warn("{}: could not set next chunk id to {}, continuing with chunk load",
		               __func__, nextChunkId);
	}

	kv::Key startKey = kv::toBytes(kChunkLatestKeyPrefix);
	kv::Key endKey = kv::prefixEnd(startKey);
	kv::KeySelector startSelector(startKey, true, 0);
	kv::KeySelector endSelector(endKey, true, 0);

	kv::Key lastKey;
	uint64_t chunkCount = 0;
	uint64_t maxChunkId = 0;

	while (true) {
		auto transaction = kvConnector_->getKVEngine()->createReadOnlyTransaction();
		auto pageResult =
		    transaction->getRange(startSelector, endSelector, kv::kDefaultGetRangeLimit);

		uint64_t chunkId{};
		uint32_t chunkVersion{};
		uint32_t lockedTo{};
		uint32_t lockId{};

		for (const auto &pair : pageResult.getPairs()) {
			chunkId = 0;
			chunkVersion = 0;
			lockedTo = 0;
			lockId = 0;
			const uint8_t *source = pair.key.data();

			if (pair.key.size() == kChunkLatestKeyPrefix.size() + sizeof(uint64_t)) {
				source += kChunkLatestKeyPrefix.size();  // Skip "CHNL_"
				chunkId = get64bit(&source);
			} else {
				safs::log_warn("Skipping malformed chunk key of size {}", pair.key.size());
				continue;
			}

			if (pair.value.size() == sizeof(uint32_t) * 3) {
				source = pair.value.data();
				get32bit(&source, chunkVersion);
				get32bit(&source, lockedTo);
				get32bit(&source, lockId);
			} else {
				safs::log_warn("Skipping malformed chunk value of size {} for chunk {}",
				               pair.value.size(), chunkId);
				continue;
			}

			if (chunkId > 0) {
				chunk_add_from_initial_metadata_load(chunkId, chunkVersion, lockedTo, lockId);
				maxChunkId = std::max(maxChunkId, chunkId);
				chunkCount++;
			}
		}

		if (!pageResult.hasMore() || pageResult.getPairs().empty()) { break; }

		lastKey = pageResult.getPairs().back().key;
		startSelector = kv::KeySelector(lastKey, false, 0);
	}

	// chunk_add_from_initial_metadata_load() creates chunks without advancing the id generator,
	// so a stale/missing META_NEXT_CHUNK_ID (checkpoint descriptor) could otherwise reuse an
	// already-loaded chunk id. Advance the watermark past the highest id seen in FDB.
	// chunk_set_next_chunkid() only moves the generator forward, so this never lowers it.
	if (maxChunkId > 0) { chunk_set_next_chunkid(maxChunkId + 1); }

	// Apply undo checkpoints so that chunk state matches the loaded checkpoint version
	// Target version is the metadataVersion value we loaded into loadedCheckpointDescriptor_
	const auto targetVersion = loadedCheckpointDescriptor_.metadataVersion;

	if (checkpointManager_ != nullptr && !checkpointManager_->restoreSectionToCheckpointVersion(
	                                         MetadataSectionKind::Chunk, targetVersion)) {
		safs::log_err("{}: failed to roll back chunks to checkpoint version {}", __func__,
		              targetVersion);
		return kOpFailure;
	}

	safs::log_info("Loaded {} chunks", chunkCount);
	safs::log_info("Section loaded successfully (CHNK 1.0): {}s", timer.elapsed_s());

	return kOpSuccess;
}

MetadataCheckpointDescriptor MetadataBackendForkless::buildCheckpointDescriptor() const {
	MetadataCheckpointDescriptor descriptor;
	descriptor.maxInodeId = gMetadata->maxInodeId().getValue();
	descriptor.metadataVersion = gMetadata->metadataVersion;
	descriptor.nextSessionId = gMetadata->nextSessionId().getValue();
	descriptor.nextChunkId = chunk_get_next_id();
	return descriptor;
}

void MetadataBackendForkless::applyCheckpointDescriptor(
    const MetadataCheckpointDescriptor &descriptor) {
	loadedCheckpointDescriptor_ = descriptor;
	gMetadata->maxInodeId().setValue(descriptor.maxInodeId);
	gMetadata->metadataVersion = descriptor.metadataVersion;
	gMetadata->nextSessionId().setValue(descriptor.nextSessionId);
}

void MetadataBackendForkless::onNodeChanged(FSNode *node) {
	if (node == nullptr) {
		safs::log_err("{}: received null node, skipping metadata update", __func__);
		return;
	}
	if (metadataWriter_) { metadataWriter_->enqueue(std::make_unique<NodeUpdateEvent>(node)); }
}

void MetadataBackendForkless::onNodeRemoved(inode_t nodeId) {
	if (metadataWriter_) { metadataWriter_->enqueue(std::make_unique<NodeRemoveEvent>(nodeId)); }
}

void MetadataBackendForkless::onEdgeChanged(inode_t parentId, inode_t childId,
                                           const HString &name) {
	if (metadataWriter_) {
		metadataWriter_->enqueue(std::make_unique<EdgeUpdateEvent>(parentId, name, childId));
	}
}

void MetadataBackendForkless::onEdgeRemoved(inode_t parentId, const HString &name) {
	if (metadataWriter_) {
		metadataWriter_->enqueue(std::make_unique<EdgeRemoveEvent>(parentId, name));
	}
}

void MetadataBackendForkless::onXAttrInodeRemoved(inode_t inode) {
	if (metadataWriter_) {
		metadataWriter_->enqueue(std::make_unique<XAttrInodeRemoveEvent>(inode));
	}
}

void MetadataBackendForkless::onXAttrChanged(inode_t inode, std::span<const uint8_t> name,
                                             std::span<const uint8_t> value) {
	if (metadataWriter_) {
		metadataWriter_->enqueue(std::make_unique<XAttrUpdateEvent>(inode, name, value));
	}
}

void MetadataBackendForkless::onXAttrRemoved(inode_t inode, std::span<const uint8_t> name) {
	if (metadataWriter_) {
		metadataWriter_->enqueue(std::make_unique<XAttrRemoveEvent>(inode, name));
	}
}

int MetadataBackendForkless::fsLoad(bool ignoreFlag) {
	for (const auto &section : metadataSections_) {
		auto result = section.loadFunction(ignoreFlag);

		if (result != kOpSuccess) {
			safs::log_err("Failed to load section: {}", section.name);
			return result;
		}
	}

	// Initialize the root node pointer after all nodes have been loaded and registered in gMetadata
	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);

	gMetadata->root =
	    gFSOperations->nodeOperations()->idToNode<FSNodeDirectory>(fsOpContext, SPECIAL_INODE_ROOT);

	if (gMetadata->root == nullptr) {
		safs::log_err("Error reading metadata: root node not found");
		return kOpFailure;
	}

	if (gMetadata->root->type != FSNodeType::kDirectory) {
		safs::log_err("Error reading metadata: root node not a directory");
		return kOpFailure;
	}

	return kOpSuccess;
}

#ifndef METARESTORE
namespace {
void fs_new() {
	gMetadata->maxInodeId().setValue(SPECIAL_INODE_ROOT);
	gMetadata->metadataVersion = 1;
	gMetadata->nextSessionId().setValue(1);

	auto *rootDirectory = FSNode::create(FSNodeType::kDirectory);
	gMetadata->root = dynamic_cast<FSNodeDirectory *>(rootDirectory);
	gMetadata->root->id = SPECIAL_INODE_ROOT;
	gMetadata->root->atime = eventloop_time();
	gMetadata->root->mtime = gMetadata->root->atime;
	gMetadata->root->ctime = gMetadata->root->mtime;
	gMetadata->root->goal = DEFAULT_GOAL;
	gMetadata->root->trashtime = kDefaultTrashTime;
	gMetadata->root->mode = 0777;
	gMetadata->root->uid = 0;
	gMetadata->root->gid = 0;

	gMetadata->addNode(gMetadata->root);  // Add the root dir and save it to database
	gMetadata->inodePool.markAsAcquired(gMetadata->root->id);

	gChunkOperations->newfs();

	gMetadata->nodes = 1;
	gMetadata->dirNodes = 1;
	gMetadata->fileNodes = 0;

	gFSOperations->metadataChecksum(ChecksumMode::kForceRecalculate);
	fsnodes_quota_update(gMetadata->root, {{QuotaResource::kInodes, +1}});
}
}  // namespace
#endif  // #ifndef METARESTORE

int8_t MetadataBackendForkless::loadNodes(bool ignoreFlag) {
	(void)ignoreFlag;  // Unused parameter

	Timer timer;
	safs::log_info("Loading nodes from FoundationDB");

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	kv::Key startKey = kv::encodeKeyBE(kNodeKeyPrefix, SPECIAL_INODE_ROOT);
	kv::Key endKey = kv::prefixEnd(kv::toBytes(kNodeKeyPrefix));
	kv::KeySelector startSelector(startKey, true, 0);
	kv::KeySelector endSelector(endKey, true, 0);

	while (true) {
		auto transaction = kvConnector_->getKVEngine()->createReadOnlyTransaction();
		auto pageResult =
		    transaction->getRange(startSelector, endSelector, kv::kDefaultGetRangeLimit);

		for (const auto &pair : pageResult.getPairs()) {
			if (pair.value.empty()) {
				safs::log_err("Error loading node: empty value in database");
				return kOpFailure;
			}

			const uint8_t *source = pair.value.data();
			auto type = static_cast<FSNodeType>(source[0]);
			FSNode *node = FSNode::create(type);
			if (node == nullptr) {
				safs::log_err("Error loading node: failed to create node of type {}",
				              static_cast<char>(type));
				return kOpFailure;
			}
			node->deserialize(&source);

			int8_t status = loadNode(fsOpContext, node);

			if (status < 0) {
				safs::log_err("Error loading node: {}", node->id);
				FSNode::destroy(node);
				return kOpFailure;
			}
		}

		if (!pageResult.hasMore() || pageResult.getPairs().empty()) { break; }

		kv::Key lastKey = pageResult.getPairs().back().key;
		startSelector = kv::KeySelector(lastKey, false, 0);
	}

	safs::log_info("Loaded {} nodes", gMetadata->nodes);
	safs::log_info("Section loaded successfully (NODE 1.0): {}s", timer.elapsed_s());

	return kOpSuccess;
}

int8_t MetadataBackendForkless::loadNode(const FilesystemOperationContext &fsOpContext,
                                         FSNode *node) {
	if (node == nullptr) {
		safs::log_err("{}: received null node, skipping", __func__);
		return kOpFailure;
	}
#ifndef METARESTORE
	auto *nodeFile = static_cast<FSNodeFile *>(node);
#endif

	switch (node->type) {
	case FSNodeType::kDirectory:
		gMetadata->dirNodes++;
		break;
	case FSNodeType::kSocket:
	case FSNodeType::kFifo:
	case FSNodeType::kBlockDev:
	case FSNodeType::kCharDev:
		// Nothing extra to do
		break;
	case FSNodeType::kSymlink:
		gMetadata->linkNodes++;
		break;
	case FSNodeType::kFile:
	case FSNodeType::kTrash:
	case FSNodeType::kReserved:
#ifndef METARESTORE
		for (const auto &sessionId : nodeFile->sessionIds) {
			matoclserv_add_open_file(sessionId, node->id);
		}
#endif
		fsnodes_quota_update(
		    node,
		    {{QuotaResource::kSize, +gFSOperations->nodeOperations()->getSize(fsOpContext, node)}});
		gMetadata->fileNodes++;
		break;
	default:
		safs::log_err("Loading node: unrecognized node type: {}", static_cast<char>(node->type));
		fsnodes_quota_update(node, {{QuotaResource::kInodes, +1}});
		return kOpFailure;
	}

	gMetadata->addNode(node, true);
	gMetadata->inodePool.markAsAcquired(node->id);
	gMetadata->nodes++;
	fsnodes_quota_update(node, {{QuotaResource::kInodes, +1}});

	return kOpSuccess;
}

int8_t MetadataBackendForkless::loadFree(bool ignoreFlag) {
	(void)ignoreFlag;  // Unused parameter

	safs::log_info("Loading free nodes");
	Timer timer;

	kv::Key startKey = kv::toBytes(kFreeKeyPrefix);
	kv::Key endKey = kv::prefixEnd(startKey);
	kv::KeySelector startSelector(startKey, true, 0);
	kv::KeySelector endSelector(endKey, true, 0);

	while (true) {
		auto transaction = kvConnector_->getKVEngine()->createReadOnlyTransaction();
		auto pageResult =
		    transaction->getRange(startSelector, endSelector, kv::kDefaultGetRangeLimit);

		inode_t inode{};
		uint32_t timeStamp{};

		for (const auto &pair : pageResult.getPairs()) {
			if (pair.key.size() < kFreeKeyPrefix.size() + sizeof(inode_t)) {
				safs::log_err("{}: malformed key of size {}", __func__, pair.key.size());
				return kOpFailure;
			}
			if (pair.value.size() < sizeof(uint32_t)) {
				safs::log_err("{}: malformed value of size {}", __func__, pair.value.size());
				return kOpFailure;
			}

			const uint8_t *source = pair.key.data();
			source += kFreeKeyPrefix.size();  // Skip "FREE_"
			getINode(&source, inode);

			source = pair.value.data();
			get32bit(&source, timeStamp);

			gMetadata->inodePool.detain(inode, timeStamp, true);
		}

		if (!pageResult.hasMore() || pageResult.getPairs().empty()) { break; }

		kv::Key lastKey = pageResult.getPairs().back().key;
		startSelector = kv::KeySelector(lastKey, false, 0);
	}

	// Connect the signal handlers after initial loading to avoid triggering them for already loaded
	// free nodes.
	gMetadata->inodePool.detainedAddedSignal.connect([this](inode_t inode, uint32_t timestamp) {
		if (metadataWriter_) {
			metadataWriter_->enqueue(std::make_unique<FreeNodeUpdateEvent>(inode, timestamp));
		}
	});

	gMetadata->inodePool.detainedRemovedSignal.connect([this](inode_t inode) {
		if (metadataWriter_) {
			metadataWriter_->enqueue(std::make_unique<FreeNodeUpdateEvent>(inode));
		}
	});

	safs::log_info("Section loaded successfully (FREE 1.0): {}s", timer.elapsed_s());
	return kOpSuccess;
}

int8_t MetadataBackendForkless::loadXAttr(bool ignoreFlag) {
	safs::log_info("Loading xattrs from FoundationDB");
	Timer timer;

	kv::Key startKey = kv::toBytes(kXAttrKeyPrefix);
	kv::Key endKey = kv::prefixEnd(startKey);
	kv::KeySelector startSelector(startKey, true, 0);
	kv::KeySelector endSelector(endKey, true, 0);

	const size_t kMinKeySize = kXAttrKeyPrefix.size() + sizeof(inode_t);

	/// Format: XATR_<InodeId><AttributeName>:<AttributeValue>
	/// e.g.: XATR_1999UserAttr:UserValue
	XAttributeInodeEntry *xattrInodeEntry = nullptr;

	while (true) {
		xattrInodeEntry = nullptr;  // Reset pointer to avoid stale values
		auto transaction = kvConnector_->getKVEngine()->createReadOnlyTransaction();
		auto pageResult =
		    transaction->getRange(startSelector, endSelector, kv::kDefaultGetRangeLimit);
		const auto &pairs = pageResult.getPairs();

		inode_t inode{};

		bool exceeded = false;
		for (const auto &pair : pairs) {
			if (pair.key.size() <= kMinKeySize) {
				safs::log_warn("Loading xattr: empty attribute name, skipping key");
				continue;
			}

			const uint8_t *source = pair.key.data();
			source += kXAttrKeyPrefix.size();  // Skip "XATR_"
			getINode(&source, inode);

			auto attributeNameBegin = pair.key.begin() + kMinKeySize;
			auto attributeNameSize = pair.key.end() - attributeNameBegin;
			if (attributeNameSize > SFS_XATTR_NAME_MAX) {
				safs::log_err("Loading xattr: attribute name too long");
				if (ignoreFlag) {
					safs::log_err(
					    "Ignoring xattr with name size {}, exceeding max of {}, due to ignore flag",
					    attributeNameSize, SFS_XATTR_NAME_MAX);
					continue;
				}
				exceeded = true;
				break;
			}

			auto attributeNameLength = static_cast<uint8_t>(attributeNameSize);
			auto attributeValueLength = static_cast<uint32_t>(pair.value.size());

			if (attributeValueLength > SFS_XATTR_SIZE_MAX) {
				safs::log_err("Loading xattr: value oversized");
				if (ignoreFlag) {
					safs::log_err(
					    "Ignoring xattr with value size {}, exceeding max of {}, due to ignore flag",
					    attributeValueLength, SFS_XATTR_SIZE_MAX);
					continue;
				}
				exceeded = true;
				break;
			}

			auto inodeHash = get_xattr_inode_hash(inode);
			xattrInodeEntry = find_xattr_inode_entry(inode, inodeHash);

			if (xattrInodeEntry != nullptr &&
			    xattrInodeEntry->attributeNameLength + attributeNameLength + 1 >
			        SFS_XATTR_LIST_MAX) {
				safs::log_err("Loading xattr: name list too long");
				if (ignoreFlag) {
					safs::log_err(
					    "Ignoring xattr with name list size {}, exceeding max of {}, due to ignore flag",
					    xattrInodeEntry->attributeNameLength + attributeNameLength + 1,
					    SFS_XATTR_LIST_MAX);
					continue;
				}
				exceeded = true;
				break;
			}

			auto xattrEntry = std::make_unique<XAttributeDataEntry>();
			xattrEntry->inode = inode;
			xattrEntry->attributeName.resize(attributeNameLength);
			passert(xattrEntry->attributeName.data());
			memcpy(xattrEntry->attributeName.data(), pair.key.data() + kMinKeySize,
			       attributeNameLength);

			if (attributeValueLength > 0) {
				xattrEntry->attributeValue.resize(attributeValueLength);
				passert(xattrEntry->attributeValue.data());
				memcpy(xattrEntry->attributeValue.data(), pair.value.data(), attributeValueLength);
			} else {
				xattrEntry->attributeValue.clear();
			}

			auto dataHash =
			    get_xattr_data_hash(inode, attributeNameLength, xattrEntry->attributeName.data());

			gMetadata->xattrDataHash[dataHash].push_back(std::move(xattrEntry));
			auto *xattrEntryPointer = gMetadata->xattrDataHash[dataHash].back().get();

			if (xattrInodeEntry != nullptr) {
				xattrInodeEntry->xattrDataEntries.push_back(xattrEntryPointer);
				xattrInodeEntry->attributeNameLength += attributeNameLength + 1U;
				xattrInodeEntry->attributeValueLength += attributeValueLength;
			} else {
				auto newXAttrInodeEntry = XAttributeInodeEntry::create(
				    inode, attributeNameLength + 1U, attributeValueLength);
				newXAttrInodeEntry->xattrDataEntries.push_back(xattrEntryPointer);
				gMetadata->xattrInodeHash[inodeHash].push_back(std::move(newXAttrInodeEntry));
			}
		}

		if (exceeded) { return SAUNAFS_ERROR_ERANGE; }
		if (!pageResult.hasMore() || pageResult.getPairs().empty()) { break; }

		// Advance the start selector past the last key in this page.
		startSelector = kv::KeySelector(pageResult.getPairs().back().key, false, 0);
	}

	safs::log_info("Section loaded successfully (XATR 1.0): {}s", timer.elapsed_s());
	return kOpSuccess;
}

int8_t MetadataBackendForkless::loadEdges(bool ignoreFlag) {
	safs::log_info("Loading edges from FoundationDB");
	Timer timer;

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadOnly);

	kv::Key startKey = kv::toBytes(kEdgeKeyPrefix);
	kv::Key endKey = kv::prefixEnd(startKey);
	kv::KeySelector startSelector(startKey, true, 0);
	kv::KeySelector endSelector(endKey, true, 0);

	loadEdge(fsOpContext, 0, 0, "init", true, true);

	// EDGE_<ParentId><Name>: <ChildId>. e.g.: EDGE_1999ChildName: 2535

	inode_t parentId{};
	inode_t childId{};
	std::string edgeName{};

	int8_t status = kOpSuccess;
	constexpr size_t kEdgePageSize = 1000;  // Number of entries to fetch per page
	// kMinEdgeKeySize = Prefix size plus sizeof(inode_t) (parentId) and at least one byte for name
	constexpr size_t kMinEdgeKeySize = kEdgeKeyPrefix.size() + sizeof(inode_t) + 1;

	while (true) {
		auto transaction = kvConnector_->getKVEngine()->createReadOnlyTransaction();
		auto pageResult = transaction->getRange(startSelector, endSelector, kEdgePageSize);

		for (const auto &pair : pageResult.getPairs()) {
			if (pair.key.size() < kMinEdgeKeySize) {
				safs::log_err("loading edge: malformed key");
				continue;  // Malformed key, skip
			}

			const uint8_t *source = pair.key.data();
			source += kEdgeKeyPrefix.size();  // Skip "EDGE_"
			getINode(&source, parentId);

			auto nameSize = pair.key.size() - kEdgeKeyPrefix.size() - sizeof(inode_t);
			edgeName = std::string(reinterpret_cast<const char *>(source), nameSize);

			if (pair.value.size() < sizeof(inode_t)) {
				safs::log_err("loading edge: {} {} error: malformed value", parentId, edgeName);
				continue;
			}

			source = pair.value.data();
			getINode(&source, childId);

			status = loadEdge(fsOpContext, parentId, childId, edgeName, ignoreFlag, false);

			if (status < 0) {
				safs::log_err("Error loading edge: {} -> {} : {}", parentId, childId, edgeName);
				return kOpFailure;
			}
		}

		if (!pageResult.hasMore() || pageResult.getPairs().empty()) { break; }

		kv::Key lastKey = pageResult.getPairs().back().key;
		startSelector = kv::KeySelector(lastKey, false, 0);
	}

	safs::log_info("Section loaded successfully (EDGE 1.0): {}s", timer.elapsed_s());
	return kOpSuccess;
}

int8_t MetadataBackendForkless::loadEdge(const FilesystemOperationContext &fsOpContext,
                                         inode_t parentId, inode_t childId, const std::string &name,
                                         bool ignoreFlag, bool init) {
	if (init) {
		currentLoadParentId_ = 0;
		return kOpSuccess;
	}

	FSNode *child = gFSOperations->nodeOperations()->idToNode(fsOpContext, childId);

	if (child == nullptr) {
		safs::log_err("loading edge: {}, {}->{} error: child not found", parentId,
		              gFSOperations->nodeOperations()->escapeName(name), childId);

		if (ignoreFlag) { return kOpSuccess; }

		return kOpFailure;
	}

	if (parentId == 0U) {
		if (child->type == FSNodeType::kTrash) {
			gMetadata->trash.insert({TrashPathKey(child), hstorage::Handle(name)});
			gMetadata->trashSpace += static_cast<FSNodeFile *>(child)->length;
			gMetadata->trashNodes++;
		} else if (child->type == FSNodeType::kReserved) {
			gMetadata->reserved.insert({child->id, hstorage::Handle(name)});
			gMetadata->reservedSpace += static_cast<FSNodeFile *>(child)->length;
			gMetadata->reservedNodes++;
		} else {
			safs::log_err("loading edge: {}, {}->{} error: bad child type ({})", parentId,
			              gFSOperations->nodeOperations()->escapeName(name), childId,
			              static_cast<char>(child->type));
			return kOpFailure;
		}
	} else {
		auto *parent =
		    gFSOperations->nodeOperations()->idToNode<FSNodeDirectory>(fsOpContext, parentId);

		if (parent == nullptr) {
			safs::log_err("loading edge: {}, {}->{} error: parent not found", parentId,
			              gFSOperations->nodeOperations()->escapeName(name), childId);

			if (ignoreFlag) {
				parent = gFSOperations->nodeOperations()->idToNode<FSNodeDirectory>(
				    fsOpContext, SPECIAL_INODE_ROOT);

				if (parent == nullptr || parent->type != FSNodeType::kDirectory) {
					safs::log_err("loading edge: {}, {}->{} root dir not found !!!", parentId,
					              gFSOperations->nodeOperations()->escapeName(name), childId);
					return kOpFailure;
				}

				safs::log_err("loading edge: {}, {}->{} attaching node to root dir", parentId,
				              gFSOperations->nodeOperations()->escapeName(name), childId);
				parentId = SPECIAL_INODE_ROOT;
			} else {
				safs::log_err("use sfsmetarestore (option -i) to attach this node to root dir");
				return kOpFailure;
			}
		}

		if (parent->type != FSNodeType::kDirectory) {
			safs::log_err("loading edge: {}, {}->{} error: bad parent type ({})", parentId,
			              gFSOperations->nodeOperations()->escapeName(name), childId,
			              static_cast<char>(parent->type));

			if (ignoreFlag) {
				parent = gFSOperations->nodeOperations()->idToNode<FSNodeDirectory>(
				    fsOpContext, SPECIAL_INODE_ROOT);

				if (parent == nullptr || parent->type != FSNodeType::kDirectory) {
					safs::log_err("loading edge: {}, {}->{} root dir not found !!!", parentId,
					              gFSOperations->nodeOperations()->escapeName(name), childId);
					return kOpFailure;
				}

				safs::log_err("loading edge: {}, {}->{} attaching node to root dir", parentId,
				              gFSOperations->nodeOperations()->escapeName(name), childId);
				parentId = SPECIAL_INODE_ROOT;
			} else {
				safs::log_err("use sfsmetarestore (option -i) to attach this node to root dir");
				return kOpFailure;
			}
		}

		if (currentLoadParentId_ != parentId) {
			if (parent->entries.size() > 0) {
				safs::log_err("loading edge: {}, {}->{} error: parent node sequence error",
				              parentId, gFSOperations->nodeOperations()->escapeName(name), childId);
				return kOpFailure;
			}

			currentLoadParentId_ = parentId;
		}

		auto handleOwner = std::make_unique<hstorage::Handle>(name);
		hstorage::Handle *handlePtr = handleOwner.get();
		if (parent->entries.insert({handlePtr, child}).second) {
			// On successful insert, the parent now owns the handle
			handleOwner.release();  // NOLINT(bugprone-unused-return-value)
			parent->entries_hash ^= handlePtr->hash();
		} else {
			// insert failed → unique_ptr cleans up automatically
			safs::log_err("{}: duplicate entry {}->{} in directory {}", __func__,
			              gFSOperations->nodeOperations()->escapeName(name), childId, parentId);
			return kOpFailure;
		}

		child->parents.push_back({parent->id, handlePtr});

		if (child->type == FSNodeType::kDirectory) {
			parent->nlink++;
		}

		StatsRecord statsRecord{};
		gFSOperations->nodeOperations()->getStats(fsOpContext, child, &statsRecord);
		gFSOperations->nodeOperations()->addStats(fsOpContext, parent, &statsRecord);
	}

	return kOpSuccess;
}

namespace {
bool isNewMetadataHeader([[maybe_unused]] const std::string& headerSignature) {
	[[maybe_unused]] static constexpr std::string_view kMetadataHeaderNew(SFSSIGNATURE "M NEW");
	[[maybe_unused]] static constexpr std::string_view kMetadataHeaderOld(SAUSIGNATURE "M NEW");
#ifndef METARESTORE
	if (metadataserver::isMaster()) {
		if (headerSignature == kMetadataHeaderNew || headerSignature == kMetadataHeaderOld) {
			fs_new();
			safs::log_info("Detected new metadata header in FDB Backend");
			// Persist the root node and seal the initial checkpoint before finalizing the header.
			// The header stays at the "M NEW" placeholder until this succeeds, so a crash can only
			// leave either "M NEW" (fresh init re-runs idempotently) or a fully durable "M 2.9",
			// never a finalized header with a missing root node or checkpoint.
			if (gMetadataBackend->fs_storeall(DumpType::kForegroundDump) == SAUNAFS_STATUS_OK) {
				initializeNewMetadataHeaderSignal.emit();
			} else {
				safs::log_err(
				    "Failed to persist initial metadata during fresh initialization; leaving M NEW "
				    "header for retry on next start");
			}
			return true;
		}
	}
#endif /* #ifndef METARESTORE */
	return false;
}

bool checkMetadataSignature() {
	static constexpr std::string_view kMetadataHeaderNewV2_9(SFSSIGNATURE "M 2.9");
	static constexpr std::string_view kMetadataHeaderOldV2_9(SAUSIGNATURE "M 2.9");
	static constexpr std::string_view kMetadataHeaderLegacy("LIZM 2.9");

	const std::string headerSignature = gForklessBackend->getHeaderSignature();

	if (isNewMetadataHeader(headerSignature)) { return false; }

	if (headerSignature != kMetadataHeaderNewV2_9 && headerSignature != kMetadataHeaderOldV2_9 &&
	    headerSignature != kMetadataHeaderLegacy) {
		throw MetadataConsistencyException("wrong metadata header version");
	}

	return true;
}
}  // namespace

#ifndef METALOGGER
void MetadataBackendForkless::loadall(int ignoreflag) {
	safs::log_info("MetadataBackendForkless::loadall: ignoreflag: {}", ignoreflag);

	bool bootstrapped = false;

#ifndef METARESTORE
	// Bootstrap must start from a headerless-and-empty FDB store. If filesystem sections already
	// exist without META_HEADER, the store is in a partially initialized state and we must fail
	// fast instead of trying to continue migration implicitly.
	if (metadataserver::isMaster() && getHeaderSignature().empty() &&
	    ::hasPersistedMetadataSectionData(kvConnector_->getKVEngine(), metadataSections_)) {
		throw MetadataConsistencyException(
		    "inconsistent forkless metadata state: metadata sections exist without metadata header");
	}

	if (metadataserver::isMaster() && sectionBootstrapper_ != nullptr) {
		bootstrapped = sectionBootstrapper_->bootstrapSections();
		if (bootstrapped) {
			safs::log_info("Metadata sections bootstrapped successfully");
		}
	}
	sectionBootstrapper_.reset();
	sectionBootstrapper_ = nullptr;
	safs::log_info("Metadata bootstrapping stage finished");

	// Re-check after bootstrap because section loaders flush filesystem metadata before
	// saveMetadataHeader() writes META_HEADER. A failed bootstrap can therefore leave section data
	// behind while the header is still empty; the empty-store fallback must not turn that state
	// into a brand-new filesystem.
	// Only a truly fresh store is allowed to enter the M NEW path, so keep the version check too.
	if (metadataserver::isMaster() && getHeaderSignature().empty() && getVersion("") == 0) {
		if (::hasPersistedMetadataSectionData(kvConnector_->getKVEngine(), metadataSections_)) {
			throw MetadataConsistencyException(
			    "inconsistent forkless metadata state: metadata sections exist without metadata header");
		}

		safs::log_warn("Initializing empty forkless metadata header");
		initializeEmptyMetadataHeader();
	}
#endif

	// Check metadata signature

	bool isSignatureValid = checkMetadataSignature();

	if (!isSignatureValid && !bootstrapped) { return; }

	// Load metadata global properties from the current checkpoint descriptor.

	if (checkpointManager_ == nullptr) {
		throw MetadataConsistencyException(
		    "checkpoint manager is not initialized for forkless metadata backend");
	}
	applyCheckpointDescriptor(checkpointManager_->loadLatestCheckpoint());

	// Load the metadata sections

	if (fsLoad(ignoreflag) != kOpSuccess) {
		throw MetadataConsistencyException(MetadataStructureReadErrorMsg);
	}

	safs::log_info("connecting files and chunks");
	{
		util::ScopedTimer timer("connecting files and chunks took");
		gFSOperations->addFilesToChunks();
	}

	safs::log_info("calculating checksum of the metadata");
	{
		util::ScopedTimer timer("calculating checksum of the metadata took");
		gFSOperations->metadataChecksum(ChecksumMode::kForceRecalculate);
	}

#ifndef METARESTORE
	safs::log_info(
	    "metadata read ({} inodes including {} directory inodes, {} file inodes, "
	    "{} symlink inodes and {} chunks)",
	    gMetadata->nodes, gMetadata->dirNodes, gMetadata->fileNodes, gMetadata->linkNodes,
	    gChunkOperations->count());
#endif
}

void MetadataBackendForkless::store_fd(FILE *fd) {
	safs::log_info("MetadataBackendForkless::store_fd: fd: {}", fd->_fileno);
}

#endif  // #ifndef METALOGGER

bool MetadataBackendForkless::flushPendingUpdates(bool flushAll) {
	if (!metadataWriter_) { return false; }
	if (flushAll) {
		return metadataWriter_->flush(MetadataWriterFDB::FlushMode::kDrainUntilEmpty);
	}

	return metadataWriter_->flush();
}

void MetadataBackendForkless::onPromotedToMaster() {
	// Idempotent: a node promoted once already has its writer; ignore repeat promotions.
	if (metadataWriter_ != nullptr) { return; }

	safs::log_info("MetadataBackendForkless: promoted to master, initializing metadata writer");

	// Re-read the durable checkpoint catalog before writing: the old master kept sealing
	// checkpoints while this node was a shadow, so the catalog loaded at startup is stale.
	// In-memory metadata is authoritative here (kept current by changelog replay), so only the
	// checkpoint catalog is refreshed, not the metadata globals.
	checkpointManager_->reloadDurableCheckpointState();

	metadataWriter_ =
	    std::make_unique<MetadataWriterFDB>(kvConnector_->getKVEngine(), checkpointManager_.get());
	registerFlushTimer();
}

void MetadataBackendForkless::registerFlushTimer() {
	if (flushTimerHandle_ != nullptr) { return; }  // already registered; never stack timers
	flushTimerHandle_ = eventloop_timeregister_ms(kMetadataFlushIntervalMs, flushMetadataCallback);

	// Install a destruct hook bound to the current eventloop's lifetime. It runs during
	// eventloop_destruct() — before eventloop_release_resources() clears the time table — and
	// clears the cached handle so the (later) static destructor does not unregister an
	// already-removed timer. Rebound on each fresh registration, so it also survives an
	// eventloop that is torn down and recreated (e.g. across test cases).
	eventloop_destructregister(forklessFlushTimerTeardown);
}

void MetadataBackendForkless::initSections() {
	metadataSections_.emplace_back("NODE 1.0", kNodeKeyPrefix,
	                               [this](bool flag) { return loadNodes(flag); });
	metadataSections_.emplace_back("EDGE 1.0", kEdgeKeyPrefix,
	                               [this](bool flag) { return loadEdges(flag); });
	metadataSections_.emplace_back("FREE 1.0", kFreeKeyPrefix,
	                               [this](bool flag) { return loadFree(flag); });
	metadataSections_.emplace_back("XATR 1.0", kXAttrKeyPrefix,
	                               [this](bool flag) { return loadXAttr(flag); });
	/*metadataSections_.emplace_back("ACLS 1.2", "ACLS_", loadACLs);
	metadataSections_.emplace_back("QUOT 1.1", "QUOT_", loadQuotas);
	metadataSections_.emplace_back("FLCK 1.0", "FLCK_", loadLocks);*/
	metadataSections_.emplace_back("CHNK 1.0", kChunkLatestKeyPrefix,
	                               [this](bool flag) { return loadChunks(flag); });
}

void MetadataBackendForkless::initializeNewMetadataHeader() {
	auto transaction = kvConnector_->getKVEngine()->createReadWriteTransaction();
	transaction->set(kv::toBytes(kMetaHeaderKey), kv::toBytes(SFSSIGNATURE "M 2.9"));

	if (!transaction->commit()) {
		const auto *message = "Failed to initialize metadata header in FDB";
		safs::log_err(message);
		throw MetadataConsistencyException(message);
	}

	safs::log_info("Metadata header initialized successfully in FDB");
}

void MetadataBackendForkless::initializeEmptyMetadataHeader() {
	auto transaction = kvConnector_->getKVEngine()->createReadWriteTransaction();
	transaction->set(kv::toBytes(kMetaHeaderKey), kv::toBytes(SFSSIGNATURE "M NEW"));

	if (!transaction->commit()) {
		const auto *message = "Failed to initialize empty metadata header in FDB";
		safs::log_err(message);
		throw MetadataConsistencyException(message);
	}
}

void MetadataBackendForkless::init() {
	kvConnector_ = std::make_shared<KVConnectorFDB>();

	if (kvConnector_->init()) {
		safs::log_info("KV store initialized successfully");
	} else {
		safs::log_err("Failed to initialize KV store");
		throw std::runtime_error("Failed to initialize KV store");
	}

	checkpointManager_ = std::make_unique<MetadataCheckpointManager>(kvConnector_->getKVEngine());

	// Writer and flush timer are only initialized for the master personality.
	// Shadows must not write to the shared FDB database; all on* signal handlers already guard on
	// metadataWriter_ != nullptr, so no events reach FDB while the pointer stays null.
	// onPromotedToMaster() creates the writer and registers the timer on shadow->master promotion.
	if (metadataserver::isMaster()) {
		metadataWriter_ = std::make_unique<MetadataWriterFDB>(kvConnector_->getKVEngine(),
		                                                      checkpointManager_.get());
		registerFlushTimer();
	}

	// Register the promotion callback so a shadow that becomes master starts writing to FDB.
	metadataserver::registerFunctionCalledOnPromotion(forklessBackendBecameMaster);

#ifndef METARESTORE
	sectionBootstrapper_ =
	    std::make_unique<MetadataSectionBootstrapFDB>(kvConnector_->getKVEngine());
#endif  // #ifndef METARESTORE

	uint64_t version = getVersion("");

	gMetadata = new FilesystemMetadata;

	// Wires both the process-global signals (once) and the per-load gMetadata signals.
	// gChunkChangedSignal is among the global ones; it is connected here, still before any
	// runtime chunk mutation, and the slot guards on metadataWriter_ (null on shadows).
	createConnections();

	safs::log_info("MetadataBackendForkless version: {}", version);
}

uint64_t MetadataBackendForkless::getVersion(const std::string & /*file*/) {
	if (kvConnector_ == nullptr || kvConnector_->getKVEngine() == nullptr) {
		safs::log_err("{}: KV connector/engine unavailable, returning version 0", __func__);
		return 0;
	}

	auto transaction = kvConnector_->getKVEngine()->createReadOnlyTransaction();
	kv::Key versionKey{kv::toBytes(kMetaVersionKey)};

	auto result = transaction->get(versionKey);

	if (result != std::nullopt && result->size() >= sizeof(uint64_t)) {
		const uint8_t *data = result.value().data();
		uint64_t version = get64bit(&data);
		return version;
	}

	return 0;
}

std::string MetadataBackendForkless::getHeaderSignature() {
	if (kvConnector_ == nullptr || kvConnector_->getKVEngine() == nullptr) {
		safs::log_err("{}: KV connector/engine unavailable, returning empty signature", __func__);
		return "";
	}

	auto transaction = kvConnector_->getKVEngine()->createReadOnlyTransaction();
	kv::Key headerKey{kv::toBytes(kMetaHeaderKey)};

	auto result = transaction->get(headerKey);

	if (result != std::nullopt) {
		const uint8_t *data = result.value().data();
		std::string signature(reinterpret_cast<const char *>(data), result.value().size());
		return signature;
	}

	return "";
}

void MetadataBackendForkless::createConnections() {
	// Process-global signals (gChunkChangedSignal, gXAttr*, initializeNewMetadataHeaderSignal):
	// connect once per process. They are never recreated and Signal has no per-slot disconnect,
	// so reconnecting on each backend init would stack duplicate slots.
	connectGlobalSignalsOnce();

	// Per-load signals on gMetadata: recreated fresh each load, so they never accumulate.
	gMetadata->nodeChangedSignal.connect([this](FSNode *node) { onNodeChanged(node); });

	gMetadata->nodeRemovedSignal.connect([this](inode_t nodeId) { onNodeRemoved(nodeId); });

	// gMetadata->edgeChangedSignal.connect(kvConnector_.get(), &IKVConnector::onEdgeChanged);

	// gMetadata->edgeRemovedSignal.connect(kvConnector_.get(), &IKVConnector::onEdgeRemoved);

	gMetadata->edgeChangedSignal.connect(
	    [this](FSNodeDirectory *parent, FSNode *child, hstorage::Handle *handlePtr) {
		    onEdgeChanged(parent->id, child->id, handlePtr->get());
	    });

	gMetadata->edgeRemovedSignal.connect(
	    [this](inode_t parentId, const HString &name) { onEdgeRemoved(parentId, name); });
}

void MetadataBackendForkless::connectGlobalSignalsOnce() {
	// These signals are process-global (file-scope / inline), so they outlive any single backend
	// instance and Signal has no per-slot disconnect. Connect exactly once per process: a second
	// backend (e.g. recreated in tests) would otherwise stack duplicate slots and enqueue every
	// mutation once per stale slot. Slots capture nothing and route through gForklessBackend
	// (cleared in the destructor), so the single connection always targets the current backend.
	static bool connected = false;
	if (connected) { return; }
	connected = true;

	// gChunkChangedSignal handler guards on metadataWriter_ (null on shadows), so shadow-side
	// chunk mutations are not persisted until promotion (see onPromotedToMaster()).
	gChunkChangedSignal.connect(
	    [](uint64_t chunkId, uint32_t version, uint32_t lockedTo, uint32_t lockId) {
		    if (gForklessBackend != nullptr && gForklessBackend->metadataWriter_) {
			    gForklessBackend->metadataWriter_->enqueue(
			        std::make_unique<ChunkUpdateEvent>(chunkId, version, lockedTo, lockId));
		    }
	    });

	gXAttrInodeRemovedSignal.connect([](inode_t inode) {
		if (gForklessBackend != nullptr) { gForklessBackend->onXAttrInodeRemoved(inode); }
	});

	gXAttrChangedSignal.connect(
	    [](inode_t inode, std::span<const uint8_t> name, std::span<const uint8_t> value) {
		    if (gForklessBackend != nullptr) {
			    gForklessBackend->onXAttrChanged(inode, name, value);
		    }
	    });

	gXAttrRemovedSignal.connect([](inode_t inode, std::span<const uint8_t> name) {
		if (gForklessBackend != nullptr) { gForklessBackend->onXAttrRemoved(inode, name); }
	});

	initializeNewMetadataHeaderSignal.connect([]() {
		if (gForklessBackend != nullptr) { gForklessBackend->initializeNewMetadataHeader(); }
	});
}
