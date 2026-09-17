/*
   Copyright 2026 Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "kv/ifuture.h"
#include "kv/ikv_engine.h"
#include "kv/itransaction.h"
#include "kv/kv_utils.h"
#include "master/filesystem_metadata.h"
#include "master/filesystem_node.h"
#include "master/filesystem_node_types.h"
#include "master/filesystem_operations.h"
#include "master/filesystem_trash_reserved_files.h"
#include "master/hstring_memstorage.h"
#include "master/kv_common_keys.h"
#include "master/metadata_checkpoint_helpers.h"
#include "master/metadata_edge_restore_helpers.h"
#include "master/metadata_edge_undo_recorder.h"

namespace {

using DurableStore = std::map<kv::Key, kv::Value>;

class StoreReadOnlyTransaction final : public kv::IReadOnlyTransaction {
public:
	explicit StoreReadOnlyTransaction(const DurableStore &store) : store_(store) {}

	std::optional<kv::Value> get(const kv::Key &key) override {
		if (const auto iterator = store_.find(key); iterator != store_.end()) {
			return iterator->second;
		}
		return std::nullopt;
	}

	std::optional<kv::Value> getSnapshot(const kv::Key &key) override { return get(key); }

	std::unique_ptr<kv::IFuture> getAsync(const kv::Key & /*key*/) override { return nullptr; }

	std::unique_ptr<kv::IFuture> getSnapshotAsync(const kv::Key & /*key*/) override {
		return nullptr;
	}

	kv::GetRangeResult getRange(const kv::KeySelector &start, const kv::KeySelector &end,
	                            int limit) override {
		if (start.getOffset() != 0 || end.getOffset() != 0) {
			throw std::logic_error("StoreReadOnlyTransaction does not support selector offsets");
		}
		if (limit <= 0) {
			throw std::invalid_argument("StoreReadOnlyTransaction requires a positive range limit");
		}

		auto iterator = start.isInclusive() ? store_.lower_bound(start.getKey())
		                                    : store_.upper_bound(start.getKey());
		auto isBeforeEnd = [&end](const kv::Key &key) {
			return end.isInclusive() ? key <= end.getKey() : key < end.getKey();
		};

		std::vector<kv::KeyValuePair> pairs;
		for (; iterator != store_.end() && isBeforeEnd(iterator->first); ++iterator) {
			if (pairs.size() == static_cast<size_t>(limit)) { return {std::move(pairs), true}; }
			pairs.push_back({.key = iterator->first, .value = iterator->second});
		}
		return {std::move(pairs), false};
	}

	std::unique_ptr<kv::IRangeFuture> getRangeAsync(const kv::KeySelector & /*start*/,
	                                                const kv::KeySelector & /*end*/,
	                                                int /*limit*/) override {
		return nullptr;
	}

private:
	const DurableStore &store_;
};

class StoreKVEngine final : public kv::IKVEngine {
public:
	std::unique_ptr<kv::IReadOnlyTransaction> createReadOnlyTransaction() override {
		return std::make_unique<StoreReadOnlyTransaction>(store_);
	}

	std::unique_ptr<kv::IReadWriteTransaction> createReadWriteTransaction() override {
		return nullptr;
	}

	DurableStore &store() { return store_; }

private:
	DurableStore store_;
};

kv::Key edgeUndoKey(uint64_t checkpointVersion, inode_t parentId, std::string_view name) {
	kv::Key key = kv::encodeKeyBE(kEdgeUndoKeyPrefix, checkpointVersion, parentId);
	key.insert(key.end(), name.begin(), name.end());
	return key;
}

kv::Key detachedPathUndoKey(uint64_t checkpointVersion, inode_t inode) {
	return kv::encodeKeyBE(kEdgeUndoKeyPrefix, checkpointVersion, inode_t{0}, inode);
}

kv::Value detachedPathUndoValue(FSNodeType nodeType, std::string_view path) {
	kv::Value value{static_cast<uint8_t>(nodeType)};
	value.insert(value.end(), path.begin(), path.end());
	return value;
}

// This test models a hierarchy inversion between a sealed checkpoint and the latest live image
// (arrows point from parent to child):
//
//     checkpoint                 latest live image
//     ----------                 -----------------
//     root                       root
//       |                          |
//       A                          B
//       |                          |
//       B                          A
//
// The corresponding undo rows restore root/A and A/B, and remove root/B and B/A. The old
// row-at-a-time implementation processed them in key order:
//
//     1. restore root/A
//     2. remove  root/B
//     3. restore A/B    -> root -> A <-> B (B/A is still live)
//     4. remove  B/A    -> root -> A -> B
//
// Step 3 temporarily forms a directory cycle. Recursive directory-stat maintenance can then
// overflow the stack even though step 4 would leave the final graph correct. The production fix
// avoids the invalid intermediate graph by detaching all affected live edges before attaching
// any checkpoint pre-images.
class CycleDetectingNodeOperations final : public FilesystemNodeOperationsBase {
public:
	// The production implementations recursively traverse directory topology. Detect a cycle
	// iteratively first and suppress that recursive call, so the regression is deterministic
	// instead of crashing with a stack overflow and can report that the bad state was observed.
	void getStats(const FilesystemOperationContext &fsOpContext, FSNode *node,
	              StatsRecord *stats) override {
		if (node != nullptr && node->type == FSNodeType::kDirectory &&
		    hasParentCycle(fsOpContext, static_cast<FSNodeDirectory *>(node))) {
			transientCycleDetected_ = true;
			*stats = {};
			return;
		}
		FilesystemNodeOperationsBase::getStats(fsOpContext, node, stats);
	}

	void addStats(const FilesystemOperationContext &fsOpContext, FSNodeDirectory *parent,
	              StatsRecord *stats) override {
		if (hasParentCycle(fsOpContext, parent)) {
			transientCycleDetected_ = true;
			return;
		}
		FilesystemNodeOperationsBase::addStats(fsOpContext, parent, stats);
	}

	void subStats(const FilesystemOperationContext &fsOpContext, FSNodeDirectory *parent,
	              StatsRecord *stats) override {
		if (hasParentCycle(fsOpContext, parent)) {
			transientCycleDetected_ = true;
			return;
		}
		FilesystemNodeOperationsBase::subStats(fsOpContext, parent, stats);
	}

	bool transientCycleDetected() const { return transientCycleDetected_; }

private:
	bool hasParentCycle(const FilesystemOperationContext &fsOpContext,
	                    const FSNodeDirectory *start) {
		if (start == nullptr) { return false; }

		std::vector<inode_t> pending;
		for (const auto &[parentId, handle] : start->parents) {
			(void)handle;
			pending.push_back(parentId);
		}

		std::unordered_set<inode_t> visited;
		while (!pending.empty()) {
			const inode_t parentId = pending.back();
			pending.pop_back();
			if (parentId == start->id) { return true; }
			if (!visited.insert(parentId).second) { continue; }

			FSNode *parent = idToNode(fsOpContext, parentId);
			if (parent == nullptr || parent->type != FSNodeType::kDirectory) { continue; }
			for (const auto &[ancestorId, handle] : parent->parents) {
				(void)handle;
				pending.push_back(ancestorId);
			}
		}
		return false;
	}

	bool transientCycleDetected_ = false;
};

class EdgeRecoveryStateTest : public ::testing::Test {
protected:
	void SetUp() override {
		previousMetadata_ = gMetadata;
		previousFSOperations_ = std::move(gFSOperations);

		gMetadata = new FilesystemMetadata;
		hstorage::Storage::reset(new hstorage::MemStorage());
		auto nodeOperations = std::make_unique<CycleDetectingNodeOperations>();
		nodeOperations_ = nodeOperations.get();
		gFSOperations = std::make_unique<FilesystemOperationsBase>(std::move(nodeOperations));

		root_ = addDirectory(/*inode=*/1);
		directoryA_ = addDirectory(/*inode=*/2);
		directoryB_ = addDirectory(/*inode=*/3);
		gMetadata->root = root_;

		// Construct the latest live hierarchy: root -> B -> A.
		ASSERT_EQ(
		    metadata::edges::restoreLoadedEdge(context_, root_->id, directoryB_->id, HString("B")),
		    kOpSuccess);
		ASSERT_EQ(metadata::edges::restoreLoadedEdge(context_, directoryB_->id, directoryA_->id,
		                                             HString("A")),
		          kOpSuccess);

		engine_.store()[kv::toBytes(kMetaCheckpointVersionsKey)] =
		    checkpoints::serializeCheckpointVersions({kCheckpointVersion});

		// Record the pre-images needed to reconstruct the checkpoint hierarchy root -> A -> B.
		// Empty values are tombstones: those edges were absent at the checkpoint and must be
		// removed from the latest live hierarchy.
		engine_.store()[edgeUndoKey(kCheckpointVersion, root_->id, "A")] =
		    kv::toBytesBE(directoryA_->id);
		engine_.store()[edgeUndoKey(kCheckpointVersion, root_->id, "B")] = {};
		engine_.store()[edgeUndoKey(kCheckpointVersion, directoryA_->id, "B")] =
		    kv::toBytesBE(directoryB_->id);
		engine_.store()[edgeUndoKey(kCheckpointVersion, directoryB_->id, "A")] = {};
	}

	void TearDown() override {
		gFSOperations.reset();
		delete gMetadata;
		gMetadata = previousMetadata_;
		gFSOperations = std::move(previousFSOperations_);
	}

	FSNodeDirectory *addDirectory(inode_t inode) {
		auto *directory = new FSNodeDirectory;
		directory->id = inode;
		gMetadata->addNode(directory, /*isFromScan=*/true);
		gMetadata->inodePool.markAsAcquired(inode);
		return directory;
	}

	FSNodeFile *addDetachedFile(inode_t inode, FSNodeType type, uint64_t length) {
		auto *file = new FSNodeFile(type);
		file->id = inode;
		file->length = length;
		gMetadata->addNode(file, /*isFromScan=*/true);
		gMetadata->inodePool.markAsAcquired(inode);
		return file;
	}

	static constexpr uint64_t kCheckpointVersion = 17;

	FilesystemMetadata *previousMetadata_ = nullptr;
	std::unique_ptr<IFilesystemOperations> previousFSOperations_;
	FilesystemOperationContext context_;
	StoreKVEngine engine_;
	CycleDetectingNodeOperations *nodeOperations_ = nullptr;
	FSNodeDirectory *root_ = nullptr;
	FSNodeDirectory *directoryA_ = nullptr;
	FSNodeDirectory *directoryB_ = nullptr;
};

TEST_F(EdgeRecoveryStateTest, DirectoryHierarchyInversionNeverCreatesTransientCycle) {
	EdgeUndoRecorder recorder(&engine_);
	ASSERT_TRUE(recorder.restoreToCheckpointVersion(kCheckpointVersion));

	// The final graph alone is insufficient: the old implementation also eventually produced the
	// right graph, but exposed A <-> B while applying its third undo row.
	EXPECT_FALSE(nodeOperations_->transientCycleDetected());

	// Verify the complete checkpoint topology root -> A -> B and the removal of both inverse live
	// edges, root/B and B/A.
	auto rootToA = root_->find(HString("A"));
	ASSERT_NE(rootToA, root_->entries.end());
	EXPECT_EQ(rootToA->second, directoryA_);
	EXPECT_EQ(root_->find(HString("B")), root_->entries.end());

	auto aToB = directoryA_->find(HString("B"));
	ASSERT_NE(aToB, directoryA_->entries.end());
	EXPECT_EQ(aToB->second, directoryB_);
	EXPECT_EQ(directoryB_->find(HString("A")), directoryB_->entries.end());
}
TEST_F(EdgeRecoveryStateTest, RestoresInodeKeyedDetachedPaths) {
	constexpr uint64_t kTrashLength = 1024;
	constexpr uint64_t kReservedLength = 2048;
	auto *trashNode = addDetachedFile(/*inode=*/4, FSNodeType::kTrash, kTrashLength);
	auto *reservedNode = addDetachedFile(/*inode=*/5, FSNodeType::kReserved, kReservedLength);

	addTrashEntry(gMetadata->trash, gMetadata->trashHandlesIndex, gMetadata->trashReservedToId,
	              trashNode, "latest/trash");
	addReservedEntry(gMetadata->reserved, gMetadata->reservedHandlesIndex,
	                 gMetadata->trashReservedToId, reservedNode, "latest/reserved");
	gMetadata->trashNodes = 1;
	gMetadata->trashSpace = kTrashLength;
	gMetadata->reservedNodes = 1;
	gMetadata->reservedSpace = kReservedLength;

	// Each inode has one undo identity regardless of its current path or container. The tagged
	// values restore both the checkpoint container kind and its path after removing the latest
	// state by inode.
	engine_.store()[detachedPathUndoKey(kCheckpointVersion, trashNode->id)] =
	    detachedPathUndoValue(FSNodeType::kTrash, "checkpoint/trash");
	engine_.store()[detachedPathUndoKey(kCheckpointVersion, reservedNode->id)] =
	    detachedPathUndoValue(FSNodeType::kReserved, "checkpoint/reserved");

	EdgeUndoRecorder recorder(&engine_);
	ASSERT_TRUE(recorder.restoreToCheckpointVersion(kCheckpointVersion));

	ASSERT_EQ(gMetadata->trash.size(), 1U);
	EXPECT_EQ((*gMetadata->trash.begin()).first.id, trashNode->id);
	EXPECT_EQ(static_cast<std::string>((*gMetadata->trash.begin()).second), "checkpoint/trash");
	EXPECT_EQ(gMetadata->trashNodes, 1U);
	EXPECT_EQ(gMetadata->trashSpace, kTrashLength);
	EXPECT_EQ(gMetadata->trashHandlesIndex.size(), 1U);

	ASSERT_EQ(gMetadata->reserved.size(), 1U);
	EXPECT_EQ((*gMetadata->reserved.begin()).first, reservedNode->id);
	EXPECT_EQ(static_cast<std::string>((*gMetadata->reserved.begin()).second),
	          "checkpoint/reserved");
	EXPECT_EQ(gMetadata->reservedNodes, 1U);
	EXPECT_EQ(gMetadata->reservedSpace, kReservedLength);
	EXPECT_EQ(gMetadata->reservedHandlesIndex.size(), 1U);
}

}  // namespace
