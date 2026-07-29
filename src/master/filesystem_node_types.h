/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2016 Skytechnology sp. z o.o.
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

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

#include "common/compact_vector.h"
#include "common/datapack.h"
#include "common/goal.h"
#include "common/serializable_interface.h"
#include "common/skip_list.h"
#include "common/type_defs.h"
#include "protocol/SFSCommunication.h"
#include "slogger/slogger.h"

#include "master/hstring_storage.h"

#define NODEHASHBITS (22)
#define NODEHASHSIZE (1 << NODEHASHBITS)
#define NODEHASHPOS(nodeid) ((nodeid) & (NODEHASHSIZE - 1))
#define NODECHECKSUMSEED 12345

#define EDGEHASHBITS (22)
#define EDGEHASHSIZE (1 << EDGEHASHBITS)
#define EDGEHASHPOS(hash) ((hash) & (EDGEHASHSIZE - 1))
#define EDGECHECKSUMSEED 1231241261

constexpr uint32_t kMaxChunkIndex = 0x7FFFFFFFU;

enum class AclInheritance : std::uint8_t {
	kInheritAcl,
	kDontInheritAcl
};

// Arguments for verify_session
enum class SessionType : std::uint8_t {
	kNotMeta,
	kOnlyMeta,
	kAny
};

enum class OperationMode : std::uint8_t {
	kReadWrite,
	kReadOnly
};

enum class ExpectedNodeType : std::uint8_t {
	kFile,
	kDirectory,
	kNotDirectory,
	kFileOrDirectory,
	kAny
};

using GoalStatistics = std::array<inode_t, GoalId::kMax + 1>;
using ParentsCompactVector = compact_vector<std::pair<inode_t, const hstorage::Handle *>, uint32_t>;

struct StatsRecord {
	inode_t inodes;
	inode_t dirs;
	inode_t files;
	inode_t links;
	uint32_t chunks;
	uint64_t length;
	uint64_t size;
	uint64_t realsize;
};

enum class FSNodeType : std::uint8_t {
	kFile = TYPE_FILE,
	kDirectory = TYPE_DIRECTORY,
	kSymlink = TYPE_SYMLINK,
	kFifo = TYPE_FIFO,
	kBlockDev = TYPE_BLOCKDEV,
	kCharDev = TYPE_CHARDEV,
	kSocket = TYPE_SOCKET,
	kTrash = TYPE_TRASH,
	kReserved = TYPE_RESERVED,
	kUnknown = TYPE_UNKNOWN
};

/*! \brief Node containing common meta data for each file system object (file or directory).
 *
 * Node size = 64B
 *
 * Estimating (taking into account directory and file node size) 150B per file.
 *
 * 10K files will occupy 1.5MB
 * 10M files will occupy 1.5GB
 * 1G files will occupy 150GB
 * 4G files will occupy 600GB
 */
class FSNode : public ISerializable {
public:
	inode_t id{};       ///< Unique number identifying node.
	uint32_t ctime{};   ///< Change time.
	uint32_t mtime{};   ///< Modification time.
	uint32_t atime{};   ///< Access time.
	FSNodeType type{};  ///< Node type. (file, directory, symlink, ...)
	uint8_t goal{};     ///< Goal id.

	/// Only 12 lowest bits are used for mode, in unix standard upper 4 are used for object type,
	/// but since there is field "type" these bits can be used as extra flags.
	uint16_t mode{};
	uint32_t uid{};        ///< User id.
	uint32_t gid{};        ///< Group id.
	uint32_t trashtime{};  ///< Trash time.

	/// Parent nodes ids + handles of entries of this node in those parents.
	/// To reduce memory usage ids are stored instead of pointers to FSNode.
	ParentsCompactVector parents;

	uint64_t checksum{};  ///< Node checksum.

	static constexpr size_t kNodeHeaderSize =
	    sizeof(type) + sizeof(id) + sizeof(goal) + sizeof(mode) + sizeof(uid) + sizeof(gid) +
	    sizeof(atime) + sizeof(mtime) + sizeof(ctime) + sizeof(trashtime);

	static constexpr uint16_t kEdgeNameMaxSize = 65535;
	static constexpr uint8_t kEdgeHeaderSize =
	    sizeof(FSNode::id) + sizeof(FSNode::id) + sizeof(kEdgeNameMaxSize);

	explicit FSNode(FSNodeType type_) : type(type_) {}

	/*! \brief Static function used for creating proper node for given type.
	 * \param type Type of node to create.
	 * \return Pointer to created node.
	 */
	static FSNode *create(FSNodeType type);

	/*! \brief Static function used for erasing node (uses node's type
	 * for correct invocation of destructors).
	 *
	 * \param node Pointer to node that should be erased.
	 */
	static void destroy(FSNode *node);

	/// Serializes the common data of FSNode
	void serializeCommon(uint8_t **destination) const {
		FSNode::serialize(destination);
	}

	// Serializable interface

	/// Returns the size of the serialized object in bytes.
	size_t serializedSize() const override { return kNodeHeaderSize; }

	/// Serializes the object into the provided destination buffer.
	/// The function assumes that the destination buffer has enough space allocated.
	/// Use serializedSize in the caller to ensure sufficient space.
	void serialize(uint8_t **destination) const override {
		put8bit(destination, static_cast<uint8_t>(type));
		putINode(destination, id);
		put8bit(destination, goal);
		put16bit(destination, mode);
		put32bit(destination, uid);
		put32bit(destination, gid);
		put32bit(destination, atime);
		put32bit(destination, mtime);
		put32bit(destination, ctime);
		put32bit(destination, trashtime);
	}

	/// Deserializes the object from the provided source buffer.
	void deserialize(const uint8_t **source) override {
		uint8_t typeTemp{};
		typeTemp = get8bit(source);
		type = static_cast<FSNodeType>(typeTemp);
		getINode(source, id);
		goal = get8bit(source);
		mode = get16bit(source);
		get32bit(source, uid);
		get32bit(source, gid);
		get32bit(source, atime);
		get32bit(source, mtime);
		get32bit(source, ctime);
		get32bit(source, trashtime);
	}
};

// Sentinel for a failed node lookup (e.g. idToNode, lookup).
constexpr FSNode *kNodeNotFound = nullptr;
// Placeholder used as lower-bound key in sorted node containers
// (paired with a handle to construct a search key for lower_bound).
constexpr FSNode *kUnknownNode = nullptr;

/*! \brief Node used for storing file object.
 *
 * Node size = 64B + 40B + 8 * chunks_count + 4 * session_count
 * Avg size (assuming 1 chunk and session id) = 104 + 8 + 4 ~ 116B
 */
class FSNodeFile : public FSNode {
public:
	uint64_t length{};
	compact_vector<uint32_t> sessionIds;
	compact_vector<uint64_t, uint32_t> chunks;

	// Constants

	static constexpr uint16_t kMaxSessionSize = 65535;
	static constexpr uint32_t kChunksBucketSize = 65536;

	/// Extra fixed size in the header
	using chunk_count_t = uint32_t;
	static constexpr size_t kFileExtraHeaderSize =
	    sizeof(FSNodeFile::length) + sizeof(chunk_count_t) + sizeof(kMaxSessionSize);

	/// Full header size, including the FSNode members
	static constexpr size_t kFileHeaderSize = FSNode::kNodeHeaderSize + kFileExtraHeaderSize;

	/// Maximum extra size of the serialized object
	static constexpr size_t kFileMaxExtraBufferSize =
	    (sizeof(uint64_t) * kChunksBucketSize) + (sizeof(uint32_t) * kMaxSessionSize);

	/// Maximum buffer size for serialized objects
	static constexpr size_t kMaxBufferSize = kFileHeaderSize + kFileMaxExtraBufferSize;

	explicit FSNodeFile(FSNodeType type_) : FSNode(type_) {
		assert(type_ == FSNodeType::kFile || type_ == FSNodeType::kTrash ||
		       type_ == FSNodeType::kReserved);
	}

	uint32_t chunkCount() const {
		for (uint32_t i = chunks.size(); i > 0; --i) {
			if (chunks[i - 1] != 0) { return i; }
		}

		return 0;
	}

	// Serializable interface

	/// Returns the size of the serialized object in bytes.
	size_t serializedSize() const override {
		auto sessionsCount = std::min<size_t>(sessionIds.size(), kMaxSessionSize);
		return kFileHeaderSize + (sizeof(uint64_t) * chunkCount()) +
		       (sizeof(uint32_t) * sessionsCount);
	}

	/// Serializes the object into the provided destination buffer.
	/// The function assumes that the destination buffer has enough space allocated.
	/// Use serializedSize in the caller to ensure sufficient space.
	void serialize(uint8_t **destination) const override {
		FSNode::serialize(destination);

		put64bit(destination, length);
		auto chunkAmount = chunkCount();
		put32bit(destination, chunkAmount);
		auto sessionsCount = std::min<int>(sessionIds.size(), kMaxSessionSize);
		put16bit(destination, sessionsCount);

		for (size_t i = 0; i < chunkAmount; ++i) { put64bit(destination, chunks[i]); }

		for (int i = 0; i < sessionsCount; ++i) { put32bit(destination, sessionIds[i]); }
	}

	/// Deserializes the object from the provided source buffer.
	void deserialize(const uint8_t **source) override {
		FSNode::deserialize(source);
		length = get64bit(source);
		uint32_t chunkAmount{};
		get32bit(source, chunkAmount);
		auto sessionsAmount = get16bit(source);

		// Chunks

		chunks.resize(chunkAmount);

		for (size_t i = 0; i < chunkAmount; ++i) { chunks[i] = get64bit(source); }

		// Sessions

		sessionIds.resize(sessionsAmount);

		for (size_t i = 0; i < sessionsAmount; ++i) { get32bit(source, sessionIds[i]); }
	}
};

/*! \brief Node used for storing symbolic link.
 *
 * Node size = 64 + 16 = 80B
 */
class FSNodeSymlink : public FSNode {
public:
	hstorage::Handle path;
	uint16_t path_length{};

	// It is stored as uint32_t
	using serialized_path_length_t = uint32_t;

	static constexpr size_t kSymlinkHeaderSize =
	    FSNode::kNodeHeaderSize + sizeof(serialized_path_length_t);

	explicit FSNodeSymlink() : FSNode(FSNodeType::kSymlink) {}

	// Serializable interface

	/// Returns the size of the serialized object in bytes.
	size_t serializedSize() const override { return kSymlinkHeaderSize + path_length; }

	/// Serializes the object into the provided destination buffer.
	/// The function assumes that the destination buffer has enough space allocated.
	/// Use serializedSize in the caller to ensure sufficient space.
	void serialize(uint8_t **destination) const override {
		FSNode::serialize(destination);
		HString::serialize(destination, path.get());
	}

	/// Deserializes the object from the provided source buffer.
	void deserialize(const uint8_t **source) override {
		FSNode::deserialize(source);

		HString pathString;
		HString::deserialize(source, pathString);
		path = hstorage::Handle(pathString);
		path_length = static_cast<uint16_t>(pathString.length());
	}
};

/*! \brief Node used for storing device object.
 *
 * Node size = 64 + 8 = 72B
 */
class FSNodeDevice : public FSNode {
public:
	uint32_t rdev{};

	static constexpr size_t kDeviceHeaderSize = FSNode::kNodeHeaderSize + sizeof(rdev);

	FSNodeDevice(FSNodeType device_type) : FSNode(device_type) {}

	// Serializable interface

	/// Returns the size of the serialized object in bytes.
	size_t serializedSize() const override { return kDeviceHeaderSize; }

	/// Serializes the object into the provided destination buffer.
	/// The function assumes that the destination buffer has enough space allocated.
	/// Use serializedSize in the caller to ensure sufficient space.
	void serialize(uint8_t **destination) const override {
		FSNode::serialize(destination);
		put32bit(destination, rdev);
	}

	/// Deserializes the object from the provided source buffer.
	void deserialize(const uint8_t **source) override {
		FSNode::deserialize(source);
		get32bit(source, rdev);
	}
};

/*! \brief Node used for storing directory.
 *
 */
class FSNodeDirectory : public FSNode {
public:
	struct HandleCompare {
		bool operator()(const std::pair<hstorage::Handle *, FSNode *> &a,
		                const std::pair<hstorage::Handle *, FSNode *> &b) const {
			return std::make_pair(a.first->data(), a.second) <
			       std::make_pair(b.first->data(), b.second);
		}
	};

	/// These parameters achieve good results in reducing memory consumption
	/// Numerator of non-promotion probability P
	static constexpr uint32_t kDefaultSkipListProbNum = 3;
	/// Denominator of non-promotion probability P
	static constexpr uint32_t kDefaultSkipListProbDen = 4; 

	using EntriesContainer = SkipList<std::pair<hstorage::Handle *, FSNode *>, HandleCompare,
	                                  kDefaultSkipListProbNum, kDefaultSkipListProbDen>;

	using iterator = EntriesContainer::iterator;
	using const_iterator = EntriesContainer::const_iterator;

	EntriesContainer entries;  ///< Directory entries (entry: name + pointer to child node).
	/// Directory entries with lower case name (entry: name + pointer to child node).
	EntriesContainer lowerCaseEntries;
	bool caseInsensitive = false;  ///< Flag for case insensitive search.
	StatsRecord stats;              ///< Directory statistics (including subdirectories).
	uint32_t nlink{2};              ///< Number of directories linking to this directory.
	uint16_t entries_hash{};

	explicit FSNodeDirectory() : FSNode(FSNodeType::kDirectory) {
		memset(&stats, 0, sizeof(stats));
	}

	/*! \brief Find directory entry with given name.
	 *
	 * \param name Name of entry to find.
	 * \return If node is found returns iterator pointing to directory entry containing node,
	 *         otherwise entries.end().
	 */
	iterator find(const HString &name) {
		uint64_t name_hash = (hstorage::Handle::HashType)name.hash();

		if (caseInsensitive) {
			HString lowerCaseNameHandle = HString::hstringToLowerCase(name);
			name_hash = (hstorage::Handle::HashType)lowerCaseNameHandle.hash();
			auto tmp_handle = hstorage::Handle(name_hash << hstorage::Handle::kHashShift);
			auto pair_to_find = std::make_pair(&tmp_handle, kUnknownNode);
			auto lowerCaseIt = lowerCaseEntries.lower_bound(pair_to_find);

			for (; lowerCaseIt != lowerCaseEntries.end(); ++lowerCaseIt) {
				if ((*lowerCaseIt).first->hash() != name_hash) { break; }
				if (*((*lowerCaseIt).first) == lowerCaseNameHandle) {
					auto it = find((*lowerCaseIt).second);
					return it;
				}
			}
		} else {
			auto tmp_handle = hstorage::Handle(name_hash << hstorage::Handle::kHashShift);
			auto pair_to_find = std::make_pair(&tmp_handle, kUnknownNode);
			auto it = entries.lower_bound(pair_to_find);

			for (; it != entries.end(); ++it) {
				if ((*it).first->hash() != name_hash) { break; }
				if (*((*it).first) == name) { return it; }
			}
		}

		return entries.end();
	}

	iterator find_lowercase_container(const HString &name) {
		uint64_t name_hash = (hstorage::Handle::HashType)name.hash();

		if (caseInsensitive) {
			HString lowerCaseNameHandle = HString::hstringToLowerCase(name);
			name_hash = (hstorage::Handle::HashType)lowerCaseNameHandle.hash();
			auto tmp_handle = hstorage::Handle(name_hash << hstorage::Handle::kHashShift);
			auto pair_to_find = std::make_pair(&tmp_handle, kUnknownNode);
			auto lowerCaseIt = lowerCaseEntries.lower_bound(pair_to_find);

			for (; lowerCaseIt != lowerCaseEntries.end(); ++lowerCaseIt) {
				if ((*lowerCaseIt).first->hash() != name_hash) { break; }
				if (*((*lowerCaseIt).first) == lowerCaseNameHandle) { return lowerCaseIt; }
			}
		}

		return lowerCaseEntries.end();
	}

	/*! \brief Find directory entry with given node.
	 *
	 * \param node Node to find.
	 * \return If node is found returns iterator pointing to directory entry containing node,
	 *         otherwise entries.end().
	 */
	iterator find(const FSNode *node) {
		HString name = HString(getChildName(node));
		uint64_t name_hash = (hstorage::Handle::HashType)name.hash();
		auto tmp_handle = hstorage::Handle(name_hash << hstorage::Handle::kHashShift);
		auto pair_to_find = std::make_pair(&tmp_handle, kUnknownNode);
		auto it = entries.lower_bound(pair_to_find);
		for (; it != entries.end(); ++it) {
			if ((*it).first->hash() != name_hash) { break; }
			if (*((*it).first) == name) { return it; }
		}
		return entries.end();
	}

	iterator find_nth(EntriesContainer::size_type nth) { return entries.find_by_order(nth); }

	const_iterator find_nth(EntriesContainer::size_type nth) const {
		return entries.find_by_order(nth);
	}

	/*! \brief Returns name for specified node.
	 *
	 * \param node Pointer to node.
	 * \return If node is found returns name associated with this node,
	 *         otherwise returns empty string.
	 */
	std::string getChildName(const FSNode *node) const {
		for (const auto &[parentId, hstring] : node->parents) {
			if (parentId == this->id) { return hstring ? hstring->get() : std::string{}; }
		}
		return {};
	}

	/*! \brief Returns the original stored child name for any-case input.
	 *
	 * This function is only meaningful for directories with the case-insensitive flag set.
	 * If the directory is not case-insensitive, it will always return an empty string.
	 *
	 * The function works as follows:
	 * - It takes any-case input (e.g., "FoO").
	 * - It converts the input to lower case and looks up the corresponding entry in
	 *   the lowerCaseEntries container (e.g., "foo").
	 * - If a match is found, it returns the original stored name as it was first added
	 *   to the parent directory (e.g., returns "Foo" if that was the original casing).
	 * - If no match is found, or the directory is not case-insensitive, it returns an empty string.
	 *
	 * Example:
	 *   - Directory contains an entry stored as "Foo".
	 *   - getBaseStoredChildName("FoO") will return "Foo".
	 *
	 * \param anyCaseName Any-case name to look up.
	 * \return The original stored child name (with original casing), or empty string if not found.
	 */
	std::string getBaseStoredChildName(const std::string &anyCaseName) {
		if (!caseInsensitive) { return {}; }

		// caseInsensitive path
		HString inputH(anyCaseName);
		HString lowerCaseNameHandle = HString::hstringToLowerCase(inputH);
		uint64_t lowerCaseHash = (hstorage::Handle::HashType)lowerCaseNameHandle.hash();
		hstorage::Handle tmp(lowerCaseHash << hstorage::Handle::kHashShift);
		auto pairToFind = std::make_pair(&tmp, kUnknownNode);
		auto lowerIt = lowerCaseEntries.lower_bound(pairToFind);

		for (; lowerIt != lowerCaseEntries.end(); ++lowerIt) {
			if ((*lowerIt).first->hash() != lowerCaseHash) { break; }
			if (*((*lowerIt).first) == lowerCaseNameHandle) {
				FSNode *child = (*lowerIt).second;
				if (!child) { return {}; }
				// ORIGINAL name from parents vector (first stored casing).
				for (const auto &[parentId, nameHandle] : child->parents) {
					if (parentId != this->id) { continue; }
					if (nameHandle == nullptr) { return {}; }
					return static_cast<std::string>(*nameHandle);
				}

				// Fallback: scan entries for pointer equality (should rarely be needed).
				for (auto it = entries.begin(); it != entries.end(); ++it) {
					if ((*it).second == child) {
						safs::log_warn(
						    "Inconsistent filesystem tree: fallback used in getBaseStoredChildName for directory id {}",
						    this->id);
						return std::string((*it).first->get());
					}
				}
				return {};
			}
		}
		return {};
	}

	/*!
	 * \brief Updates lowerCaseEntries for all entries according to caseInsensitive flag.
	 *
	 * For case-insensitive directories, this populates lowerCaseEntries so that
	 * lookups can be performed in a case-insensitive manner. If multiple entries
	 * exist whose names differ only by case (e.g., "foo" and "Foo"), only one of
	 * them will be present in lowerCaseEntries after this operation; the first one
	 * encountered during iteration. This means only one will be found by a
	 * case-insensitive lookup, and others will be hidden.
	 */
	void updateLowerCaseEntries() {
		if (!caseInsensitive) {
			// Clear and free all lowerCaseEntries
			for (auto it = lowerCaseEntries.begin(); it != lowerCaseEntries.end();) {
				delete it->first;
				auto entryToErase = it++;
				lowerCaseEntries.erase(entryToErase);
			}

			lowerCaseEntries.clear();
			return;
		}

		// Populate lowerCaseEntries for all entries in entries
		for (const auto &entry : entries) {
			FSNode *child = entry.second;
			HString lowerCaseName = HString::hstringToLowerCase(entry.first->get());

			// Reuse find_lowercase_container to check if already present
			auto lowerCaseIt = find_lowercase_container(lowerCaseName);
			bool found = (lowerCaseIt != lowerCaseEntries.end());

			if (!found) {
				auto *lowercaseHandlePtr = new hstorage::Handle(lowerCaseName);
				lowerCaseEntries.insert({lowercaseHandlePtr, child});
			}
		}
	}

	iterator begin() { return entries.begin(); }

	iterator end() { return entries.end(); }

	const_iterator begin() const { return entries.begin(); }

	const_iterator end() const { return entries.end(); }

	// Serializable interface

	/// Returns the size of the serialized object in bytes.
	size_t serializedSize() const override { return kNodeHeaderSize; }

	/// Serializes the object into the provided destination buffer.
	/// The function assumes that the destination buffer has enough space allocated.
	/// Use serializedSize in the caller to ensure sufficient space.
	void serialize(uint8_t **destination) const override {
		FSNode::serialize(destination);
	}

	/// Deserializes the object from the provided source buffer.
	void deserialize(const uint8_t **source) override {
		FSNode::deserialize(source);
	}
};
