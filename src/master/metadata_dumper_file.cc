/*
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
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

#include "common/platform.h"
#include "slogger/slogger.h"

#include "master/metadata_dumper_file.h"

#include <chrono>
#include <cstdio>
#include <sstream>
#include <string>

#include <common/massert.h>
#include <master/filesystem.h>
#include <master/metadata_backend_common.h>
#include <master/metadata_backend_interface.h>

MetadataDumperFile::MetadataDumperFile(const std::string &metadataFilename,
                                       const std::string &metadataTmpFilename)
    : useMetarestore_(false),
      dumpingSucceeded_(true),
      threadInProgress_(false),
      dumpingProcessOutputEmpty_(true),
      metadataFilename_(metadataFilename),
      metadataTmpFilename_(metadataTmpFilename) {}

MetadataDumperFile::~MetadataDumperFile() {
	// Ensure thread is properly cleaned up
	if (dumpingThread_ && dumpingThread_->joinable()) { dumpingThread_->join(); }
}

bool MetadataDumperFile::dumpSucceeded() const { return dumpingSucceeded_; }

bool MetadataDumperFile::inProgress() const { return threadInProgress_; }

bool MetadataDumperFile::useMetarestore() const { return useMetarestore_; }

void MetadataDumperFile::setMetarestorePath(const std::string &path) { metarestorePath_ = path; }

void MetadataDumperFile::setUseMetarestore(bool useMetarestore) {
	useMetarestore_ = useMetarestore;
}

/*
 * Dumping flow:
 * Master creates a thread and waits for "OK" or "ERR" message, to see if the dumping was
 * successful and if the metadata checksums (the one passed from master and the one calculated)
 * match. The thread communicates back through promise/future mechanism.
 *
 * Dump begins in fs_storeall(). There are 3 cases here.
 * `dumpType` is the argument for storeall.
 * 1) dumpType == kForegroundDump: foreground dump.
 *    Master calls executeMetarestore(), which returns false (no thread).
 * 2) dumpType == kBackgroundDump && (!metarestoreSucceeded_ || !useMetarestore_): background
 *    dump when last metarestore failed or when we don't want to use metarestore for dumping
 *    at all.
 *    Master tries to create thread and the thread dumps metadata.
 *    If everything went well, the thread signals "OK" (mocking sfsmetarestore's behaviour),
 *    so that master tries to run metarestore the next time (or he won't - useMetarestore_
 *    isn't changed).
 * 3) dumpType == kBackgroundDump && metarestoreSucceeded_ && useMetarestore_: background dump
 *    when we want to use metarestore and it didn't fail last time.
 *    Master creates thread and executes sfsmetarestore, which checks checksums and returns "OK" or
 * "ERR". In case of a syscall error, last metarestore is assumed to have failed
 *    (metarestoreSucceeded_ = false), so that the master dumps its metadata itself.
 */

bool MetadataDumperFile::start(DumpType &dumpType, uint64_t checksum) {
	if (dumpType == DumpType::kForegroundDump) { return false; }

	// Check if another thread is already running
	if (threadInProgress_.load()) {
		safs::log_err("metadata dumping thread already in progress");
		dumpType = DumpType::kForegroundDump;
		return false;
	}

	/*
	 * Changelog files were rotated before entering this function.
	 * Current changelog is now kChangelogFilename + ".1".
	 */
	std::string changelogFilename = kChangelogFilename;
	changelogFilename += ".1";
	if (useMetarestore_ && dumpingSucceeded_ && (access(changelogFilename.c_str(), F_OK) == -1)) {
		if (errno == ENOENT || errno == EACCES) {
			safs::log_err("no current changelog, dump by master");
		} else {
			safs::log_err("access error, dump by master");
		}
		dumpingSucceeded_ = false;
	}

	// Initialize thread-safe communication
	try {
		dumpingPromise_ = std::promise<bool>();
		dumpingFuture_ = dumpingPromise_.get_future();

		// Reset state
		threadInProgress_ = true;
		dumpingProcessOutputEmpty_ = true;
		lastOutput_.clear();

		safs::log_info("Starting thread for metadata dumping...");

		// Create and start the dumping thread
		dumpingThread_ = std::make_unique<std::thread>(&MetadataDumperFile::executeMetarestore,
		                                               this, checksum, changelogFilename);

		return false;  // Parent continues normally
	} catch (const std::exception &e) {
		safs::log_exception(e, "failed to create dumping thread");
		dumpType = DumpType::kForegroundDump;
		threadInProgress_ = false;
		dumpingSucceeded_ = false;
		return false;
	}
}

// Thread-based polling - checks if thread has completed
void MetadataDumperFile::pollDesc(std::vector<pollfd> & /*pdesc*/) {
	// In thread-based approach, we don't use file descriptors for polling
	// This method is kept for interface compatibility but doesn't add any pollfd
	// The actual status checking is done through atomic variables and futures
}

void MetadataDumperFile::pollServe(const std::vector<pollfd> & /*pdesc*/) {
	// Check if the dumping thread has completed
	if (threadInProgress_.load()) {
		// Check if future is ready (non-blocking)
		if (dumpingFuture_.valid() &&
		    dumpingFuture_.wait_for(std::chrono::duration<int, std::milli>(0)) ==
		        std::future_status::ready) {
			try {
				bool result = dumpingFuture_.get();
				std::lock_guard<std::mutex> lock(stateMutex_);

				if (!lastOutput_.empty()) {
					dumpingProcessOutputEmpty_ = false;
					if (result && lastOutput_ == "OK\n") {
						safs::log_info("periodic metadata dump: success");
					} else {
						safs::log_warn("metadata dumping failed: expected 'OK', received {}",
						               lastOutput_);
					}
				}

				dumpingFinished();
			} catch (const std::exception &e) {
				safs::log_exception(e, "error getting thread result");
				dumpingFinished();
			}
		}
	}
}

void MetadataDumperFile::dumpingFinished() {
	// Clean up thread resources
	if (dumpingThread_ && dumpingThread_->joinable()) { dumpingThread_->join(); }
	dumpingThread_.reset();

	threadInProgress_.store(false);

	std::lock_guard<std::mutex> lock(stateMutex_);
	if (dumpingProcessOutputEmpty_.load()) {
		safs::log_warn("the dumping thread finished without producing output");
	}
}

void MetadataDumperFile::waitUntilFinished(SteadyDuration timeout) {
	if (!threadInProgress_.load()) {
		return;  // Nothing to wait for
	}

	try {
		// Wait for the thread to complete with timeout
		if (dumpingFuture_.valid()) {
			std::future_status status = dumpingFuture_.wait_for(timeout);

			if (status == std::future_status::ready) {
				// Thread completed normally
				bool result = dumpingFuture_.get();
				safs::log_info("Metadata dumping completed with result: {}",
				               result ? "success" : "failure");
				dumpingFinished();
			} else if (status == std::future_status::timeout) {
				// Timeout occurred
				safs::log_err(
				    "dumping didn't finish in specified timeout: {}",
				    std::chrono::duration_cast<std::chrono::duration<double, std::ratio<1>>>(
				        timeout)
				        .count());
				dumpingFinished();
			}
		} else {
			// Use condition variable as fallback
			std::unique_lock<std::mutex> lock(stateMutex_);
			if (!dumpingCompleted_.wait_for(lock, timeout,
			                                [this] { return !threadInProgress_.load(); })) {
				safs::log_err(
				    "dumping didn't finish in specified timeout: {}",
				    std::chrono::duration_cast<std::chrono::duration<double, std::ratio<1>>>(
				        timeout)
				        .count());
			}
			dumpingFinished();
		}
	} catch (const std::exception &e) {
		safs::log_exception(e, "error waiting for dumping thread");
		dumpingFinished();
	}
}

void MetadataDumperFile::waitUntilFinished() {
	// Use a reasonable timeout instead of a year
	waitUntilFinished(std::chrono::duration_cast<SteadyDuration>(std::chrono::hours(1)));
}

void MetadataDumperFile::executeMetarestore(uint64_t checksum,
                                            const std::string &changelogFilename) {
	bool success = false;
	std::string output;

	try {
		std::ostringstream tidStream;
		tidStream << std::this_thread::get_id();
		safs::log_info("Thread started for metadata dumping (tid: {})", tidStream.str());

		if (useMetarestore_ && dumpingSucceeded_.load()) {
			// Build command string for metarestore execution
			std::ostringstream cmd;
			cmd << metarestorePath_ << " -m " << metadataFilename_ << " -o " << metadataTmpFilename_
			    << " -k " << checksum << " -B " << gStoredPreviousBackMetaCopies << " -# "
			    << changelogFilename;

			std::string command = cmd.str();
			safs::log_info("Executing metarestore command: {}", command);

			// Execute command and capture output using popen
			FILE *pipe = popen(command.c_str(), "r");
			if (!pipe) {
				safs::log_err("popen failed for metarestore command");
				success = false;
			} else {
				// Read output from command
				char buffer[1024];
				std::ostringstream result;
				while (fgets(buffer, sizeof(buffer), pipe) != nullptr) { result << buffer; }

				int exitCode = pclose(pipe);
				output = result.str();

				// Check if command succeeded and produced expected output
				if (exitCode == 0 && output == "OK\n") {
					success = true;
					safs::log_info("metarestore command completed successfully");
				} else {
					success = false;
					safs::log_warn("metarestore command failed: exit code {}, output: {}", exitCode,
					               output);
				}
			}
		} else {
			if (useMetarestore_ && !dumpingSucceeded_.load()) {
				safs::log_warn("something previously failed, dump by master");
			}
			// Fallback: signal that master should dump in foreground
			success = false;
		}

		// Store results
		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			lastOutput_ = output;
			dumpingProcessOutputEmpty_ = output.empty();
		}

	} catch (const std::exception &e) {
		safs::log_err("exception in metarestore thread: {}", e.what());
		success = false;
	} catch (...) {
		safs::log_err("unknown exception in metarestore thread");
		success = false;
	}

	// Set final results and notify completion
	dumpingSucceeded_.store(success);
	dumpingPromise_.set_value(success);

	// Mark thread as finished
	threadInProgress_.store(false);
	dumpingCompleted_.notify_all();

	safs::log_info("Metadata dumping thread finished with result: {}",
	               success ? "success" : "failure");
}
