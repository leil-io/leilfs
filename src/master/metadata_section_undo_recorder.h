/*
   Copyright 2026      Leil Storage OÜ

   This file is part of SaunaFS.

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

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "common/type_defs.h"
#include "kv/itransaction.h"
#include "kv/kv_types.h"
#include "master/filesystem_operation_context.h"
#include "master/hstring.h"

enum class MetadataSectionKind : uint8_t {
	Chunk = 0,
	Node,
	Edge,
	FreeNode,
	XAttr,
	Count
};

constexpr std::string_view sectionName(MetadataSectionKind section) {
	switch (section) {
	case MetadataSectionKind::Chunk:
		return "Chunk";
	case MetadataSectionKind::Node:
		return "Node";
	case MetadataSectionKind::Edge:
		return "Edge";
	case MetadataSectionKind::FreeNode:
		return "FreeNode";
	case MetadataSectionKind::XAttr:
		return "XAttr";
	case MetadataSectionKind::Count:
		// Sentinel, not a real section: report as Unknown so callers reject it.
		return "Unknown";
	}
	return "Unknown";
}

constexpr size_t kMetadataSectionKindCount = static_cast<size_t>(MetadataSectionKind::Count);

struct MetadataMutationContext {
	kv::IReadWriteTransaction *transaction{nullptr};
	uint64_t checkpointVersion{0};
};

struct ChunkSetMutation {
	uint64_t chunkId;
	kv::Key liveKey;
};

struct NodeSetMutation {
	inode_t inode;
	kv::Key liveKey;
};

struct NodeRemoveMutation {
	inode_t inode;
	kv::Key liveKey;
};

struct FreeNodeSetMutation {
	inode_t inode;
	kv::Key liveKey;
};

struct FreeNodeRemoveMutation {
	inode_t inode;
	kv::Key liveKey;
};

struct EdgeSetMutation {
	inode_t parentId;
	inode_t childId;
	HString name;
	kv::Key liveKey;
};

struct EdgeRemoveMutation {
	inode_t parentId;
	HString name;
	kv::Key liveKey;
};

struct XAttrSetMutation {
	inode_t inode;
	std::vector<uint8_t> name;
	kv::Key liveKey;
};

struct XAttrRemoveMutation {
	inode_t inode;
	std::vector<uint8_t> name;
	kv::Key liveKey;
};

struct XAttrRangeRemoveMutation {
	inode_t inode;
	kv::Key rangeBegin;
	kv::Key rangeEnd;
};

using MetadataMutation =
    std::variant<ChunkSetMutation, NodeSetMutation, NodeRemoveMutation, FreeNodeSetMutation,
                 FreeNodeRemoveMutation, EdgeSetMutation, EdgeRemoveMutation, XAttrSetMutation,
                 XAttrRemoveMutation, XAttrRangeRemoveMutation>;

class ISectionUndoRecorder {
public:
	// Polymorphic interface: suppress copy/move to prevent slicing.
	ISectionUndoRecorder(const ISectionUndoRecorder &) = delete;
	ISectionUndoRecorder(ISectionUndoRecorder &&) = delete;
	ISectionUndoRecorder &operator=(const ISectionUndoRecorder &) = delete;
	ISectionUndoRecorder &operator=(ISectionUndoRecorder &&) = delete;
	ISectionUndoRecorder() = default;
	virtual ~ISectionUndoRecorder() = default;

	virtual MetadataSectionKind sectionKind() const = 0;

	virtual void beforeMutation(const MetadataMutationContext &context,
	                            const MetadataMutation &mutation) = 0;

	virtual bool restoreToCheckpointVersion(uint64_t targetVersion) = 0;
	virtual std::pair<uint64_t, bool> restoreSingleCheckpoint(
	    const FilesystemOperationContext &fsOpContext, uint64_t checkpointVersion) = 0;

	virtual int8_t dropCheckpointData(kv::IReadWriteTransaction *transaction,
	                                  uint64_t droppedCheckpointVersion) = 0;

	virtual void resetIntervalState() = 0;
};
