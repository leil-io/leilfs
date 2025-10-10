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

#include "fdb/fdb_context.h"
#include "kv/ikv_engine.h"
#include "master/kv_connector_interface.h"

/// Implements IKVConnector using FoundationDB as KV store backend
class KVConnectorFDB : public IKVConnector {
public:
	/// Initializes the FDB context and engine using the configuration variable FDB_CLUSTER_FILE.
	/// @return true on success, false on failure
	/// @note Must be called after construction, before any other method
	[[nodiscard]] bool init() override;

	/// Returns the pointer to the concrete KV engine instance (FDB in this case)
	kv::IKVEngine *getKVEngine() override { return kvEngine_.get(); }

	/// Gets 64 bits integer in Big Endian byte order from the KV store.
	/// @param key -- the key to get the value for
	/// @param defaultValue -- the default value to return if the key does not exist
	/// @return the value associated with the key, or defaultValue if the key does not exist
	uint64_t get64bitBE(const kv::Key &key, uint64_t defaultValue) override;

	/// Gets 32 bits integer in Big Endian byte order from the KV store.
	/// @param key -- the key to get the value for
	/// @param defaultValue -- the default value to return if the key does not exist
	/// @return the value associated with the key, or defaultValue if the key does not exist
	uint32_t get32bitBE(const kv::Key &key, uint32_t defaultValue) override;

	// Event handlers for usual metadata operations
	// The handlers provide the concrete implementation on top of FDB.
	// @see KVConnectorInterface

	void onDetainedAdded(inode_t inodeId, uint32_t timestamp) override;
	void onDetainedRemoved(inode_t inodeId) override;

	void onNextSessionIdChanged(uint32_t oldSessionId, uint32_t newSessionId) override;

	void onChangelogEvent(const ChangelogEvent &event) override;

	void onNodeChanged(FSNode *node) override;

	void onEdgeChanged(FSNodeDirectory *parent, FSNode *child,
	                   hstorage::Handle *handlePtr) override;
	void onEdgeRemoved(inode_t parentId, inode_t childId) override;

private:
	std::shared_ptr<fdb::FDBContext> fdbContext_;  ///< FDBContext needed by the engine
	std::shared_ptr<kv::IKVEngine> kvEngine_;      ///< The concrete KV engine instance (FDB)
};
