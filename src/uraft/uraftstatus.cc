#include "common/platform.h"  // IWYU pragma: keep

#include <iterator>

#include <syslog.h>

#include <fmt/format.h>

#include "uraftstatus.h"

namespace {
constexpr int kStatusCachePeriodMs = 100;
constexpr int kRetryBackoffPeriodMs = 500;

enum class AcceptAction : uint8_t {
	ImmediateRetry,  // re-arm accept immediately
	BackoffRetry,    // retry after delay
	Stop             // stop accepting (close acceptor)
};

AcceptAction classifyAcceptError(const boost::system::error_code &errorCode) {
	using namespace boost::asio::error;

	// Explicit shutdown / cancellation
	// Happens on io_context stop(), acceptor.close(), or destructor.
	if (errorCode == operation_aborted) { return AcceptAction::Stop; }

	// Resource exhaustion (can cause tight loops)
	// These MUST back off or they will spin at 100% CPU.
	if (errorCode == no_descriptors ||  // EMFILE / ENFILE
	    errorCode == no_memory) {       // ENOMEM
		return AcceptAction::BackoffRetry;
	}

	// Transient / recoverable conditions
	// Very common on Linux; retry immediately is safe.
	if (errorCode == interrupted ||         // EINTR
	    errorCode == try_again ||           // EAGAIN
	    errorCode == would_block ||         // EWOULDBLOCK
	    errorCode == connection_aborted ||  // ECONNABORTED
	    errorCode == network_down ||        // ENETDOWN
	    errorCode == network_reset ||       // ENETRESET
	    errorCode == host_unreachable) {    // EHOSTUNREACH
		return AcceptAction::ImmediateRetry;
	}

	// Programming / configuration errors
	// These indicate a broken acceptor; continuing is pointless.
	if (errorCode == bad_descriptor ||  // EBADF
	    errorCode == access_denied ||   // EACCES
	    errorCode == address_family_not_supported || errorCode == invalid_argument) {
		return AcceptAction::Stop;
	}

	// Default: be resilient
	// For unknown errors, prefer liveness over silence.
	return AcceptAction::ImmediateRetry;
}

void logAsioError(int priority, const char *what, const boost::system::error_code &error) {
	if (!error) { return; }
	syslog(priority, "uRaftStatus: %s: %s (%d)", what, error.message().c_str(), error.value());
}

void shutdownAndCloseSocket(boost::asio::ip::tcp::socket &socket) {
	boost::system::error_code result_error;

	// Shutdown the socket on both ends.
	socket.shutdown(boost::asio::socket_base::shutdown_both, result_error);
	logAsioError(LOG_DEBUG, "socket shutdown failed", result_error);

	// Close the socket.
	socket.close(result_error);
	logAsioError(LOG_DEBUG, "socket close failed", result_error);
}
}  // namespace

uRaftStatusConnection::uRaftStatusConnection(boost::asio::io_context &ios)
	: socket_(ios) {
}

void uRaftStatusConnection::set_data(std::shared_ptr<const std::vector<uint8_t>> data) {
	// Safe to move: the parameter is passed by value (not a reference), so the caller's
	// shared_ptr is unaffected. The underlying vector data remains valid until this
	// connection destroys its reference, even if the cache is refreshed with new data.
	data_ = std::move(data);
}

void uRaftStatusConnection::init() {
	auto self(shared_from_this());
	if (!data_ || data_->empty()) {
		shutdownAndCloseSocket(socket_);
		return;
	}

	boost::asio::async_write(socket_, boost::asio::buffer(*data_),
	[self](const boost::system::error_code &error, std::size_t /*length*/) {
		if (error && error != boost::asio::error::operation_aborted) {
			logAsioError(LOG_WARNING, "async_write failed", error);
		}
		shutdownAndCloseSocket(self->socket_);
	});
}

boost::asio::ip::tcp::socket &uRaftStatusConnection::socket() {
	return socket_;
}

uRaftStatus::uRaftStatus(boost::asio::io_context &ios)
	: uRaft(ios),
	  status_work_guard_(boost::asio::make_work_guard(status_io_)),
	  status_cache_timer_(ios),
	  retry_timer_(status_io_),
	  cached_status_(std::make_shared<const std::vector<uint8_t>>()),
	  acceptor_(status_io_) {
}

uRaftStatus::~uRaftStatus() {
	status_work_guard_.reset();

	boost::system::error_code error;
	status_cache_timer_.cancel(error);
	logAsioError(LOG_WARNING, "status cache timer cancel failed", error);

	if (acceptor_.is_open()) {
		acceptor_.close(error);
		logAsioError(LOG_WARNING, "acceptor close failed", error);
	}

	status_io_.stop();
	if (status_thread_.joinable()) {
		status_thread_.join();
	}
}

void uRaftStatus::init() {
	uRaft::init();

	// Populate the cache immediately so the first connection never returns empty.
	{
		std::vector<uint8_t> response;
		storeData(response);
		cached_status_.store(std::make_shared<const std::vector<uint8_t>>(std::move(response)),
		                     std::memory_order_release);
	}

	// IPv4-only listener
	{
		boost::system::error_code error_code;
		boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::tcp::v4(), opt_.status_port);
		using socket_base = boost::asio::socket_base;

		acceptor_.open(endpoint.protocol(), error_code);
		if (error_code) {
			logAsioError(LOG_ERR, "acceptor.open failed", error_code);
			return;
		}

		acceptor_.set_option(socket_base::reuse_address(true), error_code);
		logAsioError(LOG_WARNING, "acceptor.set_option(reuse_address) failed", error_code);

		acceptor_.bind(endpoint, error_code);
		if (error_code) {
			logAsioError(LOG_ERR, "acceptor.bind failed", error_code);
			return;
		}

		acceptor_.listen(socket_base::max_listen_connections, error_code);
		if (error_code) {
			logAsioError(LOG_ERR, "acceptor.listen failed", error_code);
			return;
		}
	}

	// Refresh cache frequently from the main Raft io_context.
	status_cache_timer_.expires_from_now(boost::posix_time::millisec(kStatusCachePeriodMs));
	status_cache_timer_.async_wait([this](const boost::system::error_code &error) {
		refreshStatusCache(error);
	});

	// Start accepting connections
	acceptConnection();

	status_thread_ = std::thread([this]() {
		status_io_.run();
	});
}

void uRaftStatus::set_options(const Options &opt) {
	uRaft::set_options(opt);
	opt_ = opt;
}

void uRaftStatus::storeData(std::vector<uint8_t> &response) {
	response.clear();
	auto out = std::back_inserter(response);

	const char *stateName = "UNKNOWN";
	switch (state_.type) {
		case kFollower:
			stateName = "FOLLOWER";
			break;
		case kCandidate:
			stateName = "CANDIDATE";
			break;
		case kLeader:
			stateName = "LEADER";
			break;
		default:
			break;
	}
	static constexpr double kMillisPerSecond = 1000.0;

	fmt::format_to(out, "SERVER ID {}\n", opt_.id);
	if (state_.president) {
		fmt::format_to(out, "I'M THE BOOSSSS\n");
	}
	fmt::format_to(out, "president={}\n", static_cast<int>(state_.president));
	fmt::format_to(out, "state={}\n", stateName);
	fmt::format_to(out, "term={}\n", state_.current_term);
	fmt::format_to(out, "voted_for={}\n", state_.voted_for);
	fmt::format_to(out, "leader_id={}\n", state_.leader_id);
	fmt::format_to(out, "data_version={}\n", state_.data_version);
	fmt::format_to(out, "loyalty_agreement={}\n", static_cast<int>(state_.loyalty_agreement));
	fmt::format_to(out, "local_time={}\n", state_.local_time);
	fmt::format_to(out, "blocked_promote={}\n", static_cast<int>(block_leader_promotion_));

	if (state_.type != kFollower) {
		auto appendNodeLine = [&](const char *prefix, auto &&formatValue) {
			fmt::format_to(out, "{}", prefix);
			const int nodeCount = static_cast<int>(node_.size());
			for (int i = 0; i < nodeCount; i++) {
				if (i > 0) { fmt::format_to(out, "|"); }
				formatValue(out, i);
			}
			fmt::format_to(out, "]\n");
		};

		appendNodeLine("votes=[", [&](auto outIt, int nodeIndex) {
			fmt::format_to(outIt, "{:8}", static_cast<int>(node_[nodeIndex].vote_granted));
		});

		appendNodeLine("heart=[", [&](auto outIt, int nodeIndex) {
			const auto deltaHeartbeats = (state_.local_time >= node_[nodeIndex].heartbeat)
			                               ? (state_.local_time - node_[nodeIndex].heartbeat)
			                               : 0;
			const double deltaMillis = static_cast<double>(opt_.heartbeat_period) *
			                          static_cast<double>(deltaHeartbeats);
			fmt::format_to(outIt, "{:8.2f}", (deltaMillis / kMillisPerSecond));
		});

		appendNodeLine("recv =[", [&](auto outIt, int nodeIndex) {
			fmt::format_to(outIt, "{:8}", static_cast<int>(node_[nodeIndex].recv));
		});

		appendNodeLine("ver  =[", [&](auto outIt, int nodeIndex) {
			fmt::format_to(outIt, "{:8}", node_[nodeIndex].data_version);
		});
	}
}

void uRaftStatus::acceptConnection() {
	if (!acceptor_.is_open()) {
		return;
	}

	auto conn = std::make_shared<uRaftStatusConnection>(status_io_);
	acceptor_.async_accept(conn->socket(), [this, conn](const boost::system::error_code &error) {
		if (!error) { // Successful accept
			// Schedule the next accept on completion (1 outstanding accept at a time).
			acceptConnection();

			boost::system::error_code result_error;
			conn->socket().set_option(boost::asio::ip::tcp::no_delay(true), result_error);
			logAsioError(LOG_WARNING, "set_option(TCP_NODELAY) failed", result_error);

			// The atomic load increases the shared_ptr ref count, ensuring the data remains valid
			// until the connection completes. This is safe even if the cache is refreshed with new
			// data while the connection is active.
			conn->set_data(cached_status_.load(std::memory_order_acquire));
			conn->init();
			return;
		}

		if (error == boost::asio::error::operation_aborted) {
			logAsioError(LOG_DEBUG, "async_accept cancelled (shutdown)", error);
		} else {
			logAsioError(LOG_WARNING, "async_accept failed", error);
		}

		switch (classifyAcceptError(error)) {
		case AcceptAction::ImmediateRetry:
			acceptConnection();
			break;
		case AcceptAction::BackoffRetry:
			retry_timer_.expires_from_now(boost::posix_time::milliseconds(kRetryBackoffPeriodMs));
			retry_timer_.async_wait([this](const boost::system::error_code &tec) {
				if (!tec && acceptor_.is_open()) { acceptConnection(); }
			});
			break;
		case AcceptAction::Stop:
			boost::system::error_code close_error;
			acceptor_.close(close_error);
			logAsioError(LOG_WARNING, "acceptor close failed", close_error);
			return;
		}
	});
}

void uRaftStatus::refreshStatusCache(const boost::system::error_code &error) {
	if (error) {
		return;
	}

	std::vector<uint8_t> response;
	storeData(response);
	cached_status_.store(std::make_shared<const std::vector<uint8_t>>(std::move(response)),
	                     std::memory_order_release);

	status_cache_timer_.expires_from_now(boost::posix_time::millisec(kStatusCachePeriodMs));
	status_cache_timer_.async_wait([this](const boost::system::error_code &error) {
		refreshStatusCache(error);
	});
}
