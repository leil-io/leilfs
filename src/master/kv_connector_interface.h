/*
   Copyright 2023      Leil Storage OÜ

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

#pragma once

#include "common/platform.h"

#include "common/type_defs.h"
#include "kv/ikv_engine.h"
#include "master/filesystem_node_types.h"

struct ChangelogEvent;

/// Interface to provide KV store operations in an extensible way
class IKVConnector {
public:
	IKVConnector() = default;

	/// Virtual destructor for proper cleanup of derived classes
	virtual ~IKVConnector() = default;

	/// Unneeded copy and move operations
	IKVConnector(const IKVConnector &) = delete;
	IKVConnector &operator=(const IKVConnector &) = delete;
	IKVConnector(IKVConnector &&) = delete;
	IKVConnector &operator=(IKVConnector &&) = delete;

	/// Creates and initializes the concrete resources needed by the implementation
	/// @return true on success, false on failure
	[[nodiscard]] virtual bool init() = 0;

	/// Returns the KV engine instance
	virtual kv::IKVEngine *getKVEngine() = 0;

	/// Gets 64 bits integer in Big Endian byte order from the KV store.
	/// @param key -- the key to get the value for
	/// @param defaultValue -- the default value to return if the key does not exist
	/// @return the value associated with the key, or defaultValue if the key does not exist
	virtual uint64_t get64bitBE(const kv::Key &key, uint64_t defaultValue) = 0;

	/// Gets 32 bits integer in Big Endian byte order from the KV store.
	/// @param key -- the key to get the value for
	/// @param defaultValue -- the default value to return if the key does not exist
	/// @return the value associated with the key, or defaultValue if the key does not exist
	virtual uint32_t get32bitBE(const kv::Key &key, uint32_t defaultValue) = 0;

	// Event handlers for usual metadata operations

	/// Handles the addition of a detained inode id.
	/// Some implementations need to reuse inode ids, these ids are stored in the FREE section of
	/// the metadata, including the timestamp of when they were added to the detained list.
	/// @param inodeId The inode id that was added.
	/// @param timestamp The timestamp when the inode id was added.
	virtual void onDetainedAdded(inode_t inodeId, uint32_t timestamp) = 0;

	/// Handles the removal of a detained inode id.
	/// @param inodeId The inode id that was removed.
	virtual void onDetainedRemoved(inode_t inodeId) = 0;

	/// Handles the change of the next session id.
	/// @param oldSessionId The previous session id.
	/// @param newSessionId The new session id.
	virtual void onNextSessionIdChanged(uint32_t oldSessionId, uint32_t newSessionId) = 0;

	/// Allows to reacts on changelog events.
	/// @param event The changelog event that occurred.
	virtual void onChangelogEvent(const ChangelogEvent &event) = 0;

	/// Reacts on node additions or changes.
	/// @param node The node that was added or changed.
	virtual void onNodeChanged(FSNode *node) = 0;

	/// Reacts on edge additions or changes.
	/// @param parent The parent directory of the edge.
	/// @param child The child node of the edge.
	/// @param handlePtr The edge name.
	virtual void onEdgeChanged(FSNodeDirectory *parent, FSNode *child,
	                           hstorage::Handle *handlePtr) = 0;

	/// Reacts on edge removals.
	virtual void onEdgeRemoved(inode_t parentId, inode_t childId) = 0;
};
