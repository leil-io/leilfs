/*


 Copyright 2023 Leil Storage OÜ

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

#include "common/platform.h"

#include "mount/mount_info.h"

// Constructor
MountInfo::MountInfo()
    : pid_(0), mountOptions_(std::make_unique<std::map<std::string, std::string>>()) {}

// Getters
std::string MountInfo::getStartedDateUtc() const { return startedDateUtc_; }
#ifdef _WIN32
std::string MountInfo::getSid() const { return sid_; }
#else
std::string MountInfo::getUid() const { return uid_; }
std::string MountInfo::getGid() const { return gid_; }
#endif
std::string MountInfo::getUsername() const { return username_; }
int MountInfo::getPid() const { return pid_; }
std::string MountInfo::getVersion() const { return version_; }
std::string MountInfo::getCommitId() const { return commitId_; }
std::string MountInfo::getArguments() const { return arguments_; }
std::map<std::string, std::string> MountInfo::getMountOptions() const { return *mountOptions_; }
std::string MountInfo::getMountInfoStr() {
	if (mountInfoStr_.empty()) { buildMountInfoStr(); }
	return mountInfoStr_;
}

// Setters
void MountInfo::setStartedDateUtc(const std::string &date) { startedDateUtc_ = date; }
#ifdef _WIN32
void MountInfo::setSid(const std::string &sidValue) { sid_ = sidValue; }
#else
void MountInfo::setUid(const std::string &uidValue) { uid_ = uidValue; }
void MountInfo::setGid(const std::string &gidValue) { gid_ = gidValue; }
#endif
void MountInfo::setUsername(const std::string &user) { username_ = user; }
void MountInfo::setPid(int processId) { pid_ = processId; }
void MountInfo::setVersion(const std::string &ver) { version_ = ver; }
void MountInfo::setCommitId(const std::string &commit) { commitId_ = commit; }
void MountInfo::setArguments(const std::string &args) { arguments_ = args; }
void MountInfo::setMountOptions(const std::map<std::string, std::string> &options) {
	*mountOptions_ = options;
}

// Utility method
void MountInfo::buildMountInfoStr() {
	std::stringstream mountInfoStream;

	mountInfoStream << "SAUNAFS CLIENT MOUNT INFO:\n";
	mountInfoStream << "--------------------------------\n";
	mountInfoStream << "STARTED DATE: " << (startedDateUtc_.empty() ? "Unknown" : startedDateUtc_)
	                << "\n";
#ifdef _WIN32
	mountInfoStream << "SID: " << (sid_.empty() ? "Unknown" : sid_) << "\n";
#else
	mountInfoStream << "UID: " << (uid_.empty() ? "Unknown" : uid_) << "\n";
	mountInfoStream << "GID: " << (gid_.empty() ? "Unknown" : gid_) << "\n";
#endif
	mountInfoStream << "USERNAME: " << (username_.empty() ? "Unknown" : username_) << "\n";
	mountInfoStream << "PID: " << pid_ << "\n";
	mountInfoStream << "VERSION: " << (version_.empty() ? "Unknown" : version_) << "\n";
	mountInfoStream << "COMMIT_ID: " << (commitId_.empty() ? "Unknown" : commitId_) << "\n";
	mountInfoStream << "ARGUMENTS: " << (arguments_.empty() ? "None" : arguments_) << "\n";
	if (mountOptions_ && !mountOptions_->empty()) {
		mountInfoStream << "MOUNT OPTIONS:\n";
		for (const auto &opt : *mountOptions_) {
			std::string optionValueFromTweaks = gTweaks.getValueByOptionName(opt.first);
			mountInfoStream << opt.first << ": "
			                << (!optionValueFromTweaks.empty() ? optionValueFromTweaks : opt.second)
			                << "\n";
		}
	}
	mountInfoStream << "--------------------------------\n";
	mountInfoStr_ = mountInfoStream.str();
}

// Global functions
void mount_info_init(
#ifdef _WIN32
	const std::string &sid,
#else
	const int &uid, const int &gid,
#endif
	const std::string &username, int pid, const std::string &version,
	const std::string &commitId) {
	std::lock_guard lock(gMountInfoMtx);
#ifdef _WIN32
	gMountInfo.setSid(sid);
#else
	gMountInfo.setUid(std::to_string(uid));
	gMountInfo.setGid(std::to_string(gid));
#endif
	gMountInfo.setUsername(username);
	gMountInfo.setPid(pid);
	gMountInfo.setVersion(version);
	gMountInfo.setCommitId(commitId);
}

#ifdef _WIN32
std::string get_username() {
	char username[UNLEN + 1];
	DWORD username_len = UNLEN + 1;
	if (GetUserName(username, &username_len)) {
		return std::string(username);
	} else {
		return std::string();
	}
}

std::string get_current_user_sid() {
	HANDLE hToken = nullptr;
	PTOKEN_USER pTokenUser = nullptr;
	DWORD dwSize = 0;
	char *pStringSid = nullptr;

	// Open the access token associated with the current process
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
		std::wcerr << L"OpenProcessToken failed: " << GetLastError()
		           << std::endl;
		return "";
	}

	// Get the size of the user information in the token
	if (!GetTokenInformation(hToken, TokenUser, nullptr, 0, &dwSize) &&
	    GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
		std::wcerr << L"GetTokenInformation failed: " << GetLastError()
		           << std::endl;
		CloseHandle(hToken);
		return "";
	}

	// Allocate memory for the user information
	pTokenUser = (PTOKEN_USER)malloc(dwSize);
	if (!pTokenUser) {
		std::wcerr << L"Memory allocation failed" << std::endl;
		CloseHandle(hToken);
		return "";
	}

	// Retrieve the user information from the token
	if (!GetTokenInformation(hToken, TokenUser, pTokenUser, dwSize, &dwSize)) {
		std::wcerr << L"GetTokenInformation failed: " << GetLastError()
		           << std::endl;
		free(pTokenUser);
		CloseHandle(hToken);
		return "";
	}

	// Convert the SID to a string
	if (!ConvertSidToStringSid(pTokenUser->User.Sid, &pStringSid)) {
		std::wcerr << L"ConvertSidToStringSid failed: " << GetLastError()
		           << std::endl;
		free(pTokenUser);
		CloseHandle(hToken);
		return "";
	}

	// Clean up
	LocalFree(pStringSid);
	free(pTokenUser);
	CloseHandle(hToken);

	return std::string(pStringSid);
}
#else
std::string get_username_by_uid(uid_t uid) {
	struct passwd *pw = getpwuid(uid);
	if (pw) {
		return std::string(pw->pw_name);
	} else {
		return std::string();
	}
}
#endif

void set_all_mountpoint_arguments(int argc, char **argv) {
	std::string arguments;
	for (int i = 0; i < argc; i++) {
		arguments += argv[i];
		if (i < argc - 1) { arguments += " "; }
	}
	std::lock_guard lock(gMountInfoMtx);
	gMountInfo.setArguments(arguments);
}

void set_current_local_time() {
	auto now = std::chrono::system_clock::now();
	std::time_t now_time = std::chrono::system_clock::to_time_t(now);

	std::tm localtm;
#ifdef _WIN32
	localtime_s(&localtm, &now_time);
#else
	localtime_r(&now_time, &localtm);
#endif

	std::ostringstream oss;
	oss << std::put_time(&localtm, "%Y-%m-%d_%H:%M:%S");
	std::lock_guard lock(gMountInfoMtx);
	gMountInfo.setStartedDateUtc(oss.str());
}
