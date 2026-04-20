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
#include <string>
#include <vector>

#include "common/datapack.h"
#include "common/event_loop.h"
#include "common/scoped_timer.h"
#include "common/serialization.h"
#include "common/time_utils.h"
#include "kv/itransaction.h"
#include "kv/kv_utils.h"
#include "master/changelog.h"
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
#include "protocol/SFSCommunication.h"
#include "slogger/slogger.h"

namespace {
MetadataBackendForkless *gForklessBackend = nullptr;
}

// Add a static callback function
static void flushMetadataCallback() {
	if (gForklessBackend != nullptr) { (void)gForklessBackend->flushPendingUpdates(false); }
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
	// Static callbacks (periodic flush timer, promotion handler) dereference
	// gForklessBackend. Clear it on destruction so they never touch a deleted
	// instance. Guard on identity so destroying an old backend cannot clobber a
	// newer one that already claimed the pointer in its constructor.
	if (gForklessBackend == this) { gForklessBackend = nullptr; }
}

#if !defined(METARESTORE) && !defined(METALOGGER)

bool MetadataBackendForkless::commit_metadata_dump() {
	safs::log_err("MetadataBackendForkless::commit_metadata_dump");

	return true;
}

int MetadataBackendForkless::emergency_saves() {
	safs::log_err("MetadataBackendForkless::emergency_saves");

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

	// Flush ALL pending batched updates to FDB before saving metadata keys,
	// so restore-relevant keys reflect the fully persisted state.
	if (!flushPendingUpdates(true)) {
		safs::log_err("Failed to fully flush pending updates before saving metadata keys");
		broadcast_metadata_saved(SAUNAFS_ERROR_IO);
		return SAUNAFS_ERROR_IO;
	}

	// Save metadata keys required for restore (checkpoint list, next chunk id, etc.)
	if (saveMetadataKeys() != kOpSuccess) {
		safs::log_err("Failed to save metadata keys required for restore");
		broadcast_metadata_saved(SAUNAFS_ERROR_IO);
		return SAUNAFS_ERROR_IO;
	}

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

	uint64_t nextChunkId = getNextChunkId();
	auto status = chunk_set_next_chunkid(nextChunkId);

	if (status != SAUNAFS_STATUS_OK) {
		safs::log_err("{}: failed to set next chunk id to {}", __func__, nextChunkId);
		return kOpFailure;
	}

	auto transaction = kvConnector_->getKVEngine()->createReadWriteTransaction();
	kv::Key startKey = kv::toBytes(kChunkKeyPrefix);
	kv::Key endKey = kv::prefixEnd(startKey);
	kv::KeySelector startSelector(startKey, true, 0);
	kv::KeySelector endSelector(endKey, true, 0);

	kv::Key lastKey;
	uint64_t chunkCount = 0;

	while (true) {
		auto pageResult =
		    transaction->getRange(startSelector, endSelector, kv::kDefaultGetRangeLimit);

		uint64_t chunkId{};
		uint32_t chunkVersion{};
		uint32_t lockedTo{};
		uint32_t lockId{};

		for (const auto &pair : pageResult.getPairs()) {
			const uint8_t *source = pair.key.data();
			source += kChunkKeyPrefix.size();  // Skip "CHNK_"
			chunkId = get64bit(&source);
			get32bit(&source, chunkVersion);

			source = pair.value.data();
			get32bit(&source, lockedTo);
			get32bit(&source, lockId);

			if (chunkId > 0) {
				chunk_add_from_initial_metadata_load(chunkId, chunkVersion, lockedTo, lockId);
				chunkCount++;
			}
		}

		if (!pageResult.hasMore() || pageResult.getPairs().empty()) { break; }

		lastKey = pageResult.getPairs().back().key;
		startSelector = kv::KeySelector(lastKey, false, 0);
	}

	safs::log_info("Loaded {} chunks", chunkCount);
	safs::log_info("Section loaded successfully (CHNK 1.0): {}s", timer.elapsed_s());

	return kOpSuccess;
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

void MetadataBackendForkless::onXAttrChanged(inode_t inode, const std::vector<uint8_t> &name,
                                             const std::vector<uint8_t> &value) {
	if (metadataWriter_) {
		metadataWriter_->enqueue(std::make_unique<XAttrUpdateEvent>(inode, name, value));
	}
}

void MetadataBackendForkless::onXAttrRemoved(inode_t inode, const std::vector<uint8_t> &name) {
	if (metadataWriter_) {
		metadataWriter_->enqueue(std::make_unique<XAttrRemoveEvent>(inode, name));
	}
}

int8_t MetadataBackendForkless::saveNextChunkId(kv::IReadWriteTransaction *transaction) {
	// META_NEXT_CHUNK_ID: <NextChunkId> e.g. META_NEXT_CHUNK_ID: 4
	uint64_t nextChunkId = chunk_get_next_id();
	kv::Value nextChunkIdValue;
	serialize(nextChunkIdValue, nextChunkId);
	auto key = kv::toBytes(kMetaNextChunkIdKey);
	transaction->set(key, nextChunkIdValue);
	safs::log_info("{}: Saving META_NEXT_CHUNK_ID with value: {}", __func__, nextChunkId);

	return kOpSuccess;
}

int8_t MetadataBackendForkless::saveMetadataKeys(kv::IReadWriteTransaction *transaction) {
	// MaxInodeId is stored in META_MAX_INODE_ID: <maxInodeId> e.g. META_MAX_INODE_ID: 42
	kv::Key maxInodeIdKey{kv::toBytes(gMetadata->maxInodeId().getName())};
	kv::Value maxInodeIdValue;
	serialize(maxInodeIdValue, gMetadata->maxInodeId().getValue());
	transaction->set(maxInodeIdKey, maxInodeIdValue);

	// Metadata version is stored in META_VERSION: <version> e.g. META_VERSION: 3
	kv::Key versionKey{kv::toBytes(kMetaVersionKey)};
	kv::Value serializedVersion;
	serialize(serializedVersion, gMetadata->metadataVersion);
	transaction->set(versionKey, serializedVersion);

	// Next session ID is stored in META_NEXT_SESSION: <nextSessionId> e.g. META_NEXT_SESSION: 5
	kv::Key sessionKey{kv::toBytes(kMetaNextSessionKey)};
	kv::Value sessionValue;
	serialize(sessionValue, gMetadata->nextSessionId().getValue());
	transaction->set(sessionKey, sessionValue);

	return kOpSuccess;
}

int8_t MetadataBackendForkless::loadMetadataKeys() {
	// Load metadata global properties from FDB (equivalent to the file header that
	// MetadataBackendFile reads from metadata.sfs: maxInodeId, metadata version and nextSessionId).

	// MaxInodeId is stored in META_MAX_INODE_ID: <maxInodeId> e.g. META_MAX_INODE_ID: 42
	auto transaction = kvConnector_->getKVEngine()->createReadOnlyTransaction();

	inode_t maxInodeId{SPECIAL_INODE_ROOT};
	const auto maxInodeIdKey = gMetadata->maxInodeId().getName();
	auto value = transaction->get(kv::toBytes(maxInodeIdKey));
	if (value.has_value()) {
		const uint8_t *data = value.value().data();
		getINode(&data, maxInodeId);
	}

	gMetadata->maxInodeId().setValue(maxInodeId);

	// Metadata version is stored in META_VERSION: <version> e.g. META_VERSION: 3
	gMetadata->metadataVersion = kvConnector_->get64bitBE(kv::toBytes(kMetaVersionKey), 1);

	// Next session ID is stored in META_NEXT_SESSION: <nextSessionId> e.g. META_NEXT_SESSION: 5
	auto nextSessionIdValue = kvConnector_->get32bitBE(kv::toBytes(kMetaNextSessionKey), 1);
	gMetadata->nextSessionId().setValue(nextSessionIdValue);

	return kOpSuccess;
}

int8_t MetadataBackendForkless::saveMetadataKeys() {
	auto transaction = kvConnector_->getKVEngine()->createReadWriteTransaction();

	saveMetadataKeys(transaction.get());
	saveNextChunkId(transaction.get());

	if (!transaction->commit()) {
		safs::log_err("Failed to save metadata keys required for restore");
		return kOpFailure;
	}

	safs::log_info("Metadata keys saved successfully for restore");
	return kOpSuccess;
}

uint64_t MetadataBackendForkless::getNextChunkId() {
	auto transaction = kvConnector_->getKVEngine()->createReadOnlyTransaction();
	uint64_t nextChunkId = 0;

	// META_NEXT_CHUNK_ID: <NextChunkId>. e.g.: META_NEXT_CHUNK_ID: 4
	auto key = kv::toBytes(kMetaNextChunkIdKey);
	auto result = transaction->get(key);
	if (result != std::nullopt) {
		const uint8_t *data = result.value().data();
		nextChunkId = get64bit(&data);
		safs::log_info("Loaded {} as nextChunkId from FDB: {}", kMetaNextChunkIdKey,
		               nextChunkId);
	}

	return nextChunkId;
}

int MetadataBackendForkless::fsLoad(bool ignoreFlag) {
	for (const auto &section : metadataSections_) {
		auto result = section.loadFunction(ignoreFlag);

		if (result != kOpSuccess) {
			safs::log_err("Failed to load section: {}", section.name);
			return result;
		}
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

	chunk_newfs();

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
	auto transaction = kvConnector_->getKVEngine()->createReadWriteTransaction();

	kv::Key startKey = kv::encodeKeyBE(kNodeKeyPrefix, SPECIAL_INODE_ROOT);
	kv::Key endKey = kv::prefixEnd(kv::toBytes(kNodeKeyPrefix));
	kv::KeySelector startSelector(startKey, true, 0);
	kv::KeySelector endSelector(endKey, true, 0);

	uint64_t nodeCount = 0;

	while (true) {
		auto pageResult =
		    transaction->getRange(startSelector, endSelector, kv::kDefaultGetRangeLimit);

		for (const auto &pair : pageResult.getPairs()) {
			const uint8_t *source = pair.value.data();
			auto type = static_cast<FSNodeType>(source[0]);
			FSNode *node = FSNode::create(type);
			node->deserialize(&source);

			int8_t status = loadNode(fsOpContext, node);

			if (status < 0) {
				safs::log_err("Error loading node: {}", node->id);
				return kOpFailure;
			}
			nodeCount++;
		}

		if (!pageResult.hasMore() || pageResult.getPairs().empty()) { break; }

		kv::Key lastKey = pageResult.getPairs().back().key;
		startSelector = kv::KeySelector(lastKey, false, 0);
	}

	safs::log_info("Loaded {} nodes", nodeCount);
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

	auto transaction = kvConnector_->getKVEngine()->createReadWriteTransaction();

	kv::Key startKey = kv::toBytes(kFreeKeyPrefix);
	kv::Key endKey = kv::prefixEnd(startKey);
	kv::KeySelector startSelector(startKey, true, 0);
	kv::KeySelector endSelector(endKey, true, 0);

	while (true) {
		auto pageResult =
		    transaction->getRange(startSelector, endSelector, kv::kDefaultGetRangeLimit);

		inode_t inode{};
		uint32_t timeStamp{};

		for (const auto &pair : pageResult.getPairs()) {
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

	auto transaction = kvConnector_->getKVEngine()->createReadOnlyTransaction();

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

	auto transaction = kvConnector_->getKVEngine()->createReadWriteTransaction();

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
	static inode_t currentParentId;

	if (init) {
		currentParentId = 0;
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

		if (currentParentId != parentId) {
			if (parent->entries.size() > 0) {
				safs::log_err("loading edge: {}, {}->{} error: parent node sequence error",
				              parentId, gFSOperations->nodeOperations()->escapeName(name), childId);
				return kOpFailure;
			}

			currentParentId = parentId;
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
			initializeNewMetadataHeaderSignal.emit();
			gMetadataBackend->fs_storeall(DumpType::kForegroundDump);
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

	// Check metadata signature

	bool isSignatureValid = checkMetadataSignature();
	bool bootstrapped = false;

#ifndef METARESTORE
	if (metadataserver::isMaster() && sectionBootstrapper_ != nullptr) {
		bootstrapped = sectionBootstrapper_->bootstrapSections();
		if (bootstrapped) {
			safs::log_info("Metadata sections bootstrapped successfully");
		}
	}
	sectionBootstrapper_.reset();
	sectionBootstrapper_ = nullptr;
	safs::log_info("Metadata bootstrapping stage finished");
#endif

	if (!isSignatureValid && !bootstrapped) { return; }

	// Load metadata global properties from FDB

	loadMetadataKeys();

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
	    chunk_count());
#endif
}

void MetadataBackendForkless::store_fd(FILE *fd) {
	safs::log_info("MetadataBackendForkless::store_fd: fd: {}", fd->_fileno);
}

#endif  // #ifndef METALOGGER

bool MetadataBackendForkless::flushPendingUpdates(bool flushAll) {
	if (!metadataWriter_) { return false; }
	if (flushAll) {
		return metadataWriter_->flushAll();
	}

	// Periodic mode: best-effort single-batch flush.
	metadataWriter_->flush();
	return true;
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
	metadataSections_.emplace_back("CHNK 1.0", kChunkKeyPrefix,
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

void MetadataBackendForkless::init() {
	kvConnector_ = std::make_shared<KVConnectorFDB>();

	if (kvConnector_->init()) {
		safs::log_info("KV store initialized successfully");
	} else {
		safs::log_err("Failed to initialize KV store");
		throw std::runtime_error("Failed to initialize KV store");
	}

	// Initialize the metadata writer to handle metadata updates
	metadataWriter_ = std::make_unique<MetadataWriterFDB>(kvConnector_->getKVEngine());

#ifndef METARESTORE
	sectionBootstrapper_ =
	    std::make_unique<MetadataSectionBootstrapFDB>(kvConnector_->getKVEngine());
#endif  // #ifndef METARESTORE

	// Connect the signal handler for chunk changes
	// This must be done in init() rather than loadChunks() to ensure that chunks created
	// during changelog replay on shadow masters are also written to FDB
	gChunkChangedSignal.connect(
	    [this](uint64_t chunkId, uint32_t version, uint32_t lockedTo, uint32_t lockId) {
		    if (metadataWriter_) {
			    metadataWriter_->enqueue(std::make_unique<ChunkUpdateEvent>(
			        chunkId, version, lockedTo, lockId));
		    }
	    });

	// Register periodic flush (every 1s) to ensure timely persistence
	eventloop_timeregister_ms(1000, flushMetadataCallback);

	uint64_t version = getVersion("");

	if (version == 0) {  // Version does not exist, the metadata is new
		safs::log_warn("Initializing new metadata");

		auto transaction = kvConnector_->getKVEngine()->createReadWriteTransaction();

		version = 1ULL;
		kv::Value metadataVersionValue;
		serialize(metadataVersionValue, version);

		constexpr uint32_t initialValue32Bits = 1U;
		kv::Value initialValue32BitsValue;
		serialize(initialValue32BitsValue, initialValue32Bits);

		transaction->set(kv::toBytes(kMetaFormatKey), kv::toBytes("1.0"));
		transaction->set(kv::toBytes(kMetaVersionKey), metadataVersionValue);
		transaction->set(kv::toBytes(kMetaNextSessionKey), initialValue32BitsValue);

		constexpr uint64_t initialChunkId = 1ULL;
		kv::Value initialChunkIdValue;
		serialize(initialChunkIdValue, initialChunkId);
		transaction->set(kv::toBytes(kMetaNextChunkIdKey), initialChunkIdValue);

		// Initialize metadata signature to "SFSSIGNATURE M NEW"
		transaction->set(kv::toBytes(kMetaHeaderKey), kv::toBytes(SFSSIGNATURE "M NEW"));

		if (!transaction->commit()) {
			const auto *message = "Failed to initialize new metadata";
			safs::log_err(message);
			throw MetadataConsistencyException(message);
		}
	}

	gMetadata = new FilesystemMetadata;
	createConnections();

	safs::log_info("MetadataBackendForkless version: {}", version);
}

uint64_t MetadataBackendForkless::getVersion(const std::string & /*file*/) {
	auto transaction = kvConnector_->getKVEngine()->createReadWriteTransaction();
	kv::Key versionKey{kv::toBytes(kMetaVersionKey)};

	auto result = transaction->get(versionKey);

	if (result != std::nullopt) {
		const uint8_t *data = result.value().data();
		uint64_t version = get64bit(&data);
		return version;
	}

	return 0;
}

std::string MetadataBackendForkless::getHeaderSignature() {
	auto transaction = kvConnector_->getKVEngine()->createReadWriteTransaction();
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
	// gMetadata->nextSessionId().connect(kvConnector_.get(), &IKVConnector::onNextSessionIdChanged);

	// getChangelogSignal().connect(kvConnector_.get(), &IKVConnector::onChangelogEvent);

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

	gXAttrInodeRemovedSignal.connect([this](inode_t inode) { onXAttrInodeRemoved(inode); });

	gXAttrChangedSignal.connect(
	    [this](inode_t inode, const std::vector<uint8_t> &name, const std::vector<uint8_t> &value) {
		    onXAttrChanged(inode, name, value);
	    });

	gXAttrRemovedSignal.connect(
	    [this](inode_t inode, const std::vector<uint8_t> &name) { onXAttrRemoved(inode, name); });

	initializeNewMetadataHeaderSignal.connect([this]() { initializeNewMetadataHeader(); });
}
