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

#pragma once

#ifndef FLOATING_IP_MANAGER_H
#define FLOATING_IP_MANAGER_H

#include <arpa/inet.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

/**
 * @brief Interface for High Availability Floating IP Manager.
 *
 * This interface defines the methods that must be implemented by any class
 * that manages a floating IP in a high availability setup.
 */
class IHAFloatingIPManager {
public:
	IHAFloatingIPManager() = default;
	virtual ~IHAFloatingIPManager() = default;

	/// @brief Initialize the floating IP manager.
	virtual void initialize() = 0;

	/// @brief Start the floating IP manager, including any necessary
	/// event listeners, polling service or background tasks.
	virtual void start() = 0;

	/// @brief Stop the floating IP manager, including any necessary
	/// event listeners, polling service or background tasks.
	virtual void stop() = 0;

	/// @brief Check if the floating IP is alive.
	///
	/// @return True if the floating IP is alive, false otherwise.
	virtual bool isFloatingIPAlive() const = 0;

	/// @brief Restore the floating IP if it has been lost.
	///
	/// @return True if the floating IP was successfully restored, false
	/// otherwise.
	virtual bool restoreFloatingIP() = 0;
};

/**
 * @brief High Availability Floating IP Manager.
 *
 * This class implements the IHAFloatingIPManager interface and provides
 * functionality to manage a floating IP using a socket-based event listener to
 * detect loss of the floating IP.
 */
class HAFloatingIPManager : public IHAFloatingIPManager {
public:
	/// @brief Constructor for HAFloatingIPManager.
	///
	/// @param iface The network interface to which the floating IP will be
	/// assigned.
	/// @param ipAddress The floating IP address to be managed.
	/// @param checkPeriod The period (in milliseconds) to check the floating IP
	/// status.
	HAFloatingIPManager(const std::string &iface, const std::string &ipAddress,
	                    const int &checkPeriod);

	/// @brief Destructor for HAFloatingIPManager.
	virtual ~HAFloatingIPManager();

	void initialize() override;
	void start() override;
	void stop() override;
	bool isFloatingIPAlive() const override;
	bool restoreFloatingIP() override;

private:
	/// @brief Start the event listener thread.
	///
	/// Starts a background thread that uses a socket to listens for events
	/// indicating the loss of the floating IP.
	void startEventListener();

	/// @brief Stop the event listener thread.
	///
	/// Stops the background thread that listens for events indicating the loss
	/// of the floating IP.
	void stopEventListener();

	/// @brief Handle the loss of the floating IP.
	///
	/// This method is called when the floating IP is lost and triggers the
	/// failover process.
	///
	/// @param ipAddress The IP address that was lost.
	void handleIPLoss(const std::string &ipAddress);

	/// @brief Event listener thread function.
	///
	/// This function runs a dedicated listener thread that monitors changes in
	/// network interface addresses and link status using the Netlink protocol.
	/// It uses a Netlink socket to listen for RTM_DELADDR messages, which
	/// indicate the removal of an IP address from an interface. If the removed
	/// IP matches the configured floating IP, it triggers the handleIPLoss()
	/// function to take appropriate recovery actions.
	void eventListenerThread();

protected:
	std::thread listenerThread; ///< Background thread for event listening.
	std::atomic<bool> stopListenerFlag{false}; ///< Flag to stop the event listener thread.
	bool _isFloatingIPAlive{false}; ///< Flag indicating if the floating IP is alive.

	std::string floatingIPInterface; ///< Network interface for the floating IP.
	std::string floatingIPAddress;   ///< Floating IP address.
	int checkFloatingIPPeriodMS = 500; ///< Period (in milliseconds) to check the floating IP status.
};

#endif  // FLOATING_IP_MANAGER_H
