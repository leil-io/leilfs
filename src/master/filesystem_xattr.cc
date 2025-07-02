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

static uint64_t xattr_checksum(const xattr_data_entry *xde) {
	if (!xde) {
		return 0;
	}
	uint64_t seed = 645819511511147ULL;
	hashCombine(seed, xde->inode, ByteArray(xde->attributeName.data(), xde->anleng),
	            ByteArray(xde->attributeValue.data(), xde->avleng));
	return seed;
}

static void xattr_update_checksum(xattr_data_entry *xde) {
	if (!xde) {
		return;
	}
	if (gChecksumBackgroundUpdater.isXattrIncluded(xde)) {
		removeFromChecksum(gChecksumBackgroundUpdater.xattrChecksum, xde->checksum);
	}
	removeFromChecksum(gMetadata->xattrChecksum, xde->checksum);
	xde->checksum = xattr_checksum(xde);
	if (gChecksumBackgroundUpdater.isXattrIncluded(xde)) {
		addToChecksum(gChecksumBackgroundUpdater.xattrChecksum, xde->checksum);
	}
	addToChecksum(gMetadata->xattrChecksum, xde->checksum);
}

static inline void xattr_removeentry(xattr_inode_entry *entry, xattr_data_entry *xa) {
	// Remove the data from the xattr_inode_entry
	entry->xattrDataList.remove(xa);

	if (gChecksumBackgroundUpdater.isXattrIncluded(xa)) {
		removeFromChecksum(gChecksumBackgroundUpdater.xattrChecksum, xa->checksum);
	}
	removeFromChecksum(gMetadata->xattrChecksum, xa->checksum);

	// Delete the entry from the hash bucket
	auto hash = xattr_data_hash_fn(entry->inode, xa->anleng, xa->attributeName.data());
	auto start = gMetadata->xattr_data_hash[hash].begin();
	auto end = gMetadata->xattr_data_hash[hash].end();

	auto entryIt = std::find_if(
	    start, end, [xa](const std::unique_ptr<xattr_data_entry> &ptr) { return ptr.get() == xa; });

	if (entryIt != end) {
		gMetadata->xattr_data_hash[hash].remove(*entryIt);
	}
}

void xattr_checksum_add_to_background(xattr_data_entry *xde) {
	if (!xde) {
		return;
	}
	removeFromChecksum(gMetadata->xattrChecksum, xde->checksum);
	xde->checksum = xattr_checksum(xde);
	addToChecksum(gMetadata->xattrChecksum, xde->checksum);
	addToChecksum(gChecksumBackgroundUpdater.xattrChecksum, xde->checksum);
}

void xattr_recalculate_checksum() {
	gMetadata->xattrChecksum = XATTRCHECKSUMSEED;
	for (int i = 0; i < XATTR_DATA_HASH_SIZE; ++i) {
		for (const auto &xde : gMetadata->xattr_data_hash[i]) {
			xde->checksum = xattr_checksum(xde.get());
			addToChecksum(gMetadata->xattrChecksum, xde->checksum);
		}
	}
}

void xattr_removeinode(inode_t inode) {
	xattr_inode_entry *ih;

	auto hash = xattr_inode_hash_fn(inode);
	auto start = gMetadata->xattr_inode_hash[hash].begin();
	auto end = gMetadata->xattr_inode_hash[hash].end();

	for (auto attributeIterator = start; attributeIterator != end;) {
		ih = attributeIterator->get();
		if (ih->inode == inode) {
			while (!ih->xattrDataList.empty()) {
				xattr_removeentry(ih, ih->xattrDataList.front());
			}
			attributeIterator = gMetadata->xattr_inode_hash[hash].erase(attributeIterator);
		} else {
			++attributeIterator;
		}
	}
}

uint8_t xattr_setattr(inode_t inode, uint8_t anleng, const uint8_t *attrname, uint32_t avleng,
			const uint8_t *attrvalue, uint8_t mode) {
	xattr_inode_entry *xattrInodeEntry = nullptr;
	uint32_t hash;

	if (avleng > SFS_XATTR_SIZE_MAX) {
		return SAUNAFS_ERROR_ERANGE;
	}
#if SFS_XATTR_NAME_MAX < 255
	if (anleng == 0U || anleng > SFS_XATTR_NAME_MAX) {
#else
	if (anleng == 0U) {
#endif
		return SAUNAFS_ERROR_EINVAL;
	}

	auto ihash = xattr_inode_hash_fn(inode);
	auto start = gMetadata->xattr_inode_hash[ihash].begin();
	auto end = gMetadata->xattr_inode_hash[ihash].end();

	for (auto attributeIterator = start; attributeIterator != end; ++attributeIterator) {
		xattrInodeEntry = attributeIterator->get();
		if (xattrInodeEntry->inode == inode) {
			break;
		}
	}

	hash = xattr_data_hash_fn(inode, anleng, attrname);
	for (const auto &xattrDataEntry : gMetadata->xattr_data_hash[hash]) {
		if (xattrDataEntry->inode == inode && xattrDataEntry->anleng == anleng &&
		    memcmp(xattrDataEntry->attributeName.data(), attrname, anleng) == 0) {
			passert(xattrInodeEntry);

			if (mode == XATTR_SMODE_CREATE_ONLY) {  // create only
				return SAUNAFS_ERROR_EEXIST;
			}

			if (mode == XATTR_SMODE_REMOVE) {  // remove
				xattrInodeEntry->anleng -= anleng + 1U;
				xattrInodeEntry->avleng -= xattrDataEntry->avleng;

				xattr_removeentry(xattrInodeEntry, xattrDataEntry.get());

				if (xattrInodeEntry->xattrDataList.empty()) {
					if (xattrInodeEntry->anleng != 0 || xattrInodeEntry->avleng != 0) {
						safs_pretty_syslog(LOG_WARNING,
						       "xattr non zero lengths on remove "
						       "(inode:%" PRIiNode ",anleng:%" PRIu32
						       ",avleng:%" PRIu32 ")",
						       xattrInodeEntry->inode, xattrInodeEntry->anleng, xattrInodeEntry->avleng);
					}
					xattr_removeinode(inode);
				}
				return SAUNAFS_STATUS_OK;
			}

			xattrInodeEntry->avleng -= xattrDataEntry->avleng;

			if (!xattrDataEntry->attributeValue.empty()) {
				xattrDataEntry->attributeValue.clear();
			}

			if (avleng > 0) {
				xattrDataEntry->attributeValue.resize(avleng);
				passert(xattrDataEntry->attributeValue.data());
				memcpy(xattrDataEntry->attributeValue.data(), attrvalue, avleng);
			} else {
				xattrDataEntry->attributeValue.clear();
			}

			xattrDataEntry->avleng = avleng;
			xattrInodeEntry->avleng += avleng;
			xattr_update_checksum(xattrDataEntry.get());
			return SAUNAFS_STATUS_OK;
		}
	}

	if (mode == XATTR_SMODE_REPLACE_ONLY || mode == XATTR_SMODE_REMOVE) {
		return SAUNAFS_ERROR_ENOATTR;
	}

	if (xattrInodeEntry && xattrInodeEntry->anleng + anleng + 1 > SFS_XATTR_LIST_MAX) {
		return SAUNAFS_ERROR_ERANGE;
	}

	auto xattrDataEntry = std::make_unique<xattr_data_entry>();
	xattrDataEntry->inode = inode;
	xattrDataEntry->attributeName.resize(anleng);
	passert(xattrDataEntry->attributeName.data());
	memcpy(xattrDataEntry->attributeName.data(), attrname, anleng);
	xattrDataEntry->anleng = anleng;

	if (avleng > 0) {
		xattrDataEntry->attributeValue.resize(avleng);
		passert(xattrDataEntry->attributeValue.data());
		memcpy(xattrDataEntry->attributeValue.data(), attrvalue, avleng);
	} else {
		xattrDataEntry->attributeValue.clear();
	}

	xattrDataEntry->avleng = avleng;
	xattrDataEntry->checksum = 0;
	xattr_update_checksum(xattrDataEntry.get());

	gMetadata->xattr_data_hash[hash].push_back(std::move(xattrDataEntry));
	auto *xattrDataEntryPointer = gMetadata->xattr_data_hash[hash].back().get();

	if (xattrInodeEntry) {
		xattrInodeEntry->xattrDataList.push_back(xattrDataEntryPointer);
		xattrInodeEntry->anleng += anleng + 1U;
		xattrInodeEntry->avleng += avleng;
	} else {
		auto xattrInodeEntry = std::make_unique<xattr_inode_entry>();
		passert(xattrInodeEntry);
		xattrInodeEntry->inode = inode;
		xattrInodeEntry->xattrDataList.push_back(xattrDataEntryPointer);
		xattrInodeEntry->anleng = anleng + 1U;
		xattrInodeEntry->avleng = avleng;
		gMetadata->xattr_inode_hash[ihash].push_front(std::move(xattrInodeEntry));
	}

	return SAUNAFS_STATUS_OK;
}

uint8_t xattr_getattr(inode_t inode, uint8_t anleng, const uint8_t *attrname, uint32_t *avleng,
			uint8_t **attrvalue) {

	auto hash = xattr_data_hash_fn(inode, anleng, attrname);
	for (const auto &xattrDataEntry : gMetadata->xattr_data_hash[hash]) {
		if (xattrDataEntry->inode == inode && xattrDataEntry->anleng == anleng &&
		    memcmp(xattrDataEntry->attributeName.data(), attrname, anleng) == 0) {
			if (xattrDataEntry->avleng > SFS_XATTR_SIZE_MAX) {
				return SAUNAFS_ERROR_ERANGE;
			}
			*attrvalue = xattrDataEntry->attributeValue.data();
			*avleng = xattrDataEntry->avleng;
			return SAUNAFS_STATUS_OK;
		}
	}
	return SAUNAFS_ERROR_ENOATTR;
}

uint8_t xattr_listattr_leng(inode_t inode, void **xanode, uint32_t *xasize) {
	xattr_inode_entry *xattrInodeEntry;
	auto hash = xattr_inode_hash_fn(inode);
	auto start = gMetadata->xattr_inode_hash[hash].begin();
	auto end =  gMetadata->xattr_inode_hash[hash].end();

	for (auto attributeIterator = start; attributeIterator != end;) {
		xattrInodeEntry = attributeIterator->get();
		if (xattrInodeEntry->inode == inode) {
			*xanode = xattrInodeEntry;
			for (const auto &xattrDataEntry : xattrInodeEntry->xattrDataList) {
				*xasize += xattrDataEntry->anleng + 1U;
			}
			if (*xasize > SFS_XATTR_LIST_MAX) {
				return SAUNAFS_ERROR_ERANGE;
			}
			return SAUNAFS_STATUS_OK;
		}
	}

	*xanode = nullptr;
	return SAUNAFS_STATUS_OK;
}

void xattr_listattr_data(void *xattrInodeEntry, uint8_t *xabuff) {
	auto *xattrEntry = static_cast<xattr_inode_entry *>(xattrInodeEntry);

	uint32_t entryLength = 0;
	if (xattrEntry) {
		passert(xabuff);
		for (const auto &xattrDataEntry : xattrEntry->xattrDataList) {
			memcpy(xabuff + entryLength, xattrDataEntry->attributeName.data(), xattrDataEntry->anleng);
			entryLength += xattrDataEntry->anleng;
			xabuff[entryLength++] = 0;
		}
	}
}

