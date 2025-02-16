/*
   Copyright 2025 Leil Storage OÜ

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

#include "floating-ip-manager.h"

#include "common/time_utils.h"

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/syslog.h>
#include <sys/wait.h>
#include <stop_token>

HAFloatingIPManager::HAFloatingIPManager(const std::string &iface,
                                         const std::string &ipAddress,
                                         const uint &checkPeriod)
    : floatingIpInterface(iface),
      floatingIpAddress(ipAddress),
      checkFloatingIpPeriodMS(checkPeriod) {
	if (!isFloatingIpManagerEnabled()) {
		syslog(LOG_INFO, "Floating IP manager is disabled.");
	}
}

HAFloatingIPManager::~HAFloatingIPManager() {
	stopEventListener();
}

void HAFloatingIPManager::initialize() { }

void HAFloatingIPManager::start() {
	if (isFloatingIpManagerEnabled()) {
		startEventListener();
		syslog(LOG_INFO, "Floating IP manager started.");
	}
}

void HAFloatingIPManager::stop() {
	if (isFloatingIpManagerEnabled()) {
		stopEventListener();
		syslog(LOG_INFO, "Floating IP manager stopped.");
	}
}

bool HAFloatingIPManager::isFloatingIpAlive() const {
	return _isFloatingIpAlive;
}

bool HAFloatingIPManager::restoreFloatingIp() {
	const char *command = "saunafs-uraft-helper";
	const char *args[] = {command, "assign-ip", nullptr};

	constexpr int kTimeoutInMilliseconds = 3000; // 3 seconds
	constexpr int kSleepTimeInMilliseconds = 100; // 0.1 seconds

	pid_t pid = fork();
	if (pid < 0) {
		syslog(LOG_ERR, "Failed to fork process: %s", strerror(errno));
		return false;
	}

	if (pid == 0) {  // Child process
		execvp(command, const_cast<char *const *>(args));
		syslog(LOG_ERR, "Failed to execute command: %s", strerror(errno));
		_exit(127);  // Exit with error if execvp fails
	}

	// Parent process: Wait for the child process with a timeout
	int status = 0;
	pid_t result = 0;
	Timeout timer {std::chrono::milliseconds(kTimeoutInMilliseconds)};

	do {
		result = waitpid(pid, &status, WNOHANG);
		if (result != 0) { break; }  // Child exited or error occurred

		std::this_thread::sleep_for(
		    std::chrono::milliseconds(kSleepTimeInMilliseconds));
	} while (!timer.expired());

	// If the child is still running after timeout, terminate it
	if (result == 0) {  // Timeout occurred
		syslog(LOG_ERR, "Command %s timed out after %d milliseconds", command,
		       kTimeoutInMilliseconds);

		kill(pid, SIGTERM);  // Try graceful termination first
		std::this_thread::sleep_for(std::chrono::milliseconds(
		    kSleepTimeInMilliseconds));  // Give it time to exit

		if (waitpid(pid, &status, WNOHANG) == 0) {  // Still running?
			syslog(LOG_ERR, "Process did not exit, forcing termination.");
			kill(pid, SIGKILL);
		}

		waitpid(pid, &status, 0);  // Ensure the process is cleaned up
		return false;
	}

	if (result < 0) {
		syslog(LOG_ERR, "waitpid failed: %s", strerror(errno));
		return false;
	}

	// Check exit status
	if (WIFEXITED(status)) {
		if (WEXITSTATUS(status) == 0) { return true; }
		syslog(LOG_ERR, "Command failed with exit code %d",
		       WEXITSTATUS(status));
	}

	if (WIFSIGNALED(status)) {
		syslog(LOG_ERR, "Command terminated by signal %d", WTERMSIG(status));
	}
	return false;
}

void HAFloatingIPManager::startEventListener() {
	if (!listenerThread.joinable()) {
		listenerThread =
		    std::jthread(&HAFloatingIPManager::eventListenerThread, this);
		_isFloatingIpAlive = true;
	}
}

void HAFloatingIPManager::stopEventListener() {
	if (listenerThread.joinable()) {
		listenerThread.request_stop();
		_isFloatingIpAlive = false;
	}
}

void HAFloatingIPManager::handleIpLoss(const std::string &ipAddress) {
	syslog(LOG_WARNING, "[FloatingIPManager] Handling lost IP: %s",
	       ipAddress.c_str());
	_isFloatingIpAlive = false;

	// Trigger failover
	const std::stop_token stopToken = listenerThread.get_stop_token();
	if (!stopToken.stop_requested()) {
		_isFloatingIpAlive = restoreFloatingIp();
	}
}

void HAFloatingIPManager::eventListenerThread(const std::stop_token &stopToken) {
	constexpr int kRetryTimeout = 2; // timeout before retrying

	while (!stopToken.stop_requested()) {
		int socket_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
		if (socket_fd < 0) {
			syslog(LOG_WARNING,
			       "[FloatingIPManager] Failed to create socket: %s. "
			       "Retrying in %ds...",
			       strerror(errno), kRetryTimeout);
			std::this_thread::sleep_for(std::chrono::seconds(kRetryTimeout));
			continue;  // Retry loop
		}

		struct sockaddr_nl local_addr = {};
		local_addr.nl_family = AF_NETLINK;
		local_addr.nl_groups = RTMGRP_IPV4_IFADDR | RTMGRP_LINK;

		if (bind(socket_fd, reinterpret_cast<struct sockaddr *>(&local_addr),
		         sizeof(local_addr)) < 0) {
			syslog(LOG_WARNING,
			       "[FloatingIPManager] Failed to bind socket: %s. "
			       "Retrying in %ds...",
			       strerror(errno), kRetryTimeout);

			if (close(socket_fd) < 0) {
				syslog(LOG_WARNING,
				       "[FloatingIPManager] Failed to close socket: %s",
				       strerror(errno));
			}

			std::this_thread::sleep_for(std::chrono::seconds(kRetryTimeout));
			continue;  // Retry loop
		}

		// Poll for IP removal events
		pollSocketForIpRemovalEvents(socket_fd, stopToken);

		if (close(socket_fd) < 0) {
			syslog(LOG_WARNING,
			       "[FloatingIPManager] Failed to close socket: %s",
			       strerror(errno));
		}

		if (!stopToken.stop_requested()) {
			// Wait before retrying is the thread is still active
			syslog(
			    LOG_WARNING,
			    "[FloatingIPManager] Waiting %d seconds to restart the socket",
			    kRetryTimeout);
			std::this_thread::sleep_for(std::chrono::seconds(kRetryTimeout));
		}
	}
}

void HAFloatingIPManager::pollSocketForIpRemovalEvents(
    int socket_fd, const std::stop_token &stopToken) {
	constexpr int kBufferSize = 8192;  // 8KB
	constexpr long kMillisecondsInOneMicrosecond = 1000;
	char buffer[kBufferSize];
	fd_set read_fds;

	while (!stopToken.stop_requested()) {
		FD_ZERO(&read_fds);
		FD_SET(socket_fd, &read_fds);

		// checkFloatingIpPeriodMS defines the timeout in milliseconds
		struct timeval timeout = {
		    0, checkFloatingIpPeriodMS * kMillisecondsInOneMicrosecond};

		int result = select(socket_fd + 1, &read_fds, nullptr, nullptr, &timeout);

		if (result < 0) {
			syslog(LOG_WARNING,
			       "[FloatingIPManager] select() function failed: %s.",
			       strerror(errno));
			break;  // Exit on error
		}
		if (result == 0) {
			continue;  // Timeout, check again
		}

		struct iovec iov = {buffer, sizeof(buffer)};
		struct msghdr message = {};
		struct sockaddr_nl nladdr = {};
		struct nlmsghdr *nlh;

		message.msg_name = &nladdr;
		message.msg_namelen = sizeof(nladdr);
		message.msg_iov = &iov;
		message.msg_iovlen = 1;

		ssize_t bytesReceived = recvmsg(socket_fd, &message, 0);
		if (bytesReceived < 0) {
			syslog(LOG_WARNING,
			       "[FloatingIPManager] recvmsg() failed: %s. Restarting "
			       "socket...",
			       strerror(errno));
			break;  // Exit on error
		}

		for (nlh = (struct nlmsghdr *)buffer; NLMSG_OK(nlh, bytesReceived);
		     nlh = NLMSG_NEXT(nlh, bytesReceived)) {
			if (nlh->nlmsg_type == NLMSG_DONE) { break; }

			if (nlh->nlmsg_type == NLMSG_ERROR) {
				auto *errMsg =
				    reinterpret_cast<struct nlmsgerr *>(NLMSG_DATA(nlh));

				if (errMsg->error != 0) { // Error occurred
					int err = -errMsg->error;  // Convert to positive errno
					syslog(LOG_WARNING,
					       "[FloatingIPManager] Netlink error: %s (errno: %d)",
					       strerror(err),  // Convert errno to readable string
					       err);           // Log the positive errno value
				}
				continue;
			}

			if (nlh->nlmsg_type == RTM_DELADDR) {  // IP Removal Event
				auto *ifaceAddress = (struct ifaddrmsg *)NLMSG_DATA(nlh);

				// Convert interface index to name
				char networkInterface[IF_NAMESIZE] = {0};
				if (if_indextoname(ifaceAddress->ifa_index, networkInterface)) {
					syslog(LOG_WARNING,
					       "[FloatingIPManager] IP removed from interface: %s",
					       networkInterface);
				} else {
					syslog(LOG_WARNING,
					       "[FloatingIPManager] Failed to get interface name "
					       "for index %d: %s",
					       ifaceAddress->ifa_index, strerror(errno));
					continue;  // Skip processing this IP address
				}

				// Parse attributes to extract the removed IP address
				auto *rta =
				    (struct rtattr *)((char *)ifaceAddress +
				                      NLMSG_ALIGN(sizeof(*ifaceAddress)));
				int attributeLength =
				    nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*ifaceAddress));

				while (RTA_OK(rta, attributeLength)) {
					if (rta->rta_type == IFA_LOCAL) {
						char ipAddress[INET_ADDRSTRLEN] = {0};

						if (inet_ntop(AF_INET, RTA_DATA(rta), ipAddress,
						              sizeof(ipAddress)) == nullptr) {
							syslog(LOG_WARNING,
							       "[FloatingIPManager] Failed to convert IP "
							       "address: %s",
							       strerror(errno));
							continue;  // Skip processing this IP address
						}

						syslog(LOG_WARNING,
						       "[FloatingIPManager] Removed IP: %s from "
						       "interface: %s",
						       ipAddress, networkInterface);

						if (floatingIpAddress == ipAddress &&
						    floatingIpInterface == networkInterface) {
							handleIpLoss(ipAddress);
						}
					}
					rta = RTA_NEXT(rta, attributeLength);
				}
			}
		}
	}
}

bool HAFloatingIPManager::isFloatingIpManagerEnabled() const {
	return checkFloatingIpPeriodMS > 0;
}
