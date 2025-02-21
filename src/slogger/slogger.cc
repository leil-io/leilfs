/*
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
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

#include "common/platform.h"
#include "slogger/slogger.h"
#include "config/cfg.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <string>
#include <expected>

#include "errors/sfserr.h"

static safs::log_level::LogLevel log_level_from_syslog(int priority) {
	static const std::array<safs::log_level::LogLevel, 8> kSyslogToLevel = {{
		safs::log_level::critical, // emerg
		safs::log_level::critical, // alert
		safs::log_level::critical, // critical
		safs::log_level::err,      // error
		safs::log_level::warn,     // warning
		safs::log_level::info,     // notice
		safs::log_level::info,     // info
		safs::log_level::debug,    // debug
	}};
	return kSyslogToLevel[std::min<int>(priority, kSyslogToLevel.size())];
}

std::expected<safs::log_level::LogLevel, std::string>
safs::log_level_from_string(const std::string &level) {
	std::string lower_level = level;
	std::ranges::transform(
	    lower_level, lower_level.begin(),
	    [](unsigned char c_char) { return std::tolower(c_char); });

	if (lower_level == "trace") {
		return safs::log_level::trace;
	}
	if (lower_level == "debug") {
		return safs::log_level::debug;
	}
	if (lower_level == "info") {
		return safs::log_level::info;
	}
	if (lower_level == "warn" || lower_level == "warning") {
		return safs::log_level::warn;
	}
	if (lower_level == "err" || lower_level == "error") {
		return safs::log_level::err;
	}
	if (lower_level == "crit" || lower_level == "critical") {
		return safs::log_level::critical;
	}
	if (lower_level == "off") {
		return safs::log_level::off;
	}
	return std::unexpected<std::string>("Invalid log level: " + level);
}

void safs::setup_logs() {
	std::string flush_on_str = cfg_getstring("LOG_FLUSH_ON", "CRITICAL");
	safs::log_level::LogLevel priority = safs::log_level::critical;
	if (flush_on_str == "ERROR") {
		priority = safs::log_level::err;
	} else if (flush_on_str == "WARNING") {
		priority = safs::log_level::warn;
	} else if (flush_on_str == "INFO") {
		priority = safs::log_level::info;
	} else if (flush_on_str == "DEBUG") {
		priority = safs::log_level::debug;
	} else if (flush_on_str == "TRACE") {
		priority = safs::log_level::trace;
	}
	// Clear all logs first, to make sure we have a clean setup
	safs::drop_all_logs();

	// Defaults first
	safs::log_level::LogLevel level = safs::log_level::info;

	std::string log_level_str;
	auto *log_level_c = std::getenv("SAUNAFS_LOG_LEVEL");
	if (log_level_c == nullptr) {
		log_level_str = cfg_getstring("LOG_LEVEL", "info");
	} else {
		log_level_str = std::string(log_level_c);
	}

	auto result = safs::log_level_from_string(log_level_str);
	if (result) {
		level = result.value();
	}
	safs::add_log_stderr(level);
	safs::add_log_syslog(level);
	if (!result) {
		safs::log_err("{}", result.error());
		safs::log_info("Using default log level of '{}'", log_level_to_string(level));
	}

	for (std::string suffix : {"", "_A", "_B", "_C"}) {
		std::string configEntryName = "MAGIC_DEBUG_LOG" + suffix;
		std::string value = cfg_get(configEntryName.c_str(), "");
		if (value.empty()) {
			continue;
		}
		add_log_file(value.c_str(), safs::log_level::trace, 16*1024*1024, 8);
	}
	safs::set_log_flush_on(priority);
}

std::string safs::log_level_to_string(safs::log_level::LogLevel level) {
	using safs::log_level::LogLevel;
	switch (level) {
	case LogLevel::trace:
		return "trace";
	case LogLevel::debug:
		return "debug";
	case LogLevel::info:
		return "info";
	case LogLevel::warn:
		return "warn";
	case LogLevel::err:
		return "err";
	case LogLevel::critical:
		return "crit";
	case LogLevel::off:
		break;
	}
	return "off";
};

bool safs_add_log_file(const char *path, int priority, int max_file_size, int max_file_count) {
	return safs::add_log_file(path, log_level_from_syslog(priority), max_file_size, max_file_count);
}

void safs_drop_all_logs() {
	return safs::drop_all_logs();
}

bool safs_add_log_stderr(int priority) {
	return safs::add_log_stderr(log_level_from_syslog(priority));
}

bool safs::add_log_file(const char *path, log_level::LogLevel level, int max_file_size, int max_file_count) {
	try {
		LoggerPtr logger = spdlog::rotating_logger_mt(path, path, max_file_size, max_file_count);
		logger->set_level((spdlog::level::level_enum)level);
		// Format: DATE TIME [LEVEL] [PID:TID] : MESSAGE
		logger->set_pattern("%D %H:%M:%S.%e [%l] [%P:%t] : %v");
		return true;
	} catch (const spdlog::spdlog_ex &e) {
		safs_pretty_syslog(LOG_ERR, "Adding %s log file failed: %s", path, e.what());
	}
	return false;
}

void safs::set_log_flush_on(log_level::LogLevel level) {
	spdlog::apply_all([level](LoggerPtr l) {l->flush_on((spdlog::level::level_enum)level);});
}

void safs::drop_all_logs() {
	spdlog::drop_all();
}

bool safs::add_log_syslog([[maybe_unused]] log_level::LogLevel level) {
#ifndef _WIN32
	try {
		static constexpr bool kEnableFormatting = true;
		// Except for first and last arguments, the rest are defaults
		LoggerPtr logger = spdlog::syslog_logger_mt("syslog", "", 0, LOG_USER, kEnableFormatting);
		logger->set_pattern("[%t] %l: %v");
		logger->set_level((spdlog::level::level_enum)level);
		return true;
	} catch (const spdlog::spdlog_ex &e) {
		safs_pretty_syslog(LOG_ERR, "Adding syslog log failed: %s", e.what());
	}
#endif
	return false;
}

bool safs::add_log_stderr(log_level::LogLevel level) {
	try {
		LoggerPtr logger = spdlog::stderr_color_mt("stderr");
		logger->set_level((spdlog::level::level_enum)level);
		// Format: DATE TIME [LEVEL] [PID:TID] : MESSAGE
		logger->set_pattern("%D %H:%M:%S.%e [%l] [%P:%t] : %v");
		return true;
	} catch (const spdlog::spdlog_ex &e) {
		safs_pretty_syslog(LOG_ERR, "Adding stderr log failed: %s", e.what());
	}
	return false;
}

static void safs_vsyslog(int priority, const char* format, va_list ap) {
	char buf[1024];
	va_list ap2;
	va_copy(ap2, ap);
	int written = vsnprintf(buf, 1023, format, ap2);
	if (written < 0) {
		va_end(ap2);
		return;
	}
	buf[std::min<int>(written, sizeof(buf))] = '\0';
	va_end(ap2);

	spdlog::apply_all([priority, buf](LoggerPtr l) {
		l->log((spdlog::level::level_enum)log_level_from_syslog(priority), buf);
	});
}

void safs_pretty_syslog(int priority, const char* format, ...) {
	va_list ap;
	va_start(ap, format);
	safs_vsyslog(priority, format, ap);
	va_end(ap);
}

void safs_pretty_syslog_attempt(int priority, const char* format, ...) {
	va_list ap;
	va_start(ap, format);
	safs_vsyslog(priority, format, ap);
	va_end(ap);
}

void safs_pretty_errlog(int priority, const char* format, ...) {
	int err = errno;
	char buffer[1024];
	va_list ap;
	va_start(ap, format);
	int len = vsnprintf(buffer, 1023, format, ap);
	buffer[len] = 0;
	va_end(ap);
	safs_pretty_syslog(priority, "%s: %s", buffer, strerr(err));
	errno = err;
}

void safs_silent_syslog(int priority, const char* format, ...) {
	va_list ap;
	va_start(ap, format);
	safs_vsyslog(priority, format, ap);
	va_end(ap);
}

void safs_silent_errlog(int priority, const char* format, ...) {
	int err = errno;
	char buffer[1024];
	va_list ap;
	va_start(ap, format);
	int len = vsnprintf(buffer, 1023, format, ap);
	buffer[len] = 0;
	va_end(ap);
	safs_silent_syslog(priority, "%s: %s", buffer, strerr(err));
	errno = err;
}
