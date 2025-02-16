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

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/syslog.h>
#include <stop_token>


HAFloatingIPManager::HAFloatingIPManager(const std::string &iface,
                                         const std::string &ipAddress,
                                         const int &checkPeriod)
    : floatingIpInterface(iface),
      floatingIpAddress(ipAddress),
      checkFloatingIpPeriodMS(checkPeriod) { }

HAFloatingIPManager::~HAFloatingIPManager() {
	stopEventListener();
}

void HAFloatingIPManager::initialize() { }

void HAFloatingIPManager::start() {
	// Start the event listener
	startEventListener();
}

void HAFloatingIPManager::stop() {
	// Stop the event listener
	stopEventListener();
}

bool HAFloatingIPManager::isFloatingIpAlive() const {
	return _isFloatingIpAlive;
}

bool HAFloatingIPManager::restoreFloatingIp() {
	std::string command = "saunafs-uraft-helper assign-ip";
	int result = system(command.c_str());

	if (result != 0) {
		syslog(LOG_WARNING, "Command %s was not successful", command.c_str());
	}

	return (result == 0);
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
		int sock_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
		if (sock_fd < 0) {
			syslog(LOG_WARNING,
			       "[FloatingIPManager] Failed to create socket. "
			       "Retrying in %ds...",
			       kRetryTimeout);
			std::this_thread::sleep_for(std::chrono::seconds(kRetryTimeout));
			continue;  // Retry loop
		}

		struct sockaddr_nl local_addr = {};
		local_addr.nl_family = AF_NETLINK;
		local_addr.nl_groups = RTMGRP_IPV4_IFADDR | RTMGRP_LINK;

		if (bind(sock_fd, reinterpret_cast<struct sockaddr *>(&local_addr),
		         sizeof(local_addr)) < 0) {
			syslog(
			    LOG_WARNING,
			    "[FloatingIPManager] Failed to bind socket. Retrying in %ds...",
			    kRetryTimeout);
			close(sock_fd);
			std::this_thread::sleep_for(std::chrono::seconds(kRetryTimeout));
			continue;  // Retry loop
		}

		// Poll for IP removal events
		pollSocketForIpRemovalEvents(sock_fd, stopToken);

		close(sock_fd);

		if (!stopToken.stop_requested()) {
			// Wait before retrying is the thread is still active
			syslog(LOG_WARNING,
			       "[FloatingIPManager] Socket closed. Restarting in %ds...",
			       kRetryTimeout);
			std::this_thread::sleep_for(std::chrono::seconds(kRetryTimeout));
		}
	}
}

void HAFloatingIPManager::pollSocketForIpRemovalEvents(
    int sock_fd, const std::stop_token &stopToken) {
	constexpr int kBufferSize = 8192;  // 8KB
	constexpr long kMillisecondsInOneMicrosecond = 1000;
	char buffer[kBufferSize];
	fd_set read_fds;

	while (!stopToken.stop_requested()) {
		FD_ZERO(&read_fds);
		FD_SET(sock_fd, &read_fds);

		// checkFloatingIPPeriodMS defines the timeout in milliseconds
		struct timeval timeout = {
		    0, checkFloatingIpPeriodMS * kMillisecondsInOneMicrosecond};

		int result = select(sock_fd + 1, &read_fds, nullptr, nullptr, &timeout);

		if (result < 0) {
			syslog(LOG_WARNING,
			       "[FloatingIPManager] select() function failed.");
			break;  // Exit on error
		}
		if (result == 0) {
			continue;  // Timeout, check again
		}

		struct iovec iov = {buffer, sizeof(buffer)};
		struct msghdr msg = {};
		struct sockaddr_nl nladdr = {};
		struct nlmsghdr *nlh;

		msg.msg_name = &nladdr;
		msg.msg_namelen = sizeof(nladdr);
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;

		ssize_t recv_len = recvmsg(sock_fd, &msg, 0);
		if (recv_len < 0) {
			syslog(
			    LOG_WARNING,
			    "[FloatingIPManager] recvmsg() failed. Restarting socket...");
			break;  // Exit on error
		}

		for (nlh = (struct nlmsghdr *)buffer; NLMSG_OK(nlh, recv_len);
		     nlh = NLMSG_NEXT(nlh, recv_len)) {
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
					       "for index: %d",
					       ifaceAddress->ifa_index);
					continue;
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
						inet_ntop(AF_INET, RTA_DATA(rta), ipAddress,
						          sizeof(ipAddress));

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
