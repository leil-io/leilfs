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

#pragma once

#include "common/platform.h"

#include <cstdint>
#include <cstdlib>
#include <vector>

#define XATTR_INODE_HASH_SIZE 65536
#define XATTR_DATA_HASH_SIZE 524288
#define XATTRCHECKSUMSEED 29857986791741783ULL

struct xattr_data_entry {
	inode_t inode;
	uint8_t anleng;
	uint32_t avleng;
	std::vector<uint8_t> attributeName;
	std::vector<uint8_t> attributeValue;
	uint64_t checksum;
	struct xattr_data_entry **previnode, *nextinode;
	struct xattr_data_entry **prev, *next;

	xattr_data_entry() = default;

	~xattr_data_entry() {
		attributeName.clear();
		attributeValue.clear();
	}
};
void free(xattr_data_entry *);  // disable freeing using free at link time :)

struct xattr_inode_entry {
	inode_t inode;
	uint32_t anleng;
	uint32_t avleng;
	struct xattr_data_entry *data_head;
	struct xattr_inode_entry *next;
};

#ifndef METARESTORE
static inline int xattr_namecheck(uint8_t anleng, const uint8_t *attrname) {
	uint32_t i;
	for (i = 0; i < anleng; i++) {
		if (attrname[i] == '\0') {
			return -1;
		}
	}
	return 0;
}
#endif /* METARESTORE */

static inline uint32_t xattr_data_hash_fn(inode_t inode, uint8_t anleng, const uint8_t *attrname) {
	uint32_t hash = inode * 5381U;
	while (anleng) {
		hash = (hash * 33U) + (*attrname);
		attrname++;
		anleng--;
	}
	return (hash & (XATTR_DATA_HASH_SIZE - 1));
}

static inline uint32_t xattr_inode_hash_fn(inode_t inode) {
	return ((inode * 0x72B5F387U) & (XATTR_INODE_HASH_SIZE - 1));
}

void xattr_checksum_add_to_background(xattr_data_entry *xde);
void xattr_listattr_data(void *xanode, uint8_t *xabuff);
void xattr_recalculate_checksum();
void xattr_removeinode(inode_t inode);

uint8_t xattr_getattr(inode_t inode, uint8_t anleng, const uint8_t *attrname, uint32_t *avleng,
			uint8_t **attrvalue);
uint8_t xattr_listattr_leng(inode_t inode, void **xanode, uint32_t *xasize);
uint8_t xattr_setattr(inode_t inode, uint8_t anleng, const uint8_t *attrname, uint32_t avleng,
			const uint8_t *attrvalue, uint8_t mode);
