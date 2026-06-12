#pragma once

#include "common/platform.h"

#include <boost/array.hpp>
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>

/*! \brief Implementation of modified Raft consensus algorithm.
 *
 * Implements reduced version of Raft consensus algorithm (https://raftconsensus.github.io/)
 * Only the part responsible for selecting leader is used. Propagation of data and configuration
 * is removed. Additionally each node can use version variable for selecting node with highest value.
 * uRaft (micro - Raft) algorithm makes two guarantees:
 *  - there can be only one President at the time
 *  - selected President has greater or equal value of version variable than
 *    all other nodes making quorum.
 */
class uRaft {
public:
	struct Options {
		int                      id;
		int                      port;
		std::vector<std::string> server;
		int                      election_timeout_min;
		int                      election_timeout_max;
		int                      heartbeat_period;
		int                      quorum;  /// Minimum number of votes to get to become the leader.
		/// Number of heartbeats to wait before considering quorum lost.
		int quorum_loss_grace_heartbeats;
	};

protected:
	enum State {
	    kFollower=0,
	    kCandidate,
	    kLeader,
	    kStateLast
	};

	enum RpcType {
	    kRpcAppendEntries=0, /*!< RPC Append Entries. */
	    kRpcRequestVote,     /*!< RPC Request Vote. */
	    kRpcAEResponse,      /*!< RPC Append Entries Response. */
	    kRpcRVResponse,      /*!< RPC Request Vote Response. */
	    kRpcLast
	};

	/// @brief Policy controlling side effects when stepping down to follower.
	///
	/// The policy selects whether stepping down is a Raft-only state transition, or whether it
	/// also triggers a demotion of the local metadata server (via nodeDemote()) when stepping
	/// down from an active Leader (President).
	///
	/// Typical usage:
	/// - kRaftOnly:
	///   Step down in Raft (become follower, restart election timer) without invoking nodeDemote().
	///   Use this when the controller owns metadata-side effects, e.g.:
	///   - manual demotion via demoteLeader() (promotion failure recovery),
	///   - local "metadata dead" handling where VIP release / restarts are scheduled elsewhere.
	///
	/// - kDemoteIfPresident:
	///   Step down in Raft and also call nodeDemote() so the local metadata server is not left in
	///   master mode while we are no longer eligible to lead. Use this for Raft-driven stepdowns,
	///   e.g.:
	///   - observing a higher term in received RPCs (checkTerm),
	///   - quorum loss while being President (heartbeat).
	enum class StepDownPolicy : uint8_t {
		kRaftOnly,          // Update Raft state and restart election timer
		kDemoteIfPresident  // Additionally call nodeDemote() on Raft-driven step-downs
	};

	static const int kMaxPacketLength = 1024;

	//! Information about node.
	struct NodeInfo {
		boost::asio::ip::udp::endpoint addr;         /*!< Node address. */
		uint64_t                       data_version; /*!< Node data version. */
		uint64_t                       heartbeat;    /*!< Local time of latest heartbeat response */
		bool                           vote_granted; /*!< True if node granted vote. */
		bool                           recv;         /*!< True if vote from node was received. */
	};

	//! Current node uRaft state.
	struct RaftState {
		int      id;                 /*!< Node id. */
		int      type;               /*!< Node state (follower,candidate,leader). */

		uint64_t current_term;       /*!< Current term number. */
		int32_t  voted_for;          /*!< Id of node that we voted for (can be -1 if node didn't vote). */
		int32_t  leader_id;          /*!< Id of the leader. */

		uint64_t local_time;         /*!< Local time measured in heartbeats. */
		bool     president;          /*!< True if node is a president. */
		bool     loyalty_agreement;  /*!< True if node is loyal to a leader. */

		uint64_t data_version;  /*!< This node data version (updated when required by calling nodeGetVersion). */
	};

	struct RpcHeader {
		uint8_t     type; /*!< RPC packet type */
		uint64_t    term;
	};

	struct RpcRequest : RpcHeader {
		int32_t  node_id;
		uint64_t time;
		uint64_t data_version;
	};

	struct RpcResponse : RpcHeader {
		uint8_t  result;
		uint64_t req_time;
		uint64_t data_version;
	};

public:
	uRaft(boost::asio::io_context &ios);
	virtual ~uRaft();

	//! Initialization of uRaft internal data.
	void init();

	//! Force node status change to follower.
	void demoteLeader();

	//! If true blocks change of status from follower to candidate or leader.
	void set_block_promotion(bool block);

	//! Set uRaft options.
	void set_options(const Options &opt);

	/*! \brief uRaft calls this on node becoming President.
	 *
	 * Function can be overridden in derived class to get notification on status change.
	 * Function should return as quickly as possible (otherwise can block uRaft working).
	 */
	virtual void     nodePromote();

	/*! \brief uRaft calls this function after node stop being President.
	 *
	 * Function can be overridden in derived class to get notification on status change.
	 * Function should return as quickly as possible (otherwise can block uRaft working).
	 */
	virtual void     nodeDemote();

	/*! \brief Called when uRaft gets information about new leader (which might soon become President).
	 *
	 * Function can be overridden in derived class to get notification about new leader id.
	 * Function should return as quickly as possible (otherwise can block uRaft working).
	 */
	virtual void     nodeLeader(int id);

	/*! \brief uRaft calls this function when it needs current data version.
	 *
	 * Function needs to be overridden in derived class and return current value of data version.
	 * Function should return as quickly as possible (otherwise can block uRaft working).
	 */
	virtual uint64_t nodeGetVersion();

	/// @brief Allow derived classes to seed the startup Raft role before timers are armed.
	///
	/// The default implementation keeps the node as a follower. Controllers can override this to
	/// inspect local metadata state and resume as an active leader after a restart.
	virtual void bootstrapLeaderState();

	/// @brief Restore any persisted Raft snapshot before bootstrap decisions are applied.
	///
	/// The default implementation does nothing. Controllers can override this to load a snapshot
	/// from volatile storage and restore the in-memory Raft state after a restart.
	virtual void restorePersistentState();

	/// @brief Persist the current in-memory Raft snapshot.
	///
	/// The default implementation does nothing. Controllers can override this to continuously save
	/// the current Raft state to volatile storage so a restart can resume from the latest snapshot.
	virtual void persistRuntimeState();

	/*! \brief Returns true when this node runs in elector mode, false otherwise.
	 *
	 * Electors do not have a local metadata server, so some version-related logic may need to
	 * behave differently.
	 * \note This function is overridden in uRaftController class, which implements elector mode.
	 */
	virtual bool isElectorNode() const = 0;

protected:
	void checkTerm(int id, const RpcHeader &data);

	/// @brief Step down to follower state (optionally updating term and demoting metadata).
	///
	/// This is the shared implementation used by different "step down" triggers:
	/// - observing a higher term (checkTerm),
	/// - quorum loss while being President (heartbeat),
	/// - manual demotion (demoteLeader).
	///
	/// The caller chooses the StepDownPolicy:
	/// - kRaftOnly: only updates Raft state and restarts the election timer.
	/// - kDemoteIfPresident: also calls nodeDemote() so the local metadata server is not left in
	///   master mode while stepping down in Raft.
	///
	/// @param policy StepDownPolicy used to control side effects.
	/// @param newTerm Optional new term; if non-zero and greater than current, updates
	/// current_term. This is used for stepping down when observing a higher term.
	void stepDownToFollower(StepDownPolicy policy, uint64_t newTerm = 0);

	/*! \brief Checks if a received packet is structurally valid.
	 *
	 * \param data Pointer to the received packet data.
	 * \param size Size of the received packet data.
	 * \return true if the packet is valid, false otherwise.
	 */
	bool validPacket(const uint8_t *data, size_t size);
	int  findNodeID(const boost::asio::ip::udp::endpoint &addr);
	int  voteCount(bool count_loyal);

	void startElectionTimer();
	void startHearbeatTimer();
	void startReceive();
	void signLoyaltyAgreement();

	void sendHeartbeat();
	void sendRequestForVotes();

	void electionTimeout(const boost::system::error_code &error);
	void heartbeat(const boost::system::error_code &error);
	void receivePacket(const boost::system::error_code &error, size_t bytes_recvd);

	void rpcAppend(int id, const RpcRequest &data);
	void rpcAppendResponse(int id, const RpcResponse &data);
	void rpcReqVote(int id, const RpcRequest &data);
	void rpcReqVoteResponse(int id, const RpcResponse &data);

	template<typename ConstBufferSequence>
	void socketSend(const ConstBufferSequence &buffers, const boost::asio::ip::udp::endpoint &destination);
	int findMatchingAddress(const boost::asio::ip::address &addr, int &id);
	int scanLocalInterfaces();

	std::string nodeToString(int id);

protected:
	boost::asio::io_context                 &io_service_;
	boost::asio::ip::udp::socket            socket_;
	boost::asio::steady_timer             election_timer_,heartbeat_timer_;
	boost::asio::steady_timer             loyalty_agreement_timer_;
	boost::array<uint8_t,kMaxPacketLength>  packet_data_;
	boost::asio::ip::udp::endpoint          sender_endpoint_;

	std::vector<NodeInfo>                   node_;
	RaftState                               state_;
	bool                                    block_leader_promotion_;  /// If true this node cannot be promoted to leader.

	Options                                 opt_;

	/// Number of consecutive heartbeats with lost quorum before demoting leader.
	int quorum_loss_streak_ = 0;
};
