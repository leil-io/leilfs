/*
   Copyright 2013-2016 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

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

#include <atomic>
#include <map>

#include "common/aligned_allocator.h"
#include "common/chunk_connector.h"
#include "common/chunk_part_type.h"
#include "common/chunk_type_with_address.h"
#include "common/chunkserver_stats.h"
#include "common/connection_pool.h"
#include "common/network_address.h"
#include "common/read_plan.h"
#include "common/read_operation_executor.h"
#include "common/time_utils.h"
#include "common/flat_map.h"
#include "protocol/packet.h"
#include "protocol/SFSCommunication.h"

/*! \brief Class responsible for executing read plan */
class ReadPlanExecutor {
public:
	typedef flat_map<ChunkPartType, ChunkTypeWithAddress> ChunkTypeLocations;

	/*! \brief Constructor.
	 *
	 * \param chunkserverStats Reference to class storing chunkserver statistics.
	 * \param chunkId Id of the chunk to read.
	 * \param chunkVersion Version of the chunk to read.
	 * \param plan Plan to execute.
	 */
	ReadPlanExecutor(
			ChunkserverStats& chunkserverStats,
			uint64_t chunkId, uint32_t chunkVersion,
			std::unique_ptr<ReadPlan> plan);

	/*! \brief Execute the plan.
	 *
	 * The data will be appended to the buffer.
	 *
	 * \param buffer buffer, where the data will be stored
	 * \param locations locations of all the ChunkPartTypes that exist in the plan
	 * \param connector object which will be used to obtain connections to chunkservers
	 * \param connect_timeout connection timeout
	 * \param wave_timeout wave timeout
	 * \param totalTimeout timeout of the whole operation
	 */
	template <typename VectorT>
	void executePlan(VectorT &buffer, const ChunkTypeLocations &locations,
	                 ChunkConnector &connector, int connect_timeout, int wave_timeout,
	                 const Timeout &total_timeout) {
		executors_.clear();
		networking_failures_.clear();
		available_parts_.clear();
		++executions_total_;

		std::size_t initial_size_of_buffer = buffer.size();
		buffer.resize(initial_size_of_buffer + plan_->fullBufferSize());

		checkPlan(buffer.data() + initial_size_of_buffer);

		ExecuteParams params{buffer.data() + initial_size_of_buffer + plan_->readOffset(),
		                     locations,
		                     connector,
		                     connect_timeout,
		                     wave_timeout,
		                     total_timeout};

		try {
			executeReadOperations(params);
			int result_size =
			    plan_->postProcessData(buffer.data() + initial_size_of_buffer, available_parts_);
			buffer.resize(initial_size_of_buffer + result_size);
		} catch (Exception &) {
			for (const auto &fd_and_executor : executors_) {
				tcpclose(fd_and_executor.first);
				stats_.unregisterReadOperation(fd_and_executor.second.server());
			}
			buffer.resize(initial_size_of_buffer);
			throw;
		}

		for (const auto &fd_and_executor : executors_) {
			tcpclose(fd_and_executor.first);
			stats_.unregisterReadOperation(fd_and_executor.second.server());
		}
	}

	/*! \brief Function Return parts that couldn't be read in execution phase.
	 *
	 * \return set of parts that couldn't be read during the last call to executePlan
	 */
	const ReadPlan::PartsContainer& partsFailed() const {
		return networking_failures_;
	}

	/// Counter for the .saunafs_tweaks file.
	static std::atomic<uint64_t> executions_total_;

	/// Counter for the .saunafs_tweaks file.
	static std::atomic<uint64_t> executions_with_additional_operations_;

	/// Counter for the .saunafs_tweaks file.
	static std::atomic<uint64_t> executions_finished_by_additional_operations_;

protected:
	struct ExecuteParams {
		uint8_t *buffer;
		const ChunkTypeLocations &locations;
		ChunkConnector &connector;
		int connect_timeout;
		int wave_timeout;
		const Timeout &total_timeout;
	};

	void checkPlan(uint8_t *buffer_start);

	bool startReadOperation(ExecuteParams &params, ChunkPartType chunk_type,
	                        const ReadPlan::ReadOperation &op);
	void startPrefetchOperation(ExecuteParams &params, ChunkPartType chunk_type,
	                            const ReadPlan::ReadOperation &op);
	int startReadsForWave(ExecuteParams &params, int wave);
	void startPrefetchForWave(ExecuteParams &params, int wave);
	bool waitForData(ExecuteParams &params, Timeout &wave_timeout, std::vector<pollfd> &poll_fds);
	bool readSomeData(ExecuteParams &params, const pollfd &poll_fd,
	                  ReadOperationExecutor &executor);
	void executeReadOperations(ExecuteParams &params);

private:
	ChunkserverStats& stats_;
	const uint64_t chunk_id_;
	const uint32_t chunk_version_;
	std::unique_ptr<ReadPlan> plan_;

	flat_map<int, ReadOperationExecutor> executors_;
	ReadPlan::PartsContainer available_parts_;
	ReadPlan::PartsContainer networking_failures_;
	NetworkAddress last_connection_failure_;
};
