/*
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

#include "master/filesystem_node_types.h"

#if defined(SAUNAFS_HAVE_64BIT_JUDY) &&               \
    (!defined(DISABLE_JUDY_FOR_TRASHPATHCONTAINER) || \
     !defined(DISABLE_JUDY_FOR_RESERVEDPATHCONTAINER))
#include "common/judy_map.h"
#endif
#if !defined(SAUNAFS_HAVE_64BIT_JUDY) ||            \
    defined(DISABLE_JUDY_FOR_TRASHPATHCONTAINER) || \
    defined(DISABLE_JUDY_FOR_RESERVEDPATHCONTAINER)
#include <map>
#endif

// Maximum identifier value for trash or reserved names in the
// TrashReservedToIdContainer struct, the idea is to avoid using
// unsigned integer values with most significant bit set, as it
// is needed for offset querying for reserved/trash entries from
// the HandleIndexContainer consistently.
constexpr uint64_t kMaxIdForTrashOrReservedName = 0x7FFFFFFFFFFFFFFF;

// Sign bit mask for 64-bit unsigned integers. This is used to
// mark offsets with the sign bit off (bit 63 = 0) internally in the HandleIndexContainer,
// for consistent offset querying by lower_bound operations as client always provides
// offsets with sign bit off (bit 63 = 0) and keys space is limited to 63 bits only.
constexpr uint64_t k64SignBitMask = (1ULL << 63ULL);

using TrashtimeMap = std::unordered_map<uint32_t, uint32_t>;

struct TrashPathKey {
	explicit TrashPathKey(const FSNode *node)
	    :
#ifdef WORDS_BIGENDIAN
	      timestamp(std::min((uint64_t)node->ctime + node->trashtime, (uint64_t)UINT32_MAX)),
	      id(node->id)
#else
	      id(node->id),
	      timestamp(std::min((uint64_t)node->ctime + node->trashtime, (uint64_t)UINT32_MAX))
#endif
	{
	}

	TrashPathKey(uint32_t nodeId, uint32_t ctime, uint32_t trashtime)
	    :
#ifdef WORDS_BIGENDIAN
	      timestamp(std::min((uint64_t)ctime + trashtime, (uint64_t)UINT32_MAX)),
	      id(nodeId)
#else
	      id(nodeId),
	      timestamp(std::min((uint64_t)ctime + trashtime, (uint64_t)UINT32_MAX))
#endif
	{
	}

	bool operator<(const TrashPathKey &other) const {
		return std::make_pair(timestamp, id) < std::make_pair(other.timestamp, other.id);
	}

#ifdef WORDS_BIGENDIAN
	uint32_t timestamp;
	// inode_t id;
	uint32_t id;
#else
	// TODO(Guillex): the type should be inode_t, but there is an issue with Judy
	// inode_t id;
	uint32_t id;
	uint32_t timestamp;
#endif
};

#if defined(SAUNAFS_HAVE_64BIT_JUDY) && !defined(DISABLE_JUDY_FOR_TRASHPATHCONTAINER)
using TrashPathContainer = judy_map<TrashPathKey, hstorage::Handle>;
#else
using TrashPathContainer = std::map<TrashPathKey, hstorage::Handle>;
#endif

#if defined(SAUNAFS_HAVE_64BIT_JUDY) && !defined(DISABLE_JUDY_FOR_RESERVEDPATHCONTAINER)
using ReservedPathContainer = judy_map<inode_t, hstorage::Handle>;
#else
using ReservedPathContainer = std::map<inode_t, hstorage::Handle>;
#endif

/*! \brief Used as unique id generator for Trash/Reserved entries
 * This struct is used as global identifier generator for trash and reserved
 * path names handle hashes for ordering them in the HandleIndexContainer and
 * then performing lower_bound requests to that container using these identifiers as initial
 * lookup offsets.
 */
struct TrashReservedToIdContainer {
	// Key a handle hash to avoid collisions when the same name is used
	// for different inodes (which is possible, as names are not unique in trash/reserved)
	// Value is unique identifier for that handleHash
	// This unique identifier is used as part of the key in the HandleIndexContainer
	// for ordering and lower_bound queries.
	// TODO: Add mutex locking mechanism to make this struct thread-safe when
	// multi-master implementation is done.
	std::map<uint64_t, uint64_t> nameToId;
	uint64_t counter{0};

	uint64_t getOrAdd(uint64_t handle) {
		auto it = nameToId.find(handle);
		if (it != nameToId.end()) { return it->second; }
		uint64_t newId =
		    counter >= kMaxIdForTrashOrReservedName ? kMaxIdForTrashOrReservedName : ++counter;
		nameToId[handle] = newId;
		return newId;
	}

	void erase(uint64_t handle) { nameToId.erase(handle); }
};

struct HandleIndexKey {
	uint64_t data;

	HandleIndexKey(uint64_t d) : data(d) {}

	// For map lookup, you may want operator< or a custom comparator
	bool operator<(const HandleIndexKey &other) const {
		// Compare by last 63-bits from data field
		return (data & ~k64SignBitMask) < (other.data & ~k64SignBitMask);
	}
};

// Auxiliary index for Trash/Reserved entries, ordered by HandleIndexKey
// Used for readdir batching from meta mountpoints: lower_bound / upper_bound
// on Handle offset and retrieving the next N entries. Each handle maps to an inode_t.
using HandleIndexContainer = std::map<HandleIndexKey, inode_t>;

inline void addTrashEntry(TrashPathContainer &trash, HandleIndexContainer &trashHandlesIndex,
                          TrashReservedToIdContainer &trashReservedToId, FSNode *node,
                          const std::string &path) {
	TrashPathKey key(node);
	trash.insert({key, hstorage::Handle(path)});
	uint64_t handleHash = trash.at(key).data();
	uint64_t pathId = trashReservedToId.getOrAdd(handleHash);
	trashHandlesIndex.insert({HandleIndexKey(pathId), node->id});
}

inline void removeTrashEntryByKey(TrashPathContainer &trash,
                                  HandleIndexContainer &trashHandlesIndex,
                                  TrashReservedToIdContainer &trashReservedToId,
                                  const TrashPathKey &key) {
	auto trashIt = trash.find(key);
	if (trashIt == trash.end()) { return; }

	uint64_t handleHash = (*trashIt).second.data();
	auto pathIdIt = trashReservedToId.nameToId.find(handleHash);
	if (pathIdIt != trashReservedToId.nameToId.end()) {
		uint64_t pathId = pathIdIt->second;
		trashHandlesIndex.erase(HandleIndexKey(pathId));
		trashReservedToId.erase(handleHash);
	}
	trash.erase(trashIt);
}

inline void removeTrashEntry(TrashPathContainer &trash, HandleIndexContainer &trashHandlesIndex,
                             TrashReservedToIdContainer &trashReservedToId, FSNode *node) {
	TrashPathKey key(node);
	auto handleHashIt = trash.find(key);
	if (handleHashIt == trash.end()) { return; }

	uint64_t handleHash = (*handleHashIt).second.data();
	auto pathIdIt = trashReservedToId.nameToId.find(handleHash);
	if (pathIdIt == trashReservedToId.nameToId.end()) { return; }

	uint64_t pathId = pathIdIt->second;
	trashHandlesIndex.erase(HandleIndexKey(pathId));
	trashReservedToId.erase(handleHash);
	trash.erase(key);
}

inline void addReservedEntry(ReservedPathContainer &reserved,
                             HandleIndexContainer &reservedHandlesIndex,
                             TrashReservedToIdContainer &trashReservedToId, FSNode *node,
                             const std::string &path) {
	reserved.insert({node->id, hstorage::Handle(path)});
	uint64_t handleHash = reserved.at(node->id).data();
	uint64_t pathId = trashReservedToId.getOrAdd(handleHash);
	reservedHandlesIndex.insert({HandleIndexKey(pathId), node->id});
}

inline void removeReservedEntry(ReservedPathContainer &reserved,
                                HandleIndexContainer &reservedHandlesIndex,
                                TrashReservedToIdContainer &trashReservedToId, inode_t inode) {
	auto handleHashIt = reserved.find(inode);
	if (handleHashIt == reserved.end()) { return; }

	uint64_t handleHash = (*handleHashIt).second.data();
	auto pathIdIt = trashReservedToId.nameToId.find(handleHash);
	if (pathIdIt == trashReservedToId.nameToId.end()) { return; }

	uint64_t pathId = pathIdIt->second;
	reservedHandlesIndex.erase(HandleIndexKey(pathId));
	trashReservedToId.erase(handleHash);
	reserved.erase(inode);
}

inline void moveTrashToReservedEntry(TrashPathContainer &trash,
                                     HandleIndexContainer &trashHandlesIndex,
                                     ReservedPathContainer &reserved,
                                     HandleIndexContainer &reservedHandlesIndex,
                                     TrashReservedToIdContainer &trashReservedToId, FSNode *node) {
	TrashPathKey key(node);
	auto handleHashIt = trash.find(key);
	if (handleHashIt == trash.end()) { return; }

	uint64_t handleHash = (*handleHashIt).second.data();
	auto pathIdIt = trashReservedToId.nameToId.find(handleHash);
	if (pathIdIt == trashReservedToId.nameToId.end()) { return; }

	uint64_t idFromName = pathIdIt->second;

	trashHandlesIndex.erase(HandleIndexKey(idFromName));
	hstorage::Handle nameHandle = std::move(trash.at(key));
	trash.erase(key);

	reserved.insert({node->id, std::move(nameHandle)});
	reservedHandlesIndex.insert({HandleIndexKey(idFromName), node->id});
}

inline void updateTrashNameEntry(TrashPathContainer &trash, HandleIndexContainer &trashHandlesIndex,
                                 TrashReservedToIdContainer &trashReservedToId, FSNode *node,
                                 const std::string &newPath) {
	TrashPathKey key(node);
	auto handleHashIt = trash.find(key);
	if (handleHashIt == trash.end()) { return; }

	uint64_t oldHash = (*handleHashIt).second.data();
	auto oldPathIdIt = trashReservedToId.nameToId.find(oldHash);
	if (oldPathIdIt == trashReservedToId.nameToId.end()) { return; }

	uint64_t oldPathId = oldPathIdIt->second;
	trashHandlesIndex.erase(HandleIndexKey(oldPathId));
	trashReservedToId.erase(oldHash);

	trash[key] = HString(newPath);
	uint64_t newHash = trash[key].data();
	trashHandlesIndex.insert({HandleIndexKey(trashReservedToId.getOrAdd(newHash)), node->id});
}

inline void updateTrashFromOldEntry(TrashPathContainer &trash, FSNode *node,
                                    const TrashPathKey &oldKey) {
	auto handleHashIt = trash.find(oldKey);
	if (handleHashIt == trash.end()) { return; }

	hstorage::Handle path = std::move((*handleHashIt).second);
	trash.erase(oldKey);
	trash.insert({TrashPathKey(node), std::move(path)});
}
