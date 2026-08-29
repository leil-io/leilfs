/*
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "common/chunk_type_with_address.h"
#include "common/type_defs.h"
#include "slogger/slogger.h"

struct ChunkLocationInfo {
	typedef std::vector<ChunkTypeWithAddress> ChunkLocations;

	uint64_t chunkId;
	uint32_t version;
	uint64_t fileLength;
	ChunkLocations locations;

	ChunkLocationInfo()
			: chunkId(0),
			  version(0),
			  fileLength(0) {
	}

	ChunkLocationInfo(
			const uint64_t chunkId,
			const uint32_t version,
			const uint64_t fileLength,
			const ChunkLocations locations) :
		chunkId(chunkId),
		version(version),
		fileLength(fileLength),
		locations(locations) {
	}

	bool isEmptyChunk() const {
		return chunkId == 0;
	}
};

// Intended to be instantiated per descriptor.
// May cache locations of previously queried chunks.
// Thread safe.
class ReadChunkLocator {
public:
	ReadChunkLocator(const ReadChunkLocator&) = delete;
	ReadChunkLocator() {}

	std::shared_ptr<const ChunkLocationInfo> locateChunk(inode_t inode, uint32_t index);
	void invalidateCache(inode_t inode, uint32_t index);

private:
	inode_t inode_;
	uint32_t index_;

	std::shared_ptr<const ChunkLocationInfo> cache_ = nullptr;
	std::mutex mutex_;
};

class WriteChunkLocator {
public:
	WriteChunkLocator() {}

	virtual ~WriteChunkLocator() {
		try {
			if (lockId_) {
				unlockChunk();
			}
		} catch (Exception& ex) {
			safs_pretty_syslog(LOG_WARNING,
					"unlocking chunk error, inode: %" PRIiNode ", index: %" PRIu32 " - %s",
					inode_, index_, ex.what());
		}
	}

	virtual void locateAndLockChunk(inode_t inode, uint32_t index);
	virtual void unlockChunk();

	inline uint64_t fileLength() const {
		return locationInfo_.fileLength;
	}

	uint32_t chunkIndex() {
		return index_;
	}

	void updateFileLength(uint64_t fileLength) {
		locationInfo_.fileLength = fileLength;
	}

	const ChunkLocationInfo& locationInfo() const {
		return locationInfo_;
	}

	virtual bool shouldReset() const {
		return true;
	}

protected:
	WriteChunkLocator(inode_t inode, uint32_t index, uint32_t lockId)
			: inode_(inode),
			  index_(index),
			  lockId_(lockId) {
	}

	inode_t inode_ = 0;
	uint32_t index_ = 0;
	uint32_t lockId_ = 0;
	/// Write grant identity the metadata server returned beside the lock; zero when the
	/// reply predates grants. Held here so every executor for this locked chunk presents
	/// the same pair.
	uint64_t grantGeneration_ = 0;
	uint64_t grantRandom_ = 0;
	ChunkLocationInfo locationInfo_;

public:
	uint64_t grantGeneration() const { return grantGeneration_; }
	uint64_t grantRandom() const { return grantRandom_; }
};

// Fit for truncating xor chunks down when master, not client, locks a chunk
class TruncateWriteChunkLocator : public WriteChunkLocator {
public:
	// Locator is created for single operation
	explicit TruncateWriteChunkLocator(inode_t inode, uint32_t index, uint32_t lockId,
	                                   uint64_t targetSize)
		: WriteChunkLocator(inode, index, lockId) {
			targetSize_ = targetSize;
	}

	~TruncateWriteChunkLocator() {
		// Remove information about the lock to prevent ~WriteChunkLocator from unlocking the chunk
		lockId_ = 0;
	}

	// In this case a chunk is unlocked by master so this locator will be simply destroyed
	void unlockChunk() {}

	bool shouldReset() const {
		return locationInfo_.fileLength == targetSize_;
	}

private:
	// Target offset to fill with zeros up to
	uint64_t targetSize_;
};
