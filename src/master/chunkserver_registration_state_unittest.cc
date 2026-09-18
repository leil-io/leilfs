/*
   Copyright 2026      Leil Storage OÜ

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

#include "common/platform.h"

#include <chrono>

#include <gtest/gtest.h>

#include "common/saunafs_version.h"
#include "master/chunkserver_registration_state.h"

namespace {

const chunkserver::ChunkserverId kIdentity = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                                              0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe};

}  // namespace

TEST(ChunkserverRegistrationStateTests, RequestsIdentityForCompatiblePeer) {
	ChunkserverRegistrationState state;

	EXPECT_TRUE(state.begin(true, kFirstVersionWithChunkserverIdentity));
	EXPECT_TRUE(state.identityPending());
	EXPECT_FALSE(state.identity().has_value());
}

TEST(ChunkserverRegistrationStateTests, SkipsIdentityWhenDisabledOrPeerIsOld) {
	ChunkserverRegistrationState disabled;
	ChunkserverRegistrationState oldPeer;

	EXPECT_TRUE(disabled.begin(false, kFirstVersionWithChunkserverIdentity));
	EXPECT_FALSE(disabled.identityPending());
	EXPECT_TRUE(oldPeer.begin(true, kFirstVersionWithChunkserverIdentity - 1));
	EXPECT_FALSE(oldPeer.identityPending());
}

TEST(ChunkserverRegistrationStateTests, AcceptsOneNonzeroRequestedIdentity) {
	ChunkserverRegistrationState state;
	ASSERT_TRUE(state.begin(true, kFirstVersionWithChunkserverIdentity));

	EXPECT_FALSE(state.markRegistrationComplete());
	EXPECT_TRUE(state.acceptIdentity(kIdentity));
	EXPECT_FALSE(state.identityPending());
	ASSERT_TRUE(state.identity().has_value());
	EXPECT_EQ(*state.identity(), kIdentity);
	EXPECT_TRUE(state.markRegistrationComplete());
	EXPECT_FALSE(state.acceptIdentity(kIdentity));
}

TEST(ChunkserverRegistrationStateTests, RejectsUnsolicitedAndZeroIdentity) {
	ChunkserverRegistrationState unsolicited;
	EXPECT_FALSE(unsolicited.acceptIdentity(kIdentity));

	ChunkserverRegistrationState zero;
	ASSERT_TRUE(zero.begin(true, kFirstVersionWithChunkserverIdentity));
	EXPECT_FALSE(zero.acceptIdentity(chunkserver::ChunkserverId{}));
	EXPECT_TRUE(zero.identityPending());
}

TEST(ChunkserverRegistrationStateTests, HostAndCompletionTransitionsAreOneShot) {
	ChunkserverRegistrationState state;

	EXPECT_TRUE(state.begin(false, kFirstVersionWithChunkserverIdentity));
	EXPECT_FALSE(state.begin(false, kFirstVersionWithChunkserverIdentity));
	EXPECT_TRUE(state.markRegistrationComplete());
	EXPECT_FALSE(state.markRegistrationComplete());
}

TEST(ChunkserverRegistrationStateTests, IdentityDeadlineIgnoresSocketKeepalives) {
	using namespace std::chrono_literals;
	const ChunkserverRegistrationState::TimePoint started{};
	ChunkserverRegistrationState state;
	ASSERT_TRUE(state.begin(true, kFirstVersionWithChunkserverIdentity, started));

	EXPECT_FALSE(state.identityExpired(1000, started + 999ms));
	EXPECT_TRUE(state.identityExpired(1000, started + 1000ms));

	ASSERT_TRUE(state.acceptIdentity(kIdentity));
	EXPECT_FALSE(state.identityExpired(1000, started + 2000ms));
}
