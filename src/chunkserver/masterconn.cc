/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include "chunkserver/masterconn.h"

#include <netinet/in.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <syslog.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "chunkserver/bgjobs.h"
#include "chunkserver/hddspacemgr.h"
#include "chunkserver/master_connection.h"
#include "chunkserver/network_main_thread.h"
#include "common/event_loop.h"
#include "common/massert.h"
#include "common/network_address.h"
#include "common/random.h"
#include "common/sockets.h"
#include "config/cfg.h"
#include "devtools/request_log.h"
#include "protocol/SFSCommunication.h"
#include "protocol/cstoma.h"
#include "slogger/slogger.h"

//  From config
static std::string gMasterHost;
static std::string gMasterPort;
static bool gEnableLoadFactor;

static const uint64_t kSendStatusDelay = 5;

//  JobPool shared between all connections to MDSs
static std::shared_ptr<MasterJobPool> gJobPool;
static std::shared_ptr<MasterJobPool> gReplicationJobPool;

//  Singleton for the MasterConn instance (will become a list of connections in the future)
static std::unique_ptr<MasterConn> gMasterConnSingleton = nullptr;

static int gJobFD{-1};  ///< File descriptor for the job pool notifications
static int32_t gJobFDpDescPos{-1};  ///< Position in the pollfd array for the job pool notifications
static int gReplicationJobFD{-1}; ///< File descriptor for the replication job pool notifications
/// Position in the pollfd array for the replication job pool notifications
static int32_t gReplicationJobFDpDescPos{-1};

constexpr uint32_t kDefaultNumberOfWorkers = 10;
constexpr uint32_t kMinNumberOfWorkers = 2;
static uint32_t gNumberOfWorkers = kDefaultNumberOfWorkers;

constexpr uint32_t kDefaultReplicationNumberOfWorkers = 5;
constexpr uint32_t kMinReplicationNumberOfWorkers = 1;
static uint32_t gReplicationNumberOfWorkers = kDefaultReplicationNumberOfWorkers;

static void* gReconnectHook;

//  Stats
static uint32_t stats_maxjobscnt = 0;

void masterconn_stats(uint64_t *bin,uint64_t *bout,uint32_t *maxjobscnt) {
	//  For each connection, add the statistics
	auto totalBytesIn = gMasterConnSingleton->bytesIn();
	auto totalBytesOut = gMasterConnSingleton->bytesOut();

	*bin = totalBytesIn;
	*bout = totalBytesOut;

	gMasterConnSingleton->resetStats();

	// Get the stats non dependent on specific connections
	*maxjobscnt = stats_maxjobscnt;
	stats_maxjobscnt = 0;
}

void masterconn_check_hdd_reports() {
	MasterConn *eptr = gMasterConnSingleton.get();
	uint32_t errorcounter;
	if (eptr->mode() == ConnectionMode::CONNECTED) { eptr->checkPullRegistrationStartTimeout(); }
	if (eptr->mode() == ConnectionMode::CONNECTED &&
	    eptr->registrationStatus() == RegistrationStatus::kChunksRegistered) {
		if (hddGetAndResetSpaceChanged()) {
			uint64_t usedspace, totalspace, tdusedspace, tdtotalspace;
			uint32_t chunkcount, tdchunkcount;
			hddGetTotalSpace(&usedspace, &totalspace, &chunkcount, &tdusedspace, &tdtotalspace,
			                 &tdchunkcount);
			eptr->createAttachedNoVersionPacket(CSTOMA_SPACE, usedspace, totalspace, chunkcount,
			                                    tdusedspace, tdtotalspace, tdchunkcount);
		}
		errorcounter = hddGetAndResetErrorCounter();
		while (errorcounter) {
			eptr->createAttachedNoVersionPacket(CSTOMA_ERROR_OCCURRED);
			errorcounter--;
		}

		const auto chunkBulkSize = gChunkBulkSize.load(std::memory_order_relaxed);

		std::vector<ChunkWithType> chunks_with_type;
		hddGetDamagedChunks(chunks_with_type, chunkBulkSize);
		if (!chunks_with_type.empty()) {
			eptr->createAttachedPacket(cstoma::chunkDamaged::build(chunks_with_type));
		}

		hddGetLostChunks(chunks_with_type, chunkBulkSize);
		if (!chunks_with_type.empty()) {
			eptr->createAttachedPacket(cstoma::chunkLost::build(chunks_with_type));
		}

		std::vector<ChunkWithVersionAndType> chunks_with_version;
		hddGetNewChunks(chunks_with_version, chunkBulkSize);
		if (!chunks_with_version.empty()) {
			eptr->createAttachedPacket(cstoma::chunkNew::build(chunks_with_version));
		}
	}
}

void masterconn_unwantedjobfinished(uint8_t status, void *packet) {
	(void)status;
	MasterConn::deletePacket(packet);
}

std::function<void(uint8_t, void *)> masterconn_jobDeleteAfterErrorFinished(
    ChunkWithType chunkWithType) {
	return [chunkWithType](uint8_t status, void *packet) {
		(void)packet;
		// packet should be nullptr

		if (status == SAUNAFS_STATUS_OK &&
		    gMasterConnSingleton->mode() == ConnectionMode::CONNECTED) {
			// Report the chunk as lost to the master server, so it won't be registered again and
			// won't cause any inconsistencies. If the mode is connected, it means that registration
			// with the master server was successful, so we can safely report the chunk as lost. If
			// the mode is not connected, it means that registration with the master server was not
			// successful, so we can skip reporting the chunk as lost, as it won't be registered
			// anyway.
			hddReportLostChunk(chunkWithType.id, chunkWithType.type);
		}
	};
}

std::function<void(uint8_t, void *)> masterconn_unwantedLockJobFinished(
    ChunkWithType chunkWithType, uint32_t listenerId) {
	return [chunkWithType, listenerId](uint8_t status, void *packet) {
		MasterConn::deletePacket(packet);

		if (status == SAUNAFS_STATUS_OK) { return; }

		// If there was an error while writing, which is passed to the callback as status, we want
		// to remove the chunk itself and avoid registering it again with the master server, as it
		// might contain broken data. To do that, we add a delete job to the job pool, which will be
		// processed and will remove the chunk from the chunk server.
		job_delete(*gJobPool, masterconn_jobDeleteAfterErrorFinished(chunkWithType), nullptr,
		           chunkWithType.id, 0, chunkWithType.type, listenerId);
	};
}

MasterJobPool* masterconn_get_job_pool() {
	return gJobPool.get();
}

bool masterconn_canexit() {
	return gMasterConnSingleton->mode() != ConnectionMode::CONNECTED ||
	       (gJobPool->isEmpty() && gReplicationJobPool->isEmpty() &&
	        gMasterConnSingleton->isOutputQueueEmpty());
}

void masterconn_term(void) {
	//  For each connection (currently only one), release its resources.
	MasterConn *eptr = gMasterConnSingleton.get();
	eptr->releaseResources();
	gMasterConnSingleton.reset();

	//  Now reset the last reference to the job pools.
	gReplicationJobPool.reset();
	gJobPool.reset();
}

void masterconn_desc(std::vector<pollfd> &pdesc) {
	LOG_AVG_TILL_END_OF_SCOPE0("master_desc");

	// For each connection to master (currently only one), add its socket to the pollfd array.
	MasterConn *eptr = gMasterConnSingleton.get();

	// Add the descriptor for listening for background jobs finishing.
	gJobFDpDescPos = -1;
	gReplicationJobFDpDescPos = -1;

	if (eptr->mode() == ConnectionMode::CONNECTED) {
		if(gJobFD >= 0) {
			pdesc.emplace_back(gJobFD, POLLIN, 0);
			gJobFDpDescPos = static_cast<int32_t>(pdesc.size() - 1);
		}
		if(gReplicationJobFD >= 0) {
			pdesc.emplace_back(gReplicationJobFD, POLLIN, 0);
			gReplicationJobFDpDescPos = static_cast<int32_t>(pdesc.size() - 1);
		}
	}

	eptr->providePollDescriptors(pdesc, doTerminate());
}

void masterconn_send_status() {
	static uint8_t prev_factor = 0;
	MasterConn *eptr = gMasterConnSingleton.get();

	if (gEnableLoadFactor) {
		uint8_t load_factor = hddGetLoadFactor();
		if (eptr->mode() == ConnectionMode::CONNECTED && load_factor != prev_factor) {
			eptr->createAttachedPacket(cstoma::status::build(load_factor));
			prev_factor = load_factor;
		}
	}
}

void masterconn_serve(const std::vector<pollfd> &pdesc) {
	LOG_AVG_TILL_END_OF_SCOPE0("master_serve");

	MasterConn *eptr = gMasterConnSingleton.get();

	eptr->handlePollErrors(pdesc);

	// Check if there are any background jobs to process.
	if (eptr->mode() == ConnectionMode::CONNECTED) {
		if (gJobFDpDescPos >= 0 && (pdesc[gJobFDpDescPos].revents & POLLIN)) {
			gJobPool->processCompletedJobs();
		}
		if (gReplicationJobFDpDescPos >= 0 && (pdesc[gReplicationJobFDpDescPos].revents & POLLIN)) {
			gReplicationJobPool->processCompletedJobs();
		}
	}

	// For each connection to master (currently only one), process its socket.
	eptr->servePoll(pdesc);

	// Update general statistics
	if (eptr->mode() == ConnectionMode::CONNECTED) {
		uint32_t totalJobCount = 0;
		totalJobCount += (gJobPool->getJobCount() + gReplicationJobPool->getJobCount());
		stats_maxjobscnt = std::max(totalJobCount, stats_maxjobscnt);
	}

	// If the connection is in KILL mode, disable the job pool and close the socket.
	if (eptr->mode() == ConnectionMode::KILL) {
		gJobPool->disableAndChangeCallbackAll(masterconn_unwantedjobfinished);
		gJobPool->changeLockJobsCallback(masterconn_unwantedLockJobFinished);
		gReplicationJobPool->disableAndChangeCallbackAll(masterconn_unwantedjobfinished);
		tcpclose(eptr->socketFD());
		eptr->resetPackets();
		eptr->setMode(ConnectionMode::FREE);
	}
}

void masterconn_reconnect(void) {
	MasterConn *eptr = gMasterConnSingleton.get();
	if (eptr->mode() == ConnectionMode::FREE) {
		eptr->initConnect();
	}
}

static uint32_t get_cfg_timeout() {
	return 1000 * cfg_get_minmaxvalue<double>("MASTER_TIMEOUT", 60, 0.01, 1000 * 1000);
}

/// Reads the master-driven (pull) chunk registration configuration
static void masterconn_load_pull_registration_cfg() {
	gPullRegistrationStartTimeout_s =
	    cfg_get_minvalue<uint32_t>("CHUNK_REGISTRATION_START_TIMEOUT", 10, 1);
	gForcePushRegistration = cfg_getuint32("CHUNK_REGISTRATION_FORCE_PUSH", 0) != 0;
}

/// Read the label from configuration file and return true if it's changed to a valid one
bool masterconn_load_label() {
	std::string oldLabel = gLabel;
	gLabel = cfg_getstring("LABEL", MediaLabelManager::kWildcard);
	if (!MediaLabelManager::isLabelValid(gLabel)) {
		safs::log_warn("invalid label '{}'", gLabel);
		return false;
	}
	return gLabel != oldLabel;
}

void masterconn_reload(void) {
	//  Read the common configuration from file.
	gBindHostStr = cfg_getstring("BIND_HOST", "*");
	gEnableLoadFactor = static_cast<bool>(cfg_getuint32("ENABLE_LOAD_FACTOR", 0));

	uint32_t bip = 0;

	if (tcpresolve(gBindHostStr.c_str(), nullptr, &bip, nullptr, 1) < 0) { bip = 0; }

	// For each connection, reload the configuration and reconnect if needed.
	MasterConn *eptr = gMasterConnSingleton.get();

	if (eptr->isMasterAddressValid() && eptr->mode() != ConnectionMode::FREE) {
		if (eptr->bindHostAddress().ip != bip) {
			eptr->setBindHostAddress(bip, eptr->bindHostAddress().port);
			eptr->setMode(ConnectionMode::KILL);
		}

		eptr->reloadConfig();
	} else {
		eptr->setMasterAddressValid(false);
	}

	gTimeout_ms = get_cfg_timeout();
	masterconn_load_pull_registration_cfg();

	if (masterconn_load_label()) { eptr->sendRegisterLabel(); }

	eptr->sendConfig();

	uint32_t reconnectionDelay = cfg_getuint32("MASTER_RECONNECTION_DELAY", 5);
	eventloop_timechange(gReconnectHook, TIMEMODE_RUN_LATE, reconnectionDelay, 0);
}

int masterconn_init(void) {
	// Read the configuration
	uint32_t reconnectionDelay = cfg_getuint32("MASTER_RECONNECTION_DELAY", 5);
	gMasterHost = cfg_getstring("MASTER_HOST", "sfsmaster");
	gMasterPort = cfg_getstring("MASTER_PORT", "9420");
	std::string clusterId = cfg_getstring("CLUSTER_ID", "default");
	gBindHostStr = cfg_getstring("BIND_HOST", "*");
	gTimeout_ms = get_cfg_timeout();
	gEnableLoadFactor = static_cast<bool>(cfg_getuint32("ENABLE_LOAD_FACTOR", 0));
	masterconn_load_pull_registration_cfg();

	if (!masterconn_load_label()) { return -1; }

	// Create the connections (only one at this point)
	gMasterConnSingleton = std::make_unique<MasterConn>(gMasterHost, gMasterPort, clusterId,
	                                                    gJobPool, gReplicationJobPool);
	MasterConn *eptr = gMasterConnSingleton.get();
	passert(eptr);

	// Init the connections
	if (eptr->initConnect() < 0) { return -1; }

	// Register the callbacks in the event loop
	eventloop_eachloopregister(masterconn_check_hdd_reports);
	eventloop_timeregister(TIMEMODE_RUN_LATE, kSendStatusDelay,
	                       rnd_ranged<uint32_t>(kSendStatusDelay), masterconn_send_status);
	gReconnectHook =
	    eventloop_timeregister(TIMEMODE_RUN_LATE, reconnectionDelay,
	                           rnd_ranged<uint32_t>(reconnectionDelay), masterconn_reconnect);

	eventloop_destructregister(masterconn_term);
	eventloop_pollregister(masterconn_desc, masterconn_serve);
	eventloop_reloadregister(masterconn_reload);

	return 0;
}

int masterconn_init_threads(void) {
	gNumberOfWorkers = cfg_get_minvalue<uint32_t>("MASTER_NR_OF_WORKERS", kDefaultNumberOfWorkers,
	                                              kMinNumberOfWorkers);

	try {
		// Create the JobPool instance with the specified number of workers, it would be serving
		// only this master network thread, thus the number of listeners is 1.
		std::vector<int> bgJobPoolFDs(1);
		gJobPool = std::make_shared<MasterJobPool>("ma", gNumberOfWorkers, kMaxBackgroundJobsCount,
		                                           1, bgJobPoolFDs);
		gJobFD = bgJobPoolFDs[0];
	} catch (const std::exception &e) {
		safs::log_err("masterconn_init_threads: Failed to create JobPool instance: {}", e.what());
		return -1;
	}

	if (gJobPool == nullptr) {
		safs::log_err("masterconn_init_threads: jobPool is null. Unable to create worker threads.");
		return -1;
	}

	safs::log_info("master connection: {} background workers created", gNumberOfWorkers);

	gReplicationNumberOfWorkers = cfg_get_minvalue<uint32_t>("MASTER_REPLICATION_NR_OF_WORKERS",
		kDefaultReplicationNumberOfWorkers, kMinReplicationNumberOfWorkers);

	try {
		// Create the ReplicationJobPool instance with the specified number of workers, it would be
		// serving only this master network thread, thus the number of listeners is 1.
		std::vector<int> replicationJobPoolFDs(1);
		gReplicationJobPool =
		    std::make_shared<MasterJobPool>("ma_repl", gReplicationNumberOfWorkers,
		                                    kMaxBackgroundJobsCount, 1, replicationJobPoolFDs);
		gReplicationJobFD = replicationJobPoolFDs[0];
	} catch (const std::exception &e) {
		safs::log_err("masterconn_init_threads: Failed to create ReplicationJobPool instance: {}",
		              e.what());
		return -1;
	}

	if (gReplicationJobPool == nullptr) {
		safs::log_err(
		    "masterconn_init_threads: replicationJobPool is null. Unable to create "
		    "replication worker threads.");
		return -1;
	}

	safs::log_info("master connection: {} replication background workers created",
	               gReplicationNumberOfWorkers);

	return 0;
}
