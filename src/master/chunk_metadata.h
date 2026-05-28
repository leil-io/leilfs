/*
   Copyright 2026      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "common/platform.h"

/// Backend-agnostic chunk configuration, shared by every chunk-operations
/// backend and loaded from the config file during chunk subsystem init.

/// AVOID_SAME_IP_CHUNKSERVERS: when set, chunk placement avoids putting more than
/// one part of a chunk on chunkservers that share an IP address.
inline bool gAvoidSameIpChunkservers = false;
