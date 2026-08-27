/*
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

#include "chunkserver/network_worker_thread.h"

#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <syslog.h>
#include <unistd.h>
#include <atomic>
#include <cstdint>
#include <mutex>

#include "chunkserver/bgjobs.h"
#include "chunkserver/hddspacemgr.h"
#include "chunkserver/network_stats.h"
#include "common/event_loop.h"
#include "common/massert.h"
#include "slogger/slogger.h"
#include "common/sockets.h"
#include "devtools/request_log.h"
#include "devtools/TracePrinter.h"

// connection timeout in seconds
constexpr uint32_t kCSServTimeout = 10;
// forceful termination timeout in milliseconds
constexpr uint32_t kNWForcefulTerminationTimeout_ms = 30000;

constexpr int kTimeoutOdd = 300000;
constexpr int kTimeoutEven = 200000;

constexpr int getConnectTimeout(int cnt) {
	return cnt % 2 ? kTimeoutOdd * (1 << (cnt >> 1))
	               : kTimeoutEven * (1 << (cnt >> 1));
}

NetworkWorkerThread::NetworkWorkerThread(uint32_t id, uint32_t nrOfBgjobsWorkers,
                                         uint32_t bgjobsCount)
    : name_("nw_" + std::to_string(id)), doTerminate(false) {
	TRACETHIS();
	eassert(pipe(notify_pipe) != -1);
#ifdef F_SETPIPE_SZ
	// Increase the pipe size to 128 KiB to handle a larger number of jobs
	// without backpressure. On modern linux, the default pipe size is 64 KiB.
	static constexpr int kPageAlignedPipeSize = 4096 * 32;
	eassert(fcntl(notify_pipe[1], F_SETPIPE_SZ, kPageAlignedPipeSize));
#endif
	try {
		// Create the JobPool instance with the specified number of workers. It would be serving
		// only this network worker thread, thus the number of listeners is 1.
		std::vector<int> bgJobPoolWakeUpFds(1);
		bgJobPool_ = std::make_unique<ClientJobPool>(name_, nrOfBgjobsWorkers, bgjobsCount, 1,
		                                             bgJobPoolWakeUpFds, gIOPriorityMode);
		bgJobPoolWakeUpFd_ = bgJobPoolWakeUpFds[0];
	} catch (const std::exception &e) {
		safs::log_err("NetworkWorkerThread: Failed to create JobPool instance: {}", e.what());
		throw;
	}
}

void NetworkWorkerThread::operator()() {
	TRACETHIS();

	static std::atomic_uint16_t threadCounter(0);
	std::string threadName = "netWorker_" + std::to_string(threadCounter++);
	pthread_setname_np(pthread_self(), threadName.c_str());
	bool lastDoTerminateValue = false;

	while (!canTerminate_.load()) {
		if (doTerminate.load() && !lastDoTerminateValue) {
			// We've just switched to terminating mode, start wrapping up.
			lastDoTerminateValue = true;
			std::lock_guard lock(csservheadLock);
			for (auto &entry : csservEntries) { entry.closeJobs(); }
		}

		preparePollFds(doTerminate.load());
		int fdWithEvents = poll(pdesc.data(), pdesc.size(), gPollTimeout);

		if (fdWithEvents < 0) {
			if (errno == EAGAIN) {
				safs::log_warn("{} loop: poll returned EAGAIN", threadName);
				usleep(100000);
				continue;
			}

			if (errno != EINTR) {
				safs::log_warn("{} loop: poll error: {}", threadName, strerr(errno));
				break;
			}
		} else {
			if ((pdesc[0].revents) & POLLIN) {
				uint8_t notifyByte;
				eassert(read(pdesc[0].fd, &notifyByte, 1) == 1);
			}
		}

		servePoll();
	}
	this->terminate();
}

bool NetworkWorkerThread::updateAndCheckTerminationStatus() {
	// Don't even check the rest if we already know we can terminate.
	if (canTerminate_.load()) {
		return true;
	}

	std::lock_guard lock(csservheadLock);
	bool canTerminate =
	    doTerminate.load() && ((csservEntries.empty() &&
	                            (bgJobPool_.get() == nullptr || bgJobPool_->allJobsProcessed())) ||
	                           terminationTimer_.elapsed_ms() > kNWForcefulTerminationTimeout_ms);
	canTerminate_.store(canTerminate);
	return canTerminate;
}

void NetworkWorkerThread::terminate() {
	TRACETHIS();
	bgJobPool_.reset();

	std::unique_lock lock(csservheadLock);

	while (!csservEntries.empty()) {
		auto& entry = csservEntries.back();

		if (entry.isChunkOpen()) {
			entry.forceCloseOpenChunks();
		}

		csservEntries.pop_back(); // Should call the entry destructor
	}
}

void NetworkWorkerThread::preparePollFds(bool isTerminating) {
	LOG_AVG_TILL_END_OF_SCOPE0("preparePollFds");
	TRACETHIS();
	pdesc.clear();
	pdesc.emplace_back(pollfd(notify_pipe[0], POLLIN, 0));
	pdesc.emplace_back(pollfd(bgJobPoolWakeUpFd_, POLLIN, 0));
	sassert(JOB_FD_PDESC_POS == (pdesc.size() - 1));

	std::unique_lock lock(csservheadLock);
	for (auto& entry : csservEntries) {
		entry.pDescPos = -1;
		entry.fwdPDescPos = -1;
		switch (entry.state) {
			case ChunkserverEntry::State::Idle:
			case ChunkserverEntry::State::Read:
			case ChunkserverEntry::State::GetBlock:
			case ChunkserverEntry::State::WriteLast:
				pdesc.emplace_back(pollfd(entry.sock, 0, 0));
				entry.pDescPos = pdesc.size() - 1;
				if (entry.inputPacket.bytesLeft > 0 && !isTerminating) {
					pdesc.back().events |= POLLIN;
				}
				if (!entry.outputPackets.empty()) {
					pdesc.back().events |= POLLOUT;
				}
				break;
			case ChunkserverEntry::State::Connecting:
				pdesc.emplace_back(pollfd(entry.fwdSocket, POLLOUT, 0));
				entry.fwdPDescPos = pdesc.size() - 1;
				break;
			case ChunkserverEntry::State::WriteInit:
				if (entry.fwdOutputPacket.bytesLeft > 0) {
					pdesc.emplace_back(pollfd(entry.fwdSocket, POLLOUT, 0));
					entry.fwdPDescPos = pdesc.size() - 1;
				}
				break;
			case ChunkserverEntry::State::WriteForward:
				pdesc.emplace_back(pollfd(entry.fwdSocket, POLLIN, 0));
				entry.fwdPDescPos = pdesc.size() - 1;
				if (entry.fwdOutputPacket.bytesLeft > 0 && !isTerminating) {
					pdesc.back().events |= POLLOUT;
				}

				pdesc.emplace_back(pollfd(entry.sock, 0, 0));
				entry.pDescPos = pdesc.size() - 1;
				if (entry.inputPacket.bytesLeft > 0 && !isTerminating) {
					pdesc.back().events |= POLLIN;
				}
				if (!entry.outputPackets.empty()) {
					pdesc.back().events |= POLLOUT;
				}
				break;
			case ChunkserverEntry::State::IOFinish:
				if (!entry.outputPackets.empty()) {
					pdesc.emplace_back(pollfd(entry.sock, POLLOUT, 0));
					entry.pDescPos = pdesc.size() - 1;
				}
				break;
			default:
				break;
		}
	}
}

void NetworkWorkerThread::servePoll() {
	LOG_AVG_TILL_END_OF_SCOPE0("servePoll");
	TRACETHIS();
	uint32_t now = eventloop_time();
	uint64_t usecnow = eventloop_utime();
	uint32_t jobscnt;
	ChunkserverEntry::State lstate;

	// Cancel before completions are applied, not after. Applying a completed job first lets a
	// result produced under a claim that has since lapsed take effect on this iteration, and
	// cancelling afterwards is then a decision about work that already landed.
	{
		std::unique_lock cancelLock(csservheadLock);
		for (auto &entry : csservEntries) { entry.cancelJobsAfterCutoff(); }
	}

	if (pdesc[JOB_FD_PDESC_POS].revents & POLLIN) {
		bgJobPool_->processCompletedJobs();
	}
	std::unique_lock lock(csservheadLock);
	for (auto& entry : csservEntries) {
		ChunkserverEntry* eptr = &entry;

		eptr->everyLoopUpdateWrite();

		if (entry.pDescPos >= 0
				&& (pdesc[entry.pDescPos].revents & (POLLERR | POLLHUP))) {
			entry.state = ChunkserverEntry::State::Close;
		} else if (entry.fwdPDescPos >= 0
				&& (pdesc[entry.fwdPDescPos].revents & (POLLERR | POLLHUP))) {
			eptr->fwdError();
		}
		lstate = entry.state;
		if (lstate == ChunkserverEntry::State::Idle ||
		    lstate == ChunkserverEntry::State::Read ||
		    lstate == ChunkserverEntry::State::WriteLast ||
		    lstate == ChunkserverEntry::State::IOFinish ||
		    lstate == ChunkserverEntry::State::GetBlock) {
			if (entry.pDescPos >= 0 &&
			    (pdesc[entry.pDescPos].revents & POLLIN)) {
				entry.lastActivity = now;
				eptr->readFromSocket();
			}
			if (entry.pDescPos >= 0 &&
			    (pdesc[entry.pDescPos].revents & POLLOUT) &&
			    entry.state == lstate) {
				entry.lastActivity = now;
				eptr->writeToSocket();
			}
		} else if (lstate == ChunkserverEntry::State::Connecting &&
		           entry.fwdPDescPos >= 0 &&
		           (pdesc[entry.fwdPDescPos].revents &
		            POLLOUT)) {  // FD_ISSET(entry.fwdsock,wset)) {
			entry.lastActivity = now;
			eptr->fwdConnected();
			if (entry.state == ChunkserverEntry::State::WriteInit) {
				eptr->fwdWrite(); // after connect likely some data can be send
			}
			if (entry.state == ChunkserverEntry::State::WriteForward) {
				eptr->forward(); // and also some data can be forwarded
			}
		} else if (entry.state == ChunkserverEntry::State::WriteInit &&
		           entry.fwdPDescPos >= 0 &&
		           (pdesc[entry.fwdPDescPos].revents &
		            POLLOUT)) {  // FD_ISSET(entry.fwdsock,wset)) {
			entry.lastActivity = now;
			eptr->fwdWrite(); // after sending init packet
			if (entry.state == ChunkserverEntry::State::WriteForward) {
				eptr->forward(); // likely some data can be forwarded
			}
		} else if (entry.state == ChunkserverEntry::State::WriteForward) {
			if ((entry.pDescPos >= 0 &&
			     (pdesc[entry.pDescPos].revents & POLLIN)) ||
			    (entry.fwdPDescPos >= 0 &&
			     (pdesc[entry.fwdPDescPos].revents & POLLOUT))) {
				entry.lastActivity = now;
				eptr->forward();
			}
			if (entry.fwdPDescPos >= 0 &&
			    (pdesc[entry.fwdPDescPos].revents & POLLIN) &&
			    entry.state == lstate) {
				entry.lastActivity = now;
				eptr->fwdRead();
			}
			if (entry.pDescPos >= 0 &&
			    (pdesc[entry.pDescPos].revents & POLLOUT) &&
			    entry.state == lstate) {
				entry.lastActivity = now;
				eptr->writeToSocket();
			}
		}
		if (entry.state == ChunkserverEntry::State::IOFinish &&
		    entry.outputPackets.empty()) {
			entry.state = ChunkserverEntry::State::Close;
		}
		if (entry.state == ChunkserverEntry::State::Connecting &&
		    entry.connectStartTimeUSec +
		            getConnectTimeout(entry.connectRetryCounter) <
		        usecnow) {
			eptr->retryConnect();
		}
		if (entry.state != ChunkserverEntry::State::Close &&
		    entry.state != ChunkserverEntry::State::CloseWait &&
		    entry.state != ChunkserverEntry::State::Closed &&
		    entry.lastActivity + kCSServTimeout < now) {
			// Close connection if inactive for more than kCSServTimeout seconds
			entry.state = ChunkserverEntry::State::Close;
		}
		if (entry.state == ChunkserverEntry::State::Close) {
			eptr->closeJobs();
		}
	}

	jobscnt = bgJobPool_->getJobCount();
//      // Lock free stats_maxjobscnt = max(stats_maxjobscnt, jobscnt), but I don't trust myself :(...
//      uint32_t expected_value = stats_maxjobscnt;
//      while (jobscnt > expected_value
//                      && !stats_maxjobscnt.compare_exchange_strong(expected_value, jobscnt)) {
//              expected_value = stats_maxjobscnt;
//      }
// // .. Will end up with a racy code instead :(
	if (jobscnt > stats_maxjobscnt) {
		// A race is possible here, but it won't lead to any serious consequences, in a worst
		// (and unlikely) case stats_maxjobscnt will be slightly lower than it actually should be
		stats_maxjobscnt = jobscnt;
	}

	for (auto it = csservEntries.begin(); it != csservEntries.end();) {
		auto &eptr = *it;
		if (eptr.state == ChunkserverEntry::State::Closed) {
			it = csservEntries.erase(it); // Should call the entry destructor
		} else {
			++it;
		}
	}
}

void NetworkWorkerThread::askForTermination() {
	TRACETHIS();
	doTerminate = true;
	std::unique_lock lock(csservheadLock);
	terminationTimer_.reset();
}

void NetworkWorkerThread::addConnection(int newSocketFD) {
	TRACETHIS();
	tcpnonblock(newSocketFD);
	tcpnodelay(newSocketFD);

	std::unique_lock lock(csservheadLock);
	try {
		if (!bgJobPool_) { throw std::runtime_error("JobPool instance is null"); }
		csservEntries.emplace_front(newSocketFD, bgJobPool_.get(), gMaxBlocksPerHddReadJob,
		                            gMaxParallelHddReadJobsPerCsEntry, gMaxBlocksPerHddWriteJob);
		csservEntries.front().lastActivity = eventloop_time();
	} catch (const std::runtime_error &e) {
		safs::log_err("Failed to add connection: {}", e.what());
		tcpclose(newSocketFD);
	} catch (const std::exception &e) {
		safs::log_err("Unexpected error: {}", e.what());
		tcpclose(newSocketFD);
	}

	eassert(write(notify_pipe[1], "9", 1) == 1);
}
