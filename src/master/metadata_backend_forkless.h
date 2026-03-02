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

#include "kv/ikv_engine.h"
#include "master/filesystem_operation_context.h"
#include "master/kv_connector_interface.h"
#include "master/metadata_backend_interface.h"
#include "master/metadata_section_bootstrap_fdb.h"
#include "master/metadata_writer_fdb.h"

// Forward declarations to avoid heavy includes in the header
struct ChangelogEvent;
namespace hstorage { class Handle; }

/// Simplified Metadata Section structure for FoundationDB
struct MetadataSectionFDB {
	std::string name;         ///< Name of the section
	std::string_view prefix;  ///< Prefix for the section keys

	std::function<int8_t(bool)> loadFunction;  ///< Function to load the section
};

class MetadataBackendForkless : public IMetadataBackend {
public:
	MetadataBackendForkless();
	~MetadataBackendForkless() override;

	// This instance registers itself in the gForklessBackend global; copying or
	// moving it would break that identity, so disable both.
	MetadataBackendForkless(const MetadataBackendForkless &) = delete;
	MetadataBackendForkless &operator=(const MetadataBackendForkless &) = delete;
	MetadataBackendForkless(MetadataBackendForkless &&) = delete;
	MetadataBackendForkless &operator=(MetadataBackendForkless &&) = delete;

	/// Initializes the metadata backend.
	/// This method should be called before any other methods of the backend.
	void init() override;

	/// Returns version of the metadata.
	/// @param file -- path to the metadata binary file (Ignored in FDB).
	uint64_t getVersion(const std::string &file) override;

	/// Returns the current metadata header signature
	std::string getHeaderSignature() override;

	std::string backendType() override { return "MetadataBackendForkless"; }

#ifndef METALOGGER
	/// Store metadata to the given file descriptor.
	void store_fd(FILE *fd) override;

	/// Load complete metadata.
	void loadall(int ignoreflag) override;
#endif  // #ifndef METALOGGER

#if !defined(METARESTORE) && !defined(METALOGGER)
	/// Broadcasts information about status of the freshly finished
	/// metadata save process to interested modules.
	void broadcast_metadata_saved(uint8_t status) override;

	/// Commits the metadata dump by rotating the metadata files and renaming
	/// the temporary file.
	///
	/// This function attempts to rotate the metadata files and rename the
	/// temporary metadata file to the main metadata file. If the renaming
	/// fails, it tries to create an emergency metadata file with a unique name
	/// based on the current time.
	/// @return true if the metadata dump was successfully committed.
	bool commit_metadata_dump() override;

	/// Save metadata to an emergency location (most likely an error occurred
	/// during the metadata dump).
	int emergency_saves() override;

	/// Performs the actual metadata dump to persistent location.
	/// @param dumpType -- type of the dump (foreground, background, etc.).
	/// @return false in case of error.
	uint8_t fs_storeall(DumpType dumpType) override;

	IMetadataDumper *dumper() override { return dumper_.get(); }
#endif  // #if !defined(METARESTORE) && !defined(METALOGGER)

	/// Returns a pointer to the underlying key-value engine.
	kv::IKVEngine *getKVEngine() { return kvConnector_->getKVEngine(); }

	/// Flush pending batched updates.
	///
	/// This backend batches metadata updates (e.g. chunk changes) and periodically flushes
	/// them to the KV store to avoid stalling the master event loop.
	///
	/// If flushAll is false, flushes at most one batch (kMaxUpdatesPerFlush_).
	/// If flushAll is true, flushes until empty (or until a commit fails).
	///
	/// The flushAll=true mode is intended for operations that must leave the KV store in a
	/// self-consistent state before proceeding (e.g. before persisting restore-relevant keys
	/// in fs_storeall()).
	///
	/// @param flushAll Whether to flush until the queue becomes empty.
	/// @return true on success; false if a KV commit failed while flushing.
	bool flushPendingUpdates(bool flushAll = false);

private:
	/// Initializes the vector of metadata sections for later loading
	void initSections();

	///  Initializes new metadata in FDB
	void initializeNewMetadataHeader();

	///  Registers observers/watchers on selected metadata properties
	void createConnections();

	// FS Load from FDB

	/// Loads all sections
	int fsLoad(bool ignoreFlag);

	/// Loads NODE_ metadata
	/// Loads all nodes from the KV store and reconstructs the in-memory filesystem node table.
	///
	/// Nodes are stored as `NODE_<nodeId>: <serializedNode>` entries. This method paginates
	/// through the full NODE_ keyspace starting from SPECIAL_INODE_ROOT, deserializes each
	/// node, and calls loadNode() to register it in gMetadata.
	///
	/// @param ignoreFlag Currently unused; reserved for consistency with other load functions.
	/// @return kOpSuccess on success, kOpFailure on error.
	int8_t loadNodes(bool ignoreFlag);

	/// Register a single deserialized node in the in-memory metadata structures.
	///
	/// Updates per-type counters (dirNodes, fileNodes, linkNodes), registers open sessions
	/// for file-like nodes, applies quota accounting, and inserts the node into the global
	/// node hash table and inode pool.
	///
	/// @param fsOpContext Filesystem operation context (transaction).
	/// @param node        Deserialized FSNode to register. Ownership is transferred to gMetadata.
	/// @return kOpSuccess on success, kOpFailure if the node type is unrecognized.
	int8_t loadNode(const FilesystemOperationContext &fsOpContext, FSNode *node);

	/// Loads FREE_ metadata
	/// Loads all free (detained) inodes from the KV store into the in-memory inode pool.
	///
	/// Free inodes are stored as `FREE_<nodeId>: <timestamp>` entries. This method paginates
	/// through the full FREE_ keyspace, calls `gMetadata->inodePool.detain()` for each entry,
	/// and then connects signal handlers so that future detain/release operations are
	/// automatically propagated to the KV store via FreeNodeUpdateEvent.
	///
	/// @param ignoreFlag Currently unused; reserved for consistency with other load functions.
	/// @return kOpSuccess on success, kOpFailure on error.
	int8_t loadFree(bool ignoreFlag);

	/// Loads EDGE_ metadata
	/// Loads all edges from the KV store and reconstructs the in-memory directory tree.
	///
	/// Edges are stored as `EDGE_<ParentId><Name>: <ChildId>` entries. This method paginates
	/// through the full EDGE_ keyspace, deserializes each entry, and calls loadEdge() to
	/// attach the child node to its parent directory (or to the trash/reserved containers
	/// when parentId is 0).
	///
	/// @param ignoreFlag When true, missing parent/child nodes are tolerated and orphan nodes
	///                   are attached to the root directory instead of causing a failure.
	/// @return kOpSuccess on success, kOpFailure on error.
	int8_t loadEdges(bool ignoreFlag);

	/// Load a single edge into the in-memory filesystem tree.
	///
	/// Depending on parentId:
	/// - parentId == 0: the child is inserted into the trash or reserved container based on
	///   its node type.
	/// - parentId != 0: the child is inserted into the parent directory's entry map, its
	///   parent back-pointer is set, and directory statistics are propagated upward.
	///
	/// On the first call, pass `init = true` to reset internal static state (the "current parent"
	/// tracker used to detect out-of-order edges).
	///
	/// @param fsOpContext Filesystem operation context (transaction).
	/// @param parentId   Inode of the parent directory (0 for trash/reserved).
	/// @param childId    Inode of the child node.
	/// @param name       Edge name (filename component).
	/// @param ignoreFlag When true, tolerate missing nodes (see loadEdges()).
	/// @param init       When true, reset static state without loading an edge.
	/// @return kOpSuccess on success, kOpFailure on error.
	int8_t loadEdge(const FilesystemOperationContext &fsOpContext, inode_t parentId,
	                inode_t childId, const std::string &name, bool ignoreFlag, bool init = false);

	/// Loads CHNK_ metadata
	/// Loads all chunks from the KV store and reconstructs the in-memory chunk table.
	///
	/// Chunks are stored as `CHNK_<chunkId><version>: <lockedTo><lockId>` entries. This method
	/// first restores the next-chunk-id generator from the KV store, then paginates through the
	/// full CHNK_ keyspace to load each chunk.
	///
	/// @param ignoreFlag Currently unused; reserved for consistency with other load functions.
	/// @return kOpSuccess on success, kOpFailure on error.
	int8_t loadChunks(bool ignoreFlag);

	/// Persist the "next chunk ID" restore key(s).
	///
	/// The forkless keeps `META_NEXT_CHUNK_ID` up to date so that the chunk ID generator
	/// can be restored without scanning the entire dataset.
	///
	/// Keys written/updated:
	/// - `META_NEXT_CHUNK_ID`: the current next chunk id (from chunk_get_next_id()).
	///
	/// @param transaction Transaction used to persist the keys.
	/// @return kOpSuccess on success, kOpFailure on failure.
	int8_t saveNextChunkId(kv::IReadWriteTransaction *transaction);

	/// Persist metadata global properties to the KV store within an existing transaction.
	///
	/// Writes the current values of `maxInodeId`, `metadataVersion`, and `nextSessionId` from
	/// gMetadata to FDB. These are the equivalent of the file header that MetadataBackendFile
	/// writes to metadata.sfs.
	///
	/// @param transaction Transaction used to persist the keys.
	/// @return kOpSuccess on success, kOpFailure on failure.
	int8_t saveMetadataKeys(kv::IReadWriteTransaction *transaction);

	/// Load metadata global properties from the KV store.
	///
	/// Reads `maxInodeId`, `metadataVersion`, and `nextSessionId` from FDB and populates the
	/// corresponding fields in gMetadata. These are the equivalent of the file header that
	/// MetadataBackendFile reads from metadata.sfs.
	///
	/// @return kOpSuccess on success, kOpFailure on failure.
	int8_t loadMetadataKeys();

	/// Persist all forkless restore-relevant keys.
	///
	/// This is called from fs_storeall() to write the keys below:
	/// - next chunk id keys (META_NEXT_CHUNK_ID),
	///
	/// Important: this must be done only after pending metadata updates have been fully flushed
	/// (flushPendingUpdates(flushAll=true)), otherwise the KV store can temporarily contain
	/// partially persisted metadata state but "finalized" restore keys.
	///
	/// @return kOpSuccess on success, kOpFailure on failure.
	int8_t saveMetadataKeys();

	/// Enqueue a node update event to the metadata writer.
	///
	/// Called when a filesystem node is created or modified. The event serializes the node and
	/// writes `NODE_<nodeId>: <serializedNode>` to the KV store on the next flush.
	///
	/// @param node Pointer to the modified filesystem node.
	void onNodeChanged(FSNode *node);

	/// Enqueue a node removal event to the metadata writer.
	///
	/// Called when a filesystem node is removed. The event removes the `NODE_<nodeId>` key
	/// from the KV store on the next flush.
	///
	/// @param nodeId Inode of the removed filesystem node.
	void onNodeRemoved(inode_t nodeId);

	/// Enqueue an edge update event to the metadata writer.
	///
	/// Called when a directory entry is created or renamed. The event writes
	/// `EDGE_<parentId><name>: <childId>` to the KV store on the next flush.
	///
	/// @param parentId Inode of the parent directory.
	/// @param childId  Inode of the child node.
	/// @param name     Edge name (filename component).
	void onEdgeChanged(inode_t parentId, inode_t childId, const HString &name);

	/// Enqueue an edge removal event to the metadata writer.
	///
	/// Called when a directory entry is unlinked. The event removes the `EDGE_<parentId><name>` key
	/// from the KV store on the next flush.
	///
	/// @param parentId Inode of the parent directory.
	/// @param name     Edge name (filename component) to remove.
	void onEdgeRemoved(inode_t parentId, const HString &name);

	/// Returns next chunk ID value from the KV store.
	///
	/// @return Next chunk id, or 0 if META_NEXT_CHUNK_ID key is not found in the KV store.
	uint64_t getNextChunkId();

	/// Provides connection to the key-value store (FoundationDB for this implementation)
	std::shared_ptr<IKVConnector> kvConnector_;

	/// Metadata sections with their loading functions
	std::vector<MetadataSectionFDB> metadataSections_;

#if !defined(METARESTORE) && !defined(METALOGGER)
	std::unique_ptr<IMetadataDumper> dumper_;
#endif  // #ifndef METARESTORE

	/// Metadata writer for all metadata updates
	std::unique_ptr<MetadataWriterFDB> metadataWriter_;

#ifndef METARESTORE
	/// Bootstrapper for metadata sections
	std::unique_ptr<MetadataSectionBootstrapFDB> sectionBootstrapper_ = nullptr;
#endif  // #ifndef METARESTORE
};
