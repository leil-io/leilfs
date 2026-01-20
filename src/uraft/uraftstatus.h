#pragma once

// NOLINTNEXTLINE(misc-include-cleaner)
#include "common/platform.h"  // IWYU pragma: keep

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "uraft.h"

/*! \brief Helper class for uRaftStatus.
 *
 * Manages connection from client.
 */
class uRaftStatusConnection : public std::enable_shared_from_this<uRaftStatusConnection> {
public:
	uRaftStatusConnection(boost::asio::io_context &ios);
	~uRaftStatusConnection() = default;
	uRaftStatusConnection(const uRaftStatusConnection &) = delete;
	uRaftStatusConnection &operator=(const uRaftStatusConnection &) = delete;
	uRaftStatusConnection(uRaftStatusConnection &&) = delete;
	uRaftStatusConnection &operator=(uRaftStatusConnection &&) = delete;

	void init();
	boost::asio::ip::tcp::socket& socket();
	void set_data(std::shared_ptr<const std::vector<uint8_t>> data);

private:
	boost::asio::ip::tcp::socket socket_;
	std::shared_ptr<const std::vector<uint8_t>> data_;

};


/*! \brief uRaft status server.
 *
 * Server listen on specified tcp port and replies with information about uRaft status.
 */
class uRaftStatus : public uRaft {
public:
	struct Options : uRaft::Options {
		int status_port = 0;
	};

	uRaftStatus(boost::asio::io_context &ios);
	~uRaftStatus() override;
	uRaftStatus(const uRaftStatus &) = delete;
	uRaftStatus &operator=(const uRaftStatus &) = delete;
	uRaftStatus(uRaftStatus &&) = delete;
	uRaftStatus &operator=(uRaftStatus &&) = delete;

	//! Initialize server.
	void init();

	//! Set options.
	void set_options(const Options &opt);

protected:
	void acceptConnection();
	void storeData(std::vector<uint8_t> &response);

	/// @brief Refresh the cached TCP status payload.
	///
	/// The TCP status server runs on a dedicated io_context thread so it can keep accepting and
	/// replying even when the main uRaft/controller io_context is temporarily busy (e.g. due to
	/// blocking helper invocations).
	///
	/// This callback periodically rebuilds the status payload on the main uRaft io_context thread
	/// and publishes it via an atomic shared_ptr. Status connections then only read the already
	/// prepared payload, avoiding any heavy work in the status-thread hot path.
	///
	/// \param error Timer completion status. When set, the refresh is skipped (e.g. shutdown).
	void refreshStatusCache(const boost::system::error_code &error);

private:
	/// @brief Dedicated io_context used for the TCP status listener and connection I/O.
	///
	/// This is intentionally decoupled from the main uRaft io_context to prevent status queries
	/// from stalling when the controller performs blocking operations on the main event loop.
	boost::asio::io_context status_io_;

	/// @brief Work guard keeping the status io_context alive until destruction.
	boost::asio::executor_work_guard<boost::asio::io_context::executor_type> status_work_guard_;

	/// @brief Thread running the status io_context event loop.
	/// This is the thread where the TCP listener and connections run.
	std::thread status_thread_;

	/// @brief Timer used to periodically refresh the cached status payload on the main io_context.
	/// By default, the cache is refreshed every 100ms.
	boost::asio::deadline_timer status_cache_timer_;

	/// @brief Timer used to back off accept retries on resource exhaustion.
	///
	/// This prevents tight accept() failure loops (e.g. file descriptor exhaustion) from spinning
	/// at 100% CPU on the status thread.
	boost::asio::deadline_timer retry_timer_;

	/// @brief Latest serialized status payload served to TCP clients.
	///
	/// The payload is stored as an atomic shared_ptr so connections can safely keep a reference
	/// to the buffer while writing, even if the cache is refreshed concurrently.
	std::atomic<std::shared_ptr<const std::vector<uint8_t>>> cached_status_;

	/// @brief TCP acceptor bound to the status port (runs on status_io_).
	boost::asio::ip::tcp::acceptor acceptor_;

	/// @brief uRaft status configuration (includes the status port).
	Options                        opt_;
};
