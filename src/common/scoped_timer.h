/*
	Copyright 2023-2024 Leil Storage OÜ

	This file is part of LeilFS.

	LeilFS is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, version 3.

	LeilFS is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with LeilFS  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "common/platform.h"

#include <memory>
#include <string>

namespace util {

/**
 * ScopedTimer is a class that measures the time between its creation and
 * destruction.
 * It is a RAII wrapper around std::chrono::high_resolution_clock
 * It logs the elapsed time to syslog
 */
class ScopedTimer {
public:

	enum class TimeUnit {
		TU_SEC = 0,
		TU_MILLI = 1,
		TU_MICRO = 2,
		TU_NANO = 3
	};

	/**
	 * Constructor
	 */
	ScopedTimer();

	/**
	 * We don't want to allow copying or moving
	 */
	ScopedTimer(const ScopedTimer &) = delete;
	ScopedTimer &operator=(const ScopedTimer &) = delete;
	ScopedTimer(ScopedTimer &&) noexcept = delete;
	ScopedTimer &operator=(ScopedTimer &&) noexcept = delete;

	/**
	 * Constructor
	 * @param message The message to log, it is prepended with the elapsed time
	 */
	explicit ScopedTimer(const std::string &);

	/**
	 * Constructor
	 * @param time_unit The time unit to use for the elapsed time
	 */
	explicit ScopedTimer(TimeUnit);

	/**
	 * Constructor
	 * @param message The message to log, it is prepended with the elapsed time
	 * @param time_unit The time unit to use for the elapsed time
	 */
	ScopedTimer(const std::string &, TimeUnit);

	/**
	 * Destructor
	 * Logs the elapsed time to syslog
	 */
	virtual ~ScopedTimer();

	/**
	 * Set the time unit to use
	 * @param time_unit The time unit to use for the elapsed time
	 */
	void setTimeUnit(TimeUnit time_unit);

	/**
	 * Get the elapsed time as a string
	 * @return The elapsed time as a string
	 */
	std::string sayElapsed();

private:
	class Impl;
	std::unique_ptr<Impl> pimpl_;
};

}  // namespace util
