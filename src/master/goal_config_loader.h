/*
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
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

#include <cctype>
#include <cstring>
#include <map>

#include "common/goal.h"

/// Parser for files with configuration of goals
namespace goal_config {

/// goals with id below this value are initialized as wildcard * id
/// goals with id above this value are initialized as wildcard * kMaxCompatibleGoal
constexpr int kMaxCompatibleGoal = 5;

/// Reads the configuration file and parses it.
/// \throws ParseException
std::map<int, Goal> load(std::istream& stream);
std::map<int, Goal> load(std::istream&& stream);

std::pair<int, Goal> parseLine(const std::string &line);

Goal defaultGoal(int goal_id);

}

