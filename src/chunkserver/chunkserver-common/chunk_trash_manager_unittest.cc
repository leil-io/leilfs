/*
   Copyright 2023-2024  Leil Storage OÜ

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

#include "chunk_trash_manager.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>

class MockChunkTrashManagerImpl : public IChunkTrashManagerImpl {
public:
	MOCK_METHOD(int, moveToTrash,
	            (const std::filesystem::path &, const std::filesystem::path &,
	             const std::time_t &),
	            ());
	MOCK_METHOD(int, init, (const std::string &), ());
	MOCK_METHOD(void, collectGarbage, (), ());
	MOCK_METHOD(void, reloadConfig, (), ());
};

class ChunkTrashManagerTest : public ::testing::Test {
public:
	std::shared_ptr<MockChunkTrashManagerImpl> mockImpl{nullptr};

protected:
	void SetUp() override {
		mockImpl = std::make_shared<MockChunkTrashManagerImpl>();
		ChunkTrashManager::setImpl(mockImpl);
		ChunkTrashManager::isEnabled = 1;
	}

	void TearDown() override { mockImpl.reset(); }
};

TEST_F(ChunkTrashManagerTest, MoveToTrashForwardsCall) {
	std::filesystem::path filePath = "example.txt";
	std::filesystem::path diskPath = "/disk/";
	std::time_t deletionTime = 1234567890;

	EXPECT_CALL(*mockImpl, moveToTrash(filePath, diskPath, deletionTime))
	    .Times(1)
	    .WillOnce(testing::Return(0));  // Assuming 0 is a success return
	// value.

	int result =
	    ChunkTrashManager::moveToTrash(filePath, diskPath, deletionTime);
	EXPECT_EQ(result, 0);  // Validate that the return value is as expected.
}

TEST_F(ChunkTrashManagerTest, InitForwardsCall) {
	std::string const diskPath = "/disk/";

	EXPECT_CALL(*mockImpl, init(diskPath)).Times(1);

	ChunkTrashManager::init(diskPath);  // Call the method.
}

TEST_F(ChunkTrashManagerTest, CollectGarbageForwardsCall) {
	EXPECT_CALL(*mockImpl, collectGarbage()).Times(1);

	ChunkTrashManager::collectGarbage();  // Call the method.
}

TEST_F(ChunkTrashManagerTest, ReloadConfigForwardsCall) {
	EXPECT_CALL(*mockImpl, reloadConfig()).Times(1);

	ChunkTrashManager::reloadConfig();  // Call the method.
}

TEST_F(ChunkTrashManagerTest, DisabledMoveToTrashDoesNotForwardsCall) {
	std::filesystem::path filePath = "example.txt";
	std::filesystem::path diskPath = "/disk/";
	std::time_t deletionTime = 1234567890;
	ChunkTrashManager::isEnabled = 0;

	EXPECT_CALL(*mockImpl, moveToTrash(filePath, diskPath, deletionTime))
	    .Times(0);

	int result =
	    ChunkTrashManager::moveToTrash(filePath, diskPath, deletionTime);
	EXPECT_EQ(result, 0);  // Validate that the return value is as expected.
}

TEST_F(ChunkTrashManagerTest, DisabledInitForwardsCall) {
	std::string const diskPath = "/disk/";

	EXPECT_CALL(*mockImpl, init(diskPath)).Times(1);

	ChunkTrashManager::isEnabled = 0;
	ChunkTrashManager::init(diskPath);  // Call the method.
}

TEST_F(ChunkTrashManagerTest, DisabledCollectGarbageDoesNotForwardsCall) {
	EXPECT_CALL(*mockImpl, collectGarbage()).Times(0);

	ChunkTrashManager::isEnabled = 0;
	ChunkTrashManager::collectGarbage();  // Call the method.
}

TEST_F(ChunkTrashManagerTest, DisabledReloadConfigForwardsCall) {
	EXPECT_CALL(*mockImpl, reloadConfig()).Times(1);

	ChunkTrashManager::isEnabled = 0;
	ChunkTrashManager::reloadConfig();  // Call the method.
}
