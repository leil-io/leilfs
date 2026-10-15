/*
   Copyright 2026 Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include <gtest/gtest.h>
#include <sys/resource.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <latch>
#include <memory>
#include <utility>
#include <vector>

#include "chunkserver-common/cmr_disk.h"
#include "chunkserver-common/global_shared_resources.h"
#include "chunkserver-common/hdd_utils.h"
#include "chunkserver/hddspacemgr.h"
#include "chunkserver/masterconn.h"
#include "protocol/cstoma.h"
#include "protocol/matocs.h"
#include "unittests/mocks/module_mock.h"
#include "unittests/packet.h"

TEST(MasterConnStatsTests, DisconnectedBacklogDoesNotRaiseThePeak) {
	EXPECT_EQ(masterconn_sample_max_jobs_count(false, 0, 3, 0), 0U);
}

TEST(MasterConnStatsTests, ConnectedBacklogRaisesThePeak) {
	EXPECT_EQ(masterconn_sample_max_jobs_count(true, 0, 3, 0), 3U);
}

TEST(MasterConnStatsTests, PeakIsNeverLoweredByADrainingBacklog) {
	EXPECT_EQ(masterconn_sample_max_jobs_count(true, 5, 1, 0), 5U);
}

class MasterConnSpaceReportTests : public testing::Test {
protected:
	void SetUp() override {
		std::lock_guard disksLockGuard(gDisksMutex);
		ASSERT_TRUE(gDisks.empty());
		auto disk =
		    std::make_unique<CmrDisk>("/tmp/space-report/", "/tmp/space-report/", false, false);
		disk_ = disk.get();
		disk_->setTotalSpace(4096);
		disk_->setAvailableSpace(3072);
		disk_->setScanState(IDisk::ScanState::kInProgress);
		gDisks.push_back(std::move(disk));
		savedSpaceChanged_ = gHddSpaceChanged.exchange(false);
		savedForcePush_ = std::exchange(gForcePushRegistration, false);
	}

	void TearDown() override {
		for (auto &slot : state_.connections) { slot.connection.reset(); }
		hddRegistrationSweepBegin(RegistrationSweepMode::kPush);
		std::lock_guard disksLockGuard(gDisksMutex);
		gDisks.clear();
		gHddSpaceChanged.store(savedSpaceChanged_);
		gForcePushRegistration = savedForcePush_;
	}

	MasterConn &addConnection(std::size_t index = 0) {
		auto &connection = state_.connections[index].connection;
		connection = std::make_unique<MasterConn>("localhost", "9420", "default", nullptr, nullptr,
		                                          index, index);
		connection->setMode(ConnectionMode::CONNECTED);
		return *connection;
	}

	void registerHost(MasterConn &connection) {
		auto response = matocs::registerHost::build(SAUNAFS_STATUS_OK, SAUNAFS_VERSHEX, "default");
		removeHeaderInPlace(response);
		connection.onRegistered(response);
	}

	void startPull(MasterConn &connection) {
		auto start = matocs::registerChunksStart::build(1, 0);
		removeHeaderInPlace(start);
		connection.onRegisterChunksStart(start);
	}

	void makeDiskReady() {
		std::lock_guard disksLockGuard(gDisksMutex);
		disk_->setScanState(IDisk::ScanState::kWorking);
		gHddSpaceChanged.store(true);
	}

	void setPhase(MasterConn &connection, RegistrationStatus status) {
		connection.registrationStatus_ = status;
	}

	MessageBuffer pop(MasterConn &connection) {
		auto &queue = connection.nextOutputPacketQueue();
		EXPECT_FALSE(queue.empty());
		if (queue.empty()) { return {}; }
		auto packet = std::move(queue.front().packet);
		queue.pop();
		return packet;
	}

	void expectSpaceUpdate(MasterConn &connection) {
		ASSERT_FALSE(connection.isOutputQueueEmpty());
		auto packet = pop(connection);
		verifyHeader(packet, CSTOMA_SPACE);
		removeHeaderInPlace(packet);
		uint64_t used = 0, total = 0, toDelUsed = 0, toDelTotal = 0;
		uint32_t chunks = 0, toDelChunks = 0;
		ASSERT_NO_THROW(
		    deserialize(packet, used, total, chunks, toDelUsed, toDelTotal, toDelChunks));
		EXPECT_EQ(1024U, used);
		EXPECT_EQ(4096U, total);
		EXPECT_EQ(0U, toDelTotal);
		EXPECT_TRUE(connection.isOutputQueueEmpty());
	}

	MasterConnReconciliationState state_;
	CmrDisk *disk_{nullptr};
	bool savedSpaceChanged_{false};
	bool savedForcePush_{false};
};

TEST_F(MasterConnSpaceReportTests, RefreshesSpaceAfterInitialZeroBeforePullStart) {
	auto &connection = addConnection();
	registerHost(connection);
	ASSERT_EQ(RegistrationStatus::kAwaitingPullStart, connection.registrationStatus());

	makeDiskReady();
	masterconn_report_space(state_);

	auto initial = pop(connection);
	verifyHeader(initial, SAU_CSTOMA_REGISTER_SPACE);
	removeHeaderInPlace(initial);
	uint64_t used = 1, total = 1, toDelUsed = 0, toDelTotal = 0;
	uint32_t chunks = 0, toDelChunks = 0;
	ASSERT_NO_THROW(cstoma::registerSpace::deserialize(initial, used, total, chunks, toDelUsed,
	                                                   toDelTotal, toDelChunks));
	EXPECT_EQ(0U, used);
	EXPECT_EQ(0U, total);
	verifyHeader(pop(connection), SAU_CSTOMA_REGISTER_LABEL);
	verifyHeader(pop(connection), SAU_CSTOMA_REGISTER_CONFIG);
	ASSERT_NO_FATAL_FAILURE(expectSpaceUpdate(connection));
	EXPECT_FALSE(connection.isRegistered());
}

TEST_F(MasterConnSpaceReportTests, RefreshesSpaceWithoutCompletingPullRegistration) {
	auto &connection = addConnection();
	registerHost(connection);
	startPull(connection);
	connection.resetPackets();
	makeDiskReady();
	masterconn_report_space(state_);
	ASSERT_NO_FATAL_FAILURE(expectSpaceUpdate(connection));
	EXPECT_EQ(RegistrationStatus::kChunksRegistering, connection.registrationStatus());
	EXPECT_FALSE(connection.isRegistered());

	auto credit = matocs::registerChunksCredit::build(1);
	removeHeaderInPlace(credit);
	connection.onRegisterChunksCredit(credit);
	ASSERT_TRUE(connection.isRegistered());
	verifyHeader(pop(connection), SAU_CSTOMA_REGISTER_CHUNKS_END);
	gHddSpaceChanged.store(true);
	masterconn_report_space(state_);
	ASSERT_NO_FATAL_FAILURE(expectSpaceUpdate(connection));
}

TEST_F(MasterConnSpaceReportTests, PreservesChangesWhileHandshakeOrRetirementBlocksDelivery) {
	auto &connection = addConnection();
	makeDiskReady();
	for (auto phase :
	     {RegistrationStatus::kUnregistered, RegistrationStatus::kRegistrationRequested,
	      RegistrationStatus::kHostRegistered}) {
		setPhase(connection, phase);
		masterconn_report_space(state_);
		EXPECT_TRUE(connection.isOutputQueueEmpty());
		EXPECT_TRUE(gHddSpaceChanged.load());
	}
	setPhase(connection, RegistrationStatus::kChunksRegistered);
	connection.setMode(ConnectionMode::HANDSHAKE);
	masterconn_report_space(state_);
	EXPECT_TRUE(gHddSpaceChanged.load());
	EXPECT_TRUE(connection.isOutputQueueEmpty());
	connection.setMode(ConnectionMode::CONNECTED);
	state_.connections[0].retiring = true;
	masterconn_report_space(state_);
	EXPECT_TRUE(gHddSpaceChanged.load());
	EXPECT_TRUE(connection.isOutputQueueEmpty());
	state_.connections[0].retiring = false;
	masterconn_report_space(state_);
	ASSERT_NO_FATAL_FAILURE(expectSpaceUpdate(connection));
	EXPECT_FALSE(gHddSpaceChanged.load());
}

TEST_F(MasterConnSpaceReportTests, BroadcastsOnceToAllEligiblePeers) {
	const std::array phases{
	    RegistrationStatus::kAwaitingPullStart, RegistrationStatus::kChunksRegistering,
	    RegistrationStatus::kChunksRegistered,  RegistrationStatus::kRegistrationRequested,
	    RegistrationStatus::kChunksRegistered,  RegistrationStatus::kChunksRegistered};
	for (std::size_t index = 0; index < phases.size(); ++index) {
		auto &connection = addConnection(index);
		setPhase(connection, phases[index]);
	}
	state_.connections[4].retiring = true;
	state_.connections[5].connection->setMode(ConnectionMode::KILL);
	makeDiskReady();
	masterconn_report_space(state_);
	for (std::size_t index = 0; index < 3; ++index) {
		ASSERT_NO_FATAL_FAILURE(expectSpaceUpdate(*state_.connections[index].connection));
	}
	EXPECT_FALSE(gHddSpaceChanged.load());
	masterconn_report_space(state_);
	for (const auto &slot : state_.connections) {
		if (slot.connection) { EXPECT_TRUE(slot.connection->isOutputQueueEmpty()); }
	}
}

TEST_F(MasterConnSpaceReportTests, LatePeerGetsFreshSnapshotAfterSharedChangeWasConsumed) {
	auto &seed = addConnection();
	registerHost(seed);
	seed.resetPackets();
	makeDiskReady();
	masterconn_report_space(state_);
	ASSERT_NO_FATAL_FAILURE(expectSpaceUpdate(seed));
	ASSERT_FALSE(gHddSpaceChanged.load());

	auto &peer = addConnection(1);
	peer.setSendInventory(false);
	registerHost(peer);
	ASSERT_TRUE(peer.isRegistered());
	auto initial = pop(peer);
	verifyHeader(initial, SAU_CSTOMA_REGISTER_SPACE);
	removeHeaderInPlace(initial);
	uint64_t used = 0, total = 0, toDelUsed = 0, toDelTotal = 0;
	uint32_t chunks = 0, toDelChunks = 0;
	ASSERT_NO_THROW(cstoma::registerSpace::deserialize(initial, used, total, chunks, toDelUsed,
	                                                   toDelTotal, toDelChunks));
	EXPECT_EQ(1024U, used);
	EXPECT_EQ(4096U, total);
}

namespace {

struct HeldReconciliationJob {
	std::promise<void> started;
	std::latch release{1};

	void finish() {
		if (!release.try_wait()) { release.count_down(); }
	}
};

struct ReconciliationHarness {
	std::shared_ptr<MasterJobPool> jobs;
	std::shared_ptr<MasterJobPool> replications;
	ModuleMock seedServer;
	ModuleMock firstServer;
	ModuleMock secondServer;
	ModuleMock replacementServer;
	MasterConnReconciliationState state;
	std::vector<std::shared_ptr<HeldReconciliationJob>> heldJobs;

	~ReconciliationHarness() {
		// Release workers even when an assertion ends a test before its explicit drain.
		for (const auto &held : heldJobs) { held->finish(); }
	}

	void initialize(bool sendInventory = false) {
		seedServer.init();
		firstServer.init();
		secondServer.init();
		replacementServer.init();
		state.jobDescriptors.assign(kMaxMetadataConnections, -1);
		state.replicationDescriptors.assign(kMaxMetadataConnections, -1);
		jobs = std::make_shared<MasterJobPool>("reconcile", 1, 10);
		state.jobDescriptors[0] = jobs->allocateListener(0);
		replications = std::make_shared<MasterJobPool>("reconcile-repl", 1, 10);
		state.replicationDescriptors[0] = replications->allocateListener(0);

		auto &seed = state.connections[0].connection;
		seed = std::make_unique<MasterConn>("localhost", std::to_string(seedServer.port()),
		                                    "default", jobs, replications);
		seed->setMasterAddress(seedServer.address().ip, seedServer.port());
		seed->setMode(ConnectionMode::CONNECTED);
		seed->setSendInventory(sendInventory);
		auto packet = matocs::registerHost::build(SAUNAFS_STATUS_OK, SAUNAFS_VERSHEX, "default");
		removeHeaderInPlace(packet);
		seed->onRegistered(packet);
		if (sendInventory) {
			auto startPacket = matocs::registerChunksStart::build(1, 1);
			removeHeaderInPlace(startPacket);
			seed->onRegisterChunksStart(startPacket);
		}
		ASSERT_TRUE(seed->isRegistered());
	}

	MetadataClusterMember member(uint32_t serverId, const ModuleMock &server) const {
		return {serverId, server.address().ip, server.port(), kFirstVersionWithChunkserverIdentity};
	}

	void stage(std::vector<MetadataClusterMember> members) {
		auto packet = matocs::clusterMembers::build(1, members);
		removeHeaderInPlace(packet);
		state.connections[0].connection->receiveClusterSnapshot(packet);
	}

	void reconcile() { masterconn_reconcile_connections(state, jobs, replications); }

	size_t connectionCount() const {
		return std::count_if(state.connections.begin(), state.connections.end(),
		                     [](const auto &slot) { return slot.connection != nullptr; });
	}

	void holdJob(MasterJobPool &pool, uint32_t listenerId) {
		auto held = heldJobs.emplace_back(std::make_shared<HeldReconciliationJob>());
		auto started = held->started.get_future();
		auto *packet = new OutputPacket;
		cstoma::createChunk::serialize(packet->packet, 123, slice_traits::standard::ChunkPartType(),
		                               SAUNAFS_STATUS_OK);
		pool.addJob(
		    JobPool::ChunkOperation::Create,
		    MasterConn::sauJobFinished(state.connections[listenerId].connection.get()), packet,
		    [held]() -> uint8_t {
			    held->started.set_value();
			    held->release.wait();
			    return SAUNAFS_STATUS_OK;
		    },
		    listenerId);
		ASSERT_EQ(started.wait_for(std::chrono::seconds(5)), std::future_status::ready);
	}

	void drain(uint32_t listenerId) {
		auto &slot = state.connections[listenerId];
		std::vector<pollfd> descriptors;
		masterconn_add_completion_descriptors(*slot.connection, state.jobDescriptors[listenerId],
		                                      state.replicationDescriptors[listenerId],
		                                      slot.completionPoll, descriptors);
		ASSERT_GT(poll(descriptors.data(), descriptors.size(), 5000), 0);
		masterconn_serve_connection(*jobs, *replications, *slot.connection, listenerId,
		                            slot.completionPoll, descriptors);
	}
};

}  // namespace

TEST(MasterConnReconciliationTests, SnapshotAdmitsPeersOnceAndRetiresRemovedPeers) {
	ReconciliationHarness harness;
	ASSERT_NO_FATAL_FAILURE(harness.initialize());
	auto *seed = harness.state.connections[0].connection.get();
	const auto first = harness.member(2, harness.firstServer);
	const auto second = harness.member(3, harness.secondServer);
	harness.stage({first, second});
	EXPECT_EQ(harness.connectionCount(), 1U);
	harness.reconcile();
	ASSERT_EQ(harness.connectionCount(), 3U);
	ASSERT_NE(harness.state.connections[1].connection, nullptr);
	ASSERT_NE(harness.state.connections[2].connection, nullptr);
	auto *firstConnection = harness.state.connections[1].connection.get();
	auto *secondConnection = harness.state.connections[2].connection.get();
	EXPECT_EQ(firstConnection->serverId(), 2U);
	EXPECT_EQ(secondConnection->serverId(), 3U);
	EXPECT_EQ(firstConnection->address(), harness.firstServer.address());
	EXPECT_GE(firstConnection->socketFD(), 0);
	EXPECT_GE(secondConnection->socketFD(), 0);
	EXPECT_FALSE(seed->takeClusterSnapshot().has_value());

	// Repeating or reordering the same membership preserves sockets and listener assignments.
	const int firstSocket = firstConnection->socketFD();
	harness.stage({second, first});
	harness.reconcile();
	EXPECT_EQ(harness.connectionCount(), 3U);
	EXPECT_EQ(harness.state.connections[1].connection.get(), firstConnection);
	EXPECT_EQ(harness.state.connections[2].connection.get(), secondConnection);
	EXPECT_EQ(firstConnection->socketFD(), firstSocket);
	EXPECT_EQ(harness.jobs->allocatedListenerCount(), 3U);
	EXPECT_EQ(harness.replications->allocatedListenerCount(), 3U);

	harness.stage({second});
	harness.reconcile();
	EXPECT_EQ(harness.state.connections[1].connection, nullptr);
	EXPECT_EQ(harness.state.connections[2].connection.get(), secondConnection);
	harness.stage({});
	harness.reconcile();
	EXPECT_EQ(harness.connectionCount(), 1U);
	EXPECT_EQ(harness.state.connections[0].connection.get(), seed);
	EXPECT_FALSE(harness.state.connections[0].retiring);
}

TEST(MasterConnReconciliationTests, LatestStagedSnapshotWins) {
	ReconciliationHarness harness;
	ASSERT_NO_FATAL_FAILURE(harness.initialize());
	harness.stage({harness.member(2, harness.firstServer)});
	harness.stage({harness.member(3, harness.secondServer)});
	harness.reconcile();
	ASSERT_EQ(harness.connectionCount(), 2U);
	ASSERT_NE(harness.state.connections[1].connection, nullptr);
	EXPECT_EQ(harness.state.connections[1].connection->serverId(), 3U);
	EXPECT_EQ(harness.jobs->allocatedListenerCount(), 2U);
}

TEST(MasterConnReconciliationTests, EndpointReplacementWaitsForBothPools) {
	ReconciliationHarness harness;
	ASSERT_NO_FATAL_FAILURE(harness.initialize());
	const auto survivor = harness.member(3, harness.secondServer);
	harness.stage({harness.member(2, harness.firstServer), survivor});
	harness.reconcile();
	ASSERT_NE(harness.state.connections[1].connection, nullptr);
	ASSERT_NE(harness.state.connections[2].connection, nullptr);
	auto *oldConnection = harness.state.connections[1].connection.get();
	auto *survivingConnection = harness.state.connections[2].connection.get();
	const int survivorSocket = survivingConnection->socketFD();
	const int jobDescriptor = harness.state.jobDescriptors[1];
	const int replicationDescriptor = harness.state.replicationDescriptors[1];
	ASSERT_NO_FATAL_FAILURE(harness.holdJob(*harness.jobs, 1));
	ASSERT_NO_FATAL_FAILURE(harness.holdJob(*harness.replications, 1));

	harness.stage({harness.member(2, harness.replacementServer), survivor});
	harness.reconcile();
	EXPECT_TRUE(harness.state.connections[1].retiring);
	EXPECT_EQ(oldConnection->mode(), ConnectionMode::FREE);
	EXPECT_EQ(oldConnection->socketFD(), -1);
	EXPECT_EQ(harness.connectionCount(), 3U);
	masterconn_reconnect_connections(harness.state);
	EXPECT_EQ(oldConnection->socketFD(), -1);
	EXPECT_EQ(survivingConnection->socketFD(), survivorSocket);

	// Finishing normal jobs alone cannot release a slot with replication work still running.
	harness.heldJobs[0]->finish();
	ASSERT_NO_FATAL_FAILURE(harness.drain(1));
	ASSERT_TRUE(harness.jobs->isListenerIdle(1));
	ASSERT_FALSE(harness.replications->isListenerIdle(1));
	harness.reconcile();
	EXPECT_EQ(harness.state.connections[1].connection.get(), oldConnection);
	EXPECT_TRUE(harness.state.connections[1].retiring);
	EXPECT_TRUE(oldConnection->isOutputQueueEmpty());

	harness.heldJobs[1]->finish();
	ASSERT_NO_FATAL_FAILURE(harness.drain(1));
	ASSERT_TRUE(harness.replications->isListenerIdle(1));
	harness.reconcile();
	ASSERT_NE(harness.state.connections[1].connection, nullptr);
	EXPECT_FALSE(harness.state.connections[1].retiring);
	EXPECT_EQ(harness.state.connections[1].connection->serverId(), 2U);
	EXPECT_EQ(harness.state.connections[1].connection->address(),
	          harness.replacementServer.address());
	EXPECT_GE(harness.state.connections[1].connection->socketFD(), 0);
	EXPECT_EQ(harness.state.jobDescriptors[1], jobDescriptor);
	EXPECT_EQ(harness.state.replicationDescriptors[1], replicationDescriptor);
	EXPECT_EQ(harness.state.connections[2].connection.get(), survivingConnection);
	EXPECT_EQ(survivingConnection->socketFD(), survivorSocket);
}

TEST(MasterConnReconciliationTests, ReturningPeerWaitsForItsRetirementToFinish) {
	ReconciliationHarness harness;
	ASSERT_NO_FATAL_FAILURE(harness.initialize());
	const auto peer = harness.member(2, harness.firstServer);
	harness.stage({peer});
	harness.reconcile();
	ASSERT_NE(harness.state.connections[1].connection, nullptr);
	auto *retiredConnection = harness.state.connections[1].connection.get();
	const int jobDescriptor = harness.state.jobDescriptors[1];
	const int replicationDescriptor = harness.state.replicationDescriptors[1];
	ASSERT_NO_FATAL_FAILURE(harness.holdJob(*harness.jobs, 1));

	harness.stage({});
	harness.reconcile();
	ASSERT_TRUE(harness.state.connections[1].retiring);
	ASSERT_EQ(retiredConnection->socketFD(), -1);

	// The same endpoint named again keeps one connection: the slot is still draining its listener.
	harness.stage({peer});
	harness.reconcile();
	EXPECT_EQ(harness.state.connections[1].connection.get(), retiredConnection);
	EXPECT_TRUE(harness.state.connections[1].retiring);
	EXPECT_EQ(harness.state.connections[2].connection, nullptr);
	EXPECT_EQ(harness.connectionCount(), 2U);
	masterconn_reconnect_connections(harness.state);
	EXPECT_EQ(retiredConnection->socketFD(), -1);
	EXPECT_EQ(harness.connectionCount(), 2U);

	harness.heldJobs[0]->finish();
	ASSERT_NO_FATAL_FAILURE(harness.drain(1));
	harness.reconcile();
	ASSERT_NE(harness.state.connections[1].connection, nullptr);
	EXPECT_FALSE(harness.state.connections[1].retiring);
	EXPECT_EQ(harness.state.connections[1].connection->serverId(), 2U);
	EXPECT_EQ(harness.state.connections[1].connection->address(), harness.firstServer.address());
	EXPECT_GE(harness.state.connections[1].connection->socketFD(), 0);
	EXPECT_EQ(harness.state.jobDescriptors[1], jobDescriptor);
	EXPECT_EQ(harness.state.replicationDescriptors[1], replicationDescriptor);
	EXPECT_EQ(harness.connectionCount(), 2U);
}

TEST(MasterConnReconciliationTests, DetachedLockDoesNotBlockEndpointReplacement) {
	ReconciliationHarness harness;
	ASSERT_NO_FATAL_FAILURE(harness.initialize());
	const auto original = harness.member(2, harness.firstServer);
	const auto replacement = harness.member(2, harness.replacementServer);
	const auto chunkType = slice_traits::standard::ChunkPartType();
	constexpr uint64_t chunkId = 123;
	harness.stage({original});
	harness.reconcile();
	ASSERT_NE(harness.state.connections[1].connection, nullptr);
	auto *originalConnection = harness.state.connections[1].connection.get();
	auto *writeEndPacket = new OutputPacket;
	cstoma::writeEndStatus::serialize(writeEndPacket->packet, chunkId, chunkType,
	                                  SAUNAFS_STATUS_OK);
	ASSERT_TRUE(harness.jobs->startChunkLock(MasterConn::sauJobFinished(originalConnection),
	                                         writeEndPacket, chunkId, chunkType, 1));
	ASSERT_FALSE(harness.jobs->isListenerIdle(1));

	harness.stage({replacement});
	harness.reconcile();
	ASSERT_NE(harness.state.connections[1].connection, nullptr);
	auto &replacementConnection = *harness.state.connections[1].connection;
	EXPECT_FALSE(harness.state.connections[1].retiring);
	EXPECT_EQ(replacementConnection.serverId(), 2U);
	EXPECT_EQ(replacementConnection.address(), harness.replacementServer.address());
	EXPECT_TRUE(harness.jobs->isListenerIdle(1));

	EXPECT_TRUE(harness.jobs->enforceChunkLock(chunkId, chunkType));
	replacementConnection.resetPackets();
	harness.jobs->endChunkLock(chunkId, chunkType, SAUNAFS_STATUS_OK);
	EXPECT_TRUE(replacementConnection.isOutputQueueEmpty());
	EXPECT_FALSE(harness.jobs->enforceChunkLock(chunkId, chunkType));
}

TEST(MasterConnReconciliationTests, ReusedSlotDoesNotReceivePreviousPeersReply) {
	ReconciliationHarness harness;
	ASSERT_NO_FATAL_FAILURE(harness.initialize());
	harness.stage({harness.member(2, harness.firstServer)});
	harness.reconcile();
	ASSERT_NE(harness.state.connections[1].connection, nullptr);
	auto staleReply = MasterConn::sauJobFinished(harness.state.connections[1].connection.get());
	const int descriptor = harness.state.jobDescriptors[1];

	harness.stage({harness.member(3, harness.secondServer)});
	harness.reconcile();
	ASSERT_NE(harness.state.connections[1].connection, nullptr);
	auto &replacement = *harness.state.connections[1].connection;
	ASSERT_EQ(replacement.serverId(), 3U);
	EXPECT_EQ(harness.state.jobDescriptors[1], descriptor);
	EXPECT_EQ(harness.jobs->allocatedListenerCount(), 2U);
	replacement.setMode(ConnectionMode::CONNECTED);
	replacement.resetPackets();

	auto *packet = new OutputPacket;
	cstoma::createChunk::serialize(packet->packet, 123, slice_traits::standard::ChunkPartType(),
	                               SAUNAFS_STATUS_OK);
	staleReply(SAUNAFS_STATUS_OK, packet);
	EXPECT_TRUE(replacement.isOutputQueueEmpty());
}

TEST(MasterConnReconciliationTests, SeedDisconnectKeepsLastKnownPeers) {
	ReconciliationHarness harness;
	ASSERT_NO_FATAL_FAILURE(harness.initialize());
	harness.stage({harness.member(2, harness.firstServer)});
	harness.reconcile();
	ASSERT_NE(harness.state.connections[1].connection, nullptr);
	auto *peer = harness.state.connections[1].connection.get();
	auto &seed = *harness.state.connections[0].connection;
	seed.setMode(ConnectionMode::KILL);
	masterconn_close_connection(*harness.jobs, *harness.replications, seed, 0);
	harness.reconcile();
	EXPECT_EQ(harness.state.connections[1].connection.get(), peer);
	EXPECT_FALSE(harness.state.connections[1].retiring);
	EXPECT_EQ(harness.connectionCount(), 2U);
}

TEST(MasterConnReconciliationTests, InventoryRegistrationKeepsOnlyConfiguredConnection) {
	ReconciliationHarness harness;
	ASSERT_NO_FATAL_FAILURE(harness.initialize(true));
	auto *seed = harness.state.connections[0].connection.get();
	harness.reconcile();
	harness.reconcile();
	EXPECT_EQ(harness.connectionCount(), 1U);
	EXPECT_EQ(harness.state.connections[0].connection.get(), seed);
	EXPECT_TRUE(seed->isRegistered());
	EXPECT_TRUE(seed->sendsInventory());
	EXPECT_EQ(harness.jobs->allocatedListenerCount(), 1U);
	EXPECT_EQ(harness.replications->allocatedListenerCount(), 1U);
}

namespace {

void checkReconciliationAdmissionFailure(bool failSecondPool) {
	ReconciliationHarness harness;
	ASSERT_NO_FATAL_FAILURE(harness.initialize());
	const auto survivor = harness.member(2, harness.firstServer);
	harness.stage({survivor});
	harness.reconcile();
	ASSERT_NE(harness.state.connections[1].connection, nullptr);
	auto *survivingConnection = harness.state.connections[1].connection.get();
	const int survivorSocket = survivingConnection->socketFD();
	if (failSecondPool) { ASSERT_GE(harness.jobs->allocateListener(2), 0); }
	harness.stage({survivor, harness.member(3, harness.secondServer)});

	// The child alone loses descriptor allocation; restore it before assertions or retry.
	struct rlimit original;
	ASSERT_EQ(getrlimit(RLIMIT_NOFILE, &original), 0);
	auto exhausted = original;
	exhausted.rlim_cur = 0;
	ASSERT_EQ(setrlimit(RLIMIT_NOFILE, &exhausted), 0);
	bool threw = false;
	try {
		harness.reconcile();
	} catch (...) { threw = true; }
	ASSERT_EQ(setrlimit(RLIMIT_NOFILE, &original), 0);
	ASSERT_FALSE(threw);
	EXPECT_TRUE(harness.state.admissionDeferred);
	EXPECT_EQ(harness.connectionCount(), 2U);
	EXPECT_EQ(harness.state.jobDescriptors[2], -1);
	EXPECT_EQ(harness.state.replicationDescriptors[2], -1);
	EXPECT_EQ(harness.state.connections[1].connection.get(), survivingConnection);
	EXPECT_EQ(survivingConnection->socketFD(), survivorSocket);

	// A newer snapshot replaces the failed candidate, but admission waits for the timer tick.
	harness.stage({survivor, harness.member(4, harness.replacementServer)});
	harness.reconcile();
	EXPECT_EQ(harness.connectionCount(), 2U);
	EXPECT_TRUE(harness.state.admissionDeferred);
	masterconn_reconnect_connections(harness.state);
	EXPECT_FALSE(harness.state.admissionDeferred);
	harness.reconcile();
	ASSERT_EQ(harness.connectionCount(), 3U);
	ASSERT_NE(harness.state.connections[2].connection, nullptr);
	EXPECT_EQ(harness.state.connections[2].connection->serverId(), 4U);
	EXPECT_EQ(harness.state.connections[2].connection->address(),
	          harness.replacementServer.address());
	EXPECT_GE(harness.state.connections[2].connection->socketFD(), 0);
	EXPECT_EQ(harness.jobs->allocatedListenerCount(), 3U);
	EXPECT_EQ(harness.replications->allocatedListenerCount(), 3U);
	EXPECT_EQ(survivingConnection->socketFD(), survivorSocket);
}

}  // namespace

TEST(MasterConnReconciliationDeathTest, RetriesFirstPoolFailureOnReconnectTick) {
	ASSERT_EXIT(
	    {
		    checkReconciliationAdmissionFailure(false);
		    _exit(::testing::Test::HasFailure() ? 1 : 0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

TEST(MasterConnReconciliationDeathTest, RetriesSecondPoolFailureOnReconnectTick) {
	ASSERT_EXIT(
	    {
		    checkReconciliationAdmissionFailure(true);
		    _exit(::testing::Test::HasFailure() ? 1 : 0);
	    },
	    ::testing::ExitedWithCode(0), "");
}
