#include "common/platform.h"

#include <boost/lexical_cast.hpp>

#include "uraft.h"

#include <syslog.h>

#if defined(SAUNAFS_HAVE_GETIFADDRS)
 #include <sys/types.h>
 #include <ifaddrs.h>
#endif

#define BOOST_BIND_GLOBAL_PLACEHOLDERS
#include <boost/bind.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <chrono>
#include <thread>

using boost::asio::ip::udp;

uRaft::uRaft(boost::asio::io_context &ios)
	: io_service_(ios),
	  socket_(ios),
	  election_timer_(ios),
	  heartbeat_timer_(ios),
	  loyalty_agreement_timer_(ios) {
	opt_.election_timeout_min   = 300;
	opt_.election_timeout_max   = 500;
	opt_.heartbeat_period       = 20;
	opt_.id                     = -1;
	opt_.port                   = 9425;
	opt_.quorum_loss_grace_heartbeats = 5;

	state_.id           = 0;
	state_.type         = kFollower;
	state_.current_term = 0;
	state_.data_version = 0;
	state_.leader_id    = -1;
	state_.voted_for    = -1;
	state_.president    = false;
	state_.local_time   = 0;

	block_leader_promotion_ = false;
}

uRaft::~uRaft() {
}

void uRaft::nodePromote() {
}

void uRaft::nodeDemote() {
}

void uRaft::nodeLeader(int) {
}

uint64_t uRaft::nodeGetVersion() {
	return state_.data_version;
}

void uRaft::bootstrapLeaderState() {
}

void uRaft::restorePersistentState() {
}

void uRaft::persistRuntimeState() {
}

template<typename ConstBufferSequence>
void uRaft::socketSend(const ConstBufferSequence &buffers, const boost::asio::ip::udp::endpoint &destination) {
	boost::system::error_code ecode;
	socket_.send_to(buffers, destination, 0, ecode);
}

/*! \brief Returns node id.
 *
 * \param addr IP address to match
 * \return index of node with address addr
 *         -1 when there is no node matching address addr
 */
int uRaft::findNodeID(const boost::asio::ip::udp::endpoint &addr) {
	for (int i = 0; i < (int)node_.size(); i++) {
		if (node_[i].addr == addr) return i;
	}
	return -1;
}

/*! \brief Return number of nodes voting for this node.
 *
 * \param count_loyal Count only loyal nodes.
 */
int uRaft::voteCount(bool count_loyal) {
	int votes = 0;
	for (const auto &node : node_) {
		int time_from_loyalty_vote = opt_.heartbeat_period *
		                             ((state_.local_time - node.heartbeat) + 1);

		votes += (!count_loyal && node.vote_granted) ||
		         (count_loyal && time_from_loyalty_vote <= opt_.election_timeout_min);
	}
	return votes;
}

//! Check if received packet has newer term than ours. If so we switch to follower state.
void uRaft::checkTerm(int /*id*/, const RpcHeader &data) {
	if (data.term > state_.current_term) {
		if (state_.president) {
			assert(state_.type != kFollower);
			syslog(LOG_NOTICE, "(%s): Higher term detected (%lu): stepping down Leader %s",
			       __func__, data.term, nodeToString(state_.id).c_str());
		}
		stepDownToFollower(StepDownPolicy::kDemoteIfPresident, data.term);
	}
}

void uRaft::stepDownToFollower(StepDownPolicy policy, uint64_t newTerm) {
	const bool wasFollower = state_.type == kFollower;
	const bool shouldDemote = policy == StepDownPolicy::kDemoteIfPresident && !wasFollower;

	if (newTerm > 0U) { state_.current_term = newTerm; }

	state_.type = kFollower;
	state_.president = false;
	state_.voted_for = -1;
	state_.leader_id = -1;

	quorum_loss_streak_ = 0;

	if (shouldDemote) { nodeDemote(); }

	startElectionTimer();
	persistRuntimeState();
}

/*! \brief Checks if a received packet is structurally valid.
 *
 * This function validates only the packet shape (RPC type and expected message size).
 * It deliberately does NOT discard packets with a term lower than the local `state_.current_term`.
 *
 * Rationale:
 * In Raft, a follower should respond to stale RPCs (e.g. RequestVote/AppendEntries with
 * older terms) with `success/result = false` and include its current term in the response.
 * This allows candidates that are behind (for example after a restart) to learn the higher
 * term immediately, step down if needed, and catch up without waiting to increment term
 * across many election timeouts.
 *
 * Dropping lower-term RPCs at the packet-validation layer can significantly increase convergence
 * time, because it prevents the sender from observing the receiver’s higher term.
 * This is especially noticeable where metadata servers restart and then, elections are delayed
 * until the restarted node catches up its term via timeouts.
 *
 * \note Term handling is therefore performed inside the RPC handlers.
 */
bool uRaft::validPacket(const uint8_t *data, size_t size) {
	static unsigned packet_size[kRpcLast] = {sizeof(RpcRequest), sizeof(RpcRequest),
	                                        sizeof(RpcResponse), sizeof(RpcResponse)
	                                        };

	return (data[0] < kRpcLast && size == packet_size[data[0]]);
}

void uRaft::startElectionTimer() {
	int timeout = opt_.election_timeout_min +
	              rand() % (opt_.election_timeout_max - opt_.election_timeout_min);
	election_timer_.expires_after(std::chrono::milliseconds(timeout));
	election_timer_.async_wait(boost::bind(&uRaft::electionTimeout, this,
	                                       boost::asio::placeholders::error));
}

void uRaft::startHearbeatTimer() {
	heartbeat_timer_.expires_after(std::chrono::milliseconds(opt_.heartbeat_period));
	heartbeat_timer_.async_wait(boost::bind(&uRaft::heartbeat, this,
	                                        boost::asio::placeholders::error));
}

void uRaft::signLoyaltyAgreement() {
	state_.loyalty_agreement = true;
	loyalty_agreement_timer_.expires_after(std::chrono::milliseconds(opt_.election_timeout_min));
	loyalty_agreement_timer_.async_wait([this](const boost::system::error_code & error) {
		if (!error) {
			state_.loyalty_agreement = false;
			persistRuntimeState();
		}
	});
	persistRuntimeState();
}

void uRaft::startReceive() {
	socket_.async_receive_from(boost::asio::buffer(packet_data_, kMaxPacketLength), sender_endpoint_,
	                           boost::bind(&uRaft::receivePacket, this,
	                                       boost::asio::placeholders::error,
	                                       boost::asio::placeholders::bytes_transferred));
}

void uRaft::sendHeartbeat() {
	RpcRequest req;

	req.type          = kRpcAppendEntries;
	req.term          = state_.current_term;
	req.time          = state_.local_time;
	req.data_version  = state_.data_version;
	req.node_id       = state_.id;

	for (int i = 0; i < (int)node_.size(); i++) {
		if ((int)i == state_.id) continue;
		socketSend(boost::asio::buffer(&req, sizeof(req)), node_[i].addr);
	}
}

void uRaft::sendRequestForVotes() {
	RpcRequest req;

	req.type         = kRpcRequestVote;
	req.term         = state_.current_term;
	req.time         = state_.local_time;
	req.data_version = state_.data_version;
	req.node_id      = state_.id;

	for (const auto &node : node_) {
		if (node.recv) continue;
		socketSend(boost::asio::buffer(&req, sizeof(req)), node.addr);
	}
}

void uRaft::electionTimeout(const boost::system::error_code &error) {
	if (error) {
		return;
	}

	if (block_leader_promotion_) {
		startElectionTimer();
		return;
	}

	state_.type = kCandidate;
	state_.current_term++;
	state_.voted_for = state_.id;

	for (auto &node : node_) {
		node.vote_granted = false;
		node.recv = false;
	}

	node_[state_.id].vote_granted    = true;
	node_[state_.id].recv            = true;
	node_[state_.id].data_version    = state_.data_version;
	node_[state_.id].heartbeat       = state_.local_time;

	if (opt_.quorum > 1) {
		state_.data_version = nodeGetVersion();
		sendRequestForVotes();
		startElectionTimer();
	} else {
		state_.type = kLeader;
		syslog(LOG_NOTICE, "(%s): Election won with quorum: Promoting node %s to Leader", __func__,
		       nodeToString(state_.id).c_str());
		nodePromote();
	}

	persistRuntimeState();
}

/*! \brief Called every heartbeat_period ms. */
void uRaft::heartbeat(const boost::system::error_code &error) {
	if (error) {
		return;
	}

	startHearbeatTimer();

	state_.local_time++;
	node_[state_.id].heartbeat = state_.local_time;

	int loyal_votes = -1;
	bool has_quorum = false;

	if (state_.type == kLeader) {
		loyal_votes = voteCount(true);
		has_quorum = loyal_votes >= opt_.quorum;
	}

	// Roll back from being the leader if there are less than quorum
	// loyal nodes alive
	if (state_.president) {
		assert(state_.type != kFollower);
		if (!has_quorum) {
			quorum_loss_streak_++;

			syslog(LOG_WARNING,
			       "(%s): Reported quorum loss: %d out of %d before demoting Leader %s", __func__,
			       quorum_loss_streak_, opt_.quorum_loss_grace_heartbeats,
			       nodeToString(state_.id).c_str());

			if (quorum_loss_streak_ < opt_.quorum_loss_grace_heartbeats) {
				sendHeartbeat();  // Heartbeat still needs to be sent
				persistRuntimeState();
				return;  // Quorum hysteresis: tolerate transient loss
			}

			syslog(LOG_WARNING,
			       "(%s): Heartbeat quorum lost: %d out of %d nodes voted for Leader; "
			       "demoting Leader %s to follower",
			       __func__, loyal_votes, opt_.quorum, nodeToString(state_.id).c_str());

			stepDownToFollower(StepDownPolicy::kDemoteIfPresident);
			return;
		}
		quorum_loss_streak_ = 0;
	}

	if (state_.type == kCandidate) {
		sendRequestForVotes();
	}

	if (state_.type == kLeader) {
		sendHeartbeat();
		if (!state_.president) {
			if (has_quorum) {
				state_.president = true;
				syslog(LOG_NOTICE, "(%s): Heartbeat quorum confirmed: Node %s is now active Leader",
				       __func__, nodeToString(state_.id).c_str());
				nodePromote();
			}
		}
	}

	persistRuntimeState();
}

/*! \brief Handling of RPC Append Entries packet. */
void uRaft::rpcAppend(int id, const RpcRequest &data) {
	RpcResponse res;

	assert(state_.type != kLeader);

	// Reply false if term is older than current term, but still send current term back to the
	// sender to let it catch up its term and step down if needed.
	if (data.term < state_.current_term) {
		RpcResponse res{};
		res.type = kRpcAEResponse;
		res.term = state_.current_term;
		res.result = 0;
		res.req_time = data.time;
		res.data_version = state_.data_version;
		socketSend(boost::asio::buffer(&res, sizeof(res)), sender_endpoint_);
		return;
	}

	if (isElectorNode()) {
		// Electors preserve the highest data version from the Leader in every heartbeat,
		// as they do not have a local metadata server to fetch it from.
		// This ensures that electors always have the latest metadata version sent by the Leader and
		// will not vote for candidates with outdated versions.
		state_.data_version = std::max(state_.data_version, data.data_version);
	}

	if (id != state_.leader_id && !isElectorNode()) {
		// When the Leader changes, metadata nodes (non-electors) update their data version from
		// their local metadata server through the nodeGetVersion() function.
		state_.data_version = nodeGetVersion();
	}

	res.type         = kRpcAEResponse;
	res.term         = state_.current_term;
	res.result       = 1;
	res.req_time     = data.time;
	res.data_version = state_.data_version;

	if (id != state_.leader_id) nodeLeader(id);
	state_.leader_id = id;
	signLoyaltyAgreement();

	socketSend(boost::asio::buffer(&res, sizeof(res)), sender_endpoint_);

	state_.type = kFollower;
	startElectionTimer();
	persistRuntimeState();
}

/*! \brief Handling of RPC Append Response packet. */
void uRaft::rpcAppendResponse(int id, const RpcResponse &data) {
	// Stale response
	if (data.term < state_.current_term) { return; }

	if (state_.type != kLeader) {
		return;
	}

	node_[id].heartbeat    = std::max(node_[id].heartbeat,data.req_time);
	node_[id].data_version = data.data_version;
	node_[id].vote_granted = true;
	node_[id].recv         = true;

	persistRuntimeState();
}

/*! \brief Handling of RPC Request Vote packet. */
void uRaft::rpcReqVote(int id, const RpcRequest &data) {
	RpcResponse res;

	if (state_.voted_for < 0 && !isElectorNode()) {
		// Only metadata nodes (non-electors) update their data version from local metadata server.
		// Electors have no local metadata server, so they preserve the highest version ever seen
		// through heartbeats in rpcAppend().
		state_.data_version = nodeGetVersion();
	}

	res.type         = kRpcRVResponse;
	res.term         = state_.current_term;
	res.req_time     = data.time;
	res.data_version = state_.data_version;
	res.result       = state_.type == kFollower &&
	                   data.term >= state_.current_term &&
	                   data.data_version >= state_.data_version &&
	                   ((state_.loyalty_agreement && state_.leader_id == id) ||
	                    (!state_.loyalty_agreement &&
	                     (state_.voted_for < 0 || state_.voted_for == id)));

	socketSend(boost::asio::buffer(&res, sizeof(res)), sender_endpoint_);

	if (res.result) {
		state_.voted_for = id;
		startElectionTimer();
	}

	persistRuntimeState();
}

/*! \brief Handling of RPC Request Vote Response packet. */
void uRaft::rpcReqVoteResponse(int id, const RpcResponse &data) {
	// Stale response
	if (data.term < state_.current_term) { return; }

	// A vote response is proof that the peer is alive and reachable, so refresh its
	// freshness timestamp immediately. This avoids treating a freshly elected quorum as
	// stale until the first AppendEntries round-trip completes.
	node_[id].heartbeat = std::max(node_[id].heartbeat, data.req_time);

	if (state_.type != kCandidate) {
		return;
	}

	node_[id].vote_granted = data.result;
	node_[id].recv         = true;

	if (voteCount(false) >= opt_.quorum) {
		state_.type      = kLeader;
		state_.leader_id = state_.id;
		sendHeartbeat();
		election_timer_.cancel();
	}

	persistRuntimeState();
}

/*! \brief Dispatching received packet to proper handler. */
void uRaft::receivePacket(const boost::system::error_code &error,  size_t bytes_recvd) {
	if (!error && bytes_recvd > 0) {
		int id = findNodeID(sender_endpoint_);

		if (id >= 0 && validPacket(packet_data_.data(), bytes_recvd)) {
			checkTerm(id, *reinterpret_cast<RpcHeader *>(packet_data_.data()));
			switch (packet_data_[0]) {
			case kRpcAppendEntries :
				rpcAppend(id, *reinterpret_cast<RpcRequest *>(packet_data_.data()));
				break;
			case kRpcRequestVote:
				rpcReqVote(id, *reinterpret_cast<RpcRequest *>(packet_data_.data()));
				break;
			case kRpcAEResponse:
				rpcAppendResponse(id, *reinterpret_cast<RpcResponse *>(packet_data_.data()));
				break;
			case kRpcRVResponse:
				rpcReqVoteResponse(id, *reinterpret_cast<RpcResponse *>(packet_data_.data()));
				break;
			}
		}
	}

	startReceive();
}

void uRaft::init() {
	udp::resolver resolver(io_service_);

	node_.resize(opt_.server.size());

	for (auto & node : node_) {
		node.data_version = 0;
		node.heartbeat    = 0;
		node.recv         = false;
		node.vote_granted = false;
	}

	for (int i = 0; i < (int)node_.size(); i++) {
		std::string::size_type p = opt_.server[i].find(":");
		boost::system::error_code err;
		udp::resolver::results_type results;


		if (p != std::string::npos) {
			results = resolver.resolve(opt_.server[i].substr(0, p), opt_.server[i].substr(p + 1), err);
			if (err) {
				throw std::runtime_error("Failed to resolve dns name '" + opt_.server[i] + "'");
			}
		} else {
			results = resolver.resolve(opt_.server[i], boost::lexical_cast<std::string>(opt_.port), err);
			if (err) {
				throw std::runtime_error("Failed to resolve dns name '" + opt_.server[i]
				                         + ":" + boost::lexical_cast<std::string>(opt_.port)+"'");
			}
		}

		if (results.empty()) {
			throw std::runtime_error(
			    "Failed to resolve dns name '" + opt_.server[i] + ":" +
			    boost::lexical_cast<std::string>(opt_.port) +
			    "':" + "No results found");
		}
		node_[i].addr = results.begin()->endpoint();
	}

	state_.id = opt_.id;
	if (state_.id < 0) {
#if defined(SAUNAFS_HAVE_GETIFADDRS)
		unsigned detect_wait_seconds = 0;

		while ((state_.id = scanLocalInterfaces()) < 0) {
			if (detect_wait_seconds == 0 || detect_wait_seconds % 30 == 0) {
				syslog(LOG_WARNING,
				       "Waiting for local uRaft node address before auto-detecting node id");
			}
			std::this_thread::sleep_for(std::chrono::seconds(1));
			++detect_wait_seconds;
		}
		syslog(LOG_NOTICE, "Detected local uRaft node id %d after %u seconds", state_.id,
		       detect_wait_seconds);
#else
		throw std::runtime_error("Invalid id");
#endif
	}

	opt_.quorum = opt_.server.size() / 2 + 1;
	state_.local_time   = opt_.election_timeout_min / opt_.heartbeat_period + 1;

	const std::string bind_target = nodeToString(state_.id);
	unsigned          wait_seconds = 0;
	const auto        address_not_available =
	    boost::system::errc::make_error_condition(boost::system::errc::address_not_available);

	while (true) {
		boost::system::error_code open_error;
		socket_.open(boost::asio::ip::udp::v4(), open_error);
		if (open_error) {
			throw std::runtime_error("Failed to open uRaft UDP socket: " + open_error.message());
		}

		boost::system::error_code bind_error;
		socket_.bind(node_[state_.id].addr, bind_error);
		if (!bind_error) {
			if (wait_seconds > 0) {
				syslog(LOG_NOTICE,
				       "Local uRaft address %s became available after %u seconds",
				       bind_target.c_str(), wait_seconds);
			}
			break;
		}

		socket_.close();

			if (bind_error != address_not_available) {
			throw std::runtime_error("Failed to bind uRaft UDP socket to '" + bind_target +
			                         "': " + bind_error.message());
		}

		if (wait_seconds == 0 || wait_seconds % 30 == 0) {
			syslog(LOG_WARNING,
			       "Waiting for local uRaft address %s before binding UDP socket",
			       bind_target.c_str());
		}
		std::this_thread::sleep_for(std::chrono::seconds(1));
		++wait_seconds;
	}

	restorePersistentState();
	bootstrapLeaderState();
	if (state_.type != kLeader) {
		startElectionTimer();
	}
	startHearbeatTimer();
	startReceive();

	// In case of fast restart
	// It's possible that before restart we have signed agreement with a leader.
	// Now we wait for this agreement to expire.
	signLoyaltyAgreement();

	persistRuntimeState();
}

void uRaft::demoteLeader() {
	if (state_.type == kFollower) {
		return;
	}

	syslog(LOG_NOTICE, "(%s): Manual demotion: Stepping down Leader %s to follower", __func__,
	       nodeToString(state_.id).c_str());

	stepDownToFollower(StepDownPolicy::kRaftOnly);
}

void uRaft::set_block_promotion(bool block) {
	block_leader_promotion_ = block;
}

void uRaft::set_options(const Options &opt) {
	opt_ = opt;
}

int uRaft::findMatchingAddress(const boost::asio::ip::address &addr, int &id) {
	int count = 0;

	for (int i = 0; i < (int)node_.size(); i++) {
		if (node_[i].addr.address() == addr) {
			count++;
			id = i;
		}
	}

	return count;
}

int uRaft::scanLocalInterfaces() {
#if defined(SAUNAFS_HAVE_GETIFADDRS)
	struct ifaddrs *ifaddr, *ifa;

	if (getifaddrs(&ifaddr) == -1) {
		return -1;
	}

	int count = 0, id = -1;
	for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr == NULL) {
			continue;
		}

		int family = ifa->ifa_addr->sa_family;
		if (family != AF_INET) {
			continue;
		}

		sockaddr_in *saddr = reinterpret_cast<sockaddr_in *>(ifa->ifa_addr);
		boost::asio::ip::address addr = boost::asio::ip::address_v4(saddr->sin_addr.s_addr);

		count += findMatchingAddress(addr, id);
	}

	freeifaddrs(ifaddr);

	if (count != 1) {
		return -1;
	}

	assert(id >= 0);
	return id;
#else
	return -1;
#endif
}

std::string uRaft::nodeToString(int id) {
	if (id < 0 || id >= static_cast<int>(opt_.server.size())) {
		return "Invalid node ID";
	}

	std::string name = opt_.server[id];
	std::string::size_type p = name.find(":");

	if (p != std::string::npos) {
		name = name.substr(0, p);
	}

	return name;
}
