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

#include "master/metadata_xattr_undo_recorder.h"

#include <cstdint>
#include <cstring>
#include <ranges>
#include <string>
#include <vector>

#include "common/datapack.h"
#include "kv/kv_utils.h"
#include "master/filesystem_xattr.h"
#include "master/kv_common_keys.h"
#include "master/metadata_backend_interface.h"
#include "master/metadata_checkpoint_helpers.h"
#include "protocol/SFSCommunication.h"
#include "slogger/slogger.h"

namespace {

constexpr uint8_t kXAttrTombstone = 0;
constexpr uint8_t kXAttrPresent = 1;

bool startsWith(const kv::Key &key, std::string_view prefix) {
	return key.size() >= prefix.size() &&
	       std::memcmp(key.data(), prefix.data(), prefix.size()) == 0;
}

kv::Key xattrUndoPrefix(uint64_t checkpointVersion) {
	return kv::encodeKeyBE(kXAttrUndoKeyPrefix, checkpointVersion);
}

kv::Key xattrUndoKey(uint64_t checkpointVersion, inode_t inode, std::span<const uint8_t> name) {
	kv::Key key = kv::encodeKeyBE(kXAttrUndoKeyPrefix, checkpointVersion, inode);
	key.insert(key.end(), name.begin(), name.end());
	return key;
}

// Undo key format: XATRU_ + <checkpoint:u64> + <inode:inode_t> + <name>
bool decodeXAttrUndoKey(const kv::Key &key, inode_t &inode, std::vector<uint8_t> &name) {
	const size_t fixedSize = kXAttrUndoKeyPrefix.size() + sizeof(uint64_t) + sizeof(inode_t);
	if (!startsWith(key, kXAttrUndoKeyPrefix) || key.size() <= fixedSize) { return false; }

	const uint8_t *ptr = key.data() + kXAttrUndoKeyPrefix.size() + sizeof(uint64_t);
	getINode(&ptr, inode);

	name.assign(key.data() + fixedSize, key.data() + key.size());
	return true;
}

// Live key format: XATR_ + <inode:inode_t> + <name>
bool decodeLiveXAttrKey(const kv::Key &key, inode_t &inode, std::vector<uint8_t> &name) {
	const size_t fixedSize = kXAttrKeyPrefix.size() + sizeof(inode_t);
	if (!startsWith(key, kXAttrKeyPrefix) || key.size() <= fixedSize) { return false; }

	const uint8_t *ptr = key.data() + kXAttrKeyPrefix.size();
	getINode(&ptr, inode);

	name.assign(key.data() + fixedSize, key.data() + key.size());
	return true;
}

}  // namespace

XAttrUndoRecorder::XAttrUndoRecorder(kv::IKVEngine *kvEngine) : kvEngine_(kvEngine) {}

void XAttrUndoRecorder::beforeMutation(const MetadataMutationContext &context,
                                       const MetadataMutation &mutation) {
	if (context.transaction == nullptr || context.checkpointVersion == 0) { return; }

	if (const auto *xattrSet = std::get_if<XAttrSetMutation>(&mutation)) {
		beforeXAttrKey(context, xattrSet->inode, xattrSet->name, xattrSet->liveKey);
		return;
	}

	if (const auto *xattrRemove = std::get_if<XAttrRemoveMutation>(&mutation)) {
		beforeXAttrKey(context, xattrRemove->inode, xattrRemove->name, xattrRemove->liveKey);
		return;
	}

	if (const auto *xattrRange = std::get_if<XAttrRangeRemoveMutation>(&mutation)) {
		beforeXAttrRange(context, xattrRange->rangeBegin, xattrRange->rangeEnd);
		return;
	}

	safs::log_warn("{}: received non-xattr mutation for xattr recorder", __func__);
}

bool XAttrUndoRecorder::restoreToCheckpointVersion(uint64_t targetVersion) {
	auto retainedCheckpointVersions = checkpoints::loadCheckpointVersions(kvEngine_);
	if (retainedCheckpointVersions.empty()) {
		safs::log_info("No retained xattr checkpoints found");
		return true;
	}

	// sort checkpoint versions in ascending order
	std::ranges::sort(retainedCheckpointVersions);

	if (targetVersion > retainedCheckpointVersions.back()) {
		safs::log_warn("Target version {} is newer than retained latest {}", targetVersion,
		               retainedCheckpointVersions.back());
		return false;
	}

	if (targetVersion < retainedCheckpointVersions.front()) {
		safs::log_warn("Target version {} is older than retained earliest {}", targetVersion,
		               retainedCheckpointVersions.front());
		return false;
	}

	uint64_t restoredEntries = 0;
	// Iterate checkpoints in descending order and apply those for which checkpoint >= targetVersion.
	// Please see ChunkUndoRecorder::restoreToCheckpointVersion() for rationale on this stopping
	// condition (the target interval itself must be undone).
	for (const auto checkpointVersion : std::views::reverse(retainedCheckpointVersions)) {
		if (checkpointVersion < targetVersion) { break; }

		auto [entries, success] =
		    restoreSingleCheckpoint(FilesystemOperationContext{}, checkpointVersion);

		if (!success) {
			safs::log_err("{}: failed to restore xattr checkpoint version {}", __func__,
			              checkpointVersion);
			return false;
		}
		restoredEntries += entries;
	}

	safs::log_info("Restored {} xattr undo entries to version {}", restoredEntries, targetVersion);
	return true;
}

std::pair<uint64_t, bool> XAttrUndoRecorder::restoreSingleCheckpoint(
    const FilesystemOperationContext & /*fsOpContext*/, uint64_t checkpointVersion) {
	kv::Key prefix = xattrUndoPrefix(checkpointVersion);
	kv::KeySelector startSelector(prefix, true, 0);
	kv::KeySelector endSelector(kv::prefixEnd(prefix), true, 0);
	uint64_t restoredEntries = 0;

	while (true) {
		auto transaction = kvEngine_->createReadOnlyTransaction();
		auto page = transaction->getRange(startSelector, endSelector, kv::kDefaultGetRangeLimit);

		for (const auto &pair : page.getPairs()) {
			inode_t inode = 0;
			std::vector<uint8_t> name;
			if (!decodeXAttrUndoKey(pair.key, inode, name) || pair.value.empty()) { continue; }

			const auto nameLength = static_cast<uint8_t>(name.size());
			uint8_t status = SAUNAFS_STATUS_OK;
			if (pair.value[0] == kXAttrTombstone) {
				// Tombstone: xattr did not exist at the checkpoint, so remove it (idempotent).
				status = xattr_setattr(inode, nameLength, name.data(), 0, nullptr,
				                       XATTR_SMODE_REMOVE, /*emitSignals=*/false);
				if (status == SAUNAFS_ERROR_ENOATTR) { status = SAUNAFS_STATUS_OK; }
			} else {
				const uint8_t *value = pair.value.data() + 1;
				const auto valueLength = static_cast<uint32_t>(pair.value.size() - 1);
				status = xattr_setattr(inode, nameLength, name.data(), valueLength, value,
				                       XATTR_SMODE_CREATE_OR_REPLACE, /*emitSignals=*/false);
			}

			if (status != SAUNAFS_STATUS_OK) {
				safs::log_err("{}: failed to apply xattr undo for inode {} (status {})", __func__,
				              inode, status);
				return {restoredEntries, false};
			}
			++restoredEntries;
		}

		if (!page.hasMore() || page.getPairs().empty()) { break; }

		startSelector = kv::KeySelector(page.getPairs().back().key, false, 0);
	}
	return {restoredEntries, true};
}

int8_t XAttrUndoRecorder::dropCheckpointData(kv::IReadWriteTransaction *transaction,
                                             uint64_t droppedCheckpointVersion) {
	if (transaction == nullptr) { return kOpFailure; }

	kv::Key startKey = xattrUndoPrefix(droppedCheckpointVersion);
	transaction->removeRange(startKey, kv::prefixEnd(startKey));

	return kOpSuccess;
}

void XAttrUndoRecorder::beforeXAttrKey(const MetadataMutationContext &context, inode_t inode,
                                       std::span<const uint8_t> name, const kv::Key &liveKey) {
	if (context.checkpointVersion == 0) { return; }

	// The live XATR_ key uniquely identifies the xattr (inode, name); use it as the first-touch
	// dedup key so only the interval-start pre-image is captured.
	std::string dedupKey(liveKey.begin(), liveKey.end());
	if (touchedXAttrs_.contains(dedupKey)) { return; }

	recordXAttrUndo(context.transaction, context.checkpointVersion, inode, name, liveKey);

	touchedXAttrs_.insert(std::move(dedupKey));
}

void XAttrUndoRecorder::beforeXAttrRange(const MetadataMutationContext &context,
                                         const kv::Key &rangeBegin, const kv::Key &rangeEnd) {
	if (context.checkpointVersion == 0) { return; }

	// Record the pre-image of every live xattr of the inode before the inode-wide removal.
	kv::KeySelector startSelector(rangeBegin, true, 0);
	kv::KeySelector endSelector(rangeEnd, true, 0);

	while (true) {
		auto page =
		    context.transaction->getRange(startSelector, endSelector, kv::kDefaultGetRangeLimit);

		for (const auto &pair : page.getPairs()) {
			std::string dedupKey(pair.key.begin(), pair.key.end());
			if (touchedXAttrs_.contains(dedupKey)) { continue; }

			inode_t inode = 0;
			std::vector<uint8_t> name;
			if (!decodeLiveXAttrKey(pair.key, inode, name)) { continue; }

			recordXAttrUndo(context.transaction, context.checkpointVersion, inode, name, pair.key);
			touchedXAttrs_.insert(std::move(dedupKey));
		}

		if (!page.hasMore() || page.getPairs().empty()) { break; }

		startSelector = kv::KeySelector(page.getPairs().back().key, false, 0);
	}
}

void XAttrUndoRecorder::recordXAttrUndo(kv::IReadWriteTransaction *transaction,
                                        uint64_t checkpointVersion, inode_t inode,
                                        std::span<const uint8_t> name, const kv::Key &liveKey) {
	if (transaction == nullptr || checkpointVersion == 0) { return; }

	// Undo Key: XATRU_<checkpointVersion><inode><name>
	kv::Key undoKey = xattrUndoKey(checkpointVersion, inode, name);

	// If the undo row already exists, preserve the original pre-image.
	if (transaction->get(undoKey).has_value()) { return; }

	auto currentValue = transaction->get(liveKey);
	kv::Value undoValue;
	if (currentValue.has_value()) {
		// 0x01 presence byte + pre-image value (value may legitimately be empty).
		undoValue.reserve(1 + currentValue->size());
		undoValue.push_back(kXAttrPresent);
		undoValue.insert(undoValue.end(), currentValue->begin(), currentValue->end());
	} else {
		// Tombstone: xattr did not exist before the first mutation in this checkpoint interval.
		undoValue.push_back(kXAttrTombstone);
	}

	transaction->set(undoKey, undoValue);
}
