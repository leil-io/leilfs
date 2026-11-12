/*
   Copyright 2026 Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include <gtest/gtest.h>
#include <optional>
#include <utility>
#include <vector>

#include "mount/reconnect.h"

TEST(MountReconnectOrder, CurrentSuccessDoesNotResolveTheConfiguredHost) {
	std::vector<int> attempted;
	const auto connect = [&attempted](int server) {
		attempted.push_back(server);
		return server == 1;
	};
	const auto resolve = []() -> std::optional<int> {
		ADD_FAILURE() << "resolved before a working current endpoint";
		return 3;
	};

	mount_reconnect::tryKnownThenConfigured(std::vector{1, 2}, connect, resolve);
	EXPECT_EQ((std::vector{1}), attempted);
}

TEST(MountReconnectOrder, PeerSuccessDoesNotResolveTheConfiguredHost) {
	std::vector<int> attempted;
	const auto connect = [&attempted](int server) {
		attempted.push_back(server);
		return server == 2;
	};
	const auto resolve = []() -> std::optional<int> {
		ADD_FAILURE() << "resolved before a working pushed peer";
		return 3;
	};

	mount_reconnect::tryKnownThenConfigured(std::vector{1, 2}, connect, resolve);
	EXPECT_EQ((std::vector{1, 2}), attempted);
}

TEST(MountReconnectOrder, ConfiguredHostIsRetriedWithTheRefreshedSource) {
	int source = 10;
	std::vector<std::pair<int, int>> attempted;
	const auto connect = [&attempted, &source](int server) {
		attempted.emplace_back(server, source);
		return source == 20;
	};
	const auto resolve = [&attempted, &source]() -> std::optional<int> {
		EXPECT_EQ((std::vector<std::pair<int, int>>{{1, 10}, {2, 10}}), attempted);
		source = 20;
		return 1;
	};

	mount_reconnect::tryKnownThenConfigured(std::vector{1, 2}, connect, resolve);
	EXPECT_EQ((std::vector<std::pair<int, int>>{{1, 10}, {2, 10}, {1, 20}}), attempted);
}
