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
#include "master/matomlserv.h"
#include "master/metadata_backend_common.h"
#include "master/metadata_backend_interface.h"
#include "master/metadata_dumper_file.h"
#include "master/metadata_section_bootstrap_fdb.h"
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
		return SAUNAFS_ERROR_IO;
	}

	// Save metadata keys required for restore (checkpoint list, next chunk id, etc.)
	if (saveMetadataKeys() != kOpSuccess) {
		safs::log_err("Failed to save metadata keys required for restore");
		return SAUNAFS_ERROR_IO;
	}

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

int8_t MetadataBackendForkless::saveMetadataKeys() {
	auto transaction = kvConnector_->getKVEngine()->createReadWriteTransaction();

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

	uint32_t hashRootIndex = NODEHASHPOS(gMetadata->root->id);
	gMetadata->nodeHash[hashRootIndex].push_back(gMetadata->root);
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

	// Load metadata global properties and check signature

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
	/*metadataSections_.emplace_back("NODE 1.0", kNodeKeyPrefix,
	                               [this](bool flag) { return loadNodes(flag); });
	metadataSections_.emplace_back("EDGE 1.0", kEdgeKeyPrefix,
	                               [this](bool flag) { return loadEdges(flag); });
	metadataSections_.emplace_back("FREE 1.0", kFreeKeyPrefix,
	                               [this](bool flag) { return loadFree(flag); });
	metadataSections_.emplace_back("XATR 1.0", "XATR_", loadXAttr);
	metadataSections_.emplace_back("ACLS 1.2", "ACLS_", loadACLs);
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

	// gMetadata->nodeChangedSignal.connect(kvConnector_.get(), &IKVConnector::onNodeChanged);

	// gMetadata->edgeChangedSignal.connect(kvConnector_.get(), &IKVConnector::onEdgeChanged);

	// gMetadata->edgeRemovedSignal.connect(kvConnector_.get(), &IKVConnector::onEdgeRemoved);

	initializeNewMetadataHeaderSignal.connect([this]() { initializeNewMetadataHeader(); });
}
