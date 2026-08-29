/*
   Copyright 2005-2017 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2017 Skytechnology sp. z o.o.
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

#include "common/platform.h"

#include "master/matoclserv_serializer.h"

#include "common/legacy_vector.h"
#include "common/saunafs_version.h"
#include "protocol/cltoma.h"
#include "protocol/matocl.h"

const PacketSerializer *PacketSerializer::getSerializer(PacketHeader::Type type, uint32_t version) {
	sassert((type >= PacketHeader::kMinSauPacketType && type <= PacketHeader::kMaxSauPacketType) ||
	        type <= PacketHeader::kMaxOldPacketType);
	if (type <= PacketHeader::kMaxOldPacketType) {
		static LegacyPacketSerializer singleton;
		return &singleton;
	}

	static SaunaFsPacketSerializer singleton;
	if (version < kFirstECVersion) {
		static SaunaFsStdXorPacketSerializer singletonStdXor;
		return &singletonStdXor;
	}

	return &singleton;
}

// LegacyPacketSerializer implementations
void LegacyPacketSerializer::serializeFuseReadChunk(std::vector<uint8_t> &packetBuffer,
                                                    uint32_t messageId, uint8_t status) const {
	serializeLegacyPacket(packetBuffer, MATOCL_FUSE_READ_CHUNK, messageId, status);
}

void LegacyPacketSerializer::serializeFuseReadChunk(
    std::vector<uint8_t> &packetBuffer, uint32_t messageId, uint64_t fileLength, uint64_t chunkId,
    uint32_t chunkVersion, const std::vector<ChunkTypeWithAddress> &chunkCopies) const {
	LegacyVector<NetworkAddress> standardChunkCopies;
	getStandardChunkCopies(chunkCopies, standardChunkCopies);
	serializeLegacyPacket(packetBuffer, MATOCL_FUSE_READ_CHUNK, messageId, fileLength, chunkId,
	                      chunkVersion, standardChunkCopies);
}

void LegacyPacketSerializer::deserializeFuseReadChunk(const std::vector<uint8_t> &packetBuffer,
                                                      uint32_t &messageId, inode_t &inode,
                                                      uint32_t &chunkIndex) const {
	deserializeAllLegacyPacketDataNoHeader(packetBuffer, messageId, inode, chunkIndex);
}

void LegacyPacketSerializer::serializeFuseWriteChunk(std::vector<uint8_t> &packetBuffer,
                                                     uint32_t messageId, uint8_t status) const {
	serializeLegacyPacket(packetBuffer, MATOCL_FUSE_WRITE_CHUNK, messageId, status);
}

void LegacyPacketSerializer::serializeFuseWriteChunk(
    std::vector<uint8_t> &packetBuffer, uint32_t messageId, uint64_t fileLength, uint64_t chunkId,
    uint32_t chunkVersion, uint32_t lockId,
    const std::vector<ChunkTypeWithAddress> &chunkCopies) const {
	sassert(lockId == 1);
	LegacyVector<NetworkAddress> standardChunkCopies;
	getStandardChunkCopies(chunkCopies, standardChunkCopies);
	serializeLegacyPacket(packetBuffer, MATOCL_FUSE_WRITE_CHUNK, messageId, fileLength, chunkId,
	                      chunkVersion, standardChunkCopies);
}

void LegacyPacketSerializer::deserializeFuseWriteChunk(const std::vector<uint8_t> &packetBuffer,
                                                       uint32_t &messageId, inode_t &inode,
                                                       uint32_t &chunkIndex,
                                                       uint32_t &lockId) const {
	deserializeAllLegacyPacketDataNoHeader(packetBuffer, messageId, inode, chunkIndex);
	lockId = 1;
}

void LegacyPacketSerializer::serializeFuseWriteChunkEnd(std::vector<uint8_t> &packetBuffer,
                                                        uint32_t messageId, uint8_t status) const {
	serializeLegacyPacket(packetBuffer, MATOCL_FUSE_WRITE_CHUNK_END, messageId, status);
}

void LegacyPacketSerializer::deserializeFuseWriteChunkEnd(const std::vector<uint8_t> &packetBuffer,
                                                          uint32_t &messageId, uint64_t &chunkId,
                                                          uint32_t &lockId, inode_t &inode,
                                                          uint64_t &fileLength) const {
	deserializeAllLegacyPacketDataNoHeader(packetBuffer, messageId, chunkId, inode, fileLength);
	lockId = 1;
}

void LegacyPacketSerializer::serializeFuseTruncate(std::vector<uint8_t> &packetBuffer,
                                                   uint32_t type, uint32_t messageId,
                                                   uint8_t status) const {
	sassert(type == FUSE_TRUNCATE || type == FUSE_TRUNCATE_END);
	if (type == FUSE_TRUNCATE) {
		serializeLegacyPacket(packetBuffer, MATOCL_FUSE_TRUNCATE, messageId, status);
	} else {
		// this should never happen, so do anything
		serializeLegacyPacket(packetBuffer, MATOCL_FUSE_TRUNCATE, messageId,
		                      uint8_t(SAUNAFS_ERROR_ENOTSUP));
	}
}

void LegacyPacketSerializer::serializeFuseTruncate(std::vector<uint8_t> &packetBuffer,
                                                   uint32_t type, uint32_t messageId,
                                                   const Attributes &attributes) const {
	sassert(type == FUSE_TRUNCATE || type == FUSE_TRUNCATE_END);
	if (type == FUSE_TRUNCATE) {
		serializeLegacyPacket(packetBuffer, MATOCL_FUSE_TRUNCATE, messageId, attributes);
	} else {
		// this should never happen, so do anything
		serializeLegacyPacket(packetBuffer, MATOCL_FUSE_TRUNCATE, messageId,
		                      uint8_t(SAUNAFS_ERROR_ENOTSUP));
	}
}

void LegacyPacketSerializer::deserializeFuseTruncate(std::vector<uint8_t> &packetBuffer,
                                                     uint32_t &messageId, inode_t &inode,
                                                     bool &isOpened, uint32_t &uid, uint32_t &gid,
                                                     uint64_t &length) const {
	deserializeAllLegacyPacketDataNoHeader(packetBuffer, messageId, inode, isOpened, uid, gid,
	                                       length);
}

// SaunaFsStdXorPacketSerializer overrides for chunk filtering
void SaunaFsStdXorPacketSerializer::serializeFuseReadChunk(
    std::vector<uint8_t> &packetBuffer, uint32_t messageId, uint64_t fileLength, uint64_t chunkId,
    uint32_t chunkVersion, const std::vector<ChunkTypeWithAddress> &chunkCopies) const {
	std::vector<legacy::ChunkTypeWithAddress> chunk_copies;
	for (const auto &part : chunkCopies) {
		if ((int)part.chunk_type.getSliceType() >= Goal::Slice::Type::kECFirst) { continue; }
		chunk_copies.emplace_back(part.address, (legacy::ChunkPartType)part.chunk_type);
	}
	matocl::fuseReadChunk::serialize(packetBuffer, messageId, fileLength, chunkId, chunkVersion,
	                                 chunk_copies);
}

// SaunaFsPacketSerializer implementations
void SaunaFsPacketSerializer::serializeFuseReadChunk(std::vector<uint8_t> &packetBuffer,
                                                     uint32_t messageId, uint8_t status) const {
	matocl::fuseReadChunk::serialize(packetBuffer, messageId, status);
}

void SaunaFsPacketSerializer::serializeFuseReadChunk(
    std::vector<uint8_t> &packetBuffer, uint32_t messageId, uint64_t fileLength, uint64_t chunkId,
    uint32_t chunkVersion, const std::vector<ChunkTypeWithAddress> &chunkCopies) const {
	matocl::fuseReadChunk::serialize(packetBuffer, messageId, fileLength, chunkId, chunkVersion,
	                                 chunkCopies);
}

void SaunaFsPacketSerializer::deserializeFuseReadChunk(const std::vector<uint8_t> &packetBuffer,
                                                       uint32_t &messageId, inode_t &inode,
                                                       uint32_t &chunkIndex) const {
	cltoma::fuseReadChunk::deserialize(packetBuffer, messageId, inode, chunkIndex);
}

void SaunaFsPacketSerializer::serializeFuseWriteChunk(std::vector<uint8_t> &packetBuffer,
                                                      uint32_t messageId, uint8_t status) const {
	matocl::fuseWriteChunk::serialize(packetBuffer, messageId, status);
}

void SaunaFsPacketSerializer::serializeFuseWriteChunk(
    std::vector<uint8_t> &packetBuffer, uint32_t messageId, uint64_t fileLength, uint64_t chunkId,
    uint32_t chunkVersion, uint32_t lockId,
    const std::vector<ChunkTypeWithAddress> &chunkCopies) const {
	matocl::fuseWriteChunk::serialize(packetBuffer, messageId, fileLength, chunkId, chunkVersion,
	                                  lockId, chunkCopies);
}

void SaunaFsPacketSerializer::serializeFuseWriteChunk(
    std::vector<uint8_t> &packetBuffer, uint32_t messageId, uint64_t fileLength, uint64_t chunkId,
    uint32_t chunkVersion, uint32_t lockId, uint64_t grantGeneration, uint64_t grantRandom,
    const std::vector<ChunkTypeWithAddress> &chunkCopies) const {
	matocl::fuseWriteChunk::serialize(packetBuffer, messageId, fileLength, chunkId, chunkVersion,
	                                  lockId, grantGeneration, grantRandom, chunkCopies);
}

void LegacyPacketSerializer::serializeFuseWriteChunk(
    std::vector<uint8_t> &packetBuffer, uint32_t messageId, uint64_t fileLength, uint64_t chunkId,
    uint32_t chunkVersion, uint32_t lockId, uint64_t /*grantGeneration*/,
    uint64_t /*grantRandom*/, const std::vector<ChunkTypeWithAddress> &chunkCopies) const {
	// A legacy client cannot hold a grant; fall back to the grantless shape.
	serializeFuseWriteChunk(packetBuffer, messageId, fileLength, chunkId, chunkVersion, lockId,
	                        chunkCopies);
}

void SaunaFsStdXorPacketSerializer::serializeFuseWriteChunk(
    std::vector<uint8_t> &packetBuffer, uint32_t messageId, uint64_t fileLength, uint64_t chunkId,
    uint32_t chunkVersion, uint32_t lockId, uint64_t /*grantGeneration*/,
    uint64_t /*grantRandom*/, const std::vector<ChunkTypeWithAddress> &chunkCopies) const {
	// A std-xor era client cannot hold a grant either; its own override keeps the shape.
	serializeFuseWriteChunk(packetBuffer, messageId, fileLength, chunkId, chunkVersion, lockId,
	                        chunkCopies);
}

void SaunaFsPacketSerializer::deserializeFuseWriteChunk(const std::vector<uint8_t> &packetBuffer,
                                                        uint32_t &messageId, inode_t &inode,
                                                        uint32_t &chunkIndex,
                                                        uint32_t &lockId) const {
	cltoma::fuseWriteChunk::deserialize(packetBuffer, messageId, inode, chunkIndex, lockId);
}

void SaunaFsPacketSerializer::serializeFuseWriteChunkEnd(std::vector<uint8_t> &packetBuffer,
                                                         uint32_t messageId, uint8_t status) const {
	matocl::fuseWriteChunkEnd::serialize(packetBuffer, messageId, status);
}

void SaunaFsPacketSerializer::deserializeFuseWriteChunkEnd(const std::vector<uint8_t> &packetBuffer,
                                                           uint32_t &messageId, uint64_t &chunkId,
                                                           uint32_t &lockId, inode_t &inode,
                                                           uint64_t &fileLength) const {
	cltoma::fuseWriteChunkEnd::deserialize(packetBuffer, messageId, chunkId, lockId, inode,
	                                       fileLength);
}

void SaunaFsPacketSerializer::serializeFuseTruncate(std::vector<uint8_t> &packetBuffer,
                                                    uint32_t type, uint32_t messageId,
                                                    uint8_t status) const {
	sassert(type == FUSE_TRUNCATE || type == FUSE_TRUNCATE_END);
	if (type == FUSE_TRUNCATE) {
		matocl::fuseTruncate::serialize(packetBuffer, messageId, status);
	} else {
		matocl::fuseTruncateEnd::serialize(packetBuffer, messageId, status);
	}
}

void SaunaFsPacketSerializer::serializeFuseTruncate(std::vector<uint8_t> &packetBuffer,
                                                    uint32_t type, uint32_t messageId,
                                                    const Attributes &attributes) const {
	sassert(type == FUSE_TRUNCATE || type == FUSE_TRUNCATE_END);
	if (type == FUSE_TRUNCATE) {
		matocl::fuseTruncate::serialize(packetBuffer, messageId, attributes);
	} else {
		matocl::fuseTruncateEnd::serialize(packetBuffer, messageId, attributes);
	}
}

void SaunaFsPacketSerializer::deserializeFuseTruncate(std::vector<uint8_t> &packetBuffer,
                                                      uint32_t &messageId, inode_t &inode,
                                                      bool &isOpened, uint32_t &uid, uint32_t &gid,
                                                      uint64_t &length) const {
	cltoma::fuseTruncate::deserialize(packetBuffer, messageId, inode, isOpened, uid, gid, length);
}

void SaunaFsStdXorPacketSerializer::serializeFuseWriteChunk(
    std::vector<uint8_t> &packetBuffer, uint32_t messageId, uint64_t fileLength, uint64_t chunkId,
    uint32_t chunkVersion, uint32_t lockId,
    const std::vector<ChunkTypeWithAddress> &chunkCopies) const {
	std::vector<legacy::ChunkTypeWithAddress> chunk_copies;
	for (const auto &part : chunkCopies) {
		if ((int)part.chunk_type.getSliceType() >= Goal::Slice::Type::kECFirst) { continue; }
		chunk_copies.emplace_back(part.address, (legacy::ChunkPartType)part.chunk_type);
	}
	matocl::fuseWriteChunk::serialize(packetBuffer, messageId, fileLength, chunkId, chunkVersion,
	                                  lockId, chunk_copies);
}

void LegacyPacketSerializer::getStandardChunkCopies(
    const std::vector<ChunkTypeWithAddress> &allCopies,
    std::vector<NetworkAddress> &standardCopies) {
	sassert(standardCopies.empty());

	for (const auto &chunkCopy : allCopies) {
		if (slice_traits::isStandard(chunkCopy.chunk_type)) {
			standardCopies.push_back(chunkCopy.address);
		}
	}
}
