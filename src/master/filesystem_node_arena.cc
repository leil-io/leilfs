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
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/platform.h"

#include "master/filesystem_node_arena.h"

#include <cassert>
#include <utility>

#include "master/filesystem_node_types.h"

// Moves clear the source explicitly. A moved-from std::unordered_map is only "valid but
// unspecified", so without the clear() the moved-from arena's destructor could still see
// entries and destroy nodes that are now owned by the destination arena.
FSNodeArena::FSNodeArena(FSNodeArena &&other) noexcept : nodes_(std::move(other.nodes_)) {
	other.nodes_.clear();
}

FSNodeArena &FSNodeArena::operator=(FSNodeArena &&other) noexcept {
	if (this != &other) {
		destroyAll();
		nodes_ = std::move(other.nodes_);
		other.nodes_.clear();
	}
	return *this;
}

FSNodeArena::~FSNodeArena() { destroyAll(); }

void FSNodeArena::destroyAll() {
	for (auto &[_, node] : nodes_) {
		if (node != nullptr) { FSNode::destroy(node); }
	}
	nodes_.clear();
}

std::optional<FSNode *> FSNodeArena::lookup(inode_t inode) const {
	auto entryIt = nodes_.find(inode);
	if (entryIt == nodes_.end()) { return std::nullopt; }
	return entryIt->second;
}

FSNode *FSNodeArena::adopt(inode_t inode, FSNode *node) {
	assert(node != nullptr);
	// A mismatched key would pin/dedup/tombstone under the wrong inode, e.g. from a
	// corrupt backend record whose serialized id disagrees with the key it was read from.
	assert(node->id == inode);
	auto [entryIt, inserted] = nodes_.emplace(inode, node);
	if (!inserted) {
		if (entryIt->second == nullptr) {
			// Re-pin over a tombstone: the inode was deleted and recreated in this operation.
			entryIt->second = node;
		} else if (entryIt->second != node) {
			// The inode is already pinned; keep that instance so existing borrows stay valid.
			FSNode::destroy(node);
		}
	}
	return entryIt->second;
}

void FSNodeArena::releaseAndTombstone(inode_t inode) { nodes_[inode] = nullptr; }
