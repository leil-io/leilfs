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

#include <gtest/gtest.h>
#include <gmock/gmock.h>

class MockFloatingIPManager : public HAFloatingIPManager {
public:
	MockFloatingIPManager(const std::string &iface,
	                      const std::string &ipAddress,
	                      const int &checkPeriod = 500)
	    : HAFloatingIPManager(iface, ipAddress, checkPeriod) {}
	void initialize() override {
		HAFloatingIPManager::initialize();
		restored.store(false);
	}

	bool restoreFloatingIp() override {
		std::string command =
		    "sudo ip addr add " + floatingIp() + "/24 dev " + floatingIpIface();
		int result = system(command.c_str());
		restored.store(result == 0);
		return (result == 0);
	}

	bool wasFloatingIpRestored() const { return restored.load(); }

	std::string floatingIp() const { return floatingIpAddress; }
	std::string floatingIpIface() const { return floatingIpInterface; }

private:
	std::atomic<bool> restored;
};

// Test Fixture
class FloatingIPManagerTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Create a virtual network interface for testing
		if (system("sudo ip link add name test0 type dummy") != 0) {
			std::cerr << "Failed to create test0 network interface\n";
			std::exit(EXIT_FAILURE);
		}
		if (system("sudo ip addr add 192.168.1.100/24 dev test0") != 0) {
			std::cerr << "Failed to add 192.168.1.100 to test0\n";
			std::exit(EXIT_FAILURE);
		}
		if (system("sudo ip addr add 192.168.1.105/24 dev test0") != 0) {
			std::cerr << "Failed to add 192.168.1.105 to test0\n";
			std::exit(EXIT_FAILURE);
		}
		if (system("sudo ip link set test0 up") != 0) {
			std::cerr << "Failed to set test0 interface up\n";
			std::exit(EXIT_FAILURE);
		}

		// Initialize the MockFloatingIPManager
		mockManager = std::make_unique<MockFloatingIPManager>("test0",
		                                                      "192.168.1.100");
	}

	void TearDown() override {
		// Remove the virtual network interface
		if (system("sudo ip link set test0 down") != 0) {
			std::cerr << "Failed to set test0 network interface down\n";
		}
		if (system("sudo ip link delete test0") != 0) {
			std::cerr << "Failed to delete test0 network interface\n";
		}
	}

	std::unique_ptr<MockFloatingIPManager> mockManager;
};

// Test Floating IP Loss Notification
TEST_F(FloatingIPManagerTest, HandleIpLoss) {
	mockManager->start();

	// Simulate IP loss by removing the IP address
	std::string ipAddress = mockManager->floatingIp();
	std::string ipIface = mockManager->floatingIpIface();
	std::string command = "sudo ip addr del " + ipAddress + "/24 dev " + ipIface;

	if (system(command.c_str()) != 0) {
		std::cerr << "Failed to remove " + ipAddress + " from " + ipIface + "\n";
		std::exit(EXIT_FAILURE);
	}

	// Verify that the IP address is removed
	EXPECT_FALSE(mockManager->isFloatingIpAlive());

	// Wait for a short period to allow the listener to detect the IP loss
	std::this_thread::sleep_for(std::chrono::seconds(1));

	// Verify that handleIpLoss was called
	EXPECT_TRUE(mockManager->isFloatingIpAlive());
}

// Test HandleIPLoss does not run for a different IP
TEST_F(FloatingIPManagerTest, HandleLossNonFloatingIP) {
	mockManager->start();

	auto mockManager105 =
	    std::make_unique<MockFloatingIPManager>("test0", "192.168.1.105");
	mockManager105->start();

	// Simulate IP loss of a different IP address: 192.168.1.105
	std::string ipAddress = mockManager105->floatingIp();
	std::string ipIface = mockManager105->floatingIpIface();
	std::string command = "sudo ip addr del " + ipAddress + "/24 dev " + ipIface;

	if (system(command.c_str()) != 0) {
		std::cerr << "Failed to remove " + ipAddress + " from " + ipIface + "\n";
		std::exit(EXIT_FAILURE);
	}

	// Verify that the IP addresses have not been restored
	EXPECT_FALSE(mockManager105->wasFloatingIpRestored());
	EXPECT_FALSE(mockManager->wasFloatingIpRestored());

	// Wait for a short period to allow the listener to detect the IP loss
	std::this_thread::sleep_for(std::chrono::seconds(1));

	// Verify that handleIpLoss was only called for mockManager105
	EXPECT_TRUE(mockManager105->wasFloatingIpRestored());
	EXPECT_FALSE(mockManager->wasFloatingIpRestored());
}

// Test HandleIPLoss works only after calling start() function
TEST_F(FloatingIPManagerTest, HandleIPLossAfterInitialize) {
	// Simulate IP loss by removing the IP address
	std::string ipAddress = mockManager->floatingIp();
	std::string ipIface = mockManager->floatingIpIface();
	std::string command = "sudo ip addr del " + ipAddress + "/24 dev " + ipIface;

	if (system(command.c_str()) != 0) {
		std::cerr << "Failed to remove " + ipAddress + " from " + ipIface + "\n";
		std::exit(EXIT_FAILURE);
	}

	// Wait for a short period to allow the listener to detect the IP loss
	std::this_thread::sleep_for(std::chrono::seconds(1));

	// mockManager was not started, so it should not handle IP loss
	EXPECT_FALSE(mockManager->isFloatingIpAlive());

	// start the mockManager
	mockManager->start();

	// Wait for a short period to allow the listener to detect the IP loss
	std::this_thread::sleep_for(std::chrono::seconds(1));

	// mockManager is started, so it should handle IP loss
	EXPECT_TRUE(mockManager->isFloatingIpAlive());
}
