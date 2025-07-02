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

#include <common/hashfn.h>
#include <master/filesystem_metadata.h>
#include <master/filesystem_xattr.h>

static uint64_t xattr_checksum(const XAttributeDataEntry *xattrDataEntry) {
	if (!xattrDataEntry) {
		return 0;
	}

	uint64_t seed = 645819511511147ULL;
	hashCombine(
	    seed, xattrDataEntry->inode,
	    ByteArray(xattrDataEntry->attributeName.data(), xattrDataEntry->attributeNameLength),
	    ByteArray(xattrDataEntry->attributeValue.data(), xattrDataEntry->attributeValueLength));

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
	// Remove the data from the xattr_inode_entry
	entry->xattrDataList.remove(xattrDataEntry);

	if (gChecksumBackgroundUpdater.isXattrIncluded(xattrDataEntry)) {
		removeFromChecksum(gChecksumBackgroundUpdater.xattrChecksum, xattrDataEntry->checksum);
	}

	removeFromChecksum(gMetadata->xattrChecksum, xattrDataEntry->checksum);

	// Delete the entry from the hash bucket
	auto hash = get_xattr_data_hash(entry->inode, xattrDataEntry->attributeNameLength,
	                                xattrDataEntry->attributeName.data());
	auto start = gMetadata->xattrDataHash[hash].begin();
	auto end = gMetadata->xattrDataHash[hash].end();

	auto entryIt =
	    std::find_if(start, end, [xattrDataEntry](const std::unique_ptr<XAttributeDataEntry> &ptr) {
		    return ptr.get() == xattrDataEntry;
	    });

	if (entryIt != end) {
		gMetadata->xattrDataHash[hash].remove(*entryIt);
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

void xattr_removeinode(inode_t inode) {
	XAttributeInodeEntry *xattrInodeEntry = nullptr;

	auto hash = get_xattr_inode_hash(inode);
	auto start = gMetadata->xattrInodeHash[hash].begin();
	auto end = gMetadata->xattrInodeHash[hash].end();

	for (auto attributeIterator = start; attributeIterator != end;) {
		xattrInodeEntry = attributeIterator->get();
		if (xattrInodeEntry->inode == inode) {
			while (!xattrInodeEntry->xattrDataList.empty()) {
				xattr_removeentry(xattrInodeEntry, xattrInodeEntry->xattrDataList.front());
			}
			attributeIterator = gMetadata->xattrInodeHash[hash].erase(attributeIterator);
		} else {
			++attributeIterator;
		}
	}
}

uint8_t xattr_setattr(inode_t inode, uint8_t attributeNameLength, const uint8_t *attributeName,
                      uint32_t attributeValueLength, const uint8_t *attributeValue, uint8_t mode) {
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
	auto start = gMetadata->xattrInodeHash[inodeHash].begin();
	auto end = gMetadata->xattrInodeHash[inodeHash].end();

	for (auto attributeIterator = start; attributeIterator != end; ++attributeIterator) {
		xattrInodeEntry = attributeIterator->get();
		if (xattrInodeEntry->inode == inode) {
			break;
		}
	}

	auto dataHash = get_xattr_data_hash(inode, attributeNameLength, attributeName);
	for (const auto &xattrDataEntry : gMetadata->xattrDataHash[dataHash]) {
		if (xattrDataEntry->inode == inode &&
		    xattrDataEntry->attributeNameLength == attributeNameLength &&
		    memcmp(xattrDataEntry->attributeName.data(), attributeName, attributeNameLength) == 0) {
			passert(xattrInodeEntry);

			if (mode == XATTR_SMODE_CREATE_ONLY) {  // create only
				return SAUNAFS_ERROR_EEXIST;
			}

			if (mode == XATTR_SMODE_REMOVE) {  // remove
				xattrInodeEntry->attributeNameLength -= attributeNameLength + 1U;
				xattrInodeEntry->attributeValueLength -= xattrDataEntry->attributeValueLength;

				xattr_removeentry(xattrInodeEntry, xattrDataEntry.get());

				if (xattrInodeEntry->xattrDataList.empty()) {
					if (xattrInodeEntry->attributeNameLength != 0 ||
					    xattrInodeEntry->attributeValueLength != 0) {
						safs_pretty_syslog(
						    LOG_WARNING,
						    "xattr non zero lengths on remove "
						    "(inode:%" PRIiNode ",anleng:%" PRIu32 ",avleng:%" PRIu32 ")",
						    xattrInodeEntry->inode, xattrInodeEntry->attributeNameLength,
						    xattrInodeEntry->attributeValueLength);
					}
					xattr_removeinode(inode);
				}
				return SAUNAFS_STATUS_OK;
			}

			xattrInodeEntry->attributeValueLength -= xattrDataEntry->attributeValueLength;

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

			xattrDataEntry->attributeValueLength = attributeValueLength;
			xattrInodeEntry->attributeValueLength += attributeValueLength;
			xattr_update_checksum(xattrDataEntry.get());
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
	xattrDataEntry->attributeNameLength = attributeNameLength;

	if (attributeValueLength > 0) {
		xattrDataEntry->attributeValue.resize(attributeValueLength);
		passert(xattrDataEntry->attributeValue.data());
		memcpy(xattrDataEntry->attributeValue.data(), attributeValue, attributeValueLength);
	} else {
		xattrDataEntry->attributeValue.clear();
	}

	xattrDataEntry->attributeValueLength = attributeValueLength;
	xattrDataEntry->checksum = 0;
	xattr_update_checksum(xattrDataEntry.get());

	gMetadata->xattrDataHash[dataHash].push_back(std::move(xattrDataEntry));
	auto *xattrDataEntryPointer = gMetadata->xattrDataHash[dataHash].back().get();

	if (xattrInodeEntry) {
		xattrInodeEntry->xattrDataList.push_back(xattrDataEntryPointer);
		xattrInodeEntry->attributeNameLength += attributeNameLength + 1U;
		xattrInodeEntry->attributeValueLength += attributeValueLength;
	} else {
		auto xattrInodeEntry = std::make_unique<XAttributeInodeEntry>();
		passert(xattrInodeEntry);
		xattrInodeEntry->inode = inode;
		xattrInodeEntry->xattrDataList.push_back(xattrDataEntryPointer);
		xattrInodeEntry->attributeNameLength = attributeNameLength + 1U;
		xattrInodeEntry->attributeValueLength = attributeValueLength;
		gMetadata->xattrInodeHash[inodeHash].push_front(std::move(xattrInodeEntry));
	}

	return SAUNAFS_STATUS_OK;
}

uint8_t xattr_getattr(inode_t inode, uint8_t attributeNameLength, const uint8_t *attributeName,
                      uint32_t *attributeValueLength, uint8_t **attributeValue) {

	auto hash = get_xattr_data_hash(inode, attributeNameLength, attributeName);

	auto xattrDataEntryHasAttribute = [&](const auto &entry) {
		return entry->inode == inode && entry->attributeNameLength == attributeNameLength &&
		       memcmp(entry->attributeName.data(), attributeName, attributeNameLength) == 0;
	};

	for (const auto &xattrDataEntry : gMetadata->xattrDataHash[hash]) {
		if (xattrDataEntryHasAttribute(xattrDataEntry)) {
			if (xattrDataEntry->attributeValueLength > SFS_XATTR_SIZE_MAX) {
				return SAUNAFS_ERROR_ERANGE;
			}

			*attributeValue = xattrDataEntry->attributeValue.data();
			*attributeValueLength = xattrDataEntry->attributeValueLength;
			return SAUNAFS_STATUS_OK;
		}
	}
	return SAUNAFS_ERROR_ENOATTR;
}

uint8_t get_xattrs_length_for_inode(inode_t inode, void **xattrInodePointer, uint32_t *xattrSize) {
	XAttributeInodeEntry *xattrInodeEntry;
	auto hash = get_xattr_inode_hash(inode);
	auto start = gMetadata->xattrInodeHash[hash].begin();
	auto end =  gMetadata->xattrInodeHash[hash].end();

	for (auto attributeIterator = start; attributeIterator != end;) {
		xattrInodeEntry = attributeIterator->get();
		if (xattrInodeEntry->inode == inode) {
			*xattrInodePointer = xattrInodeEntry;
			for (const auto &xattrDataEntry : xattrInodeEntry->xattrDataList) {
				*xattrSize += xattrDataEntry->attributeNameLength + 1U;
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
		for (const auto &xattrDataEntry : xattrEntry->xattrDataList) {
			memcpy(xattrDataBuffer + entryLength, xattrDataEntry->attributeName.data(),
			       xattrDataEntry->attributeNameLength);
			entryLength += xattrDataEntry->attributeNameLength;
			xattrDataBuffer[entryLength++] = 0;
		}
	}
}
