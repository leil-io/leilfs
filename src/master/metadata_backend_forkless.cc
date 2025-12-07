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
#include "master/filesystem_operations_interface.h"
#include "master/kv_common_keys.h"
#include "master/kv_connector_fdb.h"
#include "master/kv_connector_interface.h"
#include "master/matoclserv.h"
#include "master/matomlserv.h"
#include "master/metadata_backend_common.h"
#include "master/metadata_backend_interface.h"
#include "master/metadata_dumper_file.h"
#include "slogger/slogger.h"

namespace {
MetadataBackendForkless *gForklessBackend = nullptr;
}

// Add a static callback function
static void flushMetadataCallback() {
	if (gForklessBackend != nullptr) { gForklessBackend->flushPendingUpdates(); }
}

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
	safs::log_err("MetadataBackendForkless::fs_storeall");

	if (gMetadata == nullptr) {
		// Periodic dump in shadow master or a request from saunafs-admin
		safs::log_info("Can't save metadata because no metadata is loaded");
		return SAUNAFS_ERROR_NOTPOSSIBLE;
	}

	// FDB backend does not need to dump anything, as all updates are performed in real-time.
	// However, to honor the interface, we simulate a successful dump here
	flushPendingUpdates();

	return SAUNAFS_STATUS_OK;
}

#endif  // #if !defined(METARESTORE) && !defined(METALOGGER)

#ifndef METALOGGER

int8_t MetadataBackendForkless::loadChunks(bool ignoreFlag) {
	(void)ignoreFlag;  // Unused parameter

	Timer timer;
	safs::log_info("Loading chunks from FoundationDB");

	auto transaction = kvConnector_->getKVEngine()->createReadWriteTransaction();
	std::string endKey = std::string(kChunkKeyPrefix) + "\\xff";
	kv::KeySelector startSelector(kv::toBytes(kChunkKeyPrefix), true, 0);
	kv::KeySelector endSelector(kv::toBytes(endKey), true, 0);

	kv::Key lastKey;
	static constexpr size_t kChunkPageSize = 1000;  // Number of entries to fetch per page

	uint64_t chunkCount = 0;

	while (true) {
		auto pageResult = transaction->getRange(startSelector, endSelector, kChunkPageSize);

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

	// Connect the signal handlers after initial loading

	gChunkChangedSignal.connect(
	    [this](uint64_t chunkId, uint32_t version, uint32_t lockedTo, uint32_t lockId) {
		    if (metadataWriter_) {
			    metadataWriter_->enqueue(
			        std::make_unique<ChunkUpdateEvent>(chunkId, version, lockedTo, lockId));
		    }
	    });

	return kOpSuccess;
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

void MetadataBackendForkless::loadall(int ignoreflag) {
	safs::log_info("MetadataBackendForkless::loadall: ignoreflag: {}", ignoreflag);

	// Load metadata global properties and check signature

	// TODO(Guillex): implement signature check

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

void MetadataBackendForkless::flushPendingUpdates() {
	if (metadataWriter_) { metadataWriter_->flush(); }
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

	// Register periodic flush (every 1s) to ensure timely persistence
	eventloop_timeregister_ms(1000, flushMetadataCallback);

	uint64_t version = getVersion("");

	if (version == 0 && gMetadata != nullptr) {  // Version does not exist, the metadata is new
		safs::log_warn("Initializing new metadata");

		auto transaction = kvConnector_->getKVEngine()->createReadWriteTransaction();

		version = gMetadata->metadataVersion;
		kv::Value metadataVersionValue;
		serialize(metadataVersionValue, version);

		constexpr uint32_t initialValue32Bits = 1U;
		kv::Value initialValue32BitsValue;
		serialize(initialValue32BitsValue, initialValue32Bits);

		transaction->set(kv::toBytes(kMetaFormatKey), kv::toBytes("1.0"));
		transaction->set(kv::toBytes(kMetaVersionKey), metadataVersionValue);
		transaction->set(kv::toBytes(kMetaNextSessionKey), initialValue32BitsValue);

		if (!transaction->commit()) {
			const auto *message = "Failed to initialize new metadata";
			safs::log_err(message);
			throw MetadataConsistencyException(message);
		}
	}

	gMetadata = new FilesystemMetadata;
	createConnections();

	safs::log_info("Metadata version: {}", version);
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

void MetadataBackendForkless::createConnections() {
	// gMetadata->nextSessionId().connect(kvConnector_.get(), &IKVConnector::onNextSessionIdChanged);

	// getChangelogSignal().connect(kvConnector_.get(), &IKVConnector::onChangelogEvent);

	// gMetadata->nodeChangedSignal.connect(kvConnector_.get(), &IKVConnector::onNodeChanged);

	// gMetadata->edgeChangedSignal.connect(kvConnector_.get(), &IKVConnector::onEdgeChanged);

	// gMetadata->edgeRemovedSignal.connect(kvConnector_.get(), &IKVConnector::onEdgeRemoved);
}
