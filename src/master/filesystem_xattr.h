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
#include <memory>
#include <vector>

#include "common/massert.h"
#include "common/observable_property.h"
#include "common/type_defs.h"

#define XATTR_INODE_HASH_SIZE 65536
#define XATTR_DATA_HASH_SIZE 524288
#define XATTRCHECKSUMSEED 29857986791741783ULL

struct XAttributeDataEntry {
	inode_t inode;
	std::vector<uint8_t> attributeName;
	std::vector<uint8_t> attributeValue;
	uint64_t checksum;

	XAttributeDataEntry() = default;
	~XAttributeDataEntry() = default;
};

struct XAttributeInodeEntry {
	inode_t inode;
	uint32_t attributeNameLength;
	uint32_t attributeValueLength;
	std::vector<XAttributeDataEntry *> xattrDataEntries;

	static std::unique_ptr<XAttributeInodeEntry> create(inode_t inode, uint32_t attributeNameLength,
	                                                    uint32_t attributeValueLength) {
		auto entry = std::make_unique<XAttributeInodeEntry>();
		passert(entry);
		entry->inode = inode;
		entry->attributeNameLength = attributeNameLength;
		entry->attributeValueLength = attributeValueLength;
		return entry;
	}
};

#ifndef METARESTORE
static inline int xattr_namecheck(uint8_t attributeNameLength, const uint8_t *attributeName) {
	for (uint32_t i = 0; i < attributeNameLength; i++) {
		if (attributeName[i] == '\0') {
			return -1;
		}
	}
	return 0;
}
#endif /* METARESTORE */

static inline uint32_t get_xattr_data_hash(inode_t inode, uint8_t attributeNameLength,
                                           const uint8_t *attributeName) {
	uint32_t hash = inode * 5381U;
	while (attributeNameLength) {
		hash = (hash * 33U) + (*attributeName);
		attributeName++;
		attributeNameLength--;
	}
	return (hash & (XATTR_DATA_HASH_SIZE - 1));
}

static constexpr uint32_t get_xattr_inode_hash(inode_t inode) {
	return ((inode * 0x72B5F387U) & (XATTR_INODE_HASH_SIZE - 1));
}

XAttributeInodeEntry *find_xattr_inode_entry(inode_t inode, uint32_t inodeHash);

void xattr_checksum_add_to_background(XAttributeDataEntry *xattrDataEntry);
void xattr_listattr_data(void *xattrInodeEntry, uint8_t *xattrDataBuffer);
void xattr_recalculate_checksum();
void xattr_removeinode(inode_t inode);

uint8_t xattr_getattr(inode_t inode, uint8_t attributeNameLength, const uint8_t *attributeName,
                      uint32_t *attributeValueLength, uint8_t **attributeValue);

uint8_t get_xattrs_length_for_inode(inode_t inode, void **xattrInodePointer, uint32_t *xattrSize);

uint8_t xattr_setattr(inode_t inode, uint8_t attributeNameLength, const uint8_t *attributeName,
                      uint32_t attributeValueLength, const uint8_t *attributeValueBuffer,
                      uint8_t mode);

/// Signal emitted when all xattrs for an inode are removed (node deletion cleanup).
inline Signal<inode_t> gXAttrInodeRemovedSignal;

/// Signal emitted when an xattr is created or its value is updated.
/// Parameters: inode, attributeName (as vector<uint8_t>), attributeValue (as vector<uint8_t>).
inline Signal<inode_t, std::vector<uint8_t>, std::vector<uint8_t>> gXAttrChangedSignal;

/// Signal emitted when a single xattr entry is removed.
/// Parameters: inode, attributeName (as vector<uint8_t>).
inline Signal<inode_t, std::vector<uint8_t>> gXAttrRemovedSignal;
