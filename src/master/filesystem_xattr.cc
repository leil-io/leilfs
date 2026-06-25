/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
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

#include <algorithm>
#include <cassert>
#include <span>

#include <common/hashfn.h>
#include <common/massert.h>
#include <master/filesystem_metadata.h>
#include <master/filesystem_xattr.h>

namespace {
std::span<const uint8_t> xattrBytesView(const uint8_t *data, size_t size) {
	// A null pointer is only acceptable when the length is zero; otherwise the API
	// produced a malformed pointer/length pair and we want to catch it, not mask it.
	// Use sassert so the check is enforced in release builds too (plain assert is
	// compiled out under NDEBUG, leaving std::span{nullptr, size} as UB).
	sassert(data != nullptr || size == 0);
	if (size == 0) { return {}; }
	return {data, size};
}
}  // namespace

XAttributeInodeEntry *find_xattr_inode_entry(inode_t inode, uint32_t inodeHash) {
	for (const auto &xattrEntry : gMetadata->xattrInodeHash[inodeHash]) {
		if (xattrEntry->inode == inode) { return xattrEntry.get(); }
	}
	return nullptr;
}

static uint64_t xattr_checksum(const XAttributeDataEntry *xattrDataEntry) {
	if (!xattrDataEntry) {
		return 0;
	}

	uint64_t seed = 645819511511147ULL;
	hashCombine(
	    seed, xattrDataEntry->inode,
	    ByteArray(xattrDataEntry->attributeName.data(), xattrDataEntry->attributeName.size()),
	    ByteArray(xattrDataEntry->attributeValue.data(), xattrDataEntry->attributeValue.size()));

	return seed;
}

static void xattr_update_checksum(XAttributeDataEntry *xattrDataEntry) {
	if (!xattrDataEntry) {
		return;
	}

	if (gChecksumBackgroundUpdater.isXattrIncluded(xattrDataEntry)) {
		removeFromChecksum(gChecksumBackgroundUpdater.xattrChecksum, xattrDataEntry->checksum);
	}

	removeFromChecksum(gMetadata->xattrChecksum, xattrDataEntry->checksum);
	xattrDataEntry->checksum = xattr_checksum(xattrDataEntry);

	if (gChecksumBackgroundUpdater.isXattrIncluded(xattrDataEntry)) {
		addToChecksum(gChecksumBackgroundUpdater.xattrChecksum, xattrDataEntry->checksum);
	}

	addToChecksum(gMetadata->xattrChecksum, xattrDataEntry->checksum);
}

static inline void xattr_removeentry(XAttributeInodeEntry *entry,
                                     XAttributeDataEntry *xattrDataEntry) {
	if (xattrDataEntry == nullptr || entry == nullptr) {
		safs::log_err("{}: xattrDataEntry or entry is nullptr", __func__);
		return;
	}

	// Remove the data from the xattr_inode_entry
	entry->xattrDataEntries.erase(
	    std::remove(entry->xattrDataEntries.begin(), entry->xattrDataEntries.end(), xattrDataEntry),
	    entry->xattrDataEntries.end());

	if (gChecksumBackgroundUpdater.isXattrIncluded(xattrDataEntry)) {
		removeFromChecksum(gChecksumBackgroundUpdater.xattrChecksum, xattrDataEntry->checksum);
	}

	removeFromChecksum(gMetadata->xattrChecksum, xattrDataEntry->checksum);

	// Delete the entry from the hash bucket
	auto hash = get_xattr_data_hash(entry->inode, xattrDataEntry->attributeName.size(),
	                                xattrDataEntry->attributeName.data());
	auto start = gMetadata->xattrDataHash[hash].begin();
	auto end = gMetadata->xattrDataHash[hash].end();

	auto entryIt =
	    std::find_if(start, end, [xattrDataEntry](const std::unique_ptr<XAttributeDataEntry> &ptr) {
		    return ptr.get() == xattrDataEntry;
	    });

	if (entryIt != end) {
		gMetadata->xattrDataHash[hash].erase(entryIt);
	}
}

void xattr_checksum_add_to_background(XAttributeDataEntry *xattrDataEntry) {
	if (!xattrDataEntry) { return; }

	removeFromChecksum(gMetadata->xattrChecksum, xattrDataEntry->checksum);
	xattrDataEntry->checksum = xattr_checksum(xattrDataEntry);

	addToChecksum(gMetadata->xattrChecksum, xattrDataEntry->checksum);
	addToChecksum(gChecksumBackgroundUpdater.xattrChecksum, xattrDataEntry->checksum);
}

void xattr_recalculate_checksum() {
	gMetadata->xattrChecksum = XATTRCHECKSUMSEED;
	for (int i = 0; i < XATTR_DATA_HASH_SIZE; ++i) {
		for (const auto &xattrDataEntry : gMetadata->xattrDataHash[i]) {
			xattrDataEntry->checksum = xattr_checksum(xattrDataEntry.get());
			addToChecksum(gMetadata->xattrChecksum, xattrDataEntry->checksum);
		}
	}
}

void xattr_removeinode(inode_t inode, bool emitSignals) {
	XAttributeInodeEntry *xattrInodeEntry = nullptr;
	bool hadEntries = false;

	auto hash = get_xattr_inode_hash(inode);
	auto &bucket = gMetadata->xattrInodeHash[hash];
	for (auto attributeIterator = bucket.begin(); attributeIterator != bucket.end();) {
		xattrInodeEntry = attributeIterator->get();
		if (xattrInodeEntry->inode == inode) {
			hadEntries = true;
			while (!xattrInodeEntry->xattrDataEntries.empty()) {
				xattr_removeentry(xattrInodeEntry, xattrInodeEntry->xattrDataEntries.front());
			}
			attributeIterator = bucket.erase(attributeIterator);
		} else {
			++attributeIterator;
		}
	}

	if (hadEntries && emitSignals) {
		gXAttrInodeRemovedSignal.emit(inode);
	}
}

uint8_t xattr_setattr(inode_t inode, uint8_t attributeNameLength, const uint8_t *attributeName,
                      uint32_t attributeValueLength, const uint8_t *attributeValue, uint8_t mode,
                      bool emitSignals) {
	XAttributeInodeEntry *xattrInodeEntry = nullptr;

	if (attributeValueLength > SFS_XATTR_SIZE_MAX) {
		return SAUNAFS_ERROR_ERANGE;
	}
#if SFS_XATTR_NAME_MAX < 255
	if (attributeNameLength == 0U || attributeNameLength > SFS_XATTR_NAME_MAX) {
#else
	if (attributeNameLength == 0U) {
#endif
		return SAUNAFS_ERROR_EINVAL;
	}

	auto inodeHash = get_xattr_inode_hash(inode);
	xattrInodeEntry = find_xattr_inode_entry(inode, inodeHash);

	auto dataHash = get_xattr_data_hash(inode, attributeNameLength, attributeName);
	for (const auto &xattrDataEntry : gMetadata->xattrDataHash[dataHash]) {
		if (xattrDataEntry->inode == inode &&
		    xattrDataEntry->attributeName.size() == attributeNameLength &&
		    memcmp(xattrDataEntry->attributeName.data(), attributeName, attributeNameLength) == 0) {
			passert(xattrInodeEntry);

			if (mode == XATTR_SMODE_CREATE_ONLY) {  // create only
				return SAUNAFS_ERROR_EEXIST;
			}

			if (mode == XATTR_SMODE_REMOVE) {  // remove
				xattrInodeEntry->attributeNameLength -= attributeNameLength + 1U;
				xattrInodeEntry->attributeValueLength -= xattrDataEntry->attributeValue.size();

				if (emitSignals && gXAttrRemovedSignal.size() > 0) {
					gXAttrRemovedSignal.emit(inode,
					                         xattrBytesView(attributeName, attributeNameLength));
				}

				xattr_removeentry(xattrInodeEntry, xattrDataEntry.get());

				if (xattrInodeEntry->xattrDataEntries.empty()) {
					if (xattrInodeEntry->attributeNameLength != 0 ||
					    xattrInodeEntry->attributeValueLength != 0) {
						safs::log_warn("xattr non zero lengths on remove (inode:%" PRIiNode
						               ", attributeNameLength: {}, attributeValueLength: {})",
						               xattrInodeEntry->inode, xattrInodeEntry->attributeNameLength,
						               xattrInodeEntry->attributeValueLength);
					}
					xattr_removeinode(inode, emitSignals);
				}
				return SAUNAFS_STATUS_OK;
			}

			xattrInodeEntry->attributeValueLength -= xattrDataEntry->attributeValue.size();

			if (!xattrDataEntry->attributeValue.empty()) {
				xattrDataEntry->attributeValue.clear();
			}

			if (attributeValueLength > 0) {
				xattrDataEntry->attributeValue.resize(attributeValueLength);
				passert(xattrDataEntry->attributeValue.data());
				memcpy(xattrDataEntry->attributeValue.data(), attributeValue, attributeValueLength);
			} else {
				xattrDataEntry->attributeValue.clear();
			}

			xattrInodeEntry->attributeValueLength += attributeValueLength;
			xattr_update_checksum(xattrDataEntry.get());

			if (emitSignals && gXAttrChangedSignal.size() > 0) {
				gXAttrChangedSignal.emit(inode, xattrBytesView(attributeName, attributeNameLength),
				                         xattrBytesView(xattrDataEntry->attributeValue.data(),
				                                        xattrDataEntry->attributeValue.size()));
			}

			return SAUNAFS_STATUS_OK;
		}
	}

	if (mode == XATTR_SMODE_REPLACE_ONLY || mode == XATTR_SMODE_REMOVE) {
		return SAUNAFS_ERROR_ENOATTR;
	}

	if (xattrInodeEntry != nullptr &&
	    xattrInodeEntry->attributeNameLength + attributeNameLength + 1 > SFS_XATTR_LIST_MAX) {
		return SAUNAFS_ERROR_ERANGE;
	}

	auto xattrDataEntry = std::make_unique<XAttributeDataEntry>();
	xattrDataEntry->inode = inode;
	xattrDataEntry->attributeName.resize(attributeNameLength);
	passert(xattrDataEntry->attributeName.data());
	memcpy(xattrDataEntry->attributeName.data(), attributeName, attributeNameLength);

	if (attributeValueLength > 0) {
		xattrDataEntry->attributeValue.resize(attributeValueLength);
		passert(xattrDataEntry->attributeValue.data());
		memcpy(xattrDataEntry->attributeValue.data(), attributeValue, attributeValueLength);
	} else {
		xattrDataEntry->attributeValue.clear();
	}

	xattrDataEntry->checksum = 0;
	xattr_update_checksum(xattrDataEntry.get());

	gMetadata->xattrDataHash[dataHash].push_back(std::move(xattrDataEntry));
	auto *xattrDataEntryPointer = gMetadata->xattrDataHash[dataHash].back().get();

	if (xattrInodeEntry != nullptr) {
		xattrInodeEntry->xattrDataEntries.push_back(xattrDataEntryPointer);
		xattrInodeEntry->attributeNameLength += attributeNameLength + 1U;
		xattrInodeEntry->attributeValueLength += attributeValueLength;
	} else {
		auto xattrInodeEntry =
		    XAttributeInodeEntry::create(inode, attributeNameLength + 1U, attributeValueLength);
		xattrInodeEntry->xattrDataEntries.push_back(xattrDataEntryPointer);
		gMetadata->xattrInodeHash[inodeHash].push_back(std::move(xattrInodeEntry));
	}

	if (emitSignals && gXAttrChangedSignal.size() > 0) {
		gXAttrChangedSignal.emit(inode, xattrBytesView(attributeName, attributeNameLength),
		                         xattrBytesView(xattrDataEntryPointer->attributeValue.data(),
		                                        xattrDataEntryPointer->attributeValue.size()));
	}

	return SAUNAFS_STATUS_OK;
}

uint8_t xattr_getattr(inode_t inode, uint8_t attributeNameLength, const uint8_t *attributeName,
                      uint32_t *attributeValueLength, uint8_t **attributeValue) {

	auto hash = get_xattr_data_hash(inode, attributeNameLength, attributeName);

	auto xattrDataEntryHasAttribute = [&](const auto &entry) {
		return entry->inode == inode && entry->attributeName.size() == attributeNameLength &&
		       memcmp(entry->attributeName.data(), attributeName, attributeNameLength) == 0;
	};

	for (const auto &xattrDataEntry : gMetadata->xattrDataHash[hash]) {
		if (xattrDataEntryHasAttribute(xattrDataEntry)) {
			if (xattrDataEntry->attributeValue.size() > SFS_XATTR_SIZE_MAX) {
				return SAUNAFS_ERROR_ERANGE;
			}

			*attributeValue = xattrDataEntry->attributeValue.data();
			*attributeValueLength = xattrDataEntry->attributeValue.size();
			return SAUNAFS_STATUS_OK;
		}
	}
	return SAUNAFS_ERROR_ENOATTR;
}

uint8_t get_xattrs_length_for_inode(inode_t inode, void **xattrInodePointer, uint32_t *xattrSize) {
	auto hash = get_xattr_inode_hash(inode);
	for (const auto &xattrInodeEntry : gMetadata->xattrInodeHash[hash]) {
		if (xattrInodeEntry->inode == inode) {
			*xattrInodePointer = xattrInodeEntry.get();
			for (const auto &xattrDataEntry : xattrInodeEntry->xattrDataEntries) {
				*xattrSize += xattrDataEntry->attributeName.size() + 1U;
			}
			if (*xattrSize > SFS_XATTR_LIST_MAX) {
				return SAUNAFS_ERROR_ERANGE;
			}
			return SAUNAFS_STATUS_OK;
		}
	}

	*xattrInodePointer = nullptr;
	return SAUNAFS_STATUS_OK;
}

void xattr_listattr_data(void *xattrInodeEntry, uint8_t *xattrDataBuffer) {
	auto *xattrEntry = static_cast<XAttributeInodeEntry *>(xattrInodeEntry);

	uint32_t entryLength = 0;
	if (xattrEntry) {
		passert(xattrDataBuffer);
		for (const auto &xattrDataEntry : xattrEntry->xattrDataEntries) {
			memcpy(xattrDataBuffer + entryLength, xattrDataEntry->attributeName.data(),
			       xattrDataEntry->attributeName.size());
			entryLength += xattrDataEntry->attributeName.size();
			xattrDataBuffer[entryLength++] = 0;
		}
	}
}
