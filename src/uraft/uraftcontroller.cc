#include "common/platform.h"

#include "common/time_utils.h"
#include "uraft.h"
#include "uraftcontroller.h"

#include <poll.h>
#include <sys/syslog.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>
#include <cstdio>
#include <memory>

#define BOOST_BIND_GLOBAL_PLACEHOLDERS
#include <boost/bind.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/version.hpp>
#include <boost/chrono.hpp>

constexpr int kCommandTimeoutMs = 5000;  ///< Default command timeout in milliseconds.

// Delay used both as a grace period for transient failures and as the retry interval while the
// metadata server remains dead (single knob).
constexpr int kDeadRecoveryDelayMs = 2000;

uRaftController::uRaftController(boost::asio::io_context &ios)
	: uRaftStatus(ios),
	  check_cmd_status_timer_(ios),
	  check_node_status_timer_(ios),
	  cmd_timeout_timer_(ios),
	  promotion_backoff_timer_(ios),
	  dead_recovery_timer_(ios) {
	command_pid_  = -1;
	command_type_ = kCmdNone;
	is_demote_pending_ = false;
	is_promote_pending_ = false;
	node_alive_   = false;

	opt_.check_node_status_period = 250;
	opt_.check_cmd_status_period  = 100;
	opt_.check_floating_ip_period = 500;
	opt_.getversion_timeout       = 100;
	opt_.promote_timeout          = 1000000;
	opt_.demote_timeout           = 1000000;
	opt_.dead_handler_timeout     = 1000000;
}

uRaftController::~uRaftController() {
}

void uRaftController::init() {
	uRaftStatus::init();

	set_block_promotion(true);
	if (opt_.elector_mode) {
		return;
	}

	check_cmd_status_timer_.expires_after(std::chrono::milliseconds(opt_.check_cmd_status_period));
	check_cmd_status_timer_.async_wait(boost::bind(&uRaftController::checkCommandStatus, this,
	                                   boost::asio::placeholders::error));

	check_node_status_timer_.expires_after(std::chrono::milliseconds(opt_.check_node_status_period));
	check_node_status_timer_.async_wait(boost::bind(&uRaftController::checkNodeStatus, this,
	                                    boost::asio::placeholders::error));

	syslog(LOG_NOTICE, "Saunafs-uraft initialized properly");
}

void uRaftController::set_options(const uRaftController::Options &opt) {
	uRaftStatus::set_options(opt);
	opt_ = opt;
}

void uRaftController::nodePromote() {
	if (promotion_backoff_active_) {
		syslog(LOG_WARNING, "Skipping promotion: backoff active");
		return;
	}

	syslog(LOG_NOTICE, "Starting metadata server switch to master mode");

	// Prevent concurrent transitions
	if (command_pid_ >= 0) {
		if (command_type_ == kCmdDemote) {
			// Make nodePromote() idempotent with respect to pending promotions
			if (!is_promote_pending_) {
				syslog(
				    LOG_WARNING,
				    "Promotion requested during demotion - will be started after completing demotion");
				is_promote_pending_ = true;
			}
			return;
		}
		// Already promoting
		syslog(LOG_DEBUG, "Promotion already in progress (PID %d)", command_pid_);
		return;
	}

	setSlowCommandTimeout(opt_.promote_timeout);
	if (runSlowCommand("leil-uraft-helper promote")) {
		command_type_ = kCmdPromote;
		// Floating IP Manager will be started after promotion completes successfully
	}
}

void uRaftController::nodeDemote() {
	syslog(LOG_NOTICE, "Starting metadata server switch to slave mode");

	// Stop Floating IP Manager immediately when starting demotion
	stopFloatingIpManager();

	if (command_pid_ >= 0) {
		if (command_type_ == kCmdPromote) {
			// Make nodeDemote() idempotent with respect to pending demotions
			if (!is_demote_pending_) {
				syslog(
				    LOG_WARNING,
				    "Demotion requested during promotion - will be started after completing promotion");
				is_demote_pending_ = true;
			}
			return;
		}
		syslog(LOG_DEBUG, "Demotion already in progress (PID %d)", command_pid_);
		return;
	}

	setSlowCommandTimeout(opt_.demote_timeout);
	if (runSlowCommand("leil-uraft-helper demote")) {
		command_type_ = kCmdDemote;
	}
}

uint64_t uRaftController::nodeGetVersion() {
	if (opt_.elector_mode) {
		return 0;
	}

	std::string versionStr;
	std::vector<std::string> params = {"leil-uraft-helper", "metadata-version",
	                                   opt_.local_master_server,
	                                   boost::lexical_cast<std::string>(opt_.local_master_port)};

	if (!runCommand(params, versionStr, opt_.getversion_timeout)) {
		syslog(LOG_WARNING, "Get metadata version timeout - metadata server may be hung");
		// Return 0 instead of cached version to prevent stale node from being elected
		return 0;
	}

	try {
		auto version = boost::lexical_cast<uint64_t>(versionStr.c_str());
		return version;
	} catch (...) {
		syslog(LOG_ERR,
		       "Invalid metadata version output (got: '%s') - metadata server appears hung",
		       versionStr.substr(0, 100).c_str());
		// Return 0 to prevent unhealthy node from being elected
		return 0;
	}
}

bool uRaftController::isElectorNode() const {
	return opt_.elector_mode != 0;
}

void uRaftController::nodeLeader(int id) {
	auto leaderNode = nodeToString(id);
	syslog(LOG_NOTICE, "Node '%s' is now a leader.", leaderNode.c_str());
}

/*! \brief Check promote/demote script status. */
void uRaftController::checkCommandStatus(const boost::system::error_code &error) {
	if (error) return;

	int  status;
	if (checkSlowCommand(status)) {
		cmd_timeout_timer_.cancel();

		// Check if command completed successfully
		bool commandSucceeded = WIFEXITED(status) && WEXITSTATUS(status) == 0;

		if (command_type_ == kCmdDemote) {
			if (commandSucceeded) {
				syslog(LOG_NOTICE, "Metadata server switch to slave mode succeeded");
			} else {
				syslog(LOG_ERR, "Demotion failed with exit code: %d", WEXITSTATUS(status));
			}

			command_type_ = kCmdNone;
			command_pid_  = -1;

			if (is_promote_pending_) {
				// Only execute pending promotion if node is still Leader. This prevents
				// synchronization issues between uRaft state and SaunaFS state.
				if (state_.type == kLeader) {
					syslog(LOG_WARNING, "Starting pending promotion to master mode for the Leader");
					nodePromote();
				} else {
					syslog(LOG_WARNING, "Canceling pending promotion (no longer Leader, state=%d)",
					       state_.type);
				}
				is_promote_pending_ = false;
			}
		} else if (command_type_ == kCmdPromote) {
			if (commandSucceeded) {
				syslog(LOG_NOTICE, "Metadata server switch to master mode done");
				startPromotionBackoff(/*reset=*/true);

				if (state_.type == kLeader) {
					// Start floating IP only if promoted node is still Leader
					startFloatingIpManager();
				} else {
					syslog(LOG_ERR,
					       "Promotion completed but no longer uRaft Leader (state=%d) - reverting",
					       state_.type);
					// Request demotion to fix inconsistency (may be deferred)
					nodeDemote();
				}
			} else {
				syslog(LOG_ERR, "Promotion failed with exit code: %d",
				       WIFEXITED(status) ? WEXITSTATUS(status) : -1);
				handlePromotionFailure();
			}

			command_type_ = kCmdNone;
			command_pid_  = -1;

			if (is_demote_pending_) {
				// Only demote if node is no longer Leader
				if (state_.type != kLeader) {
					syslog(LOG_WARNING,
					       "Starting pending demotion to slave mode: no longer the Leader");
					nodeDemote();
				} else {
					syslog(LOG_WARNING, "Canceling pending demotion: still Leader");
				}
				is_demote_pending_ = false;
			}
		} else if (command_type_ == kCmdStatusDead) {
			if (commandSucceeded) {
				syslog(LOG_NOTICE, "Dead-metadata handler completed");
			} else {
				syslog(LOG_ERR, "Dead-metadata handler failed with exit code: %d",
				       WIFEXITED(status) ? WEXITSTATUS(status) : -1);
			}
			command_type_ = kCmdNone;
			command_pid_  = -1;
		}
	}

	check_cmd_status_timer_.expires_after(std::chrono::milliseconds(opt_.check_cmd_status_period));
	check_cmd_status_timer_.async_wait(boost::bind(&uRaftController::checkCommandStatus, this,
	                                   boost::asio::placeholders::error));
}

/*! \brief Check metadata server status. */
void uRaftController::checkNodeStatus(const boost::system::error_code &error) {
	if (error) return;

	std::vector<std::string> params = { "leil-uraft-helper", "isalive" };
	std::string              result;
	bool                     is_alive = node_alive_;

	if (command_type_ == kCmdNone) {
		if (runCommand(params, result, opt_.getversion_timeout)) {
			if (result == "alive" || result == "dead") {
				is_alive = result == "alive";
			} else {
				syslog(LOG_ERR, "Invalid metadata server status.");
			}
		} else {
			syslog(LOG_WARNING, "(%s): Isalive timeout reported after %d ms.", __func__,
			       opt_.getversion_timeout);
		}

		// Always reconcile promotion blocking
		const bool block_promotion = (!is_alive) || promotion_backoff_active_;
		set_block_promotion(block_promotion);

		if (is_alive != node_alive_) {
			if (is_alive) {
				syslog(LOG_NOTICE, "Metadata server is alive");
				cancelDeadRecovery();
			} else {
				syslog(LOG_NOTICE, "Metadata server is dead");
				// Controller-owned dead handling:
				// 1) Raft-only step-down (no implicit nodeDemote()) so another node can take over.
				// 2) Run the helper's 'dead' command to release floating IPs without restarting sfsmaster.
				stepDownToFollower(StepDownPolicy::kRaftOnly);
				startDeadMetadataHandler();
				scheduleDeadRecovery();
			}
			node_alive_ = is_alive;
		}
	}

	check_node_status_timer_.expires_after(std::chrono::milliseconds(opt_.check_node_status_period));
	check_node_status_timer_.async_wait(boost::bind(&uRaftController::checkNodeStatus, this,
	                                    boost::asio::placeholders::error));
}

void uRaftController::startDeadMetadataHandler() {
	// Ensure floating IP manager stops trying to restore IPs.
	stopFloatingIpManager();

	// Prevent concurrent transitions; caller should already ensure kCmdNone.
	if (command_pid_ >= 0) {
		syslog(LOG_WARNING,
		       "Dead-metadata handler requested while another command is running (type=%d pid=%d)",
		       command_type_, command_pid_);
		return;
	}

	setSlowCommandTimeout(opt_.dead_handler_timeout);
	if (runSlowCommand("leil-uraft-helper dead")) {
		command_type_ = kCmdStatusDead;
	}
}

void uRaftController::cancelDeadRecovery() {
	dead_recovery_pending_ = false;
	dead_recovery_timer_.cancel();
}

void uRaftController::scheduleDeadRecovery() {
	// Idempotent: don't stack multiple recovery timers
	if (dead_recovery_pending_) { return; }
	dead_recovery_pending_ = true;

	dead_recovery_timer_.expires_after(std::chrono::milliseconds(kDeadRecoveryDelayMs));
	dead_recovery_timer_.async_wait([this](const boost::system::error_code &ec) {
		if (ec) { return; }

		// If metadata recovered, stop dead recovery process
		if (node_alive_) {
			cancelDeadRecovery();
			return;
		}

		// If another command is running, retry after the same delay
		if (command_pid_ >= 0 || command_type_ != kCmdNone) {
			cancelDeadRecovery();
			scheduleDeadRecovery();
			return;
		}

		// Still dead and safe to act: restart shadow via demote path
		syslog(LOG_WARNING, "Metadata still dead: restarting shadow via demote");
		dead_recovery_pending_ = false;
		nodeDemote();
	});
}

void uRaftController::setSlowCommandTimeout(int timeout) {
	cmd_timeout_timer_.expires_after(std::chrono::milliseconds(timeout));
	cmd_timeout_timer_.async_wait([this, timeout](const boost::system::error_code & error) {
		if (!error) {
			syslog(LOG_ERR, "Metadata server mode switching timeout after %d ms", timeout);

			// Cleanup based on operation type
			if (command_type_ == kCmdPromote) {
				syslog(LOG_WARNING, "Promotion timeout - attempting cleanup");
				handlePromotionFailure();
			} else if (command_type_ == kCmdDemote) {
				syslog(LOG_WARNING, "Demotion timeout - killing stuck command");
			} else if (command_type_ == kCmdStatusDead) {
				syslog(LOG_WARNING, "Dead handler timeout - killing stuck command");
			}

			stopSlowCommand();
		}
	});
}

//! Check if slow command stopped working.
bool uRaftController::checkSlowCommand(int &status) {
	if (command_pid_ < 0) {
		return false;
	}
	return waitpid(command_pid_, &status, WNOHANG) > 0;
}

//! Kills slow command.
bool uRaftController::stopSlowCommand() {
	if (command_pid_ < 0) {
		return false;
	}

	int status;
	kill(command_pid_, SIGKILL);
	waitpid(command_pid_, &status, 0);

	command_pid_  = -1;
	command_type_ = kCmdNone;

	return true;
}

/*! \brief Start new program.
 *
 * \param cmd String with name and parameters of program to run.
 * \return true if there was no error.
 */
bool uRaftController::runSlowCommand(const std::string &cmd) {
	command_timer_.reset();

#if (BOOST_VERSION >= 104700)
	io_service_.notify_fork(boost::asio::io_context::fork_prepare);
#endif

	command_pid_ = fork();
	if (command_pid_ == -1) {
		return false;
	}
	if (command_pid_ == 0) {
		execlp("/bin/sh", "/bin/sh", "-c", cmd.c_str(), NULL);
		exit(1);
	}

#if (BOOST_VERSION >= 104700)
	io_service_.notify_fork(boost::asio::io_context::fork_parent);
#endif

	return true;
}

/*! \brief Start new program.
 *
 * \param cmd vector of string with name and parameters of program to run.
 * \param result string with the data that was written to stdout by program.
 * \param timeout time in ms after which the program will be killed.
 * \return true if there was no error and program did finish in timeout time.
 */
bool uRaftController::runCommand(const std::vector<std::string> &cmd, std::string &result, int timeout) {
	pid_t pid;
	int   pipe_fd[2];

	if (pipe(pipe_fd) == -1) {
		return false;
	}

#if (BOOST_VERSION >= 104700)
	io_service_.notify_fork(boost::asio::io_context::fork_prepare);
#endif

	pid = fork();
	if (pid == -1) {
		close(pipe_fd[0]);
		close(pipe_fd[1]);
		return false;
	}

	if (pid == 0) {
		close(pipe_fd[0]);
		dup2(pipe_fd[1], 1);

		std::vector<const char *> argv(cmd.size() + 1, 0);
		for (int i = 0; i < (int)cmd.size(); i++) {
			argv[i] = cmd[i].c_str();
		}

		execvp(argv[0], (char * const *)&argv[0]);
		exit(1);
	}

#if (BOOST_VERSION >= 104700)
	io_service_.notify_fork(boost::asio::io_context::fork_parent);
#endif

	close(pipe_fd[1]);

	int r = readString(pipe_fd[0], result, timeout);

	close(pipe_fd[0]);

	if (r <= 0) {
		kill(pid, SIGKILL);
	}

	int status;
	waitpid(pid, &status, 0);

	return r > 0;
}

/*! Read string from file descriptor
 *
 * Reads data from file descriptor and store them in string (with timeout).
 * \param fd file descriptor to read
 * \param result string with read data.
 * \param timeout time (ms) after which we stop reading data.
 * \return -1 error
 *         0  timeout did occur
 *         1  no error
 */
int uRaftController::readString(int fd, std::string &result, const int timeout) {
	static const int read_size = 128;

	Timeout tm {std::chrono::milliseconds(timeout)};
	char    buff[read_size + 1];
	pollfd  pdata;

	pdata.fd      = fd;
	pdata.events  = POLLIN;
	pdata.revents = 0;

	result.clear();

	while (1) {
		int     r;

		if (tm.expired()) {
			return 0;
		}

		r = poll(&pdata, 1, tm.remaining_ms());
		if (r <= 0) return r;

		r = read(fd, buff, read_size);
		if (r < 0) {
			return -1;
		}
		if (r == 0) {
			break;
		}

		buff[r] = 0;
		result += buff;
	}

	return 1;
}

void uRaftController::startFloatingIpManager() {
	haFloatingIpManager = std::make_unique<HAFloatingIPManager>(
	    opt_.floating_iface, opt_.floating_ip, opt_.check_floating_ip_period);

	std::function<bool()> restoreFloatingIpFunction = [this]() -> bool {
		std::vector<std::string> params = {"leil-uraft-helper", "assign-ip"};
		std::string result;
		int timeout = 6 * opt_.check_floating_ip_period;
		return runCommand(params, result, timeout);
	};

	haFloatingIpManager->setCallback(restoreFloatingIpFunction);

	haFloatingIpManager->start();
}

void uRaftController::stopFloatingIpManager() {
	if (haFloatingIpManager) {
		haFloatingIpManager->stop();
		haFloatingIpManager.reset();
	} else {
		syslog(LOG_INFO, "Floating IP Manager is not running, nothing to stop");
	}
}

/// @brief Clean up dirty metadata state after failed promotion.
///
/// This prevents subsequent promotion attempts from failing due to leftover files from crashed or
/// timed-out promotion operations.
///
/// \note Uses a 5-second timeout for the cleanup operation.
/// \note Logs errors if cleanup fails but does not throw exceptions.
void uRaftController::cleanupDirtyPromotion() {
	std::vector<std::string> cleanup = {"leil-uraft-helper", "cleanup"};
	std::string result;

	if (!runCommand(cleanup, result, kCommandTimeoutMs)) {
		syslog(LOG_ERR, "Failed to cleanup dirty state: %s", result.c_str());
	}
}

/// @brief Handle failed promotion attempts and restore cluster consistency.
///
/// This ensures that if a node wins a Raft election but cannot complete the promotion to
/// SaunaFS master (e.g., due to corrupted metadata, resource exhaustion, or script failures),
/// the cluster can recover by electing a different node as leader.
///
/// Without this recovery mechanism, the cluster could enter a deadlock where:
/// - The failed node remains Raft leader but cannot serve as master.
/// - Other nodes cannot win elections due to Raft term constraints.
/// - Manual intervention becomes necessary to restore cluster operation.
void uRaftController::handlePromotionFailure() {
	// Promotion failed while kCmdPromote is active (we're inside checkCommandStatus).
	// Do NOT call nodeDemote() here: that would run a demote script while promotion is still
	// active, racing the controller command state machine and causing double scheduling.
	//
	// Instead we:
	// - clean up promotion leftovers,
	// - step down in uRaft (Raft-only) so a different node can become Leader/President,
	// - request a pending demotion that will be executed once kCmdPromote is cleared,
	// - enable a bounded backoff to avoid a tight promote/fail loop.
	cleanupDirtyPromotion();

	// Raft-only: stop being Leader/Candidate so another node can win.
	demoteLeader();

	// Controller-owned: schedule demotion after kCmdPromote is cleared.
	// checkCommandStatus() already has logic to run pending demotion after promotion finishes.
	is_demote_pending_ = true;

	startPromotionBackoff(/*reset=*/false);
}

int uRaftController::computePromotionBackoffMs() const {
	// 1s * 2^(streak-1), capped at 60s
	constexpr int kMaxPromotionBackoffMs = 60000;
	if (promotion_failure_streak_ <= 0) { return 0; }
	const int capped = std::min(promotion_failure_streak_ - 1, 6);  // cap exponent at 6
	const int promotionBackoffTimeMs = 1000 * (1 << capped);
	return std::min(promotionBackoffTimeMs, kMaxPromotionBackoffMs);
}

void uRaftController::startPromotionBackoff(bool reset) {
	if (reset) {
		promotion_failure_streak_ = 0;
		promotion_backoff_active_ = false;
		promotion_backoff_timer_.cancel();
		return;
	}

	promotion_failure_streak_++;
	const int backoff_ms = computePromotionBackoffMs();
	if (backoff_ms <= 0) { return; }

	promotion_backoff_active_ = true;
	set_block_promotion(true);
	syslog(LOG_WARNING, "Promotion backoff enabled (%d ms), streak=%d", backoff_ms,
	       promotion_failure_streak_);

	promotion_backoff_timer_.expires_after(std::chrono::milliseconds(backoff_ms));
	promotion_backoff_timer_.async_wait([this](const boost::system::error_code &ec) {
		if (ec) { return; }
		promotion_backoff_active_ = false;
		syslog(LOG_WARNING, "Promotion backoff expired");
		// We will update block_leader_promotion_ in the next checkNodeStatus() call.
	});
}
