/*
   Copyright 2023      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include "master/filesystem_node_types.h"

#include <gtest/gtest.h>
#include <sys/types.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "common/datapack.h"
#include "master/hstring_memstorage.h"

using namespace hstorage;

class FilesystemNodeTypesTest : public ::testing::Test {
protected:
	void SetUp() override {
		Storage::reset(new MemStorage());
		EXPECT_EQ(Storage::instance().name(), "MemStorage");
	}

	void TearDown() override {
		for (auto *node : nodes) { FSNode::destroy(node); }

		nodes.clear();
	}

	static bool areEqual(FSNode *first, FSNode *second) {
		if (first->type != second->type) { return false; }
		if (first->id != second->id) { return false; }
		if (first->ctime != second->ctime) { return false; }
		if (first->mtime != second->mtime) { return false; }
		if (first->atime != second->atime) { return false; }
		if (first->goal != second->goal) { return false; }
		if (first->mode != second->mode) { return false; }
		if (first->uid != second->uid) { return false; }
		if (first->gid != second->gid) { return false; }
		if (first->trashtime != second->trashtime) { return false; }

		return true;
	}

	static FSNode *createFSNode(int index, FSNodeType type) {
		FSNode *node = FSNode::create(type);
		node->id = index;
		node->ctime = kRandomTime + index;
		node->mtime = kRandomTime + index;
		node->atime = kRandomTime + index;
		node->goal = kRandomGoal + index;
		node->mode = kRandomMode;
		node->uid = kRandomUid + index;
		node->gid = kRandomGid + index;
		node->trashtime = kRandomTrashTime + index;

		return node;
	}

	static constexpr uint32_t kRandomTime = 12345U;
	static constexpr uint8_t kRandomGoal = 1U;
	static constexpr uint16_t kRandomMode = 0755U;
	static constexpr uint32_t kRandomUid = 1000U;
	static constexpr uint32_t kRandomGid = 1000U;
	static constexpr uint32_t kRandomTrashTime = 3600U;

	std::vector<FSNode *> nodes;

	static constexpr uint32_t kMaxSize =
	    FSNodeFile::kFileHeaderSize + FSNodeFile::kFileMaxExtraBufferSize;

	std::array<uint8_t, kMaxSize> serializeBuffer;
};

static_assert(sizeof(FSNodeFile) == 104,
              "KV-only chunk-table bookkeeping must not grow in-memory file nodes");

TEST_F(FilesystemNodeTypesTest, SerializeFSNodeDirectory) {
	constexpr int finalNode = 12;

	for (int i = 2; i < finalNode; ++i) {
		FSNode *node = createFSNode(i, FSNodeType::kDirectory);
		nodes.push_back(node);
	}

	uint8_t *buffer = serializeBuffer.data();

	for (auto *node : nodes) {
		node->serialize(&buffer);

		// Rewind the buffer to the beginning and get the serialized type
		buffer = serializeBuffer.data();
		const uint8_t *readPtrForType = buffer;
		auto type = static_cast<FSNodeType>(get8bit(&readPtrForType));

		// Create a new FSNode of the deserialized type
		const uint8_t *readPtr = buffer;
		auto *deserializedNode = FSNode::create(type);
		deserializedNode->deserialize(&readPtr);

		ASSERT_TRUE(areEqual(node, deserializedNode));
		FSNode::destroy(deserializedNode);
	}
}

TEST_F(FilesystemNodeTypesTest, SerializeFSNodeSymlink) {
	constexpr int finalNode = 12;

	for (int i = 2; i < finalNode; ++i) {
		FSNode *node = createFSNode(i, FSNodeType::kSymlink);

		auto *symlinkNode = static_cast<FSNodeSymlink *>(node);
		std::string target = "/path/to/target_" + std::to_string(i);
		symlinkNode->path = HString(target);
		symlinkNode->path_length = target.length();

		nodes.push_back(node);
	}

	uint8_t *buffer = serializeBuffer.data();

	for (auto *node : nodes) {
		node->serialize(&buffer);

		// Rewind the buffer to the beginning and get the serialized type
		buffer = serializeBuffer.data();
		const uint8_t *readPtrForType = buffer;
		auto type = static_cast<FSNodeType>(get8bit(&readPtrForType));

		// Create a new FSNode of the deserialized type
		const uint8_t *readPtr = buffer;
		auto *deserializedNode = FSNode::create(type);
		deserializedNode->deserialize(&readPtr);

		ASSERT_TRUE(areEqual(node, deserializedNode));

		auto *originalSymlinkNode = static_cast<FSNodeSymlink *>(node);
		auto *deserializedSymlinkNode = static_cast<FSNodeSymlink *>(deserializedNode);
		ASSERT_EQ(originalSymlinkNode->path.get(), deserializedSymlinkNode->path.get());
		ASSERT_EQ(originalSymlinkNode->path_length, deserializedSymlinkNode->path_length);

		FSNode::destroy(deserializedNode);
	}
}

TEST_F(FilesystemNodeTypesTest, SerializeFSNodeDevice) {
	constexpr int finalNode = 12;

	for (int i = 2; i < finalNode; ++i) {
		FSNode *node = createFSNode(i, FSNodeType::kBlockDev);

		auto *deviceNode = static_cast<FSNodeDevice *>(node);
		deviceNode->rdev = static_cast<uint32_t>(i);

		nodes.push_back(node);
	}

	uint8_t *buffer = serializeBuffer.data();

	for (auto *node : nodes) {
		node->serialize(&buffer);

		// Rewind the buffer to the beginning and get the serialized type
		buffer = serializeBuffer.data();
		const uint8_t *readPtrForType = buffer;
		auto type = static_cast<FSNodeType>(get8bit(&readPtrForType));

		// Create a new FSNode of the deserialized type
		const uint8_t *readPtr = buffer;
		auto *deserializedNode = FSNode::create(type);
		deserializedNode->deserialize(&readPtr);

		ASSERT_TRUE(areEqual(node, deserializedNode));

		auto *originalDeviceNode = static_cast<FSNodeDevice *>(node);
		auto *deserializedDeviceNode = static_cast<FSNodeDevice *>(deserializedNode);
		ASSERT_EQ(originalDeviceNode->rdev, deserializedDeviceNode->rdev);

		FSNode::destroy(deserializedNode);
	}
}

TEST_F(FilesystemNodeTypesTest, SerializeFSNodeFile) {
	constexpr int finalNode = 12;

	for (int i = 2; i < finalNode; ++i) {
		FSNode *node = createFSNode(i, FSNodeType::kFile);

		auto *fileNode = static_cast<FSNodeFile *>(node);

		fileNode->length = static_cast<uint64_t>(i * 10) * 1024 * 1024; // Increasing file size
		auto chunks = static_cast<uint32_t>(fileNode->length / static_cast<uint32_t>(SFSCHUNKSIZE));

		for (uint32_t j = 0; j < chunks; ++j) {
			fileNode->chunks.push_back(static_cast<uint64_t>(j + 1) * 1000); // Dummy chunk IDs
		}

		for (int j = 0; j < i; ++j) {
			fileNode->sessionIds.push_back(j + 1); // Dummy session IDs
		}

		nodes.push_back(node);
	}

	uint8_t *buffer = serializeBuffer.data();

	for (auto *node : nodes) {
		node->serialize(&buffer);

		// Rewind the buffer to the beginning and get the serialized type
		buffer = serializeBuffer.data();
		const uint8_t *readPtrForType = buffer;
		auto type = static_cast<FSNodeType>(get8bit(&readPtrForType));

		// Create a new FSNode of the deserialized type
		const uint8_t *readPtr = buffer;
		auto *deserializedNode = FSNode::create(type);
		deserializedNode->deserialize(&readPtr);

		ASSERT_TRUE(areEqual(node, deserializedNode));

		auto *originalFileNode = static_cast<FSNodeFile *>(node);
		auto *deserializedFileNode = static_cast<FSNodeFile *>(deserializedNode);
		ASSERT_EQ(originalFileNode->length, deserializedFileNode->length);
		ASSERT_EQ(originalFileNode->chunks, deserializedFileNode->chunks);
		ASSERT_EQ(originalFileNode->sessionIds, deserializedFileNode->sessionIds);

		FSNode::destroy(deserializedNode);
	}
}
