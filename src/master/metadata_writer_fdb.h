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
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "common/platform.h"

#include <kv/ikv_engine.h>
#include <master/kv_connector_interface.h>
#include <master/metadata_backend_interface.h>

class IMetadataUpdateEvent {
public:
	IMetadataUpdateEvent() = default;
	IMetadataUpdateEvent(const IMetadataUpdateEvent &) = default;
	IMetadataUpdateEvent(IMetadataUpdateEvent &&) = default;
	IMetadataUpdateEvent &operator=(const IMetadataUpdateEvent &) = default;
	IMetadataUpdateEvent &operator=(IMetadataUpdateEvent &&) = default;

	virtual ~IMetadataUpdateEvent() = default;

	virtual void applyEvent(kv::IReadWriteTransaction * /*txn*/) {};
};

/// Update event types for different metadata sections
class ChunkUpdateEvent : public IMetadataUpdateEvent {
public:
	ChunkUpdateEvent(uint64_t _chunkId, uint32_t _version, uint32_t _lockedTo, uint32_t _lockId);
	~ChunkUpdateEvent() override = default;

	void applyEvent(kv::IReadWriteTransaction *txn) override;

private:
	uint64_t chunkId;
	uint32_t version;
	uint32_t lockedTo;
	uint32_t lockId;
};

/// Update event for node changes
class NodeUpdateEvent : public IMetadataUpdateEvent {
public:
	NodeUpdateEvent(FSNode *_node);
	~NodeUpdateEvent() override = default;

	void applyEvent(kv::IReadWriteTransaction *txn) override;

private:
	inode_t nodeId;
	kv::Value serializedNode;
};

class FreeNodeUpdateEvent : public IMetadataUpdateEvent {
public:
	FreeNodeUpdateEvent(inode_t _nodeId, uint32_t _timestamp = 0);
	~FreeNodeUpdateEvent() override = default;

	void applyEvent(kv::IReadWriteTransaction *txn) override;

private:
	inode_t nodeId;
	uint32_t timestamp;
};

class EdgeUpdateEvent : public IMetadataUpdateEvent {
public:
	EdgeUpdateEvent(inode_t _parentId, HString _name, inode_t _childId);
	~EdgeUpdateEvent() override = default;

	void applyEvent(kv::IReadWriteTransaction *txn) override;

private:
	inode_t parentId;
	HString name;
	inode_t childId;
};

class EdgeRemoveEvent : public IMetadataUpdateEvent {
public:
	EdgeRemoveEvent(inode_t _parentId, HString _name);
	~EdgeRemoveEvent() override = default;

	void applyEvent(kv::IReadWriteTransaction *txn) override;

private:
	inode_t parentId;
	HString name;
};

/// Metadata writer that preserves changelog ordering
class MetadataWriterFDB {
public:
	explicit MetadataWriterFDB(kv::IKVEngine *kvEngine);

	/// Enqueue an update (thread-safe)
	void enqueue(std::unique_ptr<IMetadataUpdateEvent> event);

	/// Flushes at most kMaxUpdatesPerFlush_ pending updates to FDB (thread-safe).
	/// Intended for periodic/background flushing to avoid long stalls.
	void flush();

	/// Flushes all pending updates to FDB (thread-safe).
	/// Returns true if all updates were flushed, false on commit failure.
	bool flushAll();

	/// Get count of pending updates
	size_t pendingCount() const;

private:
	bool flushNoLock();

	kv::IKVEngine *kvEngine_;

	mutable std::mutex mutex_;
	std::vector<std::unique_ptr<IMetadataUpdateEvent>> pendingUpdates_;

	constexpr static size_t kInitialSize_ = 1000;
	constexpr static size_t kMaxUpdatesPerFlush_ = 1000;
};
