/*
   Copyright 2023      Leil Storage OÜ

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

#include "common/platform.h"

#include <sys/stat.h>

#ifdef _WIN32
#include <shellapi.h>
#include <windows.h>
#include <string>
#include <vector>

using saunafs_stat_t = struct _stat64;

// Represents UTF-8 command-line arguments for Windows.
// Keeps argvPtrs alive for the lifetime of the object.
class Utf8CmdArguments {
public:
	Utf8CmdArguments() {
		argvUtf8_ = get_utf8_argv();
		argvPtrs_.reserve(argvUtf8_.size() + 1);
		for (auto &s : argvUtf8_) { argvPtrs_.push_back(s.data()); }
		argvPtrs_.push_back(nullptr);
	}

	char **getArgv() const { return const_cast<char **>(argvPtrs_.data()); }
	int getArgc() const { return static_cast<int>(argvUtf8_.size()); }

private:
	static std::vector<std::string> get_utf8_argv() {
		int argcW = 0;
		LPWSTR *argvW = CommandLineToArgvW(GetCommandLineW(), &argcW);
		std::vector<std::string> argvUtf8;
		if (!argvW || argcW <= 0) { return argvUtf8; }
		argvUtf8.reserve(argcW);

		for (int i = 0; i < argcW; ++i) {
			int size = WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, nullptr, 0, nullptr, nullptr);
			if (size <= 1) {
				argvUtf8.emplace_back();
				continue;
			}
			std::string utf8(size, '\0');
			int written =
			    WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, &utf8[0], size, nullptr, nullptr);
			if (written != size || written == 0) {
				argvUtf8.emplace_back();
				continue;
			}
			utf8.resize(size - 1);  // Remove the null terminator
			argvUtf8.push_back(std::move(utf8));
		}
		LocalFree(argvW);
		return argvUtf8;
	}

	std::vector<std::string> argvUtf8_;
	std::vector<char *> argvPtrs_;
};

inline int utf8_stat(const char *path_utf8, saunafs_stat_t *st) {
	int wlen = MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, nullptr, 0);
	if (wlen <= 0) { return -1; }

	std::wstring wpath(wlen, L'\0');  // include null terminator
	MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, wpath.data(), wlen);

	return _wstat64(wpath.c_str(), st);
}

// Unified portable wrapper
inline int stat_portable(const char *path, saunafs_stat_t *st) {
	return utf8_stat(path, reinterpret_cast<struct _stat64 *>(st));
}
#else
using saunafs_stat_t = struct stat;

// On POSIX just call stat
inline int stat_portable(const char *path, saunafs_stat_t *st) { return stat(path, st); }
#endif
