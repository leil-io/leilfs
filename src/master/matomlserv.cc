
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

#include "master/matomlserv.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <queue>
#include <set>

#include "common/crc.h"
#include "common/datapack.h"
#include "common/event_loop.h"
#include "common/input_packet.h"
#include "common/loop_watchdog.h"
#include "common/massert.h"
#include "common/output_packet.h"
#include "common/saunafs_version.h"
#include "common/sockets.h"
#include "config/cfg.h"
#include "master/filesystem.h"
#include "master/filesystem_operations_interface.h"
#include "master/metadata_backend_common.h"
#include "master/metadata_backend_interface.h"
#include "master/personality.h"
#include "protocol/SFSCommunication.h"
#include "protocol/matoml.h"
#include "protocol/mltoma.h"
#include "slogger/slogger.h"

constexpr uint32_t kMaxPacketSize = 1500000;
constexpr uint16_t kOldChangesBlockSize = 5000;

enum class MetaloggerConnectionMode : uint8_t {
	KILL,
	HEADER,
	DATA
};

struct MatomlservEntry {
	MetaloggerConnectionMode mode = MetaloggerConnectionMode::HEADER;
	int sock;
	int32_t pollFdIndex{-1};
	uint32_t lastRead{0};
	uint32_t lastWrite{0};
	std::array<uint8_t, PacketHeader::kSize> headerBuff{0};
	InputPacket inputPacket{kMaxPacketSize};
	std::queue<OutputPacket> outputPackets;
	uint16_t timeout{10};
	std::string serviceStrIp;
	uint32_t version{};
	uint32_t serviceIp{};
	uint16_t servicePort{};
	std::string config;
	bool shadow{false};
	int metaFD{-1};
	int chain1FD{-1};
	int chain2FD{-1};

	MatomlservEntry(int sock_, uint32_t now) : sock(sock_), lastRead(now), lastWrite(now) {}
};

static std::deque<MatomlservEntry> matomlservList;
static int listenSocket;
static int32_t listenSocketPollFdIndex;
static bool gExiting = false;

// from config
static std::string gListenHost;
static std::string gListenPort;
static uint16_t gChangelogSecondsToRemember;

/// Minimal period (in seconds) between two metadata save processes requested by shadow masters
static uint32_t gMinMetadataSaveRequestPeriod_s;

/// Timestamp of the last metadata save request
static uint32_t gLastMetadataSaveRequestTimestamp = 0;

struct OldChangesEntry {
	uint64_t version{};
	std::vector<uint8_t> data;
};

struct OldChangesBlock {
	std::vector<OldChangesEntry> oldChangesBlock;
	uint32_t minTimestamp{};
	uint64_t minVersion{};
};

std::deque<OldChangesBlock> oldChangesBlocks;

void matomlserv_createpacket(MatomlservEntry *eptr, const std::vector<uint8_t> &data);

/*! \brief Keep queue of Shadows interested in receiving information
 * regarding status of requested metadata dump.
 */
class ShadowQueue {
public:
	/*! \brief Add shadow connection handler to the queue.
	 *
	 * \param eptr - connection handler to be queued.
	 */
	void addRequest(MatomlservEntry *eptr) { shadowRequests_.insert(eptr); }

	/*! \brief Remove given connection handler from queue.
	 *
	 * \param eptr - connection handler to be removed from queue.
	 */
	void removeRequest(MatomlservEntry *eptr) { shadowRequests_.erase(eptr); }

	/*! \brief Handle all awaiting Shadows in queue.
	 *
	 * Broadcast metadata dump status and clear the queue.
	 *
	 * \param status - status of recently finished metadata dump.
	 */
	void handleRequests(uint8_t status) {
		for (const auto &eptr : shadowRequests_) {
			matomlserv_createpacket(eptr, matoml::changelogApplyError::build(status));
		}
		shadowRequests_.clear();
	}

private:
	using ShadowRequests = std::set<MatomlservEntry *>;
	ShadowRequests shadowRequests_;
};

static ShadowQueue gShadowQueue;

/*! \brief Forward metadata dump status to Shadow queue.
 *
 * \param status - status to be forwarded.
 */
void matomlserv_broadcast_metadata_saved(uint8_t status) { gShadowQueue.handleRequests(status); }

void matomlserv_store_logstring(uint64_t version, uint8_t *logstr, uint32_t logstrsize) {
	if (gChangelogSecondsToRemember == 0) {
		oldChangesBlocks.clear();
		return;
	}

	uint32_t ts = eventloop_time();

	// Create new block if needed
	if (oldChangesBlocks.empty() ||
	    oldChangesBlocks.back().oldChangesBlock.size() >= kOldChangesBlockSize) {
		OldChangesBlock newBlock;
		newBlock.minVersion = version;
		newBlock.minTimestamp = ts;
		oldChangesBlocks.push_back(std::move(newBlock));

		while (oldChangesBlocks.size() > 1 &&
		       oldChangesBlocks[1].minTimestamp + gChangelogSecondsToRemember < ts) {
			oldChangesBlocks.pop_front();
		}
	}

	// Add new entry to the current block
	OldChangesBlock &block = oldChangesBlocks.back();
	block.oldChangesBlock.emplace_back(OldChangesEntry{
	    .version = version, .data = std::vector<uint8_t>(logstr, logstr + logstrsize)});
}

uint32_t matomlserv_mloglist_size(void) {
	uint32_t i = 0;

	for (const auto &eptr : matomlservList) {
		if (!eptr.shadow && eptr.mode != MetaloggerConnectionMode::KILL) { i++; }
	}

	return i * (sizeof(MatomlservEntry::version) + sizeof(MatomlservEntry::serviceIp));
}

void matomlserv_mloglist_data(uint8_t *ptr) {
	for (const auto &eptr : matomlservList) {
		if (!eptr.shadow && eptr.mode != MetaloggerConnectionMode::KILL) {
			put32bit(&ptr, eptr.version);
			put32bit(&ptr, eptr.serviceIp);
		}
	}
}

std::vector<MetadataserverListEntry> matomlserv_shadows() {
	std::vector<MetadataserverListEntry> ret;
	for (const auto &eptr : matomlservList) {
		if (eptr.shadow) { ret.emplace_back(eptr.serviceIp, eptr.servicePort, eptr.version); }
	}
	return ret;
}

std::string get_metaloggers_config() {
	std::map<std::string, std::string> configs;
	for (const auto &eptr : matomlservList) {
		if (eptr.shadow) { continue; }
		NetworkAddress addr(eptr.serviceIp, eptr.servicePort);
		configs[addr.toString()] = eptr.config;
	}
	return cfg_yaml_list("metaloggers", configs);
}

uint32_t matomlserv_shadows_count() {
	uint32_t count = 0;
	for (const auto &eptr : matomlservList) {
		if (eptr.shadow) { count++; }
	}
	return count;
}

void matomlserv_status(void) {
	for (const auto &eptr : matomlservList) {
		if (eptr.mode == MetaloggerConnectionMode::HEADER ||
		    eptr.mode == MetaloggerConnectionMode::DATA) {
			return;
		}
	}
	safs::log_warn("no meta loggers connected !!!");
}

uint8_t *matomlserv_createpacket(MatomlservEntry *eptr, uint32_t type, uint32_t size) {
	eptr->outputPackets.emplace(PacketHeader(type, size));
	return eptr->outputPackets.back().packet.data() + PacketHeader::kSize;
}

void matomlserv_createpacket(MatomlservEntry *eptr, const std::vector<uint8_t> &data) {
	eptr->outputPackets.emplace(data);
}

void matomlserv_send_old_changes(MatomlservEntry *eptr, uint64_t version) {
	if (oldChangesBlocks.empty()) { return; }

	if (oldChangesBlocks.front().minVersion > version) {
		safs::log_warn(
		    "meta logger wants changes since version: {}, but minimal version in storage is: {}",
		    version, oldChangesBlocks.front().minVersion);
		// TODO(msulikowski) send a special message which will cause the shadow master to unload fs
	}

	bool start = false;
	for (std::size_t idx = 0; idx < oldChangesBlocks.size(); ++idx) {
		const auto &block = oldChangesBlocks[idx];

		if (block.minVersion >= version) {
			start = true;
		} else if (block.minVersion <= version) {
			if (idx + 1 >= oldChangesBlocks.size() ||
			    oldChangesBlocks[idx + 1].minVersion > version) {
				start = true;
			}
		}

		if (start) {
			for (std::size_t i = 0; i < block.oldChangesBlock.size(); ++i) {
				const auto &entry = block.oldChangesBlock[i];
				if (version < entry.version) {
					uint8_t *data = matomlserv_createpacket(eptr, MATOML_METACHANGES_LOG,
					                                        9 + entry.data.size());
					put8bit(&data, 0xFF);
					put64bit(&data, entry.version);
					std::memcpy(data, entry.data.data(), entry.data.size());
				}
			}
		}
	}
}

void matomlserv_register(MatomlservEntry *eptr, const uint8_t *data, uint32_t length) {
	if (eptr->version > 0) {
		safs::log_warn("got register message from registered metalogger !!!");
		eptr->mode = MetaloggerConnectionMode::KILL;
		return;
	}

	if (length < 1) {
		safs::log_info("MLTOMA_REGISTER - wrong size ({})", length);
		eptr->mode = MetaloggerConnectionMode::KILL;
		return;
	} else {
		uint8_t rversion = get8bit(&data);
		if (rversion < 1 || rversion > 4) {
			safs::log_info("MLTOMA_REGISTER - wrong version ({})", rversion);
			eptr->mode = MetaloggerConnectionMode::KILL;
			return;
		}

		static const uint32_t expected_length[] = {0, 7, 7 + 8, 7, 7 + 8};
		if (length != expected_length[rversion]) {
			safs::log_info("MLTOMA_REGISTER (ver {}) - wrong size ({}/{})", rversion, length,
			               expected_length[rversion]);
			eptr->mode = MetaloggerConnectionMode::KILL;
			return;
		}

		get32bit(&data, eptr->version);
		eptr->timeout = get16bit(&data);
		eptr->shadow = (rversion == 3 || rversion == 4);
		if (eptr->shadow) {
			// supported shadow master servers register using SAU_MLTOMA_REGISTER_SHADOW packet
			safs::log_info("MLTOMA_REGISTER (ver {}) - rejected old shadow master (v{}) from {}",
			               rversion, saunafsVersionToString(eptr->version), eptr->serviceStrIp);
			eptr->mode = MetaloggerConnectionMode::KILL;
			return;
		}

		if (rversion == 2 || rversion == 4) {
			uint64_t minversion = get64bit(&data);
			matomlserv_send_old_changes(eptr, minversion);
		}

		if (eptr->timeout < 10) {
			safs::log_info(
			    "MLTOMA_REGISTER communication timeout too small ({} seconds - should be at least 10 seconds)",
			    eptr->timeout);
			if (eptr->timeout < 3) { eptr->timeout = 3; }
		}
	}
}

void matomlserv_register_shadow(MatomlservEntry *eptr, const uint8_t *data, uint32_t length) {
	uint32_t version, timeout_ms;
	uint64_t shadowMetadataVersion;
	mltoma::registerShadow::deserialize(data, length, version, timeout_ms, shadowMetadataVersion);
	eptr->timeout = timeout_ms / 1000;
	eptr->version = version;
	eptr->shadow = true;
	if (eptr->timeout < 10) {
		safs::log_info(
		    "MLTOMA_REGISTER_SHADOW communication timeout too small ({} milliseconds)"
		    " - should be at least 10 seconds; increasing to 10 seconds",
		    timeout_ms);
		if (eptr->timeout < 10) { eptr->timeout = 10; }
	}

	if (eptr->version < SAUNAFS_VERSHEX) {
		safs::log_info("MLTOMA_REGISTER_SHADOW - rejected old client (v{}) from {}",
		               saunafsVersionToString(eptr->version), eptr->serviceStrIp);
		matomlserv_createpacket(eptr,
		                        matoml::registerShadow::build(uint8_t(SAUNAFS_ERROR_REGISTER)));
		return;
	}

	uint64_t myMedatataVersion = gFSOperations->getMetadataVersion();
	uint64_t replyVersion;
	if (myMedatataVersion > shadowMetadataVersion && !oldChangesBlocks.empty() &&
	    oldChangesBlocks.front().minVersion <= shadowMetadataVersion) {
		// Our version is newer than shadow's, but we can cheat a bit by sending old changes
		replyVersion = shadowMetadataVersion;
	} else {
		// We don't have the required changes in memory. Let's say what is our version of metadata
		// and shadow will have to download our metadata file
		replyVersion = myMedatataVersion;
	}

	matomlserv_createpacket(eptr, matoml::registerShadow::build(SAUNAFS_VERSHEX, replyVersion));
	matomlserv_send_old_changes(eptr, replyVersion - 1);  // this function expects lastlogversion
}

void matomlserv_register_config(MatomlservEntry *eptr, const uint8_t *data, uint32_t length) {
	mltoma::dumpConfiguration::deserialize(data, length, eptr->config);
}

void matomlserv_matoclport(MatomlservEntry *eptr, const uint8_t *data, uint32_t length) {
	mltoma::matoclport::deserialize(data, length, eptr->servicePort);
}

void matomlserv_download_start(MatomlservEntry *eptr, const uint8_t *data, uint32_t length) {
	if (length != 1) {
		safs::log_info("MLTOMA_DOWNLOAD_START - wrong size ({}/1)", length);
		eptr->mode = MetaloggerConnectionMode::KILL;
		return;
	}
	if (gExiting) {
		safs::log_info("MLTOMA_DOWNLOAD_START - ignoring in the exit phase");
		return;
	}
	uint8_t filenum = get8bit(&data);

	if ((filenum == DOWNLOAD_METADATA_SFS) || (filenum == DOWNLOAD_SESSIONS_SFS)) {
		if (eptr->metaFD >= 0) {
			close(eptr->metaFD);
			eptr->metaFD = -1;
		}
		if (eptr->chain1FD >= 0) {
			close(eptr->chain1FD);
			eptr->chain1FD = -1;
		}
		if (eptr->chain2FD >= 0) {
			close(eptr->chain2FD);
			eptr->chain2FD = -1;
		}
	}
	if (filenum == DOWNLOAD_METADATA_SFS) {
		eptr->metaFD = open(kMetadataFilename, O_RDONLY);
		eptr->chain1FD = open(kChangelogFilename, O_RDONLY);
		eptr->chain2FD = open((std::string(kChangelogFilename) + ".1").c_str(), O_RDONLY);
	} else if (filenum == DOWNLOAD_SESSIONS_SFS) {
		eptr->metaFD = open(kSessionsFilename, O_RDONLY);
	} else if (filenum == DOWNLOAD_CHANGELOG_SFS) {
		if (eptr->metaFD >= 0) { close(eptr->metaFD); }
		eptr->metaFD = eptr->chain1FD;
		eptr->chain1FD = -1;
	} else if (filenum == DOWNLOAD_CHANGELOG_SFS_1) {
		if (eptr->metaFD >= 0) { close(eptr->metaFD); }
		eptr->metaFD = eptr->chain2FD;
		eptr->chain2FD = -1;
	} else {
		eptr->mode = MetaloggerConnectionMode::KILL;
		return;
	}
	uint8_t *ptr;
	if (eptr->metaFD < 0) {
		if (filenum == 11 || filenum == 12) {
			ptr = matomlserv_createpacket(eptr, MATOML_DOWNLOAD_START, 8);
			put64bit(&ptr, 0);
			return;
		} else {
			ptr = matomlserv_createpacket(eptr, MATOML_DOWNLOAD_START, 1);
			put8bit(&ptr, 0xff);  // error
			return;
		}
	}
	uint64_t size = lseek(eptr->metaFD, 0, SEEK_END);
	ptr = matomlserv_createpacket(eptr, MATOML_DOWNLOAD_START, 8);
	put64bit(&ptr, size);  // ok
}

void matomlserv_download_data(MatomlservEntry *eptr, const uint8_t *data, uint32_t length) {
	uint8_t *ptr;
	uint64_t offset;
	uint32_t leng;
	uint32_t crc;
	ssize_t ret;

	if (length != 12) {
		safs::log_info("MLTOMA_DOWNLOAD_DATA - wrong size ({}/12)", length);
		eptr->mode = MetaloggerConnectionMode::KILL;
		return;
	}
	if (gExiting) {
		safs::log_info("MLTOMA_DOWNLOAD_DATA - ignoring in the exit phase");
		return;
	}
	if (eptr->metaFD < 0) {
		safs::log_info("MLTOMA_DOWNLOAD_DATA - file not opened");
		eptr->mode = MetaloggerConnectionMode::KILL;
		return;
	}
	offset = get64bit(&data);
	get32bit(&data, leng);
	ptr = matomlserv_createpacket(eptr, MATOML_DOWNLOAD_DATA, 16 + leng);
	put64bit(&ptr, offset);
	put32bit(&ptr, leng);
	ret = pread(eptr->metaFD, ptr + 4, leng, offset);
	if (ret != (ssize_t)leng) {
		safs::log_err("error reading metafile");
		eptr->mode = MetaloggerConnectionMode::KILL;
		return;
	}
	crc = mycrc32(0, ptr + 4, leng);
	put32bit(&ptr, crc);
}

void matomlserv_download_end(MatomlservEntry *eptr, const uint8_t *data, uint32_t length) {
	(void)data;
	if (length != 0) {
		safs::log_info("MLTOMA_DOWNLOAD_END - wrong size ({}/0)", length);
		eptr->mode = MetaloggerConnectionMode::KILL;
		return;
	}
	if (eptr->metaFD >= 0) {
		close(eptr->metaFD);
		eptr->metaFD = -1;
	}
}

void matomlserv_changelog_apply_error(MatomlservEntry *eptr, const uint8_t *data, uint32_t length) {
	uint8_t recvStatus;
	mltoma::changelogApplyError::deserialize(data, length, recvStatus);
	if (gExiting) {
		safs::log_info("SAU_MLTOMA_CHANGELOG_APPLY_ERROR - ignoring in the exit phase");
		return;
	}

	int32_t secondsSinceLastRequest = eventloop_time() - gLastMetadataSaveRequestTimestamp;
	if (secondsSinceLastRequest >= int32_t(gMinMetadataSaveRequestPeriod_s)) {
		gLastMetadataSaveRequestTimestamp = eventloop_time();
		safs::log_debug("master.mltoma_changelog_apply_error: do");
		safs::log_info("SAU_MLTOMA_CHANGELOG_APPLY_ERROR, status: {} - storing metadata",
		               saunafs_error_string(recvStatus));
		gShadowQueue.addRequest(eptr);
		gMetadataBackend->fs_storeall(DumpType::kBackgroundDump);
		if (recvStatus == SAUNAFS_ERROR_BADMETADATACHECKSUM) {
			gFSOperations->startChecksumRecalculation();
		}
	} else {
		safs::log_debug("master.mltoma_changelog_apply_error: delay");
		safs::log_info(
		    "SAU_MLTOMA_CHANGELOG_APPLY_ERROR, status: {} - "
		    "refusing to store metadata because only {} seconds elapsed since the "
		    "previous request and METADATA_SAVE_REQUEST_MIN_PERIOD={}",
		    saunafs_error_string(recvStatus), secondsSinceLastRequest,
		    gMinMetadataSaveRequestPeriod_s);
		matomlserv_createpacket(eptr, matoml::changelogApplyError::build(SAUNAFS_ERROR_DELAYED));
	}
}

void matomlserv_broadcast_logstring(uint64_t version, uint8_t *logstr, uint32_t logstrsize) {
	uint8_t *data;

	matomlserv_store_logstring(version, logstr, logstrsize);

	for (auto &eptr : matomlservList) {
		if (eptr.version > 0) {
			data = matomlserv_createpacket(&eptr, MATOML_METACHANGES_LOG, 9 + logstrsize);
			put8bit(&data, 0xFF);
			put64bit(&data, version);
			memcpy(data, logstr, logstrsize);
		}
	}
}

void matomlserv_broadcast_logrotate() {
	uint8_t *data;

	for (auto &eptr : matomlservList) {
		if (eptr.version > 0) {
			data = matomlserv_createpacket(&eptr, MATOML_METACHANGES_LOG, 1);
			put8bit(&data, FORCE_LOG_ROTATE);
		}
	}
}

void matomlserv_beforeclose(MatomlservEntry *eptr) {
	if (eptr->metaFD >= 0) {
		close(eptr->metaFD);
		eptr->metaFD = -1;
	}
	if (eptr->chain1FD >= 0) {
		close(eptr->chain1FD);
		eptr->chain1FD = -1;
	}
	if (eptr->chain2FD >= 0) {
		close(eptr->chain2FD);
		eptr->chain2FD = -1;
	}
	gShadowQueue.removeRequest(eptr);
}

void matomlserv_gotpacket(MatomlservEntry *eptr, uint32_t type, const uint8_t *data,
                          uint32_t length) {
	try {
		switch (type) {
		case ANTOAN_NOP:
			break;
		case ANTOAN_UNKNOWN_COMMAND:  // for future use
			break;
		case ANTOAN_BAD_COMMAND_SIZE:  // for future use
			break;
		case MLTOMA_REGISTER:
			matomlserv_register(eptr, data, length);
			break;
		case SAU_MLTOMA_REGISTER_SHADOW:
			matomlserv_register_shadow(eptr, data, length);
			break;
		case SAU_MLTOMA_DUMP_CONFIG:
			matomlserv_register_config(eptr, data, length);
			break;
		case MLTOMA_DOWNLOAD_START:
			matomlserv_download_start(eptr, data, length);
			break;
		case MLTOMA_DOWNLOAD_DATA:
			matomlserv_download_data(eptr, data, length);
			break;
		case MLTOMA_DOWNLOAD_END:
			matomlserv_download_end(eptr, data, length);
			break;
		case SAU_MLTOMA_CHANGELOG_APPLY_ERROR:
			matomlserv_changelog_apply_error(eptr, data, length);
			break;
		case SAU_MLTOMA_CLTOMA_PORT:
			matomlserv_matoclport(eptr, data, length);
			break;
		default:
			safs::log_info("master <-> metaloggers module: got unknown message (type:{})", type);
			eptr->mode = MetaloggerConnectionMode::KILL;
		}
	} catch (IncorrectDeserializationException &ex) {
		safs::log_info("Packet 0x{} - can't deserialize: {}", type, ex.what());
		eptr->mode = MetaloggerConnectionMode::KILL;
	}
}

void matomlserv_term() {
	safs::log_info("master <-> metaloggers module: closing {}:{}", gListenHost, gListenPort);
	tcpclose(listenSocket);

	for (auto &eptr : matomlservList) {
		// Remove from shadow queue
		gShadowQueue.removeRequest(&eptr);
	}
	matomlservList.clear();
}

void matomlserv_read(MatomlservEntry *eptr) {
	SignalLoopWatchdog watchdog;
	int32_t i;

	watchdog.start();
	while (eptr->mode != MetaloggerConnectionMode::KILL) {
		uint32_t bytesToRead = eptr->inputPacket.bytesToBeRead();
		i = read(eptr->sock, eptr->inputPacket.pointerToBeReadInto(), bytesToRead);
		if (i == 0) {
			safs::log_info("connection with ML({}) has been closed by peer", eptr->serviceStrIp);
			eptr->mode = MetaloggerConnectionMode::KILL;
			return;
		}

		if (i < 0) {
			if (errno != EAGAIN) {
				safs::log_err("read from ML({}) error", eptr->serviceStrIp);
				eptr->mode = MetaloggerConnectionMode::KILL;
			}
			return;
		}

		try {
			eptr->inputPacket.increaseBytesRead(i);
		} catch (InputPacketTooLongException &ex) {
			safs::log_warn("reading from ML({}): {}", eptr->serviceStrIp, ex.what());
			eptr->mode = MetaloggerConnectionMode::KILL;
			return;
		}

		if (eptr->inputPacket.hasData()) {
			auto header = eptr->inputPacket.getHeader();
			const auto data = eptr->inputPacket.getData();
			matomlserv_gotpacket(eptr, header.type, data.data(), data.size());
			eptr->inputPacket.reset();
		}

		if (watchdog.expired()) { break; }
	}
}

void matomlserv_write(MatomlservEntry *eptr) {
	SignalLoopWatchdog watchdog;
	int32_t bytesWritten;

	watchdog.start();
	while (!eptr->outputPackets.empty()) {
		OutputPacket &outputPacket = eptr->outputPackets.front();
		bytesWritten = write(eptr->sock, outputPacket.packet.data() + outputPacket.bytesSent,
		                     outputPacket.packet.size() - outputPacket.bytesSent);
		if (bytesWritten < 0) {
			if (errno != EAGAIN) {
				safs::log_err("main master server module: (ip:{}) write error", eptr->serviceStrIp);
				eptr->mode = MetaloggerConnectionMode::KILL;
			}
			return;
		}
		outputPacket.bytesSent += bytesWritten;

		if (outputPacket.bytesSent >= outputPacket.packet.size()) {
			eptr->outputPackets.pop();
		} else {
			return;
		}

		if (watchdog.expired()) { break; }
	}
}

void matomlserv_desc(std::vector<pollfd> &pdesc) {
	if (!gExiting) {
		pdesc.push_back({listenSocket, POLLIN, 0});
		listenSocketPollFdIndex = pdesc.size() - 1;
	} else {
		listenSocketPollFdIndex = -1;
	}
	for (auto &eptr : matomlservList) {
		pdesc.push_back({eptr.sock, POLLIN, 0});
		eptr.pollFdIndex = pdesc.size() - 1;
		if (!eptr.outputPackets.empty()) { pdesc.back().events |= POLLOUT; }
	}
}

void matomlserv_serve(const std::vector<pollfd> &pdesc) {
	uint32_t now = eventloop_time();
	int ns;

	if (listenSocketPollFdIndex >= 0 && (pdesc[listenSocketPollFdIndex].revents & POLLIN)) {
		ns = tcpaccept(listenSocket);
		if (ns < 0) {
			safs_silent_errlog(LOG_NOTICE, "master<->ML socket: accept error");
		} else if (metadataserver::isMaster()) {
			tcpnonblock(ns);
			tcpnodelay(ns);
			matomlservList.emplace_front(ns, now);
			MatomlservEntry &eptr = matomlservList.front();
			tcpgetpeer(eptr.sock, &(eptr.serviceIp), nullptr);
			eptr.serviceStrIp = ipToString(eptr.serviceIp);
			eptr.config.clear();
		} else {
			tcpclose(ns);
		}
	}

	for (auto &eptr : matomlservList) {
		if (eptr.pollFdIndex >= 0) {
			if (pdesc[eptr.pollFdIndex].revents & (POLLERR | POLLHUP)) {
				eptr.mode = MetaloggerConnectionMode::KILL;
			}

			if ((pdesc[eptr.pollFdIndex].revents & POLLIN) &&
			    eptr.mode != MetaloggerConnectionMode::KILL) {
				eptr.lastRead = now;
				matomlserv_read(&eptr);
			}

			if ((pdesc[eptr.pollFdIndex].revents & POLLOUT) &&
			    eptr.mode != MetaloggerConnectionMode::KILL) {
				eptr.lastWrite = now;
				matomlserv_write(&eptr);
			}
		}

		if ((uint32_t)(eptr.lastRead + eptr.timeout) < (uint32_t)now) {
			eptr.mode = MetaloggerConnectionMode::KILL;
		}

		if ((uint32_t)(eptr.lastWrite + (eptr.timeout / 3)) < (uint32_t)now &&
		    eptr.outputPackets.empty() && !gExiting) {
			matomlserv_createpacket(&eptr, ANTOAN_NOP, 0);
		}
	}

	for (auto it = matomlservList.begin(); it != matomlservList.end();) {
		if (it->mode == MetaloggerConnectionMode::KILL) {
			matomlserv_beforeclose(&(*it));
			tcpclose(it->sock);
			it = matomlservList.erase(it);
		} else {
			++it;
		}
	}
}

void matomlserv_wantexit(void) {
	gExiting = true;
	for (auto &eptr : matomlservList) {
		matomlserv_createpacket(&eptr, matoml::endSession::build());
	}
	// Now we won't create any new packets, but we will wait for all existing packets to be
	// transmitted to shadow masters and metaloggers
}

int matomlserv_canexit(void) {
	// Exit when all connections are closed
	return matomlservList.empty();
}

void matomlserv_become_master() {
	eventloop_timeregister(TIMEMODE_SKIP_LATE, 3600, 0, matomlserv_status);
	return;
}

/// Read values from the config file into global variables.
/// Used on init and reload.
void matomlserv_read_config_file() {
	gMinMetadataSaveRequestPeriod_s = cfg_getuint32("METADATA_SAVE_REQUEST_MIN_PERIOD", 1800);
}

void matomlserv_reload(void) {
	std::string oldListenHost;
	std::string oldListenPort;
	int newListenSock;

	matomlserv_read_config_file();

	oldListenHost = gListenHost;
	oldListenPort = gListenPort;
	gListenHost = cfg_getstring("MATOML_LISTEN_HOST", "*");
	gListenPort = cfg_getstring("MATOML_LISTEN_PORT", "9419");
	if (oldListenHost == gListenHost && oldListenPort == gListenPort) {
		safs::log_info("master <-> metaloggers module: socket address hasn't changed ({}:{})",
		               gListenHost, gListenPort);
		return;
	}

	newListenSock = tcpsocket();
	if (newListenSock < 0) {
		safs::log_warn(
		    "master <-> metaloggers module: socket address has changed, but can't create new socket");
		gListenHost = oldListenHost;
		gListenPort = oldListenPort;
		return;
	}

	tcpnonblock(newListenSock);
	tcpnodelay(newListenSock);
	tcpreuseaddr(newListenSock);
	if (tcpsetacceptfilter(newListenSock) < 0 && errno != ENOTSUP) {
		safs_silent_errlog(LOG_NOTICE, "master <-> metaloggers module: can't set accept filter");
	}

	if (tcpstrlisten(newListenSock, gListenHost.c_str(), gListenPort.c_str(), 100) < 0) {
		safs::log_err(
		    "master <-> metaloggers module: socket address has changed, but can't listen on socket ({}:{})",
		    gListenHost, gListenPort);
		gListenHost = oldListenHost;
		gListenPort = oldListenPort;
		tcpclose(newListenSock);
		return;
	}

	safs::log_info("master <-> metaloggers module: socket address has changed, now listen on {}:{}",
	               gListenHost, gListenPort);
	tcpclose(listenSocket);
	listenSocket = newListenSock;

	gChangelogSecondsToRemember = cfg_getuint16("MATOML_LOG_PRESERVE_SECONDS", 600);
	if (gChangelogSecondsToRemember > 3600) {
		safs::log_warn(
		    "Number of seconds of change logs to be preserved in master is too big ({}) - decreasing to 3600 seconds",
		    gChangelogSecondsToRemember);
		gChangelogSecondsToRemember = 3600;
	}
}

int matomlserv_init(void) {
	matomlserv_read_config_file();
	gListenHost = cfg_getstring("MATOML_LISTEN_HOST", "*");
	gListenPort = cfg_getstring("MATOML_LISTEN_PORT", "9419");

	listenSocket = tcpsocket();
	if (listenSocket < 0) {
		safs::log_err("master <-> metaloggers module: can't create socket");
		return -1;
	}
	tcpnonblock(listenSocket);
	tcpnodelay(listenSocket);
	tcpreuseaddr(listenSocket);
	if (tcpsetacceptfilter(listenSocket) < 0 && errno != ENOTSUP) {
		safs::log_err("master <-> metaloggers module: can't set accept filter");
	}

	if (tcpstrlisten(listenSocket, gListenHost.c_str(), gListenPort.c_str(), 100) < 0) {
		safs::log_err("master <-> metaloggers module: can't listen on {}:{}", gListenHost,
		              gListenPort);
		return -1;
	}
	safs::log_info("master <-> metaloggers module: listen on {}:{}", gListenHost, gListenPort);

	matomlservList.clear();
	gChangelogSecondsToRemember = cfg_getuint16("MATOML_LOG_PRESERVE_SECONDS", 600);
	if (gChangelogSecondsToRemember > 3600) {
		safs::log_warn(
		    "Number of seconds of change logs to be preserved in master is too big ({}) - decreasing to 3600 seconds",
		    gChangelogSecondsToRemember);
		gChangelogSecondsToRemember = 3600;
	}

	eventloop_wantexitregister(matomlserv_wantexit);
	eventloop_canexitregister(matomlserv_canexit);
	eventloop_reloadregister(matomlserv_reload);
	metadataserver::registerFunctionCalledOnPromotion(matomlserv_become_master);
	eventloop_destructregister(matomlserv_term);
	eventloop_pollregister(matomlserv_desc, matomlserv_serve);
	if (metadataserver::isMaster()) { matomlserv_become_master(); }
	return 0;
}
