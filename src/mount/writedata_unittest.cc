/*

   Copyright 2026 Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

#include "errors/saunafs_error_codes.h"
#include "mount/writedata.h"

using namespace WriteAlgorithm;

namespace {

constexpr inode_t kInode = 42;
constexpr uint32_t kCacheSizeMB = 16;
constexpr uint32_t kRetries = 1;
constexpr uint32_t kWriteWindowSize = 1;
constexpr uint32_t kChunkserverTimeoutMs = 100;
constexpr uint32_t kCachePerInodePercentage = 100;
constexpr uint32_t kWaveTimeoutMs = 100;
constexpr uint32_t kMaxChunksInParallel = 1;

/// An inode has to be released once nothing references it and its writes have finished. Those two
/// can happen in either order, because a failed write makes write_data_flush() stop waiting, so
/// the last close can overtake a write that is still outstanding.
class WriteDataInodeLifetimeTest : public ::testing::Test {
protected:
	void SetUp() override {
		// No write workers, so a queued chunk write stays outstanding for as long as the test
		// needs it to. That is what makes both orderings reproducible without a cluster.
		write_data_init(kCacheSizeMB, kRetries, /*workers=*/0, kWriteWindowSize,
		                kChunkserverTimeoutMs, kCachePerInodePercentage, kWaveTimeoutMs,
		                kMaxChunksInParallel, /*useWriteFlushPacket=*/false);
	}

	void TearDown() override { write_data_term(); }

	/// Opens the inode and leaves exactly one chunk write outstanding on it.
	void *openWithPendingChunk() {
		void *vid = write_data_new(kInode);
		EXPECT_NE(vid, nullptr);
		std::vector<uint8_t> buffer(1024, 0);
		write_data(vid, 0, buffer.size(), buffer.data(), buffer.size());
		EXPECT_TRUE(testhooks::inodeDataExists(kInode));
		return vid;
	}
};

// The write finishes first, so the last close is what makes the inode releasable.
TEST_F(WriteDataInodeLifetimeTest, ReleasesInodeWhenWriteFinishesBeforeLastClose) {
	void *vid = openWithPendingChunk();

	ASSERT_TRUE(testhooks::completeOnePendingChunk(SAUNAFS_ERROR_QUOTA));
	EXPECT_EQ(write_data_end(vid), SAUNAFS_ERROR_QUOTA);

	EXPECT_FALSE(testhooks::inodeDataExists(kInode))
	    << "inode still in the writer's table after its last reference went away";
}

// The last close comes first, so the write finishing is what makes the inode releasable. This is
// the ordering a quota rejection produces: the error latched on the inode makes the flush in
// write_data_end() return without waiting for the chunk write still in progress.
TEST_F(WriteDataInodeLifetimeTest, ReleasesInodeWhenLastCloseBeatsTheWrite) {
	void *vid = openWithPendingChunk();

	testhooks::latchInodeStatus(vid, SAUNAFS_ERROR_QUOTA);
	EXPECT_EQ(write_data_end(vid), SAUNAFS_ERROR_QUOTA);

	// Nothing references the inode now, but it still owns an outstanding write, so this is not
	// yet the moment it can be freed.
	ASSERT_TRUE(testhooks::inodeDataExists(kInode));

	// Once that write finishes nothing is left to hold the inode. Before the release was made to
	// happen on this path too, nothing noticed: the inode stayed in the table with its error
	// latched, and every later operation on it inherited that error for the life of the mount.
	ASSERT_TRUE(testhooks::completeOnePendingChunk(SAUNAFS_ERROR_QUOTA));

	EXPECT_FALSE(testhooks::inodeDataExists(kInode))
	    << "inode leaked with its error latched; a later truncate or write on it would fail "
	       "without the request ever reaching the master";
}

}  // namespace
