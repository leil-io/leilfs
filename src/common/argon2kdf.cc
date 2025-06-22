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

#include "common/platform.h"

#include "common/argon2kdf.h"
#include "slogger/slogger.h"

#include <cstdlib>
#include <cstring>

void make_argon2_digest(uint8_t digest[ARGON2_DIGEST_SIZE], const uint8_t *password, size_t pwlen) {
	uint8_t salt[ARGON2_SALT_SIZE];
	for (size_t i = 0; i < ARGON2_SALT_SIZE; ++i) { salt[i] = static_cast<uint8_t>(rand() & 0xFF); }

	uint32_t time_cost = 2;
	uint32_t memory_cost = 1 << 16;
	uint32_t parallelism = 1;

	uint8_t hash[32];
	int rc =
	    argon2_hash(time_cost, memory_cost, parallelism, password, pwlen, salt, ARGON2_SALT_SIZE,
	                hash, sizeof(hash), nullptr, 0, Argon2_id, ARGON2_VERSION_13);
	if (rc != ARGON2_OK) {
		std::memset(digest, 0, ARGON2_DIGEST_SIZE);
		return;
	}

	// Pack digest
	digest[0] = 0x01;
	std::memcpy(digest + 1, salt, ARGON2_SALT_SIZE);
	std::memcpy(digest + 1 + ARGON2_SALT_SIZE, hash, ARGON2_HASH_PREFIX_SIZE);
}

bool verify_argon2_digest(const uint8_t digest[ARGON2_DIGEST_SIZE], const uint8_t *password,
                          size_t pwlen) {
	const uint8_t *salt = digest + 1;
	const uint8_t *expected_prefix = digest + 1 + ARGON2_SALT_SIZE;

	uint32_t time_cost = 2;
	uint32_t memory_cost = 1 << 16;
	uint32_t parallelism = 1;

	uint8_t hash[32];
	int rc =
	    argon2_hash(time_cost, memory_cost, parallelism, password, pwlen, salt, ARGON2_SALT_SIZE,
	                hash, sizeof(hash), nullptr, 0, Argon2_id, ARGON2_VERSION_13);
	if (rc != ARGON2_OK) { return false; }

	return std::memcmp(hash, expected_prefix, ARGON2_HASH_PREFIX_SIZE) == 0;
}
