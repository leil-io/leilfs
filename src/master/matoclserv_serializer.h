/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
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

#include <cstdint>
#include <vector>

#include "common/attributes.h"
#include "common/chunk_type_with_address.h"
#include "common/type_defs.h"
#include "protocol/packet.h"

// chunkDelayedOperation types
enum DelayedChunkOperationType : std::uint32_t {
	FUSE_WRITE,           /// Reply to FUSE_WRITE_CHUNK is delayed
	FUSE_TRUNCATE,        /// Reply to FUSE_TRUNCATE which does not require writing is delayed
	FUSE_TRUNCATE_BEGIN,  /// Reply to FUSE_TRUNCATE which does require writing is delayed
	FUSE_TRUNCATE_END     /// Reply to FUSE_TRUNCATE_END is delayed
};

class PacketSerializer {
public:
	/// Factory method to get a serializer instance for a given packet type and client version.
	/// @param type Packet type
	/// @param version Client version
	static const PacketSerializer *getSerializer(PacketHeader::Type type, uint32_t version);

	/// Default constructor
	PacketSerializer() = default;

	/// Unneeded copy and move constructors and assignment operators
	PacketSerializer(const PacketSerializer &) = delete;
	PacketSerializer &operator=(const PacketSerializer &) = delete;
	PacketSerializer(PacketSerializer &&) = delete;
	PacketSerializer &operator=(PacketSerializer &&) = delete;

	/// Virtual destructor for polymorphic base class
	virtual ~PacketSerializer() = default;

	virtual bool isSaunaFsPacketSerializer() const = 0;

	virtual void serializeFuseReadChunk(std::vector<uint8_t> &packetBuffer, uint32_t messageId,
	                                    uint8_t status) const = 0;
	virtual void serializeFuseReadChunk(
	    std::vector<uint8_t> &packetBuffer, uint32_t messageId, uint64_t fileLength,
	    uint64_t chunkId, uint32_t chunkVersion,
	    const std::vector<ChunkTypeWithAddress> &chunkCopies) const = 0;
	virtual void deserializeFuseReadChunk(const std::vector<uint8_t> &packetBuffer,
	                                      uint32_t &messageId, inode_t &inode,
	                                      uint32_t &chunkIndex) const = 0;

	virtual void serializeFuseWriteChunk(std::vector<uint8_t> &packetBuffer, uint32_t messageId,
	                                     uint8_t status) const = 0;
	virtual void serializeFuseWriteChunk(
	    std::vector<uint8_t> &packetBuffer, uint32_t messageId, uint64_t fileLength,
	    uint64_t chunkId, uint32_t chunkVersion, uint32_t lockId,
	    const std::vector<ChunkTypeWithAddress> &chunkCopies) const = 0;
	/// The grant-bearing reply. Only the modern serializer can carry it on the wire; the
	/// legacy and std-xor shapes fall back to the grantless reply, and their clients never
	/// hold a grant to begin with.
	virtual void serializeFuseWriteChunk(
	    std::vector<uint8_t> &packetBuffer, uint32_t messageId, uint64_t fileLength,
	    uint64_t chunkId, uint32_t chunkVersion, uint32_t lockId, uint64_t grantGeneration,
	    uint64_t grantRandom, const std::vector<ChunkTypeWithAddress> &chunkCopies) const = 0;
	virtual void deserializeFuseWriteChunk(const std::vector<uint8_t> &packetBuffer,
	                                       uint32_t &messageId, inode_t &inode,
	                                       uint32_t &chunkIndex, uint32_t &lockId) const = 0;

	virtual void serializeFuseWriteChunkEnd(std::vector<uint8_t> &packetBuffer, uint32_t messageId,
	                                        uint8_t status) const = 0;
	virtual void deserializeFuseWriteChunkEnd(const std::vector<uint8_t> &packetBuffer,
	                                          uint32_t &messageId, uint64_t &chunkId,
	                                          uint32_t &lockId, inode_t &inode,
	                                          uint64_t &fileLength) const = 0;

	virtual void serializeFuseTruncate(std::vector<uint8_t> &packetBuffer,
	                                   uint32_t type /* FUSE_TRUNCATE | FUSE_TRUNCATE_END*/,
	                                   uint32_t messageId, uint8_t status) const = 0;
	virtual void serializeFuseTruncate(std::vector<uint8_t> &packetBuffer,
	                                   uint32_t type /* FUSE_TRUNCATE | FUSE_TRUNCATE_END*/,
	                                   uint32_t messageId, const Attributes &attributes) const = 0;
	virtual void deserializeFuseTruncate(std::vector<uint8_t> &packetBuffer, uint32_t &messageId,
	                                     inode_t &inode, bool &isOpened, uint32_t &uid,
	                                     uint32_t &gid, uint64_t &length) const = 0;
};

class LegacyPacketSerializer : public PacketSerializer {
public:
	bool isSaunaFsPacketSerializer() const override { return false; }

	void serializeFuseReadChunk(std::vector<uint8_t> &packetBuffer, uint32_t messageId,
	                            uint8_t status) const override;

	void serializeFuseReadChunk(
	    std::vector<uint8_t> &packetBuffer, uint32_t messageId, uint64_t fileLength,
	    uint64_t chunkId, uint32_t chunkVersion,
	    const std::vector<ChunkTypeWithAddress> &chunkCopies) const override;

	void deserializeFuseReadChunk(const std::vector<uint8_t> &packetBuffer, uint32_t &messageId,
	                              inode_t &inode, uint32_t &chunkIndex) const override;

	void serializeFuseWriteChunk(std::vector<uint8_t> &packetBuffer, uint32_t messageId,
	                             uint8_t status) const override;

	void serializeFuseWriteChunk(
	    std::vector<uint8_t> &packetBuffer, uint32_t messageId, uint64_t fileLength,
	    uint64_t chunkId, uint32_t chunkVersion, uint32_t lockId,
	    const std::vector<ChunkTypeWithAddress> &chunkCopies) const override;

	void serializeFuseWriteChunk(
	    std::vector<uint8_t> &packetBuffer, uint32_t messageId, uint64_t fileLength,
	    uint64_t chunkId, uint32_t chunkVersion, uint32_t lockId, uint64_t grantGeneration,
	    uint64_t grantRandom,
	    const std::vector<ChunkTypeWithAddress> &chunkCopies) const override;

	void deserializeFuseWriteChunk(const std::vector<uint8_t> &packetBuffer, uint32_t &messageId,
	                               inode_t &inode, uint32_t &chunkIndex,
	                               uint32_t &lockId) const override;

	void serializeFuseWriteChunkEnd(std::vector<uint8_t> &packetBuffer, uint32_t messageId,
	                                uint8_t status) const override;

	void deserializeFuseWriteChunkEnd(const std::vector<uint8_t> &packetBuffer, uint32_t &messageId,
	                                  uint64_t &chunkId, uint32_t &lockId, inode_t &inode,
	                                  uint64_t &fileLength) const override;

	void serializeFuseTruncate(std::vector<uint8_t> &packetBuffer, uint32_t type,
	                           uint32_t messageId, uint8_t status) const override;

	void serializeFuseTruncate(std::vector<uint8_t> &packetBuffer, uint32_t type,
	                           uint32_t messageId, const Attributes &attributes) const override;

	void deserializeFuseTruncate(std::vector<uint8_t> &packetBuffer, uint32_t &messageId,
	                             inode_t &inode, bool &isOpened, uint32_t &uid, uint32_t &gid,
	                             uint64_t &length) const override;

private:
	/// Extracts network addresses of standard chunk copies from a list of all chunk copies.
	/// @param allCopies List of all chunk copies with their addresses
	/// @param standardCopies Output vector to store the addresses of standard chunk copies
	static void getStandardChunkCopies(const std::vector<ChunkTypeWithAddress> &allCopies,
	                                   std::vector<NetworkAddress> &standardCopies);
};

class SaunaFsPacketSerializer : public PacketSerializer {
public:
	bool isSaunaFsPacketSerializer() const override { return true; }

	void serializeFuseReadChunk(std::vector<uint8_t> &packetBuffer, uint32_t messageId,
	                            uint8_t status) const override;

	void serializeFuseReadChunk(
	    std::vector<uint8_t> &packetBuffer, uint32_t messageId, uint64_t fileLength,
	    uint64_t chunkId, uint32_t chunkVersion,
	    const std::vector<ChunkTypeWithAddress> &chunkCopies) const override;

	void deserializeFuseReadChunk(const std::vector<uint8_t> &packetBuffer, uint32_t &messageId,
	                              inode_t &inode, uint32_t &chunkIndex) const override;

	void serializeFuseWriteChunk(std::vector<uint8_t> &packetBuffer, uint32_t messageId,
	                             uint8_t status) const override;

	void serializeFuseWriteChunk(
	    std::vector<uint8_t> &packetBuffer, uint32_t messageId, uint64_t fileLength,
	    uint64_t chunkId, uint32_t chunkVersion, uint32_t lockId,
	    const std::vector<ChunkTypeWithAddress> &chunkCopies) const override;

	void serializeFuseWriteChunk(
	    std::vector<uint8_t> &packetBuffer, uint32_t messageId, uint64_t fileLength,
	    uint64_t chunkId, uint32_t chunkVersion, uint32_t lockId, uint64_t grantGeneration,
	    uint64_t grantRandom,
	    const std::vector<ChunkTypeWithAddress> &chunkCopies) const override;

	void deserializeFuseWriteChunk(const std::vector<uint8_t> &packetBuffer, uint32_t &messageId,
	                               inode_t &inode, uint32_t &chunkIndex,
	                               uint32_t &lockId) const override;

	void serializeFuseWriteChunkEnd(std::vector<uint8_t> &packetBuffer, uint32_t messageId,
	                                uint8_t status) const override;

	void deserializeFuseWriteChunkEnd(const std::vector<uint8_t> &packetBuffer, uint32_t &messageId,
	                                  uint64_t &chunkId, uint32_t &lockId, inode_t &inode,
	                                  uint64_t &fileLength) const override;

	void serializeFuseTruncate(std::vector<uint8_t> &packetBuffer, uint32_t type,
	                           uint32_t messageId, uint8_t status) const override;

	void serializeFuseTruncate(std::vector<uint8_t> &packetBuffer, uint32_t type,
	                           uint32_t messageId, const Attributes &attributes) const override;

	void deserializeFuseTruncate(std::vector<uint8_t> &packetBuffer, uint32_t &messageId,
	                             inode_t &inode, bool &isOpened, uint32_t &uid, uint32_t &gid,
	                             uint64_t &length) const override;
};

class SaunaFsStdXorPacketSerializer : public SaunaFsPacketSerializer {
public:
	void serializeFuseReadChunk(
	    std::vector<uint8_t> &packetBuffer, uint32_t messageId, uint64_t fileLength,
	    uint64_t chunkId, uint32_t chunkVersion,
	    const std::vector<ChunkTypeWithAddress> &chunkCopies) const override;

	void serializeFuseWriteChunk(
	    std::vector<uint8_t> &packetBuffer, uint32_t messageId, uint64_t fileLength,
	    uint64_t chunkId, uint32_t chunkVersion, uint32_t lockId,
	    const std::vector<ChunkTypeWithAddress> &chunkCopies) const override;

	void serializeFuseWriteChunk(
	    std::vector<uint8_t> &packetBuffer, uint32_t messageId, uint64_t fileLength,
	    uint64_t chunkId, uint32_t chunkVersion, uint32_t lockId, uint64_t grantGeneration,
	    uint64_t grantRandom,
	    const std::vector<ChunkTypeWithAddress> &chunkCopies) const override;
};
