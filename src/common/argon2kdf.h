/*
   Copyright 2025      Leil Storage OÜ

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

#include <argon2.h>
#include <array>
#include <inttypes.h>
#include <random>
#include <string>
#include <vector>

#define ARGON2_DIGEST_SIZE 16
#define ARGON2_SALT_SIZE 8
#define ARGON2_HASH_PREFIX_SIZE 7

void make_argon2_digest(uint8_t digest[ARGON2_DIGEST_SIZE], const uint8_t *password, size_t pwlen);
bool verify_argon2_digest(const uint8_t digest[ARGON2_DIGEST_SIZE], const uint8_t *password, size_t pwlen);
