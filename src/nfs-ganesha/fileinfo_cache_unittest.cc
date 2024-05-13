// SPDX-License-Identifier: LGPL-3.0-or-later
/*

   Copyright 2017 Skytechnology sp. z o.o.
   Copyright 2023 Leil Storage OÜ

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301 USA
*/

#include "fileinfo_cache.h"

#include <gtest/gtest.h>

TEST(FileInfoCache, Basic) {
	FileInfoCache_t *cache = createFileInfoCache(16, 0);

	FileInfoEntry_t *entry7 = acquireFileInfoCache(cache, 7);
	FileInfoEntry_t *entry11 = acquireFileInfoCache(cache, 11);
	ASSERT_EQ(extractFileInfo(entry7), nullptr);
	ASSERT_EQ(extractFileInfo(entry11), nullptr);

	attachFileInfo(entry7, (fileinfo_t *)0xb00b1e5);
	attachFileInfo(entry11, (fileinfo_t *)0xface);

	FileInfoEntry_t *expired = popExpiredFileInfoCache(cache);
	ASSERT_EQ(expired, nullptr);
	releaseFileInfoCache(cache, entry7);

	FileInfoEntry_t *entry7_2 = acquireFileInfoCache(cache, 7);
	ASSERT_NE(extractFileInfo(entry7_2), nullptr);
	ASSERT_EQ(extractFileInfo(entry7_2), (fileinfo_t *)0xb00b1e5);
	ASSERT_EQ(extractFileInfo(entry11), (fileinfo_t *)0xface);

	expired = popExpiredFileInfoCache(cache);
	ASSERT_EQ(expired, nullptr);

	releaseFileInfoCache(cache, entry7_2);

	expired = popExpiredFileInfoCache(cache);
	ASSERT_NE(expired, nullptr);
	fileInfoEntryFree(expired);

	releaseFileInfoCache(cache, entry11);
	expired = popExpiredFileInfoCache(cache);
	while (expired != nullptr) {
		fileInfoEntryFree(expired);
		expired = popExpiredFileInfoCache(cache);
	}

	destroyFileInfoCache(cache);
}

TEST(FileInfoCache, Full) {
	FileInfoCache_t *cache = createFileInfoCache(3, 0);

	auto *entry1 = acquireFileInfoCache(cache, 1);
	auto *entry2 = acquireFileInfoCache(cache, 1);
	auto *entry3 = acquireFileInfoCache(cache, 1);
	auto *entry4 = acquireFileInfoCache(cache, 1);

	ASSERT_NE(entry1, nullptr);
	ASSERT_NE(entry2, nullptr);
	ASSERT_NE(entry3, nullptr);
	ASSERT_NE(entry4, nullptr);

	eraseFileInfoCache(cache, entry1);
	releaseFileInfoCache(cache, entry2);
	releaseFileInfoCache(cache, entry3);
	releaseFileInfoCache(cache, entry4);

	auto *expired = popExpiredFileInfoCache(cache);
	ASSERT_NE(expired, nullptr);
	fileInfoEntryFree(expired);

	expired = popExpiredFileInfoCache(cache);
	ASSERT_NE(expired, nullptr);
	fileInfoEntryFree(expired);

	expired = popExpiredFileInfoCache(cache);
	ASSERT_NE(expired, nullptr);
	fileInfoEntryFree(expired);

	expired = popExpiredFileInfoCache(cache);
	ASSERT_EQ(expired, nullptr);

	destroyFileInfoCache(cache);
}

TEST(FileInfoCache, Reset) {
	FileInfoCache_t *cache = createFileInfoCache(100000, 100000);

	auto *entry1 = acquireFileInfoCache(cache, 1);
	releaseFileInfoCache(cache, entry1);

	auto expired = popExpiredFileInfoCache(cache);
	ASSERT_EQ(expired, nullptr);

	resetFileInfoCacheParameters(cache, 0, 0);

	expired = popExpiredFileInfoCache(cache);
	ASSERT_NE(expired, nullptr);
	fileInfoEntryFree(expired);

	destroyFileInfoCache(cache);
}
