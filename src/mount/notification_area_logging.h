/*
 Copyright 2023      Leil Storage OÜ

 SaunaFS is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, version 3.

 SaunaFS is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#ifdef _WIN32
#include <shellapi.h>
#include <windows.h>
#include <string>
#else
#include <cstdlib>
#endif

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

inline bool gShowMessagesOnNotificationArea = false;

struct MessageCache {
	std::string message;
	std::chrono::steady_clock::time_point timestamp;
};

inline std::jthread notificationThread;

#ifdef _WIN32
inline std::string mountpointIdentifierName = "SaunaFS Windows Client";	
#else
inline std::string mountpointIdentifierName = "SaunaFS Linux Client";
#endif
inline std::unordered_map<std::string, MessageCache> messageCache;
inline std::map<int, MessageCache> fullPathFromInodeCache;
inline std::chrono::seconds gMessageSuppressionPeriodSeconds;
inline std::mutex messageCacheMutex;
inline std::condition_variable cv;
inline std::atomic<bool> stopThread = false;

#ifdef _WIN32
inline void ShowWindowsNotification(const std::string &message) {
	// Create a hidden window
	HWND hWnd = CreateWindowEx(0, "STATIC", "HiddenWindow", 0, 0, 0, 0, 0,
	                           HWND_MESSAGE, NULL, NULL, NULL);

	// Define the NOTIFYICONDATA structure
	NOTIFYICONDATA nid = {};
	nid.cbSize = sizeof(NOTIFYICONDATA);
	nid.hWnd = hWnd;                   // Handle to the hidden window
	nid.uID = 1001;                    // Unique ID for the notification icon
	nid.uFlags = NIF_INFO | NIF_ICON;  // Display an info balloon and an icon
	nid.dwInfoFlags = NIIF_INFO;       // Info icon
	strcpy_s(nid.szInfo, message.c_str());                // Set the message
	strcpy_s(nid.szInfoTitle, mountpointIdentifierName.c_str());  // Set the title
	nid.hIcon = LoadIcon(NULL, IDI_INFORMATION);  // Load a standard icon

	// Display the notification
	if (!Shell_NotifyIcon(NIM_ADD, &nid)) {
		// Handle error
		safs::log_debug(
		    "ShowMessageOnWindowsNotificationArea: Failed to "
		    "display notification icon");
	}

	// Clean up
	DestroyWindow(hWnd);
}
#else
inline void ShowLinuxNotification(const std::string &message) {
	std::string command = R"(notify-send ")" + mountpointIdentifierName +
	                      R"(" ")" + message + "\"";

	if (std::system(command.c_str()) != 0) {
		safs::log_debug(
		    "addNotificationMessage: Failed to show notification using "
		    "notify-send");
	}
}
#endif

inline void NotificationThread() {
	while (!stopThread) {
		std::unique_lock<std::mutex> lock(messageCacheMutex);
		cv.wait(lock, [] { return !messageCache.empty() || stopThread; });

		if (stopThread) { break; }

		auto now = std::chrono::steady_clock::now();
		for (auto it = messageCache.begin(); it != messageCache.end();) {
			if ((now - it->second.timestamp) >=
			    gMessageSuppressionPeriodSeconds) {
				// Show the message
#ifdef _WIN32
				ShowWindowsNotification(it->second.message);
#else
				ShowLinuxNotification(it->second.message);
#endif
				it = messageCache.erase(it);
			} else {
				++it;
			}
		}

		for (auto it = fullPathFromInodeCache.begin();
		     it != fullPathFromInodeCache.end();) {
			if ((now - it->second.timestamp) >=
			    gMessageSuppressionPeriodSeconds) {
				it = fullPathFromInodeCache.erase(it);
			} else {
				++it;
			}
		}
	}
}

inline void addNotificationMessage(const std::string &message) {
	if (stopThread || !gShowMessagesOnNotificationArea) { return; }
	auto now = std::chrono::steady_clock::now();

	std::unique_lock<std::mutex> lock(messageCacheMutex);
	auto it = messageCache.find(message);
	if (it != messageCache.end() &&
	    (now - it->second.timestamp) < gMessageSuppressionPeriodSeconds) {
		// Suppress the message
		return;
	}

	// Update the cache with the current message and timestamp
	messageCache[message] = {message, now};
	cv.notify_one();
}

inline void addPathByInodeBasedNotificationMessage(const std::string &message,
                                                   uint32_t inode) {
	if (stopThread || !gShowMessagesOnNotificationArea) { return; }
	auto now = std::chrono::steady_clock::now();

	std::string fullPath;
	std::unique_lock<std::mutex> lock(messageCacheMutex);
	auto it = fullPathFromInodeCache.find(inode);
	if (it != fullPathFromInodeCache.end() &&
	    (now - it->second.timestamp) < gMessageSuppressionPeriodSeconds) {
		fullPath = it->second.message;
	} else {
		fs_fullpath(inode, 0, 0, fullPath);
		fullPathFromInodeCache[inode] = {fullPath, now};
	}
	lock.unlock();

	addNotificationMessage(message + ": " + fullPath);
}

inline void StartNotificationThread() {
	stopThread = false;
	notificationThread = std::jthread(NotificationThread);
}

inline void StopNotificationThread() {
	{
		std::unique_lock<std::mutex> lock(messageCacheMutex);
		stopThread = true;
		cv.notify_all();
	}
	if (notificationThread.joinable()) { notificationThread.join(); }
}

inline void notifications_area_logging_init(
    bool log_notification_area, unsigned message_suppression_period,
    std::string mountpoint_identifier_name) {
	mountpointIdentifierName += " (" + mountpoint_identifier_name + ")";
	gShowMessagesOnNotificationArea = log_notification_area;
	gMessageSuppressionPeriodSeconds =
	    std::chrono::seconds(message_suppression_period);
	if (gShowMessagesOnNotificationArea) { StartNotificationThread(); }
}

inline void notifications_area_logging_term() {
	if (gShowMessagesOnNotificationArea) { StopNotificationThread(); }
}
