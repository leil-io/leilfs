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

#include <gtest/gtest.h>

#include "master/filesystem_trash_reserved_files.h"
#include "master/hstring_memstorage.h"

using namespace hstorage;

class FilesystemTrashReservedTests : public ::testing::Test {
protected:
	void SetUp() override {
		Storage::reset(new MemStorage());
		EXPECT_EQ(Storage::instance().name(), "MemStorage");
	}
	void TearDown() override {}

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
};

TEST_F(FilesystemTrashReservedTests, AddAndRemoveTrashEntryFromContainers) {
	TrashPathContainer trash;
	HandleIndexContainer trashHandlesIndex;
	TrashReservedToIdContainer trashReservedToId;
	FSNode *node = createFSNode(1, FSNodeType::kFile);

	std::string path = "/path/to/file";

	// Add to trash
	addTrashEntry(trash, trashHandlesIndex, trashReservedToId, node, path);

	ASSERT_EQ(trash.size(), 1U);
	ASSERT_EQ(trashHandlesIndex.size(), 1U);
	ASSERT_EQ(trashReservedToId.counter, 1U);

	// Remove from trash
	removeTrashEntry(trash, trashHandlesIndex, trashReservedToId, node);

	ASSERT_EQ(trash.size(), 0U);
	ASSERT_EQ(trashHandlesIndex.size(), 0U);
	ASSERT_EQ(trashReservedToId.counter, 1U);

	FSNode::destroy(node);
}

TEST_F(FilesystemTrashReservedTests, RemoveTrashEntryByKeyWithoutNode) {
	TrashPathContainer trash;
	HandleIndexContainer trashHandlesIndex;
	TrashReservedToIdContainer trashReservedToId;
	FSNode *node = createFSNode(1, FSNodeType::kFile);

	std::string path = "/path/to/file";

	addTrashEntry(trash, trashHandlesIndex, trashReservedToId, node, path);

	ASSERT_EQ(trash.size(), 1U);
	ASSERT_EQ(trashHandlesIndex.size(), 1U);
	ASSERT_EQ(trashReservedToId.counter, 1U);

	TrashPathKey key(node);
	FSNode::destroy(node);

	removeTrashEntryByKey(trash, trashHandlesIndex, trashReservedToId, key);

	ASSERT_EQ(trash.size(), 0U);
	ASSERT_EQ(trashHandlesIndex.size(), 0U);
	ASSERT_EQ(trashReservedToId.counter, 1U);
}

TEST_F(FilesystemTrashReservedTests, AddAndRemoveReservedEntryFromContainers) {
	ReservedPathContainer reserved;
	HandleIndexContainer reservedHandlesIndex;
	TrashReservedToIdContainer reservedReservedToId;
	FSNode *node = createFSNode(1, FSNodeType::kFile);

	std::string path = "/path/to/file";

	// Add to reserved
	addReservedEntry(reserved, reservedHandlesIndex, reservedReservedToId, node, path);

	ASSERT_EQ(reserved.size(), 1U);
	ASSERT_EQ(reservedHandlesIndex.size(), 1U);
	ASSERT_EQ(reservedReservedToId.counter, 1U);

	// Remove from reserved
	removeReservedEntry(reserved, reservedHandlesIndex, reservedReservedToId, node->id);

	ASSERT_EQ(reserved.size(), 0U);
	ASSERT_EQ(reservedHandlesIndex.size(), 0U);
	ASSERT_EQ(reservedReservedToId.counter, 1U);

	FSNode::destroy(node);
}

TEST_F(FilesystemTrashReservedTests, MoveTrashToReservedContainer) {
	TrashPathContainer trash;
	HandleIndexContainer trashHandlesIndex;
	ReservedPathContainer reserved;
	HandleIndexContainer reservedHandlesIndex;
	TrashReservedToIdContainer trashReservedToId;

	FSNode *node = createFSNode(1, FSNodeType::kFile);

	std::string path = "/path/to/file";

	// Add to trash
	addTrashEntry(trash, trashHandlesIndex, trashReservedToId, node, path);

	ASSERT_EQ(trash.size(), 1U);
	ASSERT_EQ(trashHandlesIndex.size(), 1U);
	ASSERT_EQ(trashReservedToId.counter, 1U);

	// Move from trash to reserved
	moveTrashToReservedEntry(trash, trashHandlesIndex, reserved, reservedHandlesIndex,
	                         trashReservedToId, node);

	ASSERT_EQ(trash.size(), 0U);
	ASSERT_EQ(trashHandlesIndex.size(), 0U);
	ASSERT_EQ(reserved.size(), 1U);
	ASSERT_EQ(reservedHandlesIndex.size(), 1U);
	ASSERT_EQ(trashReservedToId.counter, 1U);

	FSNode::destroy(node);
}

TEST_F(FilesystemTrashReservedTests, UpdateTrashNameEntry) {
	TrashPathContainer trash;
	HandleIndexContainer trashHandlesIndex;
	TrashReservedToIdContainer trashReservedToId;

	FSNode *node = createFSNode(1, FSNodeType::kFile);

	std::string path1 = "/path/to/file1";
	std::string path2 = "/path/to/file2";

	// Add to trash
	addTrashEntry(trash, trashHandlesIndex, trashReservedToId, node, path1);

	ASSERT_EQ(trash.size(), 1U);
	ASSERT_EQ(trashHandlesIndex.size(), 1U);
	ASSERT_EQ(trashReservedToId.counter, 1U);

	// Update trash name entry
	updateTrashNameEntry(trash, trashHandlesIndex, trashReservedToId, node, path2);

	ASSERT_EQ(trash.size(), 1U);
	ASSERT_EQ(trashHandlesIndex.size(), 1U);
	ASSERT_EQ(trashReservedToId.counter, 2U);

	std::string retrievedPath = static_cast<std::string>(trash.at(TrashPathKey(node)));
	ASSERT_EQ(retrievedPath, path2);

	FSNode::destroy(node);
}

TEST_F(FilesystemTrashReservedTests, UpdateTrashFromOldEntry) {
	TrashPathContainer trash;
	HandleIndexContainer trashHandlesIndex;
	TrashReservedToIdContainer trashReservedToId;

	FSNode *node1 = createFSNode(1, FSNodeType::kFile);

	std::string path = "/path/to/file";

	// Add to trash
	addTrashEntry(trash, trashHandlesIndex, trashReservedToId, node1, path);

	ASSERT_EQ(trash.size(), 1U);
	ASSERT_EQ(trashHandlesIndex.size(), 1U);
	ASSERT_EQ(trashReservedToId.counter, 1U);

	uint32_t retrivedCTime = (*trash.begin()).first.timestamp;
	ASSERT_EQ(retrivedCTime,
	          std::min((uint64_t)node1->ctime + node1->trashtime, (uint64_t)UINT32_MAX));

	TrashPathKey oldKey(node1);

	// Update node1 creation time
	node1->ctime += 100;

	// Update trash from old entry
	updateTrashFromOldEntry(trash, node1, oldKey);

	ASSERT_EQ(trash.size(), 1U);
	ASSERT_EQ(trashHandlesIndex.size(), 1U);
	ASSERT_EQ(trashReservedToId.counter, 1U);

	retrivedCTime = (*trash.begin()).first.timestamp;
	ASSERT_EQ(retrivedCTime,
	          std::min((uint64_t)node1->ctime + node1->trashtime, (uint64_t)UINT32_MAX));

	FSNode::destroy(node1);
}
