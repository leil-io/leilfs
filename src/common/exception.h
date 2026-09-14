/*
   Copyright 2013-2014 EditShare
   Copyright 2013-2017 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include <cassert>
#include <exception>
#include <string>

#include "errors/saunafs_error_codes.h"

class Exception : public std::exception {
public:
	Exception(const std::string& message) : message_(message), status_(SAUNAFS_ERROR_UNKNOWN) {
	}

	Exception(const std::string& message, uint8_t status) : message_(message), status_(status) {
		assert(status != SAUNAFS_STATUS_OK);
		if (status != SAUNAFS_ERROR_UNKNOWN) {
			message_ += " (" + std::string(saunafs_error_string(status)) + ")";
		}
	}

	~Exception() noexcept {
	}

	const char* what() const noexcept override {
		return message_.c_str();
	}

	const std::string& message() const {
		return message_;
	}

	uint8_t status() const {
		return status_;
	}

private:
	std::string message_;
	uint8_t status_;
};

#define SAUNAFS_CREATE_EXCEPTION_CLASS(name, base) \
	class name : public base { \
	public: \
		name(const std::string& message) : base(message) {} \
		name(const std::string& message, uint8_t status) : base(message, status) {} \
		~name() noexcept {} \
	}

#define SAUNAFS_CREATE_EXCEPTION_CLASS_MSG(name, base, message) \
	class name : public base { \
	public: \
		name() : base(std::string(message)) {} \
		name(uint8_t status) : base(std::string(message), status) {} \
		~name() noexcept {} \
	}
