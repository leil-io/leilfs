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

#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <cstdint>
#include <shellapi.h>
#include <vector>

using saunafs_stat_t = struct _stat64;

// Converts a UTF-8 string to a wide string (UTF-16) on Windows.
inline std::wstring utf8_to_wstring(const std::string &str) {
	int wlen = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);

	if (wlen <= 0) { return L""; }

	std::wstring wstr(wlen, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], wlen);

	if (!wstr.empty() && wstr.back() == L'\0') { wstr.pop_back(); }

	return wstr;
}

// Prints a UTF-16 string to console or file, handling console vs pipe/file redirection.
inline void winapi_print(const std::wstring &msg, bool is_error = false) {
	HANDLE h = GetStdHandle(is_error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
	if (h == nullptr || h == INVALID_HANDLE_VALUE) return;

	DWORD mode = 0;
	BOOL isConsole = GetConsoleMode(h, &mode);

	if (isConsole) {
		// Console → Write UTF-16 directly
		DWORD written;
		WriteConsoleW(h, msg.c_str(), (DWORD)msg.size(), &written, nullptr);
	} else {
		// Pipe / file / redirection → must convert to UTF-8
		int utf8len = WideCharToMultiByte(CP_UTF8, 0, msg.c_str(), (int)msg.size(), nullptr, 0,
		                                  nullptr, nullptr);

		if (utf8len <= 0) { return; }

		std::string utf8(utf8len, '\0');

		WideCharToMultiByte(CP_UTF8, 0, msg.c_str(), (int)msg.size(), utf8.data(), utf8len, nullptr,
		                    nullptr);

		DWORD written;
		WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
	}
}

inline void print_unicode_console(const std::wstring &msg) { winapi_print(msg, false); }

inline void print_unicode_console_error(const std::wstring &msg) { winapi_print(msg, true); }

// This class is a RAII guard to restore console code pages on destruction on
// Windows.
// Used to temporarily set console code pages to UTF-8 and restore them later
// to their original values.
class ConsoleCodePageGuard {
public:
	explicit ConsoleCodePageGuard(uint32_t newCodePage) {
		oldOutputCP_ = GetConsoleOutputCP();
		oldInputCP_ = GetConsoleCP();
		SetConsoleOutputCP(newCodePage);
		SetConsoleCP(newCodePage);
	}

	~ConsoleCodePageGuard() {
		SetConsoleOutputCP(oldOutputCP_);
		SetConsoleCP(oldInputCP_);
	}

	ConsoleCodePageGuard(const ConsoleCodePageGuard &) = delete;
	ConsoleCodePageGuard &operator=(const ConsoleCodePageGuard &) = delete;

private:
	uint32_t oldOutputCP_;
	uint32_t oldInputCP_;
};

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

// Reads the entire file into a std::string (UTF-8)
inline bool read_file_to_string(const std::string &path, std::string &out) {
#ifdef _WIN32
	std::filesystem::path fsPath{utf8_to_wstring(path)};
#else
	std::filesystem::path fsPath{path};
#endif
	std::ifstream ifs(fsPath, std::ios::binary);
	if (!ifs) { return false; }

	ifs.seekg(0, std::ios::end);
	std::streampos size = ifs.tellg();
	if (size < 0) { return false; }
	ifs.seekg(0, std::ios::beg);

	out.clear();
	if (size == 0) { return true; }

	out.resize(static_cast<std::size_t>(size));
	if (!ifs.read(out.data(), size)) {
		out.clear();
		return false;
	}
	return true;
}
