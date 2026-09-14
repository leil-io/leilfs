/*
   Copyright 2023      Leil Storage OÜ

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

#include <cstring>
#include <string>

/**
 * @brief Normalize a single argument casing by converting uppercase characters
 * in the options names to its lowercase equivalent.
 */
void normalize_argument_casing(char *arg) {
	// A single argument looks like this: arg1=val1,arg2=val2,...,argn=valn
	// So, case change should be applied at the beginning and between a comma
	// appearance and the next equal sign

	std::string argument(arg);
	bool should_apply_case_change = true;

	for (auto &c : argument) {
		if (c == ',') {
			should_apply_case_change = true;
		} else if (c == '=') {
			should_apply_case_change = false;
		} else if (should_apply_case_change) {
			if (std::isupper(c)) { c = std::tolower(c); }
		}
	}

	// Copy the modified string back to the original C-string argument
	std::copy(argument.begin(), argument.end(), arg);
	
	// Null-terminate the modified C-string
	arg[argument.size()] = '\0';
}

/**
 * @brief Normalize options casing by converting uppercase characters in the
 * options names to its lowercase equivalent.
 */
void normalize_options_casing(int argc, char *argv[]) {
	bool is_previous_dash_o = false;
	for (int index = 1; index < argc; index++) {
		size_t current_arg_len = strlen(argv[index]);
		if (is_previous_dash_o ||
		    (current_arg_len > 2 && argv[index][0] == '-' &&
		     (argv[index][1] == 'o' || argv[index][1] == '-'))) {
			if (is_previous_dash_o) {
				normalize_argument_casing(argv[index]);
			} else {
				normalize_argument_casing(argv[index] + 2);
			}
			is_previous_dash_o = false;
			continue;
		}
		is_previous_dash_o = (strcmp(argv[index], "-o") == 0);
	}
}
