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

static inline void xattr_removeentry(xattr_data_entry *xa) {
	*(xa->previnode) = xa->nextinode;
	if (xa->nextinode) {
		xa->nextinode->previnode = xa->previnode;
	}
	*(xa->prev) = xa->next;
	if (xa->next) {
		xa->next->prev = xa->prev;
	}
	if (gChecksumBackgroundUpdater.isXattrIncluded(xa)) {
		removeFromChecksum(gChecksumBackgroundUpdater.xattrChecksum, xa->checksum);
	}
	removeFromChecksum(gMetadata->xattrChecksum, xa->checksum);
	delete xa;
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
		for (xattr_data_entry *xde = gMetadata->xattr_data_hash[i]; xde; xde = xde->next) {
			xde->checksum = xattr_checksum(xde);
			addToChecksum(gMetadata->xattrChecksum, xde->checksum);
		}
	}
}

void xattr_removeinode(inode_t inode) {
	xattr_inode_entry *ih, **ihp;

	ihp = &(gMetadata->xattr_inode_hash[xattr_inode_hash_fn(inode)]);
	while ((ih = *ihp)) {
		if (ih->inode == inode) {
			while (ih->data_head) {
				xattr_removeentry(ih->data_head);
			}
			*ihp = ih->next;
			free(ih);
		} else {
			ihp = &(ih->next);
		}
	}
}

uint8_t xattr_setattr(inode_t inode, uint8_t anleng, const uint8_t *attrname, uint32_t avleng,
			const uint8_t *attrvalue, uint8_t mode) {
	xattr_inode_entry *ih;
	xattr_data_entry *xa;
	uint32_t hash, ihash;

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

	ihash = xattr_inode_hash_fn(inode);
	for (ih = gMetadata->xattr_inode_hash[ihash]; ih && ih->inode != inode; ih = ih->next) {
	}

	hash = xattr_data_hash_fn(inode, anleng, attrname);
	for (xa = gMetadata->xattr_data_hash[hash]; xa; xa = xa->next) {
		if (xa->inode == inode && xa->anleng == anleng &&
		    memcmp(xa->attributeName.data(), attrname, anleng) == 0) {
			passert(ih);
			if (mode == XATTR_SMODE_CREATE_ONLY) {  // create only
				return SAUNAFS_ERROR_EEXIST;
			}
			if (mode == XATTR_SMODE_REMOVE) {  // remove
				ih->anleng -= anleng + 1U;
				ih->avleng -= xa->avleng;
				xattr_removeentry(xa);
				if (ih->data_head == NULL) {
					if (ih->anleng != 0 || ih->avleng != 0) {
						safs_pretty_syslog(LOG_WARNING,
						       "xattr non zero lengths on remove "
						       "(inode:%" PRIiNode ",anleng:%" PRIu32
						       ",avleng:%" PRIu32 ")",
						       ih->inode, ih->anleng, ih->avleng);
					}
					xattr_removeinode(inode);
				}
				return SAUNAFS_STATUS_OK;
			}
			ih->avleng -= xa->avleng;
			if (!xa->attributeValue.empty()) {
				xa->attributeValue.clear();
			}
			if (avleng > 0) {
				xa->attributeValue.resize(avleng);
				passert(xa->attributeValue.data());
				memcpy(xa->attributeValue.data(), attrvalue, avleng);
			} else {
				xa->attributeValue.clear();
			}
			xa->avleng = avleng;
			ih->avleng += avleng;
			xattr_update_checksum(xa);
			return SAUNAFS_STATUS_OK;
		}
	}

	if (mode == XATTR_SMODE_REPLACE_ONLY || mode == XATTR_SMODE_REMOVE) {
		return SAUNAFS_ERROR_ENOATTR;
	}

	if (ih && ih->anleng + anleng + 1 > SFS_XATTR_LIST_MAX) {
		return SAUNAFS_ERROR_ERANGE;
	}

	xa = new xattr_data_entry;
	xa->inode = inode;
	xa->attributeName.resize(anleng);
	passert(xa->attributeName.data());
	memcpy(xa->attributeName.data(), attrname, anleng);
	xa->anleng = anleng;
	if (avleng > 0) {
		xa->attributeValue.resize(avleng);
		passert(xa->attributeValue.data());
		memcpy(xa->attributeValue.data(), attrvalue, avleng);
	} else {
		xa->attributeValue.clear();
	}
	xa->avleng = avleng;
	xa->next = gMetadata->xattr_data_hash[hash];
	if (xa->next) {
		xa->next->prev = &(xa->next);
	}
	xa->prev = gMetadata->xattr_data_hash + hash;
	gMetadata->xattr_data_hash[hash] = xa;

	if (ih) {
		xa->nextinode = ih->data_head;
		if (xa->nextinode) {
			xa->nextinode->previnode = &(xa->nextinode);
		}
		xa->previnode = &(ih->data_head);
		ih->data_head = xa;
		ih->anleng += anleng + 1U;
		ih->avleng += avleng;
	} else {
		ih = (xattr_inode_entry *)malloc(sizeof(xattr_inode_entry));
		passert(ih);
		ih->inode = inode;
		xa->nextinode = NULL;
		xa->previnode = &(ih->data_head);
		ih->data_head = xa;
		ih->anleng = anleng + 1U;
		ih->avleng = avleng;
		ih->next = gMetadata->xattr_inode_hash[ihash];
		gMetadata->xattr_inode_hash[ihash] = ih;
	}
	xa->checksum = 0;
	xattr_update_checksum(xa);
	return SAUNAFS_STATUS_OK;
}

uint8_t xattr_getattr(inode_t inode, uint8_t anleng, const uint8_t *attrname, uint32_t *avleng,
			uint8_t **attrvalue) {
	xattr_data_entry *xa;

	for (xa = gMetadata->xattr_data_hash[xattr_data_hash_fn(inode, anleng, attrname)]; xa;
	     xa = xa->next) {
		if (xa->inode == inode && xa->anleng == anleng &&
		    memcmp(xa->attributeName.data(), attrname, anleng) == 0) {
			if (xa->avleng > SFS_XATTR_SIZE_MAX) {
				return SAUNAFS_ERROR_ERANGE;
			}
			*attrvalue = xa->attributeValue.data();
			*avleng = xa->avleng;
			return SAUNAFS_STATUS_OK;
		}
	}
	return SAUNAFS_ERROR_ENOATTR;
}

uint8_t xattr_listattr_leng(inode_t inode, void **xanode, uint32_t *xasize) {
	xattr_inode_entry *ih;
	xattr_data_entry *xa;

	for (ih = gMetadata->xattr_inode_hash[xattr_inode_hash_fn(inode)]; ih; ih = ih->next) {
		if (ih->inode == inode) {
			*xanode = ih;
			for (xa = ih->data_head; xa; xa = xa->nextinode) {
				*xasize += xa->anleng + 1U;
			}
			if (*xasize > SFS_XATTR_LIST_MAX) {
				return SAUNAFS_ERROR_ERANGE;
			}
			return SAUNAFS_STATUS_OK;
		}
	}
	*xanode = NULL;
	return SAUNAFS_STATUS_OK;
}

void xattr_listattr_data(void *xanode, uint8_t *xabuff) {
	xattr_inode_entry *ih = (xattr_inode_entry *)xanode;
	xattr_data_entry *xa;
	uint32_t l;

	l = 0;
	if (ih) {
		for (xa = ih->data_head; xa; xa = xa->nextinode) {
			memcpy(xabuff + l, xa->attributeName.data(), xa->anleng);
			l += xa->anleng;
			xabuff[l++] = 0;
		}
	}
}

