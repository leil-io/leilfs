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

#include "master/metadata_section_bootstrap_fdb.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

#include "common/datapack.h"
#include "common/memory_mapped_file.h"
#include "common/type_defs.h"
#include "kv/itransaction.h"
#include "kv/kv_utils.h"
#include "master/hstring.h"
#include "master/kv_common_keys.h"
#include "master/metadata_backend_common.h"
#include "master/metadata_backend_interface.h"
#include "master/metadata_writer_fdb.h"
#include "slogger/slogger.h"

namespace {
constexpr uint8_t kMetadataSectionHeaderSize = 16;
constexpr uint8_t kMetadataSectionNameSize = 8;
constexpr size_t kFlushThreshold = 1000;

bool isEndOfMetadata(const uint8_t *sectionPtr) {
	static constexpr std::string_view kMetadataTrailer("[" SFSSIGNATURE " EOF MARKER]");
	static constexpr std::string_view kMetadataLegacyTrailer("[MFS EOF MARKER]");
	return ((memcmp(sectionPtr, kMetadataTrailer.data(),
	              ::kMetadataSectionHeaderSize) == kOpSuccess) ||
	        (memcmp(sectionPtr, kMetadataLegacyTrailer.data(),
	              ::kMetadataSectionHeaderSize) == kOpSuccess));
}
}  // namespace

MetadataSectionBootstrapFDB::MetadataSectionBootstrapFDB(kv::IKVEngine *kvEngine)
    : kvEngine_(kvEngine) {
	initMetadataFileSections();
}

bool MetadataSectionBootstrapFDB::prepare(const std::string &metadataFilePath) {
	static constexpr uint8_t kMetadataHeaderOffset = 8;
	maxInodeId_ = 0;
	metadataVersion_ = 0;
	nextSessionId_ = 0;

	sectionMarkers_.clear();

	try {
		metadataFile_ = std::make_shared<MemoryMappedFile>(metadataFilePath);
	} catch (const std::exception &ex) {
		safs::log_warn("Failed to open {} for bootstrap: {}", metadataFilePath, ex.what());
		return false;
	}

	// Skip file signature
	const uint8_t *metadataHeaderPtr = metadataFile_->seek(kMetadataHeaderOffset);
	if (metadataHeaderPtr == nullptr) {
		safs::log_err("Failed to seek to metadata header offset; file too small or corrupted");
		return false;
	}

	getINode(&metadataHeaderPtr, maxInodeId_);

	metadataVersion_ = get64bit(&metadataHeaderPtr);

	get32bit(&metadataHeaderPtr, nextSessionId_);

	size_t offsetBegin = metadataFile_->offset(metadataHeaderPtr);
	const uint8_t *sectionPtr = metadataFile_->seek(offsetBegin);

	while (sectionPtr != nullptr && !isEndOfMetadata(sectionPtr)) {
		const uint8_t *sectionLengthPtr = sectionPtr + kMetadataSectionNameSize;
		const uint64_t sectionLength = get64bit(&sectionLengthPtr);
		const size_t sectionOffset = metadataFile_->offset(sectionLengthPtr);

		for (const auto &section : metadataFileSections_) {
			if (section.matchesSectionTypeOf(sectionPtr)) {
				sectionMarkers_[section.name] = {.offset = sectionOffset, .length = sectionLength};
				break;
			}
		}

		offsetBegin += static_cast<size_t>(sectionLength) + kMetadataSectionHeaderSize;
		sectionPtr = metadataFile_->seek(offsetBegin);
	}

	return true;
}

bool MetadataSectionBootstrapFDB::bootstrapSections() {
	safs::log_info("{}: Bootstrapping metadata sections from file into FDB backend", __func__);
	if (!prepare(kMetadataFilename)) { return false; }

	bool bootstrapped = false;
	for (const auto &section : metadataFileSections_) {
		if (!section.isBootstrapNeeded || !section.loadFunction) { continue; }
		int8_t needed = section.isBootstrapNeeded(false);
		if (needed < 0) { return false; }
		if (needed > 0) {
			if (section.loadFunction(false) != kOpSuccess) { return false; }
			bootstrapped = true;
		}
	}

	if (bootstrapped && saveMetadataHeader() != kOpSuccess) { return false; }

	return bootstrapped;
}

int8_t MetadataSectionBootstrapFDB::saveMetadataHeader() {
	auto transaction = kvEngine_->createReadWriteTransaction();

	transaction->set(kv::toBytes(kMetaHeaderKey), kv::toBytes(SFSSIGNATURE "M 2.9"));
	transaction->set(kv::toBytes(kMetaFormatKey), kv::toBytes("1.0"));

	kv::Value maxInodeIdValue;
	serialize(maxInodeIdValue, maxInodeId_);
	transaction->set(kv::toBytes(kMetaMaxInodeIdKey), maxInodeIdValue);

	kv::Value metadataVersionValue;
	serialize(metadataVersionValue, metadataVersion_);
	transaction->set(kv::toBytes(kMetaVersionKey), metadataVersionValue);

	kv::Value nextSessionIdValue;
	serialize(nextSessionIdValue, nextSessionId_);
	transaction->set(kv::toBytes(kMetaNextSessionKey), nextSessionIdValue);

	if (!transaction->commit()) {
		safs::log_err("Failed to commit bootstrapped metadata header to FDB");
		return kOpFailure;
	}

	return kOpSuccess;
}

int8_t MetadataSectionBootstrapFDB::isSectionBootstrapNeeded(std::string_view prefix) {
	if (kvEngine_ == nullptr) { return -1; }
	auto transaction = kvEngine_->createReadOnlyTransaction();
	kv::Key startKey = kv::toBytes(prefix);
	kv::Key endKey = kv::prefixEnd(startKey);

	auto page = transaction->getRange(kv::KeySelector(startKey, true, 0),
	                                  kv::KeySelector(endKey, true, 0), 1);
	return page.getPairs().empty() ? 1 : 0;
}

int8_t MetadataSectionBootstrapFDB::loadNodesSection() {
	if (kvEngine_ == nullptr || metadataFile_ == nullptr) { return kOpFailure; }

	auto marker = findSection("NODE 1.0");
	if (!marker.has_value()) {
		safs::log_warn("No metadata section marker found for NODE 1.0");
		return kOpFailure;
	}

	if (marker->length > std::numeric_limits<size_t>::max() - marker->offset) {
		safs::log_err("Bootstrapping nodes: section bounds overflow");
		return kOpFailure;
	}
	const size_t sectionEnd = marker->offset + static_cast<size_t>(marker->length);
	const uint8_t *ptr = metadataFile_->seek(marker->offset);
	uint64_t nodeCount = 0;

	MetadataWriterFDB writer(kvEngine_);
	size_t pending = 0;

	while (metadataFile_->offset(ptr) < sectionEnd) {
		const uint8_t *nodeBegin = ptr;
		uint8_t typeU8 = get8bit(&ptr);

		// End marker for NODE section in metadata.sfs
		if (typeU8 == 0) { break; }

		auto type = static_cast<FSNodeType>(typeU8);
		FSNode *node = FSNode::create(type);
		if (node == nullptr) {
			safs::log_err("Failed to create FSNode for type {}", typeU8);
			return kOpFailure;
		}

		// A freshly created node reports its type-specific minimum serialized size
		// (fixed header + per-type fixed fields, e.g. kFileHeaderSize for files). Reject if the
		// section cannot even hold that, so deserialize() does not read past the mapped region.
		if (sectionEnd - metadataFile_->offset(nodeBegin) < node->serializedSize()) {
			safs::log_err("{}: truncated node entry", __func__);
			FSNode::destroy(node);
			return kOpFailure;
		}

		// deserialize expects the type byte to be present in the stream
		ptr = nodeBegin;
		node->deserialize(&ptr);

		// Guard against a truncated/corrupt section advancing the pointer past the section end.
		if (metadataFile_->offset(ptr) > sectionEnd) {
			safs::log_err("{}: node read past section end", __func__);
			FSNode::destroy(node);
			return kOpFailure;
		}

		// Enqueue node update with checkpointVersion=0 to avoid NODEU_ entries.
		writer.enqueue(std::make_unique<NodeUpdateEvent>(node));

		FSNode::destroy(node);
		nodeCount++;

		if (++pending >= kFlushThreshold) {
			if (!writer.flush()) { return kOpFailure; }
			pending = 0;
		}
	}

	if (!writer.flush(MetadataWriterFDB::FlushMode::kDrainUntilEmpty)) {
		safs::log_err("Failed to flush bootstrapped nodes to FDB");
		return kOpFailure;
	}

	safs::log_info("Bootstrapped {} nodes from metadata file into FDB", nodeCount);
	return kOpSuccess;
}

int8_t MetadataSectionBootstrapFDB::loadChunkSection() {
	if (kvEngine_ == nullptr || metadataFile_ == nullptr) { return kOpFailure; }

	auto marker = findSection("CHNK 1.0");
	if (!marker.has_value()) {
		safs::log_warn("No metadata section marker found for CHNK 1.0");
		return kOpFailure;
	}

	if (marker->length > std::numeric_limits<size_t>::max() - marker->offset) {
		safs::log_err("Bootstrapping chunks: section bounds overflow");
		return kOpFailure;
	}
	const size_t sectionEnd = marker->offset + static_cast<size_t>(marker->length);

	// The section starts with the next-chunk-id (uint64_t); reject a section too small to
	// hold it so the read below and the loop bound below cannot underflow.
	if (marker->length < sizeof(uint64_t)) {
		safs::log_err("{}: section length {} is too small", __func__, marker->length);
		return kOpFailure;
	}

	const uint8_t *ptr = metadataFile_->seek(marker->offset);

	uint64_t nextChunkId = get64bit(&ptr);
	uint64_t chunkCount = 0;

	MetadataWriterFDB writer(kvEngine_);
	size_t pending = 0;

	constexpr size_t kEntrySize =
	    sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);

	// Subtraction (rather than offset + kEntrySize <= sectionEnd) keeps the bound check free
	// of integer overflow. offset(ptr) <= sectionEnd holds on entry (guard above) and after
	// each iteration advances by exactly kEntrySize.
	while (sectionEnd - metadataFile_->offset(ptr) >= kEntrySize) {
		uint64_t chunkId = get64bit(&ptr);
		uint32_t chunkVersion{};
		uint32_t lockedTo{};
		uint32_t lockId{};

		get32bit(&ptr, chunkVersion);
		get32bit(&ptr, lockedTo);
		get32bit(&ptr, lockId);

		if (chunkId == 0) { break; }

		// Enqueue chunk update event for FDB with no checkpoint version to avoid undo logging
		writer.enqueue(std::make_unique<ChunkUpdateEvent>(chunkId, chunkVersion, lockedTo, lockId));

		chunkCount++;
		if (++pending >= kFlushThreshold) {
			if (!writer.flush()) { return kOpFailure; }
			pending = 0;
		}
	}

	if (!writer.flush(MetadataWriterFDB::FlushMode::kDrainUntilEmpty)) {
		safs::log_err("Failed to flush bootstrapped chunks to FDB");
		return kOpFailure;
	}

	auto transaction = kvEngine_->createReadWriteTransaction();
	kv::Value nextChunkIdValue(sizeof(uint64_t));
	uint8_t *nextChunkPtr = nextChunkIdValue.data();
	put64bit(&nextChunkPtr, nextChunkId);
	transaction->set(kv::toBytes(kMetaNextChunkIdKey), nextChunkIdValue);

	if (metadataVersion_ > 0) {
		kv::Value metadataVersionValue;
		serialize(metadataVersionValue, metadataVersion_);
		transaction->set(kv::toBytes(kMetaVersionKey), metadataVersionValue);
	}

	if (!transaction->commit()) {
		safs::log_err("Failed to commit bootstrapped metadata keys to FDB");
		return kOpFailure;
	}

	safs::log_info("Bootstrapped {} chunks from metadata file into FDB", chunkCount);
	return kOpSuccess;
}

int8_t MetadataSectionBootstrapFDB::loadEdgesSection() {
	if (kvEngine_ == nullptr || metadataFile_ == nullptr) { return kOpFailure; }

	auto marker = findSection("EDGE 1.0");
	if (!marker.has_value()) {
		safs::log_warn("No metadata section marker found for EDGE 1.0");
		return kOpFailure;
	}

	if (marker->length > std::numeric_limits<size_t>::max() - marker->offset) {
		safs::log_err("Bootstrapping edges: section bounds overflow");
		return kOpFailure;
	}
	const size_t sectionEnd = marker->offset + static_cast<size_t>(marker->length);
	const uint8_t *ptr = metadataFile_->seek(marker->offset);
	uint64_t edgeCount = 0;

	MetadataWriterFDB writer(kvEngine_);
	size_t pending = 0;

	while (metadataFile_->offset(ptr) < sectionEnd) {
		// Ensure the fixed-size edge header fits before reading it (subtraction avoids overflow;
		// offset(ptr) < sectionEnd is guaranteed by the loop condition).
		if (sectionEnd - metadataFile_->offset(ptr) < (sizeof(inode_t) * 2) + sizeof(uint16_t)) {
			safs::log_err("{}: truncated edge entry header", __func__);
			return kOpFailure;
		}

		inode_t parentId{};
		getINode(&ptr, parentId);
		inode_t childId{};
		getINode(&ptr, childId);
		auto edgeNameSize = get16bit(&ptr);

		// End-of-edges marker: parentId == 0 && childId == 0
		if (parentId == 0 && childId == 0) { break; }

		if (edgeNameSize == 0) {
			safs::log_err("Bootstrapping edge: empty name for edge {}->{}", parentId, childId);
			return kOpFailure;
		}

		// Ensure the edge name bytes are within the section before reading them (subtraction
		// avoids overflow; offset(ptr) <= sectionEnd holds after the header read above).
		if (edgeNameSize > sectionEnd - metadataFile_->offset(ptr)) {
			safs::log_err("{}: truncated edge name for edge {}->{}", __func__, parentId, childId);
			return kOpFailure;
		}

		std::string name(reinterpret_cast<const char *>(ptr), edgeNameSize);
		ptr += edgeNameSize;

		writer.enqueue(std::make_unique<EdgeUpdateEvent>(parentId, HString(name), childId));
		edgeCount++;

		if (++pending >= kFlushThreshold) {
			if (!writer.flush()) { return kOpFailure; }
			pending = 0;
		}
	}

	if (!writer.flush(MetadataWriterFDB::FlushMode::kDrainUntilEmpty)) {
		safs::log_err("Failed to flush bootstrapped edges to FDB");
		return kOpFailure;
	}

	safs::log_info("Bootstrapped {} edges from metadata file into FDB", edgeCount);
	return kOpSuccess;
}

int8_t MetadataSectionBootstrapFDB::loadFreeSection() {
	if (kvEngine_ == nullptr || metadataFile_ == nullptr) { return kOpFailure; }

	auto marker = findSection("FREE 1.0");
	if (!marker.has_value()) {
		safs::log_warn("No metadata section marker found for FREE 1.0");
		return kOpFailure;
	}

	if (marker->length > std::numeric_limits<size_t>::max() - marker->offset) {
		safs::log_err("Bootstrapping free: section bounds overflow");
		return kOpFailure;
	}
	const size_t sectionEnd = marker->offset + static_cast<size_t>(marker->length);
	const uint8_t *ptr = metadataFile_->seek(marker->offset);

	// The section starts with the free-node count (inode_t); reject a section too small to hold it
	// so the count-vs-length computation below cannot underflow.
	if (marker->length < sizeof(inode_t)) {
		safs::log_err("{}: section length {} is too small", __func__, marker->length);
		return kOpFailure;
	}

	inode_t freeNodesNumber{};
	getINode(&ptr, freeNodesNumber);

	constexpr uint8_t kFreeNodesEntrySize = sizeof(inode_t) + sizeof(uint32_t);
	if (marker->length > 0 &&
	    freeNodesNumber != (marker->length - sizeof(inode_t)) / kFreeNodesEntrySize) {
		safs::log_warn("FREE section: count ({}) does not match section length, adjusting",
		               freeNodesNumber);
		freeNodesNumber = (marker->length - sizeof(inode_t)) / kFreeNodesEntrySize;
	}

	MetadataWriterFDB writer(kvEngine_);
	size_t pending = 0;
	uint64_t loadedCount = 0;

	for (inode_t i = 0; i < freeNodesNumber; ++i) {
		// Ensure the next entry fits before reading it (subtraction avoids overflow; offset(ptr)
		// <= sectionEnd is maintained because freeNodesNumber is clamped to the section length).
		if (kFreeNodesEntrySize > sectionEnd - metadataFile_->offset(ptr)) {
			safs::log_err("{}: truncated entry at index {}", __func__, i);
			return kOpFailure;
		}

		inode_t inode{};
		uint32_t timestamp{};
		getINode(&ptr, inode);
		get32bit(&ptr, timestamp);

		writer.enqueue(std::make_unique<FreeNodeUpdateEvent>(inode, timestamp));
		loadedCount++;

		if (++pending >= kFlushThreshold) {
			if (!writer.flush()) { return kOpFailure; }
			pending = 0;
		}
	}

	if (!writer.flush(MetadataWriterFDB::FlushMode::kDrainUntilEmpty)) {
		safs::log_err("Failed to flush bootstrapped free nodes to FDB");
		return kOpFailure;
	}

	safs::log_info("Bootstrapped {} free nodes from metadata file into FDB", loadedCount);
	return kOpSuccess;
}

int8_t MetadataSectionBootstrapFDB::loadXAttrSection() {
	if (kvEngine_ == nullptr || metadataFile_ == nullptr) { return kOpFailure; }

	auto marker = findSection("XATR 1.0");
	if (!marker.has_value()) {
		safs::log_warn("No metadata section marker found for XATR 1.0");
		return kOpFailure;
	}

	const uint8_t *ptr = metadataFile_->seek(marker->offset);
	if (marker->length > std::numeric_limits<size_t>::max() - marker->offset) {
		safs::log_err("Bootstrapping xattr: section bounds overflow");
		return kOpFailure;
	}
	const size_t sectionEnd = marker->offset + static_cast<size_t>(marker->length);
	constexpr size_t kXAttrHeaderSize = sizeof(inode_t) + sizeof(uint8_t) + sizeof(uint32_t);

	MetadataWriterFDB writer(kvEngine_);
	uint64_t xattrCount = 0;
	size_t pending = 0;

	while (metadataFile_->offset(ptr) < sectionEnd) {
		// Subtraction avoids overflow; offset(ptr) < sectionEnd is guaranteed by the loop.
		if (kXAttrHeaderSize > sectionEnd - metadataFile_->offset(ptr)) {
			safs::log_err("Bootstrapping xattr: truncated entry header");
			return kOpFailure;
		}

		inode_t inode{};
		getINode(&ptr, inode);
		uint8_t attributeNameLength = get8bit(&ptr);

		uint32_t attributeValueLength{};
		get32bit(&ptr, attributeValueLength);

		if (inode == 0) { break; }

		if (attributeNameLength == 0) {
			safs::log_err("Bootstrapping xattr: empty name for inode {}", inode);
			return kOpFailure;
		}

		if (attributeValueLength > SFS_XATTR_SIZE_MAX) {
			safs::log_err("Bootstrapping xattr: value oversized for inode {}", inode);
			return kOpFailure;
		}

		const size_t attributeDataLength =
		    static_cast<size_t>(attributeNameLength) + static_cast<size_t>(attributeValueLength);
		// Subtraction avoids overflow; offset(ptr) <= sectionEnd holds after the header read.
		if (attributeDataLength > sectionEnd - metadataFile_->offset(ptr)) {
			safs::log_err("Bootstrapping xattr: truncated attribute data for inode {}", inode);
			return kOpFailure;
		}

		std::span<const uint8_t> attributeName(ptr, attributeNameLength);
		ptr += attributeNameLength;

		std::span<const uint8_t> attributeValue(ptr, attributeValueLength);
		ptr += attributeValueLength;

		writer.enqueue(std::make_unique<XAttrUpdateEvent>(inode, attributeName, attributeValue));
		xattrCount++;

		if (++pending >= kFlushThreshold) {
			if (!writer.flush()) { return kOpFailure; }
			pending = 0;
		}
	}

	if (!writer.flush(MetadataWriterFDB::FlushMode::kDrainUntilEmpty)) {
		safs::log_err("Failed to flush bootstrapped xattrs to FDB");
		return kOpFailure;
	}

	safs::log_info("Bootstrapped {} xattrs from metadata file into FDB", xattrCount);
	return kOpSuccess;
}

std::optional<MetadataSectionBootstrapFDB::SectionMarker> MetadataSectionBootstrapFDB::findSection(
    std::string_view name) const {
	auto sectionIterator = sectionMarkers_.find(std::string(name));
	if (sectionIterator == sectionMarkers_.end()) { return std::nullopt; }
	return sectionIterator->second;
}

void MetadataSectionBootstrapFDB::initMetadataFileSections() {
	metadataFileSections_.clear();

	// Filesystem MetadataSection "NODE 1.0"
	metadataFileSections_.emplace_back(MetadataFileSection{
	    .name = "NODE 1.0",
	    .isBootstrapNeeded = [this](bool) { return isSectionBootstrapNeeded(kNodeKeyPrefix); },
	    .loadFunction = [this](bool) { return loadNodesSection(); },
	});

	// Filesystem MetadataSection "EDGE 1.0"
	metadataFileSections_.emplace_back(MetadataFileSection{
	    .name = "EDGE 1.0",
	    .isBootstrapNeeded = [this](bool) { return isSectionBootstrapNeeded(kEdgeKeyPrefix); },
	    .loadFunction = [this](bool) { return loadEdgesSection(); },
	});

	// Filesystem MetadataSection "FREE 1.0"
	metadataFileSections_.emplace_back(MetadataFileSection{
	    .name = "FREE 1.0",
	    .isBootstrapNeeded = [this](bool) { return isSectionBootstrapNeeded(kFreeKeyPrefix); },
	    .loadFunction = [this](bool) { return loadFreeSection(); },
	});

	// Filesystem MetadataSection "XATR 1.0"
	metadataFileSections_.emplace_back(MetadataFileSection{
	    .name = "XATR 1.0",
	    .isBootstrapNeeded = [this](bool) { return isSectionBootstrapNeeded(kXAttrKeyPrefix); },
	    .loadFunction = [this](bool) { return loadXAttrSection(); },
	});

	// Filesystem MetadataSection "ACLS 1.2"
	// Filesystem MetadataSection "QUOT 1.1"
	// Filesystem MetadataSection "FLCK 1.0"

	// Filesystem MetadataSection "CHNK 1.0"
	metadataFileSections_.emplace_back(MetadataFileSection{
	    .name = "CHNK 1.0",
	    .isBootstrapNeeded = [this](bool) { return isSectionBootstrapNeeded(kChunkLatestKeyPrefix); },
	    .loadFunction = [this](bool) { return loadChunkSection(); },
	});
}
