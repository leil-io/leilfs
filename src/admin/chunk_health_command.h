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

#include <map>

#include "admin/saunafs_admin_command.h"
#include "common/chunk_health_freshness.h"
#include "common/chunks_availability_state.h"
#include "common/server_connection.h"

class ChunksHealthCommand : public SaunaFsAdminCommand {
public:
	virtual std::string name() const;
	virtual SupportedOptions supportedOptions() const;
	virtual void usage() const;
	virtual void run(const Options& options) const;

	/// Human-readable sentence on when the counts were measured, printed before them.
	static std::string freshnessSummary(const ChunkHealthFreshness &freshness, uint32_t serverTime);
	/// Porcelain MEA row, printed after the counters: generation, age, duration, chunks scanned,
	/// chunks excluded, chunkservers unreachable. Generation 0 means nothing measured yet.
	static std::string freshnessRow(const ChunkHealthFreshness &freshness, uint32_t serverTime);

private:
	static const std::string kOptionAll;
	static const std::string kOptionAvailability;
	static const std::string kOptionReplication;
	static const std::string kOptionDeletion;

	static void initializeGoals(ServerConnection& connection);

	/// Human-readable paragraph on when the counts were measured, printed before them.
	void printFreshness(const ChunkHealthFreshness &freshness, uint32_t serverTime) const;
	/// Porcelain MEA row, printed after the counters: generation, age, duration, chunks scanned,
	/// chunks excluded, chunkservers unreachable. Generation 0 means nothing measured yet.
	void printFreshnessRow(const ChunkHealthFreshness &freshness, uint32_t serverTime) const;
	void printState(const ChunksAvailabilityState& state, bool isPorcelain) const;
	void printState(bool isReplication, const ChunksReplicationState& state,
			bool isPorcelain) const;
	std::string print(uint64_t number) const;

	static std::vector<uint8_t> goals;
	static std::map<uint8_t, std::string> goalNames;
};
