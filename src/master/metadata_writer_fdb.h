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

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "common/type_defs.h"
#include "kv/ikv_engine.h"
#include "master/filesystem_node_types.h"

class MetadataCheckpointManager;

struct MetadataWriteContext {
	kv::IReadWriteTransaction *transaction{nullptr};
	MetadataCheckpointManager *checkpointManager{nullptr};

	// Snapshot of the active checkpoint interval used for first-touch undo capture.
	// This is passed explicitly so events do not need to query manager state.
	uint64_t checkpointVersion{0};
};

class IMetadataUpdateEvent {
public:
	IMetadataUpdateEvent() = default;
	IMetadataUpdateEvent(const IMetadataUpdateEvent &) = default;
	IMetadataUpdateEvent(IMetadataUpdateEvent &&) = default;
	IMetadataUpdateEvent &operator=(const IMetadataUpdateEvent &) = default;
	IMetadataUpdateEvent &operator=(IMetadataUpdateEvent &&) = default;

	virtual ~IMetadataUpdateEvent() = default;

	virtual void applyEvent(const MetadataWriteContext &context) = 0;
};

/// Update event types for different metadata sections
class ChunkUpdateEvent : public IMetadataUpdateEvent {
public:
	ChunkUpdateEvent(uint64_t _chunkId, uint32_t _version, uint32_t _lockedTo, uint32_t _lockId);
	~ChunkUpdateEvent() override = default;

	void applyEvent(const MetadataWriteContext &context) override;

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

	void applyEvent(const MetadataWriteContext &context) override;

private:
	inode_t nodeId;
	kv::Value serializedNode;
};

/// Removal event for nodes
class NodeRemoveEvent : public IMetadataUpdateEvent {
public:
	NodeRemoveEvent(inode_t _nodeId);
	~NodeRemoveEvent() override = default;

	void applyEvent(const MetadataWriteContext &context) override;

private:
	inode_t nodeId;
};

class FreeNodeUpdateEvent : public IMetadataUpdateEvent {
public:
	FreeNodeUpdateEvent(inode_t _nodeId, uint32_t _timestamp = 0);
	~FreeNodeUpdateEvent() override = default;

	void applyEvent(const MetadataWriteContext &context) override;

private:
	inode_t nodeId;
	uint32_t timestamp;
};

class EdgeUpdateEvent : public IMetadataUpdateEvent {
public:
	EdgeUpdateEvent(inode_t _parentId, HString _name, inode_t _childId);
	~EdgeUpdateEvent() override = default;

	void applyEvent(const MetadataWriteContext &context) override;

private:
	inode_t parentId;
	HString name;
	inode_t childId;
};

class EdgeRemoveEvent : public IMetadataUpdateEvent {
public:
	EdgeRemoveEvent(inode_t _parentId, HString _name);
	~EdgeRemoveEvent() override = default;

	void applyEvent(const MetadataWriteContext &context) override;

private:
	inode_t parentId;
	HString name;
};

/// Update event for xattr creation or value change.
/// Writes XATR_<InodeId><AttributeName>: <AttributeValue> to FDB.
class XAttrUpdateEvent : public IMetadataUpdateEvent {
public:
	XAttrUpdateEvent(inode_t _inode, std::span<const uint8_t> _name,
	                 std::span<const uint8_t> _value);
	~XAttrUpdateEvent() override = default;

	void applyEvent(const MetadataWriteContext &context) override;

private:
	inode_t inode;
	std::vector<uint8_t> name;
	std::vector<uint8_t> value;
};

/// Removal event for a single xattr entry.
/// Removes XATR_<InodeId><AttributeName> from FDB.
class XAttrRemoveEvent : public IMetadataUpdateEvent {
public:
	XAttrRemoveEvent(inode_t _inode, std::span<const uint8_t> _name);
	~XAttrRemoveEvent() override = default;

	void applyEvent(const MetadataWriteContext &context) override;

private:
	inode_t inode;
	std::vector<uint8_t> name;
};

/// Removal event for all xattrs of an inode.
/// Removes the range XATR_<InodeId> .. XATR_<InodeId+1> from FDB.
class XAttrInodeRemoveEvent : public IMetadataUpdateEvent {
public:
	explicit XAttrInodeRemoveEvent(inode_t _inode);
	~XAttrInodeRemoveEvent() override = default;

	void applyEvent(const MetadataWriteContext &context) override;

private:
	inode_t inode;
};

/// Metadata writer that preserves changelog ordering
class MetadataWriterFDB {
public:
	/// Controls which set of queued updates a flush operation must persist.
	///
	/// Snapshot mode flushes only the updates that were already queued when the call started.
	/// Drain-until-empty mode keeps flushing until the queue becomes empty, including updates
	/// enqueued while the flush is in progress.
	enum class FlushMode : uint8_t {
		kSnapshot,        ///< Flush only the initial snapshot of pending updates.
		kDrainUntilEmpty, ///< Keep flushing until no pending updates remain.
	};

	explicit MetadataWriterFDB(kv::IKVEngine *kvEngine,
	                           MetadataCheckpointManager *checkpointManager = nullptr);

	/// Enqueue an update (thread-safe)
	void enqueue(std::unique_ptr<IMetadataUpdateEvent> event);

	/// Flushes pending updates that were queued when the call started (thread-safe).
	/// Intended for periodic/background flushing to avoid unbounded backlog growth.
	bool flush(FlushMode mode = FlushMode::kSnapshot);

	/// Get count of pending updates
	size_t pendingCount() const;

private:
	using UpdateQueue = std::deque<std::unique_ptr<IMetadataUpdateEvent>>;

	UpdateQueue takeBatch(size_t maxUpdates);
	void restoreBatch(UpdateQueue updates);
	bool flushBatch(size_t maxUpdates);

	kv::IKVEngine *kvEngine_;
	MetadataCheckpointManager *checkpointManager_;

	mutable std::mutex mutex_;
	UpdateQueue pendingUpdates_;

	constexpr static size_t kMaxUpdatesPerFlush_ = 1000;
};
