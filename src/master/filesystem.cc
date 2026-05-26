/*
   Copyright 2005-2017 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2017 Skytechnology sp. z o.o.
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

#include "master/filesystem.h"

#include <fstream>
#include <memory>

#include "common/event_loop.h"
#include "common/lockfile.h"
#include "common/main.h"
#include "common/scoped_timer.h"
#include "config/cfg.h"
#include "errors/saunafs_error_codes.h"
#include "master/changelog.h"
#include "master/chunk_operations_in_memory.h"
#include "master/chunks.h"
#include "master/datacachemgr.h"
#include "master/deferred_metadata_dump_task.h"
#include "master/filesystem_checksum_updater.h"
#include "master/filesystem_metadata.h"
#include "master/filesystem_node.h"
#include "master/filesystem_operations.h"
#include "master/filesystem_operations_interface.h"
#include "master/filesystem_periodic.h"
#include "master/filesystem_snapshot.h"
#include "master/goal_config_loader.h"
#include "master/id_generator_incremental.h"
#include "master/matoclserv_sessions.h"
#include "master/matocsserv.h"
#include "master/metadata_backend_common.h"
#include "master/metadata_backend_interface.h"
#include "master/restore.h"
#include "slogger/slogger.h"

#ifdef METARESTORE
#include "master/filesystem_freenode.h"
#include "master/metadata_backend_file.h"
#endif

FilesystemMetadata* gMetadata = nullptr;
std::unique_ptr<Lockfile> gMetadataLockfile;

#ifndef METARESTORE

static bool gAutoRecovery = false;
bool gMagicAutoFileRepair = false;
bool gAtimeDisabled = false;

bool gDisableEmptyFoldersMetadataOnFullDisk = false;

uint32_t gTestStartTime;

static bool gSaveMetadataAtExit = true;
static uint32_t gOperationsDelayInit;
static uint32_t gOperationsDelayDisconnect;

// Configuration of goals
std::map<int, Goal> gGoalDefinitions;

#endif // ifndef METARESTORE

// Checksum validation
bool gDisableChecksumVerification = false;

ChecksumBackgroundUpdater gChecksumBackgroundUpdater;

#ifdef METARESTORE

void fs_disable_checksum_verification(bool value) {
	gDisableChecksumVerification = value;
}

#endif

void fs_unlock() {
	gMetadataLockfile->unlock();
}

#ifndef METARESTORE

static void metadataPollDesc(std::vector<pollfd> &pdesc) {
	gMetadataBackend->dumper()->pollDesc(pdesc);
}

static void metadataPollServe(const std::vector<pollfd> &pdesc) {
	auto *dumper = gMetadataBackend->dumper();

	bool metadataDumpInProgress = dumper->inProgress();
	dumper->pollServe(pdesc);

	if (metadataDumpInProgress && !dumper->inProgress()) {
		if (dumper->dumpSucceeded()) {
			if (gMetadataBackend->commit_metadata_dump()) {
				gMetadataBackend->broadcast_metadata_saved(SAUNAFS_STATUS_OK);
			} else {
				gMetadataBackend->broadcast_metadata_saved(SAUNAFS_ERROR_IO);
			}
		} else {
			gMetadataBackend->broadcast_metadata_saved(SAUNAFS_ERROR_IO);
			if (dumper->useMetarestore()) {
				// master should recalculate its checksum
				safs_pretty_syslog(LOG_WARNING, "dumping metadata failed, recalculating checksum");
				gFSOperations->startChecksumRecalculation();
			}
			unlink(kMetadataTmpFilename);
		}
	}
}

void fs_periodic_storeall() {
	// Prevent metadata dump while chunks registration is in progress to prevent slowing down
	// the chunks registration process
	auto isChunkRegistrationInProgress = !gTimeoutSinceLastChunkRegistration.expired();
	if (isChunkRegistrationInProgress) {
		safs::log_info(
		    "periodic metadata dump was skipped while chunks registration is in progress");
		return;
	}

	gMetadataBackend->fs_storeall(DumpType::kBackgroundDump);  // ignore error
}

void fs_term(void) {
	auto *dumper = gMetadataBackend->dumper();

	if (dumper->inProgress()) {
		dumper->waitUntilFinished();
	}
	bool metadataStored = false;
	if (gMetadata != nullptr && gSaveMetadataAtExit) {
		for (;;) {
			metadataStored =
			    (gMetadataBackend->fs_storeall(DumpType::kForegroundDump) == SAUNAFS_STATUS_OK);
			if (metadataStored) {
				break;
			}
			safs_pretty_syslog(LOG_ERR,"can't store metadata - try to make more space on your hdd or change privieleges - retrying after 10 seconds");
			sleep(10);
		}
	}
	if (metadataStored) {
		// Remove the lock to say that the server has gently stopped and saved its metadata.
		fs_unlock();
	} else if (gMetadata != nullptr && !gSaveMetadataAtExit) {
		// We will leave the lockfile present to indicate, that our metadata.sfs file should not be
		// loaded (it is not up to date -- some changelogs need to be applied). Write a message
		// which tells that the lockfile is not left because of a crash, but because we have been
		// asked to stop without saving metadata. Include information about version of metadata
		// which can be recovered using our changelogs.
		auto message = "quick_stop: " + std::to_string(gMetadata->metadataVersion) + "\n";
		gMetadataLockfile->writeMessage(message);
	} else {
		// We will leave the lockfile present to indicate, that our metadata.sfs file should not be
		// loaded (it is not up to date, because we didn't manage to download the most recent).
		// Write a message which tells that the lockfile is not left because of a crash, but because
		// we have been asked to stop before loading metadata. Don't overwrite 'quick_stop' though!
		if (!gMetadataLockfile->hasMessage()) {
			gMetadataLockfile->writeMessage("no_metadata: 0\n");
		}
	}
	delete gMetadata;
}

void fs_disable_metadata_dump_on_exit() {
	gSaveMetadataAtExit = false;
}

#else  // #ifndef METARESTORE
void fs_storeall(const char *fname) {
	FILE *fd;
	fd = fopen(fname,"w");
	if (fd==NULL) {
		safs_pretty_syslog(LOG_ERR, "can't open metadata file");
		return;
	}
	gMetadataBackend->store_fd(fd);

	if (ferror(fd)!=0) {
		safs_pretty_syslog(LOG_ERR, "can't write metadata");
	} else if (fflush(fd) == EOF) {
		safs_pretty_syslog(LOG_ERR, "can't fflush metadata");
	} else if (fsync(fileno(fd)) == -1) {
		safs_pretty_syslog(LOG_ERR, "can't fsync metadata");
	}
	fclose(fd);
}

void fs_term(const char *fname, bool noLock) {
	if (!noLock) {
		gMetadataLockfile->eraseMessage();
	}
	fs_storeall(fname);
	if (!noLock) {
		fs_unlock();
	}
}
#endif

void fs_strinit(bool isFromInit) {
	if (isFromInit) {
		if (gMetadata == nullptr) { gMetadata = new FilesystemMetadata; }
	} else {
		// Could be called from masterconn in Shadow mode
		gMetadata = new FilesystemMetadata;
	}
}

static void ensureChunkIdGenerator() {
	if (!gChunkIdGenerator) {
		gChunkIdGenerator = std::make_unique<IdGeneratorIncremental<uint64_t>>();
	}
}

static void initFSOperations() {
	if (!gFSOperations) {
		auto nodeOps = std::make_unique<FilesystemNodeOperationsBase>();
		gFSOperations = std::make_unique<FilesystemOperationsBase>(std::move(nodeOps));
	}
	// In-memory chunk operations for leil-master. The MDS binds its KV variant
	// earlier (metadata_backend_init), so this guarded assignment is a no-op there.
	if (!gChunkOperations) { gChunkOperations = std::make_unique<ChunkOperationsInMemory>(); }
}

/* executed in master mode */
#ifndef METARESTORE

/// Returns true iff we are allowed to swallow a stale lockfile and apply changelogs.
static bool fs_can_do_auto_recovery() {
	return gAutoRecovery || main_has_extra_argument("auto-recovery", CaseSensitivity::kIgnore);
}

void fs_erase_message_from_lockfile() {
	if (gMetadataLockfile != nullptr) {
		gMetadataLockfile->eraseMessage();
	}
}

/// @brief Executes a metadata dump operation, either immediately or deferred.
///
/// This function checks for the "defer-metadata-dump" option. If present, it schedules
/// a deferred metadata dump task using the TaskManager, logging the outcome upon completion.
/// Otherwise, it performs an immediate metadata dump after applying changelogs.
///
/// The deferred dump uses the global metadata backend and submits a one-time task.
/// The immediate dump invokes the metadata backend's fs_storeall() function with a foreground dump
/// type.
///
/// Logging is performed to indicate the success or failure of the operation.
void executeMetadataDump() {
	bool deferDump = main_has_extra_argument("defer-metadata-dump", CaseSensitivity::kIgnore);

	if (deferDump) {
		safs::log_info("Deferring metadata dump option detected");

		auto *metadataBackendPtr = gMetadataBackend.get();
		auto metadataDumpTask = std::make_unique<DeferredMetadataDumpTask>(metadataBackendPtr);

		// Schedule one-time deferred metadata dump task using TaskManager
		gMetadata->taskManager.submitTask(
		    0, 1, metadataDumpTask.release(), DeferredMetadataDumpTask::generateDescription(),
		    [](int status) {
			    if (status == SAUNAFS_STATUS_OK) {
				    safs::log_info("Deferred metadata dump completed successfully");
			    } else {
				    safs::log_err("Deferred metadata dump failed with status: {}", status);
			    }
		    });
	} else {
		// Original behavior: dump the new metadata immediately
		gMetadataBackend->fs_storeall(DumpType::kForegroundDump);
		safs::log_info("Metadata dumped successfully after applying changelogs");
	}
}

int fs_loadall(bool isFromInit = true) {
	fs_strinit(isFromInit);

	ensureChunkIdGenerator();
	gChunkOperations->strinit();

	gChunkIdGenerator->initialize();
	gInodeIdGenerator->initialize();
	if (gSessionIdGenerator) { gSessionIdGenerator->initialize(); }

	{
		auto scopedTimer = util::ScopedTimer("metadata load time");
		gMetadataBackend->loadall(0);
	}

	bool autoRecovery = fs_can_do_auto_recovery();

	if (autoRecovery || (metadataserver::getPersonality() == metadataserver::Personality::kShadow)) {
		safs::log_info("{} - applying changelogs from {}",
		               (autoRecovery ? "AUTO_RECOVERY enabled" : "running in shadow mode"),
		               fs::getCurrentWorkingDirectoryNoThrow().c_str());

		// Save the current personality
		metadataserver::Personality personality = metadataserver::getPersonality();
		// Force shadow personality to avoid permission issues and possible changes broadcasting
		metadataserver::setPersonality(metadataserver::Personality::kShadow);

		// Load the changelogs
		load_changelogs();
		safs::log_info("all needed changelogs applied successfully");

		// Dump the new metadata
		executeMetadataDump();

		// Restore the original personality
		metadataserver::setPersonality(personality);
	}

	return 0;
}

void fs_cs_disconnected(void) {
	gTestStartTime = eventloop_time() + gOperationsDelayDisconnect;
}

/*
 * Initialize subsystems required by Master personality of metadataserver.
 */
void fs_become_master() {
	if (!gMetadata) {
		safs_pretty_syslog(LOG_ERR, "Attempted shadow->master transition without metadata - aborting");
		exit(1);
	}
	dcm_clear();
	gTestStartTime = eventloop_time() + gOperationsDelayInit;
	fs_periodic_master_init();
	return;
}

static void fs_read_goals_from_stream(std::istream& stream) {
	auto goals = goal_config::load(stream);
	std::swap(gGoalDefinitions, goals);
}

static void fs_read_goals_from_stream(std::istream&& stream) {
	fs_read_goals_from_stream(stream);
}

static void fs_read_goal_config_file() {
	std::string goalConfigFile =
			cfg_getstring("CUSTOM_GOALS_FILENAME", "");
	if (goalConfigFile.empty()) {
		// file is not specified
		const char *defaultGoalConfigFile = ETC_PATH "/leil-goals.cfg";
		const char *legacyGoalConfigFile = ETC_PATH "/sfsgoals.cfg";
		if (access(defaultGoalConfigFile, F_OK) == 0) {
			// the default file exists - use it
			goalConfigFile = defaultGoalConfigFile;
		} else if (access(legacyGoalConfigFile, F_OK) == 0) {
			goalConfigFile = legacyGoalConfigFile;
		} else {
			safs_pretty_syslog(LOG_WARNING,
					"goal configuration files %s and %s not found - using default goals; if you "
					"don't want to define custom goals create an empty file %s to disable this "
					"warning",
					defaultGoalConfigFile, legacyGoalConfigFile, defaultGoalConfigFile);
			fs_read_goals_from_stream(std::stringstream()); // empty means defaults
			return;
		}
	} else {
		const std::string defaultGoalConfigFile = ETC_PATH "/leil-goals.cfg";
		const std::string legacyGoalConfigFile = ETC_PATH "/sfsgoals.cfg";
		if (goalConfigFile == defaultGoalConfigFile && access(defaultGoalConfigFile.c_str(), F_OK) != 0 &&
		    access(legacyGoalConfigFile.c_str(), F_OK) == 0) {
			goalConfigFile = legacyGoalConfigFile;
		}
	}
	std::ifstream goalConfigStream(goalConfigFile);
	if (!goalConfigStream.good()) {
		throw ConfigurationException("failed to open goal definitions file " + goalConfigFile);
	}
	try {
		fs_read_goals_from_stream(goalConfigStream);
		safs_pretty_syslog(LOG_INFO,
				"initialized goal definitions from file %s",
				goalConfigFile.c_str());
	} catch (Exception& ex) {
		throw ConfigurationException(
				"malformed goal definitions in " + goalConfigFile + ": " + ex.message());
	}
}

static void fs_read_config_file() {
	gClusterId = cfg_getstring("CLUSTER_ID", "default");
	gAutoRecovery = cfg_getint32("AUTO_RECOVERY", 0) == 1;
	gDisableChecksumVerification = cfg_getint32("DISABLE_METADATA_CHECKSUM_VERIFICATION", 0) != 0;
	gMagicAutoFileRepair = cfg_getint32("MAGIC_AUTO_FILE_REPAIR", 0) == 1;
	gAtimeDisabled = cfg_getint32("NO_ATIME", 0) == 1;
	gStoredPreviousBackMetaCopies = cfg_get_maxvalue(
			"BACK_META_KEEP_PREVIOUS",
			kDefaultStoredPreviousBackMetaCopies,
			kMaxStoredPreviousBackMetaCopies);

	ChecksumUpdater::setPeriod(cfg_getint32("METADATA_CHECKSUM_INTERVAL", 50));
	gChecksumBackgroundUpdater.setSpeedLimit(
			cfg_getint32("METADATA_CHECKSUM_RECALCULATION_SPEED", 100));
	auto *dumper = gMetadataBackend->dumper();
	dumper->setMetarestorePath(cfg_get(
	    "SFSMETARESTORE_PATH", std::string(SBIN_PATH "/sfsmetarestore")));
	dumper->setUseMetarestore(cfg_getint32("MAGIC_PREFER_BACKGROUND_DUMP", 0));

	// Set deprecated values first, then override them if newer version is found
	gOperationsDelayInit = cfg_getuint32("REPLICATIONS_DELAY_INIT", 300);
	gOperationsDelayDisconnect = cfg_getuint32("REPLICATIONS_DELAY_DISCONNECT", 3600);
	gOperationsDelayInit = cfg_getuint32("OPERATIONS_DELAY_INIT", gOperationsDelayInit);
	gOperationsDelayDisconnect = cfg_getuint32("OPERATIONS_DELAY_DISCONNECT", gOperationsDelayDisconnect);
	if (cfg_isdefined("REPLICATIONS_DELAY_INIT") || cfg_isdefined("REPLICATIONS_DELAY_DISCONNECT")) {
		safs_pretty_syslog(LOG_WARNING, "REPLICATIONS_DELAY_INIT and REPLICATION_DELAY_DISCONNECT"
		" entries are deprecated. Use OPERATIONS_DELAY_INIT and OPERATIONS_DELAY_DISCONNECT instead.");
	}
	gEmptyReservedFilesPeriod = cfg_getuint32("EMPTY_RESERVED_FILES_PERIOD_MSECONDS", 0);

	gDisableEmptyFoldersMetadataOnFullDisk =
	    cfg_getint32("CREATE_EMPTY_FOLDERS_WHEN_SPACE_DEPLETED", 1) == 0;
	if (gDisableEmptyFoldersMetadataOnFullDisk) {
		safs::log_info(
		    "CREATE_EMPTY_FOLDERS_WHEN_SPACE_DEPLETED option is disabled. "
		    "Empty folders will not be created when space is depleted.");
	}

	gChunkOperations->invalidateGoalCache();
	fs_read_goal_config_file(); // may throw
	fs_read_snapshot_config_file();
	fs_read_periodic_config_file();
}

void fs_reload(void) {
	try {
		fs_read_config_file();
	} catch (Exception& ex) {
		safs_pretty_syslog(LOG_WARNING, "Error in configuration: %s", ex.what());
	}
}

void fs_unload() {
	safs_pretty_syslog(LOG_WARNING, "unloading filesystem at %" PRIu64,
	                   gFSOperations->getMetadataVersion());
	restore_reset();
	matoclserv_session_unload();
	gChunkOperations->unload();
	dcm_clear();
	delete gMetadata;
	gMetadata = nullptr;
}

int fs_init(bool doLoad) {
	// Initialize the concrete filesystem operations before any FS call
	initFSOperations();

	try {
		fs_read_config_file();
	} catch (Exception &ex) {
		safs::log_err("Error in configuration: {}", ex.what());
		throw;
	}

	if (!gMetadataLockfile) {
		gMetadataLockfile = std::make_unique<Lockfile>(kMetadataFilename + std::string(".lock"));
	}

	if (!gMetadataLockfile->isLocked()) {
		try {
			gMetadataLockfile->lock((fs_can_do_auto_recovery() || !metadataserver::isMaster())
			                            ? Lockfile::StaleLock::kSwallow
			                            : Lockfile::StaleLock::kReject);
		} catch (const LockfileException &e) {
			if (e.reason() == LockfileException::Reason::kStaleLock) {
				throw LockfileException(
				    std::string(e.what()) +
				        ", consider running `sfsmetarestore -a' to fix problems with your datadir.",
				    LockfileException::Reason::kStaleLock);
			}
			throw;
		}
	}

	changelog_init(kChangelogFilename, 0, 50);

	if (doLoad || (metadataserver::isMaster())) {
		fs_loadall(true);
	}

	eventloop_reloadregister(fs_reload);
	metadataserver::registerFunctionCalledOnPromotion(fs_become_master);
	auto metadataDumpPeriod = cfg_getint32("METADATA_DUMP_PERIOD_SECONDS", 3600);

	if (metadataDumpPeriod > 0) {  /// 0 means disabled periodic metadata dumps
		eventloop_timeregister(TIMEMODE_RUN_LATE, metadataDumpPeriod, 0,
		                       fs_periodic_storeall);
	}

	if (metadataserver::isMaster()) {
		fs_become_master();
	}

	eventloop_pollregister(metadataPollDesc, metadataPollServe);
	eventloop_destructregister(fs_term);

	return 0;
}

/*
 * Initialize filesystem subsystem if currently metadataserver have Master personality.
 */
int fs_init() {
	return fs_init(false);
}

#else   // METARESTORE mode
int fs_init(const char *fname, int ignoreflag, bool noLock) {
	gMetadataBackend = std::make_unique<MetadataBackendFile>();
	dynamic_cast<MetadataBackendFile *>(gMetadataBackend.get())->setMetadataFile(fname);

	// Initialize the concrete filesystem operations before any FS call
	initFSOperations();

	if (!noLock) {
		gMetadataLockfile.reset(new Lockfile(fs::dirname(fname) + "/" + kMetadataFilename + ".lock"));
		gMetadataLockfile->lock(Lockfile::StaleLock::kSwallow);
	}

	fs_strinit(true);
	ensureChunkIdGenerator();
	gChunkOperations->strinit();
	gInodeIdGenerator = std::make_unique<IdGeneratorWithDetainer>();
	gMetadataBackend->loadall(ignoreflag);
	return 0;
}
#endif  // #ifndef METARESTORE
