#include "common/platform.h"

#include "common/time_utils.h"
#include "uraft.h"
#include "uraftcontroller.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <poll.h>
#include <sys/stat.h>
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

namespace {

constexpr char kRuntimeStateDirectory[] = "/run/saunafs-uraft";
constexpr char kRuntimeStatePath[] = "/run/saunafs-uraft/quorum.state";
constexpr char kRuntimeStateTmpPath[] = "/run/saunafs-uraft/quorum.state.tmp";
constexpr char kRuntimeStateMagic[] = "URAFTSN1";
constexpr uint32_t kRuntimeStateVersion = 1;

template<typename T>
bool writeField(std::ostream &out, const T &value) {
	out.write(reinterpret_cast<const char *>(&value), sizeof(T));
	return static_cast<bool>(out);
}

template<typename T>
bool readField(std::istream &in, T &value) {
	in.read(reinterpret_cast<char *>(&value), sizeof(T));
	return static_cast<bool>(in);
}

}  // namespace

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

	node_alive_ = !opt_.elector_mode;
	set_block_promotion(state_.type != kLeader);
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

void uRaftController::bootstrapLeaderState() {
	if (opt_.elector_mode) {
		return;
	}

	std::vector<std::string> personality_cmd = {"saunafs-uraft-helper", "metadata-personality"};
	std::string personality;

	if (!runCommand(personality_cmd, personality, kCommandTimeoutMs)) {
		syslog(LOG_WARNING, "Unable to query local metadata personality at startup - booting as follower");
		state_.type = kFollower;
		state_.president = false;
		return;
	}

	if (personality != "master") {
		if (personality.empty()) {
			syslog(LOG_NOTICE, "Local metadata personality is unavailable - booting as follower");
		} else {
			syslog(LOG_NOTICE, "Local metadata personality is '%s' - booting as follower",
			       personality.c_str());
		}
		state_.type = kFollower;
		state_.president = false;
		if (state_.leader_id == state_.id) {
			state_.leader_id = -1;
		}
		return;
	}

	std::vector<std::string> version_cmd = {"saunafs-uraft-helper", "metadata-version"};
	std::string version;

	if (runCommand(version_cmd, version, kCommandTimeoutMs)) {
		try {
			state_.data_version = boost::lexical_cast<uint64_t>(version.c_str());
		} catch (...) {
			syslog(LOG_WARNING, "Invalid metadata version '%s' during leader bootstrap - using 0",
			       version.c_str());
			state_.data_version = 0;
		}
	} else {
		syslog(LOG_WARNING, "Unable to query metadata version during leader bootstrap - using 0");
		state_.data_version = 0;
	}

	state_.type = kLeader;
	state_.president = true;
	state_.leader_id = state_.id;
	state_.voted_for = state_.id;
	node_[state_.id].vote_granted = true;
	node_[state_.id].recv = true;
	node_[state_.id].heartbeat = state_.local_time;
	node_[state_.id].data_version = state_.data_version;
	node_alive_ = true;
	set_block_promotion(false);

	syslog(LOG_NOTICE,
	       "Local sfsmaster is master - bootstrapping uRaft as active Leader (version=%lu)",
	       state_.data_version);

	startFloatingIpManager();
}

void uRaftController::restorePersistentState() {
	std::ifstream state_file(kRuntimeStatePath, std::ios::binary);
	if (!state_file) {
		return;
	}

	char magic[sizeof(kRuntimeStateMagic) - 1];
	uint32_t version = 0;
	uint32_t node_count = 0;
	int32_t node_id = -1;
	uint32_t heartbeat_period = 0;
	int64_t snapshot_ns = 0;
	uint64_t current_term = 0;
	int32_t voted_for = -1;
	int32_t leader_id = -1;
	uint32_t state_type = 0;
	uint64_t local_time = 0;
	uint64_t data_version = 0;
	uint8_t president = 0;
	uint8_t loyalty_agreement = 0;
	int32_t quorum_loss_streak = 0;

	if (!state_file.read(magic, sizeof(magic))) {
		return;
	}
	if (std::memcmp(magic, kRuntimeStateMagic, sizeof(magic)) != 0) {
		syslog(LOG_WARNING, "Ignoring invalid uRaft runtime snapshot (bad magic)");
		return;
	}
	if (!readField(state_file, version) || version != kRuntimeStateVersion) {
		syslog(LOG_WARNING, "Ignoring invalid uRaft runtime snapshot (bad version)");
		return;
	}
	if (!readField(state_file, node_count) ||
	    !readField(state_file, node_id) ||
	    !readField(state_file, heartbeat_period) ||
	    !readField(state_file, snapshot_ns) ||
	    !readField(state_file, current_term) ||
	    !readField(state_file, voted_for) ||
	    !readField(state_file, leader_id) ||
	    !readField(state_file, state_type) ||
	    !readField(state_file, local_time) ||
	    !readField(state_file, data_version) ||
	    !readField(state_file, president) ||
	    !readField(state_file, loyalty_agreement) ||
	    !readField(state_file, quorum_loss_streak)) {
		syslog(LOG_WARNING, "Ignoring truncated uRaft runtime snapshot");
		return;
	}

	if (node_count != node_.size() || node_id != state_.id || heartbeat_period != static_cast<uint32_t>(opt_.heartbeat_period)) {
		syslog(LOG_WARNING,
		       "Ignoring uRaft runtime snapshot due to configuration mismatch (nodes=%u/%zu id=%d/%d heartbeat=%u/%d)",
		       node_count, node_.size(), node_id, state_.id, heartbeat_period, opt_.heartbeat_period);
		return;
	}

	for (size_t i = 0; i < node_.size(); ++i) {
		uint64_t node_data_version = 0;
		uint64_t node_heartbeat = 0;
		uint8_t node_vote_granted = 0;
		uint8_t node_recv = 0;

		if (!readField(state_file, node_data_version) ||
		    !readField(state_file, node_heartbeat) ||
		    !readField(state_file, node_vote_granted) ||
		    !readField(state_file, node_recv)) {
			syslog(LOG_WARNING, "Ignoring truncated uRaft runtime snapshot node block");
			return;
		}

		node_[i].data_version = node_data_version;
		node_[i].heartbeat = node_heartbeat;
		node_[i].vote_granted = node_vote_granted != 0;
		node_[i].recv = node_recv != 0;
	}

	if (state_type < static_cast<uint32_t>(kStateLast)) {
		state_.type = static_cast<int>(state_type);
	}

	state_.current_term = current_term;
	state_.voted_for = voted_for;
	state_.leader_id = leader_id;
	state_.local_time = local_time;
	state_.data_version = data_version;
	state_.president = president != 0;
	state_.loyalty_agreement = loyalty_agreement != 0;
	quorum_loss_streak_ = quorum_loss_streak;

	const auto now = std::chrono::steady_clock::now().time_since_epoch();
	const auto snapshot_time = std::chrono::nanoseconds(snapshot_ns);
	if (snapshot_ns > 0 && now > snapshot_time) {
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - snapshot_time).count();
		if (elapsed > 0 && opt_.heartbeat_period > 0) {
			const uint64_t elapsed_heartbeats = static_cast<uint64_t>(elapsed / opt_.heartbeat_period);
			if (elapsed_heartbeats > 0) {
				const uint64_t max_value = std::numeric_limits<uint64_t>::max();
				if (max_value - state_.local_time < elapsed_heartbeats) {
					state_.local_time = max_value;
				} else {
					state_.local_time += elapsed_heartbeats;
				}
			}
		}
	}

	syslog(LOG_NOTICE, "Restored uRaft runtime snapshot (term=%lu, leader=%d, voted_for=%d, local_time=%lu)",
	       state_.current_term, state_.leader_id, state_.voted_for, state_.local_time);
}

void uRaftController::persistRuntimeState() {
	if (mkdir(kRuntimeStateDirectory, 0755) < 0 && errno != EEXIST) {
		syslog(LOG_ERR, "Unable to create runtime state directory %s: %s",
		       kRuntimeStateDirectory, strerror(errno));
		return;
	}

	std::ofstream state_file(kRuntimeStateTmpPath, std::ios::binary | std::ios::trunc);
	if (!state_file) {
		syslog(LOG_ERR, "Unable to open runtime state snapshot %s for writing", kRuntimeStateTmpPath);
		return;
	}

	const uint32_t version = kRuntimeStateVersion;
	const uint32_t node_count = static_cast<uint32_t>(node_.size());
	const int32_t node_id = static_cast<int32_t>(state_.id);
	const uint32_t heartbeat_period = static_cast<uint32_t>(opt_.heartbeat_period);
	const int64_t snapshot_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
	    std::chrono::steady_clock::now().time_since_epoch()).count();
	const uint32_t state_type = static_cast<uint32_t>(state_.type);
	const uint8_t president = state_.president ? 1U : 0U;
	const uint8_t loyalty_agreement = state_.loyalty_agreement ? 1U : 0U;

	if (!state_file.write(kRuntimeStateMagic, sizeof(kRuntimeStateMagic) - 1) ||
	    !writeField(state_file, version) ||
	    !writeField(state_file, node_count) ||
	    !writeField(state_file, node_id) ||
	    !writeField(state_file, heartbeat_period) ||
	    !writeField(state_file, snapshot_ns) ||
	    !writeField(state_file, state_.current_term) ||
	    !writeField(state_file, state_.voted_for) ||
	    !writeField(state_file, state_.leader_id) ||
	    !writeField(state_file, state_type) ||
	    !writeField(state_file, state_.local_time) ||
	    !writeField(state_file, state_.data_version) ||
	    !writeField(state_file, president) ||
	    !writeField(state_file, loyalty_agreement) ||
	    !writeField(state_file, quorum_loss_streak_)) {
		syslog(LOG_ERR, "Failed to write uRaft runtime snapshot header");
		state_file.close();
		::unlink(kRuntimeStateTmpPath);
		return;
	}

	for (const auto &node : node_) {
		const uint8_t vote_granted = node.vote_granted ? 1U : 0U;
		const uint8_t recv = node.recv ? 1U : 0U;
		if (!writeField(state_file, node.data_version) ||
		    !writeField(state_file, node.heartbeat) ||
		    !writeField(state_file, vote_granted) ||
		    !writeField(state_file, recv)) {
			syslog(LOG_ERR, "Failed to write uRaft runtime snapshot node state");
			state_file.close();
			::unlink(kRuntimeStateTmpPath);
			return;
		}
	}

	state_file.flush();
	if (!state_file) {
		syslog(LOG_ERR, "Failed to flush uRaft runtime snapshot");
		state_file.close();
		::unlink(kRuntimeStateTmpPath);
		return;
	}
	state_file.close();

	if (std::rename(kRuntimeStateTmpPath, kRuntimeStatePath) != 0) {
		syslog(LOG_ERR, "Failed to publish uRaft runtime snapshot: %s", strerror(errno));
		::unlink(kRuntimeStateTmpPath);
	}
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
	} else {
		syslog(LOG_ERR, "Unable to launch demotion helper");
		syslog(LOG_WARNING, "Scheduling demotion retry");
		scheduleDeadRecovery(false);
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
				syslog(LOG_ERR, "Demotion failed with exit code: %d",
				       WIFEXITED(status) ? WEXITSTATUS(status) : -1);
				syslog(LOG_WARNING, "Scheduling demotion retry");
				scheduleDeadRecovery(false);
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
				if (dead_recovery_pending_ && dead_recovery_requires_dead_) {
					cancelDeadRecovery();
				}
			} else {
				syslog(LOG_NOTICE, "Metadata server is dead");
				// Controller-owned dead handling:
				// 1) Raft-only step-down (no implicit nodeDemote()) so another node can take over.
				// 2) Run the helper's 'dead' command to release floating IPs without restarting sfsmaster.
				stepDownToFollower(StepDownPolicy::kRaftOnly);
				startDeadMetadataHandler();
				// Keep retrying the follow-up demotion even if sfsmaster starts answering again.
				// Raft already stepped down, so the local metadata role still needs to catch up.
				scheduleDeadRecovery(false);
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
	dead_recovery_requires_dead_ = true;
	dead_recovery_timer_.cancel();
}

void uRaftController::scheduleDeadRecovery(bool requires_dead) {
	// Idempotent: don't stack multiple recovery timers
	if (dead_recovery_pending_) {
		dead_recovery_requires_dead_ = dead_recovery_requires_dead_ && requires_dead;
		return;
	}
	dead_recovery_pending_ = true;
	dead_recovery_requires_dead_ = requires_dead;

	dead_recovery_timer_.expires_after(std::chrono::milliseconds(kDeadRecoveryDelayMs));
	dead_recovery_timer_.async_wait([this](const boost::system::error_code &ec) {
		if (ec) { return; }

		const bool requires_dead = dead_recovery_requires_dead_;

		// If metadata recovered and we only care about dead metadata, stop recovery.
		if (requires_dead && node_alive_) {
			cancelDeadRecovery();
			return;
		}

		// If another command is running, retry after the same delay
		if (command_pid_ >= 0 || command_type_ != kCmdNone) {
			cancelDeadRecovery();
			scheduleDeadRecovery(requires_dead);
			return;
		}

		// Still dead and safe to act: restart shadow via demote path
		syslog(LOG_WARNING, "Metadata still dead: restarting shadow via demote");
		dead_recovery_pending_ = false;
		dead_recovery_requires_dead_ = true;
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
