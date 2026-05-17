/*
   Copyright 2026 Leil Storage OÜ

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

#include "uraft.h"

#include <gtest/gtest.h>

class TestURaft : public uRaft {
public:
	explicit TestURaft(boost::asio::io_context &ios) : uRaft(ios) {}

	bool isElectorNode() const override { return false; }
	void nodePromote() override {}
	void nodeDemote() override {}
	void nodeLeader(int) override {}
	uint64_t nodeGetVersion() override { return 0; }
	void bootstrapLeaderState() override {}
	void restorePersistentState() override {}
	void persistRuntimeState() override {}

	using uRaft::node_;
	using uRaft::opt_;
	using RpcResponse = uRaft::RpcResponse;
	using uRaft::rpcReqVoteResponse;
	using uRaft::state_;
	using uRaft::voteCount;

	static constexpr int kCandidateState = uRaft::kCandidate;
	static constexpr uint8_t kRequestVoteResponseType = uRaft::kRpcRVResponse;
};

TEST(URaftTest, VoteResponseRefreshesHeartbeatFreshness) {
	boost::asio::io_context io;
	TestURaft raft(io);

	raft.opt_.heartbeat_period = 20;
	raft.opt_.election_timeout_min = 400;
	raft.opt_.election_timeout_max = 600;
	raft.opt_.quorum = 3;
	raft.opt_.server = {"node0", "node1", "node2"};

	raft.node_.resize(3);
	raft.state_.id = 0;
	raft.state_.type = TestURaft::kCandidateState;
	raft.state_.current_term = 7;
	raft.state_.voted_for = 0;
	raft.state_.leader_id = -1;
	raft.state_.local_time = 25;
	raft.state_.data_version = 1234;

	raft.node_[0].heartbeat = raft.state_.local_time;
	raft.node_[0].vote_granted = true;
	raft.node_[0].recv = true;

	TestURaft::RpcResponse response{};
	response.type = TestURaft::kRequestVoteResponseType;
	response.term = raft.state_.current_term;
	response.result = 1;
	response.req_time = raft.state_.local_time;
	response.data_version = raft.state_.data_version;

	raft.rpcReqVoteResponse(1, response);

	EXPECT_EQ(raft.node_[1].heartbeat, raft.state_.local_time);
	EXPECT_EQ(raft.voteCount(true), 2);
}
