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


HAFloatingIPManager::HAFloatingIPManager(const std::string &iface,
                                         const std::string &ipAddress,
                                         const int &checkPeriod)
    : floatingIPInterface(iface),
      floatingIPAddress(ipAddress),
      checkFloatingIPPeriodMS(checkPeriod) { }

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

bool HAFloatingIPManager::isFloatingIPAlive() const {
	return _isFloatingIPAlive;
}

bool HAFloatingIPManager::restoreFloatingIP() {
	std::string command = "saunafs-uraft-helper assign-ip";
	int result = system(command.c_str());

	if (result != 0) {
		syslog(LOG_WARNING, "Command %s was not successful", command.c_str());
	}

	return (result == 0);
}

void HAFloatingIPManager::startEventListener() {
	stopListenerFlag = false;
	if (!listenerThread.joinable()) {
		listenerThread =
		    std::thread(&HAFloatingIPManager::eventListenerThread, this);
		_isFloatingIPAlive = true;
	}
}

void HAFloatingIPManager::stopEventListener() {
	stopListenerFlag = true;
	if (listenerThread.joinable()) {
		listenerThread.join();
		_isFloatingIPAlive = false;
	}
}

void HAFloatingIPManager::handleIPLoss(const std::string &ipAddress) {
	syslog(LOG_WARNING, "[FloatingIPManager] Handling lost IP: %s",
	       ipAddress.c_str());
	_isFloatingIPAlive = false;
	// Trigger failover
	if (!stopListenerFlag) { _isFloatingIPAlive = restoreFloatingIP(); }
}

void HAFloatingIPManager::eventListenerThread() {
	constexpr int kBufferSize = 8192; // 8KB
	char buffer[kBufferSize];
	constexpr int kRetryTimeout = 2; // timeout before retrying
	constexpr long kMillisecondsInOneMicrosecond = 1000;

	while (!stopListenerFlag) {
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

		while (!stopListenerFlag) {
			fd_set read_fds;
			FD_ZERO(&read_fds);
			FD_SET(sock_fd, &read_fds);

			// checkFloatingIPPeriodMS defines the timeout in milliseconds
			struct timeval timeout = {
			    0, checkFloatingIPPeriodMS * kMillisecondsInOneMicrosecond};

			int result =
			    select(sock_fd + 1, &read_fds, nullptr, nullptr, &timeout);

			if (result < 0) {
				syslog(LOG_WARNING,
				       "[FloatingIPManager] select() function failed.");
				break;
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
				syslog(LOG_WARNING,
				       "[FloatingIPManager] recvmsg() failed. "
				       "Restarting socket...");
				break;  // Restart socket
			}

			for (nlh = (struct nlmsghdr *)buffer; NLMSG_OK(nlh, recv_len);
			     nlh = NLMSG_NEXT(nlh, recv_len)) {
				if (nlh->nlmsg_type == NLMSG_DONE) { break; }
				if (nlh->nlmsg_type == NLMSG_ERROR) {
					syslog(LOG_WARNING,
					       "[FloatingIPManager] Received an error message.");
					continue;
				}

				if (nlh->nlmsg_type == RTM_DELADDR) {
					auto *ifa = (struct ifaddrmsg *)NLMSG_DATA(nlh);

					// Convert interface index to name
					char networkInterface[IF_NAMESIZE] = {0};

					if (if_indextoname(ifa->ifa_index, networkInterface)) {
						syslog(
						    LOG_WARNING,
						    "[FloatingIPManager] IP removed from interface: %s",
						    networkInterface);
					} else {
						syslog(LOG_WARNING,
						       "[FloatingIPManager] Failed to get "
						       "interface name for index: %d",
						       ifa->ifa_index);
						continue;  // It happens when the IP is already removed
					}

					// Parse attributes to extract the removed IP address
					auto *rta = (struct rtattr *)((char *)ifa +
					                              NLMSG_ALIGN(sizeof(*ifa)));
					int attr_len = nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*ifa));

					while (RTA_OK(rta, attr_len)) {
						if (rta->rta_type == IFA_LOCAL) { // Local IP address
							char ipAddress[INET_ADDRSTRLEN] = {0};
							inet_ntop(AF_INET, RTA_DATA(rta), ipAddress,
							          sizeof(ipAddress));

							syslog(LOG_WARNING,
							       "[FloatingIPManager] Removed "
							       "IP: %s from interface: %s",
							       ipAddress, networkInterface);

							if (floatingIPAddress == ipAddress &&
							    floatingIPInterface == networkInterface) {
								handleIPLoss(ipAddress);
							}
						}
						rta = RTA_NEXT(rta, attr_len);
					}
				}
			}
		}

		close(sock_fd);

		if (!stopListenerFlag) {
			// Wait before retrying is the thread is still active
			syslog(LOG_WARNING,
			       "[FloatingIPManager] Socket closed. Restarting in %ds...",
			       kRetryTimeout);
			std::this_thread::sleep_for(std::chrono::seconds(kRetryTimeout));
		}
	}
}
