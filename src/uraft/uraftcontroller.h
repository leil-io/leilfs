#pragma once

#include "common/platform.h"

#include "floating-ip-manager.h"

#include "common/time_utils.h"
#include "uraftstatus.h"

#include <unistd.h>

/*! \brief Management of SaunaFS metadata server based on uRaft algorithm.
 *
 * This class manages local SaunaFS master/shadow server. This is done using
 * base class uRaft for selection of leader. We get informed on leader change by call
 * to one of virtual node* methods. Then we call helper script to switch metadata server
 * to proper mode (master/shadow).
 *
 * class gets information from uRaft class about required state change.
 */
class uRaftController : public uRaftStatus {
public:
	enum CommandType { kCmdNone,kCmdPromote,kCmdDemote,kCmdStatusDead };

	struct Options : uRaftStatus::Options {
		std::string local_master_server;      //!< Local SaunaFS master server address. //
		int         local_master_port;        //!< Local SaunaFS master server matocl port. //
		int         elector_mode;             //!< Enable elector mode. //
		int         check_node_status_period; //!< How often we check master server status. //
		int         check_cmd_status_period;  //!< How often we check script status. //
		int         getversion_timeout;       //!< Time after which we kill get version script. //
		int         promote_timeout;          //!< Time after which we kill promote script. //
		int         demote_timeout;           //!< Time after which we kill demote script. //
		int         dead_handler_timeout;     //!< Time after which we kill dead script. //
		std::string floating_ip;              //!< Floating IP assigned to the master server. //
		std::string floating_iface;           //!< Network interface to manage floating IP. //
		unsigned int check_floating_ip_period; //!< How often we check floating ip status. //
	};

public:
	uRaftController(boost::asio::io_context &);
	virtual ~uRaftController();

	//! Initialize data.
	void init();

	//! Set options.
	void set_options(const Options &opt);

	//! called by uRaft when node is becoming leader.
	void nodePromote() override;

	//! called by uRaft when node is not longer a leader.
	void nodeDemote() override;

	//! called by uRaft when it needs to know metadata version.
	uint64_t nodeGetVersion() override;

	//! Returns true when this node runs in elector mode.
	bool isElectorNode() const override;

	//! called by uRaft with new leader id.
	void nodeLeader(int id) override;

protected:
	void  checkCommandStatus(const boost::system::error_code &error);
	void  checkNodeStatus(const boost::system::error_code &error);

	bool  runSlowCommand(const std::string &cmd);
	bool  checkSlowCommand(int &status);
	bool  stopSlowCommand();
	void  setSlowCommandTimeout(int timeout);

	bool  runCommand(const std::vector<std::string> &cmd, std::string &result, int timeout);
	int   readString(int fd, std::string &result, int timeout);

private:
	void startFloatingIpManager();
	void stopFloatingIpManager();

	/// @brief Run the controller-owned "dead metadata" handler.
	///
	/// This invokes the helper's 'dead' command to release floating IPs without restarting
	/// the metadata process. It is used when local health checks report that metadata is dead,
	/// so the VIP is immediately released while follow-up recovery is scheduled separately.
	void startDeadMetadataHandler();

	/// @brief Schedule a delayed dead recovery attempt (restart shadow).
	///
	/// The recovery is implemented as a single-delay retry loop: after the timer fires, it
	/// either performs the recovery action if safe (no other command running), or reschedules
	/// itself with the same delay. In case the metadata is alive again and the demotion is not
	/// performed, the timer is canceled.
	void scheduleDeadRecovery();

	/// @brief Cancel pending dead recovery attempts.
	///
	/// Clears the pending flag and cancels the recovery timer so no further demote/restart
	/// actions are attempted after metadata is observed alive again.
	void cancelDeadRecovery();

	/// @brief Clean up dirty metadata state after failed promotion.
	///
	/// This function is called when a promotion fails or times out, leaving the metadata server in
	/// an inconsistent state. It invokes the leil-uraft-helper 'cleanup' command to remove
	/// temporary metadata file (metadata.sfs.tmp) if no process is holding it.
	///
	/// This is a recovery operation to prevent deadlocks caused by incomplete metadata dumps from
	/// incomplete promotion attempts.
	///
	/// \see handlePromotionFailure()
	void cleanupDirtyPromotion();

	/// @brief Handle failed promotion attempts and restore cluster consistency.
	///
	/// This prevents the cluster from getting stuck when a node wins an election but fails
	/// to complete the promotion to master. The controller will step down in uRaft so another
	/// node can take over, and will schedule a safe demotion of the local metadata server after
	/// the promotion command completes.
	///
	/// This function is invoked in two scenarios:
	/// 1. When a promotion command exits with a non-zero status (checkCommandStatus).
	/// 2. When a promotion times out (setSlowCommandTimeout).
	///
	/// It performs the following recovery steps:
	/// - Cleans up any dirty metadata state left by the failed promotion.
	/// - Demotes the node from Leader/Candidate state in the uRaft algorithm (Raft-only step-down).
	/// - Schedules a demotion of the local metadata server after the promotion command finishes,
	///   to avoid promote/demote command-state races.
	/// - Enables a temporary promotion backoff (blocks new promotions for a bounded time) to avoid
	///   tight promote/fail loops under unhealthy local state.
	///
	/// \see cleanupDirtyPromotion()
	/// \see demoteLeader()
	/// \see startPromotionBackoff()
	/// \see computePromotionBackoffMs()
	void handlePromotionFailure();

	/// @brief Start or reset the promotion backoff state.
	///
	/// When enabled, backoff blocks uRaft leader promotion by calling set_block_promotion(true)
	/// and keeps it blocked until the backoff timer expires (or until reset is requested).
	///
	/// \param reset If true, clears the failure streak, disables backoff, and cancels the timer.
	void startPromotionBackoff(bool reset);

	/// @brief Compute the current promotion backoff duration in milliseconds.
	///
	/// The duration grows exponentially with the promotion failure streak and is capped to a
	/// bounded maximum to avoid long recovery delays.
	///
	/// \return Backoff duration in milliseconds (0 means no backoff should be applied).
	int computePromotionBackoffMs() const;

protected:
	boost::asio::steady_timer check_cmd_status_timer_;
	boost::asio::steady_timer check_node_status_timer_;
	boost::asio::steady_timer cmd_timeout_timer_;

	/// @brief Timer used to implement promotion backoff after failed promotions.
	boost::asio::steady_timer promotion_backoff_timer_;
	/// @brief Number of consecutive promotion failures used to compute exponential backoff.
	/// This value is reset on successful promotion.
	int promotion_failure_streak_ = 0;
	/// @brief True while promotion backoff is active (promotions are temporarily blocked).
	bool promotion_backoff_active_ = false;

	/// @brief Timer used to schedule delayed recovery after detecting dead metadata.
	boost::asio::steady_timer dead_recovery_timer_;
	/// @brief True while a dead recovery timer is scheduled (prevents stacking retries).
	bool dead_recovery_pending_ = false;

	pid_t                       command_pid_;   /// Last run command pid.
	int                         command_type_;  /// Last run command type.
	Timer                       command_timer_;
	/// @brief Flag indicating a demotion request is pending.
	/// Set to true when nodeDemote() is called while a promotion is in progress.
	/// After the promotion completes, the pending demotion will be executed.
	/// This prevents concurrent promotion/demotion race conditions.
	bool                        is_demote_pending_;
	/// @brief Flag indicating a promotion request is pending.
	/// Set to true when nodePromote() is called while a demotion is in progress.
	/// After the demotion completes, the pending promotion will be executed.
	/// This prevents concurrent promotion/demotion race conditions.
	bool                        is_promote_pending_;
	bool                        node_alive_;  /// Last is_alive node status.
	Options                     opt_;
	///< Smart pointer to the floating IP manager.
	std::unique_ptr<IHAFloatingIPManager> haFloatingIpManager = nullptr;
};
