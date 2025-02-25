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

#include "master/masterconn.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/stat.h>
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
#include <memory>
#include <queue>
#include <string>

#include "common/crc.h"
#include "common/datapack.h"
#include "common/event_loop.h"
#include "common/loop_watchdog.h"
#include "common/massert.h"
#include "common/rotate_files.h"
#include "common/saunafs_version.h"
#include "common/sockets.h"
#include "common/time_utils.h"
#include "config/cfg.h"
#include "errors/saunafs_error_codes.h"
#include "master/changelog.h"
#include "master/metadata_backend_common.h"
#include "master/metadata_backend_interface.h"
#include "protocol/SFSCommunication.h"
#include "protocol/matoml.h"
#include "protocol/mltoma.h"
#include "slogger/slogger.h"

#ifndef METALOGGER
#include "master/filesystem.h"
#include "master/personality.h"
#include "master/restore.h"
#endif /* #ifndef METALOGGER */

namespace {
#ifdef METALOGGER
const std::string metadataFilename = kMetadataMlFilename;
const std::string metadataTmpFilename = kMetadataMlTmpFilename;
const std::string changelogFilename = kChangelogMlFilename;
const std::string changelogTmpFilename = kChangelogMlTmpFilename;
const std::string sessionsFilename = kSessionsMlFilename;
const std::string sessionsTmpFilename = kSessionsMlTmpFilename;
#else  /* #ifdef METALOGGER */
const std::string metadataFilename = kMetadataFilename;
const std::string metadataTmpFilename = kMetadataTmpFilename;
const std::string changelogFilename = kChangelogFilename;
const std::string changelogTmpFilename = kChangelogTmpFilename;
const std::string sessionsFilename = kSessionsFilename;
const std::string sessionsTmpFilename = kSessionsTmpFilename;
#endif /* #else #ifdef METALOGGER */
}  // namespace

/// Block size for metadata download (1 MB).
constexpr uint32_t kMetadataDownloadBlocksize = 1000000U;

/// Safety measure about expected max packet size.
constexpr uint32_t kMaxPacketSize = 1500000U;

struct PacketStruct {
	uint8_t *startPtr;
	uint32_t bytesLeft;
	std::vector<uint8_t> packet;
};

/// Represents a connection to the master server.
/// Holds the connection state and the data that is being sent or received.
struct MasterConn {
	// Useful constants.
	static constexpr uint8_t kHeaderSize = 8;
	static constexpr uint8_t kPacketTypeSize = 4;
	static constexpr uint8_t kChangeLogApplyErrorTimeout = 10;
	static constexpr int kInvalidFD = -1;
	static constexpr int kInvalidPollDescPos = -1;
	static constexpr uint32_t kInvalidMasterVersion = 0;

	static constexpr uint32_t kMaxMasterTimeout = 65536;
	static constexpr uint32_t kMinMasterTimeout = 10;
	static constexpr uint32_t kMaxBackMetaCopies = 100;

	enum class Mode : uint8_t {
		Free,        ///< Connection is not in use.
		Connecting,  ///< Connection is being established.
		Header,      ///< Header is being read.
		Data,        ///< Data is being read.
		Kill         ///< Connection is being closed.
	};

	enum class State : uint8_t {
		/// Initial state.
		kNone,
		/// Metadata was downloaded and we have the same version as the master.
		kSynchronized,
		/// Downloading metadata from the master.
		kDownloading,
		/// Waiting for the master to produce up-to-date metadata image.
		kDumpRequestPending,
		/// Got response from master regarding its inability to dump metadata.
		kLimbo
	};

	Mode mode{Mode::Free};  ///< Current connection mode.
	State state{State::kNone};  ///< Current synchronization state.

	int sock{kInvalidFD};  ///< Socket descriptor.
	int32_t pollDescPos{kInvalidFD};  ///< Position in the poll desc. array.
	uint32_t lastRead{};  ///< Timestamp of the last read operation.
	uint32_t lastWrite{};  ///< Timestamp of the last write operation.

	/// Master version in the other end. Known after registration.
	uint32_t masterVersion{kInvalidMasterVersion};

	std::array<uint8_t, kHeaderSize> headerBuffer{};  ///< Buffer for headers.
	PacketStruct inputPacket{};  ///< Structure for the input packet.
	/// Queue of output packets.
	std::queue<std::unique_ptr<PacketStruct>> outputQueue;

	uint32_t bindIP{};  ///< IP address to bind the socket.
	uint32_t masterIP{};  ///< IP address of the master server.
	uint16_t masterPort{};  ///< Port of the master server.
	uint8_t isMasterAddressValid{0};  /// Known after (re)connections.

	uint8_t downloadRetryCnt{};  ///< Retry count for downloads.
	/// Number of the file being downloaded (metadata, changelogs, sessions).
	/// 0 if no download is in progress.
	uint8_t downloadingFileNum{};
	int downloadFD{kInvalidFD};  ///< FD for the file being downloaded.
	uint64_t fileSize{};  ///< Size of the file being downloaded.
	uint64_t downloadOffset{};  ///< Offset for the download.
	uint64_t downloadStartTimeInMicroSeconds{};  ///< Download start time.

	/// Callback to download sessions periodically.
	void* sessionsDownloadInitHandle{};
	/// Callback to flush the changelog file(s) periodically.
	void* changelogFlushHandle{};

	uint8_t errorStatus{};  ///< Error status for mltoma::changelogApplyError.
	/// Timeout for mltoma::changelogApplyError packets.
	Timeout changelogApplyErrorTimeout{
	    std::chrono::seconds(kChangeLogApplyErrorTimeout)};

	uint64_t lastLogVersion = 0;

	// from config
	uint32_t BackMetaCopies;
	std::string MasterHost;
	std::string MasterPort;
	std::string BindHost;
	uint32_t masterTimeout;

	// Callbacks
	void *reconnect_hook;
#ifdef METALOGGER
	void *download_hook;
#endif /* #ifdef METALOGGER */

	// Configuration

	void loadConfig();
	void reload();
	void sendMetaloggerConfig();

	// Connection and network packets

	uint8_t *createPacket(uint32_t type, uint32_t size);
	void createPacket(std::vector<uint8_t> data);
	void gotPacket(uint32_t type,const uint8_t *data,uint32_t length);

	int initConnect();
	void onConnected();
	void sendRegister();
	void onRegistered(const uint8_t *data, uint32_t length);
	void connectTest();
	void reconnect();
	void sendMatoClPort();

	// Metadata and sessions download

	void downloadNext();
	void downloadData(const uint8_t *data, uint32_t length);
	void forceMetadataDownload();
	int downloadEnd();
	void downloadInit(uint8_t filenum);
	void downloadStart(const uint8_t *data, uint32_t length);
	void requestMetadataDump();
	int metadataCheck(const std::string& name);

	// Changelogs

	void metachangesLog(const uint8_t *data, uint32_t length);
	void changelogApplyError(const uint8_t *data, uint32_t length);
	void handleChangelogApplyError(uint8_t status);

	// IO and polling

	void readFromSocket();
	void writeToSocket();
	void pollDesc(std::vector<pollfd> &pdesc);
	void serve(const std::vector<pollfd> &pdesc);

	// Promotion

	void becomeMaster();

	// Shutting down

	void endSession(const uint8_t* data, uint32_t length);
	void killSession();
	void beforeClose();
	void terminate();
};

static MasterConn *gMasterConn = nullptr;

uint8_t *MasterConn::createPacket(uint32_t type, uint32_t size) {
	auto outpacket = std::make_unique<PacketStruct>();
	passert(outpacket.get());
	uint32_t psize = size + MasterConn::kHeaderSize;
	outpacket->packet.resize(psize);
	passert(outpacket->packet.data());
	outpacket->bytesLeft = psize;
	auto *ptr = outpacket->packet.data();
	put32bit(&ptr,type);
	put32bit(&ptr,size);
	outpacket->startPtr = outpacket->packet.data();
	outputQueue.push(std::move(outpacket));

	return ptr;
}

void MasterConn::createPacket(std::vector<uint8_t> data) {
	auto outpacket = std::make_unique<PacketStruct>();
	passert(outpacket);
	outpacket->packet = std::move(data);
	passert(outpacket->packet.data());
	outpacket->bytesLeft = outpacket->packet.size();
	outpacket->startPtr = outpacket->packet.data();
	outputQueue.push(std::move(outpacket));
}

void MasterConn::sendRegister() {
	uint8_t *buff;

	downloadingFileNum = 0;
	downloadFD = kInvalidFD;

#ifndef METALOGGER
	// shadow master registration
	uint64_t metadataVersion = 0;
	if (state == State::kSynchronized) {
		metadataVersion = fs_getversion();
	}
	auto request = mltoma::registerShadow::build(SAUNAFS_VERSHEX, masterTimeout * 1000, metadataVersion);
	createPacket(std::move(request));
	return;
#endif

	if (lastLogVersion>0) {
		buff = createPacket(MLTOMA_REGISTER, 1 + 4 + 2 + 8);
		put8bit(&buff,2);
		put16bit(&buff,SAUNAFS_PACKAGE_VERSION_MAJOR);
		put8bit(&buff,SAUNAFS_PACKAGE_VERSION_MINOR);
		put8bit(&buff,SAUNAFS_PACKAGE_VERSION_MICRO);
		put16bit(&buff,masterTimeout);
		put64bit(&buff,lastLogVersion);
	} else {
		buff = createPacket(MLTOMA_REGISTER, 1 + 4 + 2);
		put8bit(&buff,1);
		put16bit(&buff,SAUNAFS_PACKAGE_VERSION_MAJOR);
		put8bit(&buff,SAUNAFS_PACKAGE_VERSION_MINOR);
		put8bit(&buff,SAUNAFS_PACKAGE_VERSION_MICRO);
		put16bit(&buff,masterTimeout);
	}
}

void MasterConn::sendMetaloggerConfig() {
	std::string config = cfg_yaml_string();
	auto request = mltoma::dumpConfiguration::build(config);
	createPacket(std::move(request));
}

void MasterConn::killSession() {
	if (mode != Mode::Free) {
		mode = Mode::Kill;
	}
}

void MasterConn::forceMetadataDownload() {
#ifndef METALOGGER
	state = MasterConn::State::kNone;
	fs_unload();
	restore_reset();
#endif
	lastLogVersion = 0;
	killSession();
}

void MasterConn::requestMetadataDump() {
	createPacket(mltoma::changelogApplyError::build(errorStatus));
	state = State::kDumpRequestPending;
	changelogApplyErrorTimeout.reset();
}

void MasterConn::handleChangelogApplyError(uint8_t status) {
	if (masterVersion <= saunafsVersion(2, 5, 0)) {
		safs_pretty_syslog(LOG_NOTICE, "Dropping in-memory metadata and starting download from master");
		forceMetadataDownload();
	} else {
		safs_pretty_syslog(LOG_NOTICE, "Waiting for master to produce up-to-date metadata image");
		errorStatus = status;
		requestMetadataDump();
	}
}

#ifndef METALOGGER
void MasterConn::sendMatoClPort() {
	static std::string previousPort;

	if (masterVersion < SAUNAFS_VERSION(2, 5, 5)) {
		return;
	}

	std::string portStr = cfg_getstring("MATOCL_LISTEN_PORT", "9421");
	static uint16_t port = 0;

	if (portStr != previousPort) {
		if (tcpresolve(nullptr, portStr.c_str(), nullptr, &port, false) < 0) {
			safs_pretty_syslog(LOG_WARNING, "Cannot resolve MATOCL_LISTEN_PORT: %s", portStr.c_str());
			return;
		}
		previousPort = portStr;
	}

	createPacket(mltoma::matoclport::build(port));
}

void MasterConn::onRegistered(const uint8_t *data, uint32_t length) {
	PacketVersion responseVersion{};
	deserializePacketVersionNoHeader(data, length, responseVersion);
	if (responseVersion == matoml::registerShadow::kStatusPacketVersion) {
		uint8_t status{};
		matoml::registerShadow::deserialize(data, length, status);
		safs_pretty_syslog(LOG_NOTICE, "Cannot register to master: %s", saunafs_error_string(status));
		mode = Mode::Kill;
	} else if (responseVersion == matoml::registerShadow::kResponsePacketVersion) {
		uint32_t incommingMasterVersion{};
		uint64_t masterMetadataVersion{};
		matoml::registerShadow::deserialize(data, length, incommingMasterVersion, masterMetadataVersion);
		masterVersion = incommingMasterVersion;
		sendMatoClPort();
		if ((state == State::kSynchronized) && (fs_getversion() != masterMetadataVersion)) {
			forceMetadataDownload();
		}
	} else {
		safs_pretty_syslog(LOG_NOTICE, "Unknown register response: #%u", unsigned(responseVersion));
	}
}
#endif

void MasterConn::metachangesLog(const uint8_t *data,uint32_t length) {
	if ((length == 1) && (data[0] == FORCE_LOG_ROTATE)) {
#ifdef METALOGGER
		// In metalogger rotates are forced by the master server. Shadow masters rotate changelogs
		// every hour -- when creating a new metadata file.
		changelog_rotate();
#endif /* #ifdef METALOGGER */
		return;
	}
	if (length<10) {
		safs_pretty_syslog(LOG_NOTICE,"MATOML_METACHANGES_LOG - wrong size (%" PRIu32 "/9+data)",length);
		mode = Mode::Kill;
		return;
	}
	if (data[0]!=0xFF) {
		safs_pretty_syslog(LOG_NOTICE,"MATOML_METACHANGES_LOG - wrong packet");
		mode = Mode::Kill;
		return;
	}
	if (data[length-1]!='\0') {
		safs_pretty_syslog(LOG_NOTICE,"MATOML_METACHANGES_LOG - invalid string");
		mode = Mode::Kill;
		return;
	}

	data++;
	uint64_t version = get64bit(&data);
	const char* changelogEntry = reinterpret_cast<const char*>(data);

	if ((lastLogVersion > 0) && (version != (lastLogVersion + 1))) {
		safs_pretty_syslog(LOG_WARNING, "some changes lost: [%" PRIu64 "-%" PRIu64 "], download metadata again",lastLogVersion,version-1);
		handleChangelogApplyError(SAUNAFS_ERROR_METADATAVERSIONMISMATCH);
		return;
	}

#ifndef METALOGGER
	if (state == State::kSynchronized) {
		std::string buf(": ");
		buf.append(changelogEntry);
		static char const network[] = "network";
		uint8_t status;
		if ((status = restore(network, version, buf.c_str(),
				RestoreRigor::kDontIgnoreAnyErrors)) != SAUNAFS_STATUS_OK) {
			safs_pretty_syslog(LOG_WARNING, "malformed changelog sent by the master server, can't apply it. status: %s",
					saunafs_error_string(status));
			handleChangelogApplyError(status);
			return;
		}
	}
#endif /* #ifndef METALOGGER */
	changelog(version, changelogEntry);
	lastLogVersion = version;
}

void MasterConn::endSession(const uint8_t* data, uint32_t length) {
	matoml::endSession::deserialize(data, length); // verify the empty packet
	safs_pretty_syslog(LOG_NOTICE, "Master server is terminating; closing the connection...");
	killSession();
}

int MasterConn::downloadEnd() {
	downloadingFileNum = 0;
	createPacket(MLTOMA_DOWNLOAD_END, 0);

	if (downloadFD >= 0) {
		if (::close(downloadFD) < 0) {
			safs_silent_errlog(LOG_NOTICE,"error closing metafile");
			downloadFD = kInvalidFD;
			return -1;
		}

		downloadFD = kInvalidFD;
	}

	return 0;
}

void MasterConn::downloadInit(uint8_t filenum) {
	if ((mode == Mode::Header || mode == Mode::Data) && downloadingFileNum == 0) {
		auto *ptr = createPacket(MLTOMA_DOWNLOAD_START, 1);
		put8bit(&ptr, filenum);
		downloadingFileNum = filenum;

		if (filenum == DOWNLOAD_METADATA_SFS) {
			state = State::kDownloading;
		}
	}
}

int MasterConn::metadataCheck(const std::string& name) {
	try {
		gMetadataBackend->getVersion(name);
		return 0;
	} catch (MetadataCheckException& ex) {
		safs_pretty_syslog(LOG_NOTICE, "Verification of the downloaded metadata file failed: %s", ex.what());
		return -1;
	}
}

void MasterConn::downloadNext() {
	uint8_t *ptr;
	uint8_t filenum;
	int64_t dltime;

	if (downloadOffset >= fileSize) {   // end of file
		filenum = downloadingFileNum;
		if (downloadEnd() < 0) {
			return;
		}
		dltime = eventloop_utime() - downloadStartTimeInMicroSeconds;
		if (dltime<=0) {
			dltime=1;
		}
		std::string changelogFilename_1 = changelogFilename + ".1";
		std::string changelogFilename_2 = changelogFilename + ".2";
		safs_pretty_syslog(LOG_NOTICE, "%s downloaded %" PRIu64 "B/%" PRIu64 ".%06" PRIu32 "s (%.3f MB/s)",
				(filenum == DOWNLOAD_METADATA_SFS) ? "metadata" :
				(filenum == DOWNLOAD_SESSIONS_SFS) ? "sessions" :
				(filenum == DOWNLOAD_CHANGELOG_SFS) ? changelogFilename_1.c_str() :
				(filenum == DOWNLOAD_CHANGELOG_SFS_1) ? changelogFilename_2.c_str() : "???",
				fileSize, dltime/1000000, (uint32_t)(dltime%1000000),
				(double)(fileSize) / (double)(dltime));
		if (filenum == DOWNLOAD_METADATA_SFS) {
			if (metadataCheck(metadataTmpFilename) == 0) {
				if (BackMetaCopies>0) {
					rotateFiles(metadataFilename, BackMetaCopies, 1);
				}
				if (rename(metadataTmpFilename.c_str(), metadataFilename.c_str()) < 0) {
					safs_pretty_syslog(LOG_NOTICE,"can't rename downloaded metadata - do it manually before next download");
				}
			}
			downloadInit(DOWNLOAD_CHANGELOG_SFS);
		} else if (filenum == DOWNLOAD_CHANGELOG_SFS) {
			if (::rename(changelogTmpFilename.c_str(), changelogFilename_1.c_str()) < 0) {
				safs_pretty_syslog(LOG_NOTICE,"can't rename downloaded changelog - do it manually before next download");
			}
			downloadInit(DOWNLOAD_CHANGELOG_SFS_1);
		} else if (filenum == DOWNLOAD_CHANGELOG_SFS_1) {
			if (::rename(changelogTmpFilename.c_str(), changelogFilename_2.c_str()) < 0) {
				safs_pretty_syslog(LOG_NOTICE,"can't rename downloaded changelog - do it manually before next download");
			}
			downloadInit(DOWNLOAD_SESSIONS_SFS);
		} else if (filenum == DOWNLOAD_SESSIONS_SFS) {
			if (::rename(sessionsTmpFilename.c_str(), sessionsFilename.c_str()) < 0) {
				safs_pretty_syslog(LOG_NOTICE,"can't rename downloaded sessions - do it manually before next download");
			} else {
#ifndef METALOGGER
				/*
				 * We can have other state if we are synchronized or we got changelog apply error
				 * during independent sessions download session.
				 */
				if (state == MasterConn::State::kDownloading) {
					try {
						fs_loadall();
						lastLogVersion = fs_getversion() - 1;
						safs_pretty_syslog(LOG_NOTICE, "synced at version = %" PRIu64, lastLogVersion);
						state = MasterConn::State::kSynchronized;
					} catch (Exception& ex) {
						safs_pretty_syslog(LOG_WARNING, "can't load downloaded metadata and changelogs: %s",
								ex.what());
						uint8_t status = ex.status();
						if (status == SAUNAFS_STATUS_OK) {
							// unknown error - tell the master to apply changelogs and hope that
							// all will be good
							status = SAUNAFS_ERROR_CHANGELOGINCONSISTENT;
						}
						handleChangelogApplyError(status);
					}
				}
#else /* #ifndef METALOGGER */
				state = MasterConn::State::kSynchronized;
#endif /* #else #ifndef METALOGGER */
			}
		}
	} else {        // send request for next data packet
		ptr = createPacket(MLTOMA_DOWNLOAD_DATA, 12);
		put64bit(&ptr, downloadOffset);
		if (fileSize - downloadOffset > kMetadataDownloadBlocksize) {
			put32bit(&ptr, kMetadataDownloadBlocksize);
		} else {
			put32bit(&ptr, fileSize - downloadOffset);
		}
	}
}

void MasterConn::downloadStart(const uint8_t *data, uint32_t length) {
	if (length != 1 && length != 8) {
		safs_pretty_syslog(LOG_NOTICE,"MATOML_DOWNLOAD_START - wrong size (%" PRIu32 "/1|8)",length);
		mode = Mode::Kill;
		return;
	}

	passert(data);

	if (length == 1) {
		downloadingFileNum = 0;
		safs_pretty_syslog(LOG_NOTICE,"download start error");
		return;
	}

#ifndef METALOGGER
	// We are a shadow master and we are going to do some changes in the data dir right now
	fs_erase_message_from_lockfile();
#endif

	fileSize = get64bit(&data);
	downloadOffset = 0;
	downloadRetryCnt = 0;
	downloadStartTimeInMicroSeconds = eventloop_utime();

	if (downloadingFileNum == DOWNLOAD_METADATA_SFS) {
		downloadFD = ::open(metadataTmpFilename.c_str(), O_WRONLY | O_TRUNC | O_CREAT, 0666);
	} else if (downloadingFileNum == DOWNLOAD_SESSIONS_SFS) {
		downloadFD = ::open(sessionsTmpFilename.c_str(), O_WRONLY | O_TRUNC | O_CREAT, 0666);
	} else if ((downloadingFileNum == DOWNLOAD_CHANGELOG_SFS)
			|| (downloadingFileNum == DOWNLOAD_CHANGELOG_SFS_1)) {
		downloadFD = ::open(changelogTmpFilename.c_str(), O_WRONLY | O_TRUNC | O_CREAT, 0666);
	} else {
		safs_pretty_syslog(LOG_NOTICE,"unexpected MATOML_DOWNLOAD_START packet");
		mode = Mode::Kill;
		return;
	}

	if (downloadFD < 0) {
		safs_silent_errlog(LOG_NOTICE,"error opening metafile");
		downloadEnd();
		return;
	}

	downloadNext();
}

void MasterConn::downloadData(const uint8_t *data,uint32_t length) {
	uint64_t offset;
	uint32_t leng;
	uint32_t crc;
	ssize_t ret;

	if (downloadFD < 0) {
		safs_pretty_syslog(LOG_NOTICE,"MATOML_DOWNLOAD_DATA - file not opened");
		mode = MasterConn::Mode::Kill;
		return;
	}

	if (length<16) {
		safs_pretty_syslog(LOG_NOTICE,"MATOML_DOWNLOAD_DATA - wrong size (%" PRIu32 "/16+data)",length);
		mode = MasterConn::Mode::Kill;
		return;
	}

	passert(data);
	offset = get64bit(&data);
	leng = get32bit(&data);
	crc = get32bit(&data);

	if (leng+16!=length) {
		safs_pretty_syslog(LOG_NOTICE,"MATOML_DOWNLOAD_DATA - wrong size (%" PRIu32 "/16+%" PRIu32 ")",length,leng);
		mode = MasterConn::Mode::Kill;
		return;
	}

	if (offset != downloadOffset) {
		safs_pretty_syslog(LOG_NOTICE,"MATOML_DOWNLOAD_DATA - unexpected file offset (%" PRIu64 "/%" PRIu64 ")",offset,downloadOffset);
		mode = MasterConn::Mode::Kill;
		return;
	}

	if (offset + leng > fileSize) {
		safs_pretty_syslog(LOG_NOTICE,"MATOML_DOWNLOAD_DATA - unexpected file size (%" PRIu64 "/%" PRIu64 ")",offset+leng,fileSize);
		mode = MasterConn::Mode::Kill;
		return;
	}

#ifdef SAUNAFS_HAVE_PWRITE
	ret = ::pwrite(downloadFD,data,leng,offset);
#else /* SAUNAFS_HAVE_PWRITE */
	::lseek(metafd,offset,SEEK_SET);
	ret = ::write(metafd,data,leng);
#endif /* SAUNAFS_HAVE_PWRITE */

	if (ret!=(ssize_t)leng) {
		safs_silent_errlog(LOG_NOTICE,"error writing metafile");
		if (downloadRetryCnt >= 5) {
			downloadEnd();
		} else {
			downloadRetryCnt++;
			downloadNext();
		}
		return;
	}

	if (crc != mycrc32(0,data,leng)) {
		safs_pretty_syslog(LOG_NOTICE,"metafile data crc error");
		if (downloadRetryCnt >= 5) {
			downloadEnd();
		} else {
			downloadRetryCnt++;
			downloadNext();
		}
		return;
	}

	if (::fsync(downloadFD) < 0) {
		safs_silent_errlog(LOG_NOTICE,"error syncing metafile");
		if (downloadRetryCnt >= 5) {
			downloadEnd();
		} else {
			downloadRetryCnt++;
			downloadNext();
		}
		return;
	}

	downloadOffset+=leng;
	downloadRetryCnt=0;

	downloadNext();
}

void MasterConn::changelogApplyError(const uint8_t *data, uint32_t length) {
	uint8_t status{};
	matoml::changelogApplyError::deserialize(data, length, status);
	safs_silent_syslog(LOG_DEBUG, "master.matoml_changelog_apply_error status: %u", status);

	if (status == SAUNAFS_STATUS_OK) {
		forceMetadataDownload();
	} else if (status == SAUNAFS_ERROR_DELAYED) {
		state = State::kLimbo;
		safs_pretty_syslog(LOG_NOTICE, "Master temporarily refused to produce a new metadata image");
	} else {
		state = State::kLimbo;
		safs_pretty_syslog(LOG_NOTICE, "Master failed to produce a new metadata image: %s", saunafs_error_string(status));
	}
}

void MasterConn::beforeClose() {
	if (downloadFD >= 0) {
		::close(downloadFD);
		downloadFD = MasterConn::kInvalidFD;
		::unlink(metadataTmpFilename.c_str());
		::unlink(sessionsTmpFilename.c_str());
		::unlink(changelogTmpFilename.c_str());
	}
}

void MasterConn::gotPacket(uint32_t type,const uint8_t *data,uint32_t length) {
	try {
		switch (type) {
			case ANTOAN_NOP:
				break;
			case ANTOAN_UNKNOWN_COMMAND: // for future use
				break;
			case ANTOAN_BAD_COMMAND_SIZE: // for future use
				break;
#ifndef METALOGGER
			case SAU_MATOML_REGISTER_SHADOW:
			    onRegistered(data, length);
			    break;
#endif
			case MATOML_METACHANGES_LOG:
				metachangesLog(data,length);
				break;
			case SAU_MATOML_END_SESSION:
			    endSession(data, length);
			    break;
			case MATOML_DOWNLOAD_START:
			    downloadStart(data, length);
			    break;
			case MATOML_DOWNLOAD_DATA:
			    downloadData(data, length);
			    break;
			case SAU_MATOML_CHANGELOG_APPLY_ERROR:
				changelogApplyError(data, length);
				break;
			default:
				safs_pretty_syslog(LOG_NOTICE,"got unknown message (type:%" PRIu32 ")",type);
				mode = Mode::Kill;
				break;
		}
	} catch (IncorrectDeserializationException& ex) {
		safs_pretty_syslog(LOG_NOTICE, "Packet 0x%" PRIX32 " - can't deserialize: %s", type, ex.what());
		mode = Mode::Kill;
	}
}

void MasterConn::terminate() {
	if (mode != Mode::Free) {
		tcpclose(sock);

		if (mode != Mode::Connecting) {
			inputPacket.packet.clear();

			while(!outputQueue.empty()) {
				outputQueue.pop();
			}
		}
	}
}

void MasterConn::onConnected() {
	tcpnodelay(sock);
	mode = Mode::Header;
	masterVersion = kInvalidMasterVersion;
	inputPacket.bytesLeft = kHeaderSize;
	inputPacket.startPtr = headerBuffer.data();
	outputQueue = std::queue<std::unique_ptr<PacketStruct>>();

	sendRegister();

#ifdef METALOGGER
	sendMetaloggerConfig();
#endif

	if (lastLogVersion == 0) {
		downloadInit(DOWNLOAD_METADATA_SFS);
	} else if (state == State::kDumpRequestPending) {
		requestMetadataDump();
	}

	lastRead = lastWrite = eventloop_time();
}

int MasterConn::initConnect() {
	int status;
	if (isMasterAddressValid == 0) {
		uint32_t mip,bip;
		uint16_t mport;
		if (tcpresolve(BindHost.c_str(), NULL, &bip, NULL, 1)>=0) {
			bindIP = bip;
		} else {
			bindIP = 0;
		}
		if (tcpresolve(MasterHost.c_str(), MasterPort.c_str(), &mip, &mport, 0)>=0) {
			masterIP = mip;
			masterPort = mport;
			isMasterAddressValid = 1;
		} else {
			safs_pretty_syslog(LOG_WARNING,
					"can't resolve master host/port (%s:%s)",
					MasterHost.c_str(), MasterPort.c_str());
			return -1;
		}
	}

	sock = tcpsocket();

	if (sock < 0) {
		safs_pretty_errlog(LOG_WARNING,"create socket, error");
		return -1;
	}

	if (tcpnonblock(sock) < 0) {
		safs_pretty_errlog(LOG_WARNING,"set nonblock, error");
		tcpclose(sock);
		sock = kInvalidFD;
		return -1;
	}

	if (bindIP > 0) {
		if (tcpnumbind(sock, bindIP, 0) < 0) {
			safs_pretty_errlog(LOG_WARNING,"can't bind socket to given ip");
			tcpclose(sock);
			sock = kInvalidFD;
			return -1;
		}
	}

	status = tcpnumconnect(sock, masterIP, masterPort);

	if (status < 0) {
		safs_pretty_errlog(LOG_WARNING,"connect failed, error");
		tcpclose(sock);
		sock = kInvalidFD;
		isMasterAddressValid = 0;
		return -1;
	}

	if (status == 0) {
		safs_pretty_syslog(LOG_NOTICE,"connected to Master immediately");
		onConnected();
	} else {
		mode = Mode::Connecting;
		safs_pretty_syslog_attempt(LOG_NOTICE,"connecting to Master");
	}

	return 0;
}

void MasterConn::connectTest() {
	auto status = tcpgetstatus(sock);

	if (status != SAUNAFS_STATUS_OK) {
		safs::log_warn("connection failed, error");
		tcpclose(sock);
		sock = kInvalidFD;
		mode = Mode::Free;
		isMasterAddressValid = 0;
		masterVersion = kInvalidMasterVersion;
	} else {
		safs::log_info("connected to Master");
		onConnected();
	}
}

void MasterConn::readFromSocket() {
	SignalLoopWatchdog watchdog;
	int32_t i;
	uint32_t type,size;
	const uint8_t *ptr;

	watchdog.start();

	while (mode != MasterConn::Mode::Kill) {
		i = ::read(sock, inputPacket.startPtr, inputPacket.bytesLeft);
		if (i==0) {
			safs_pretty_syslog(LOG_NOTICE,"connection was reset by Master");
			killSession();
			return;
		}
		if (i<0) {
			if (errno!=EAGAIN) {
				safs_silent_errlog(LOG_NOTICE,"read from Master error");
				killSession();
			}
			return;
		}

		inputPacket.startPtr += i;
		inputPacket.bytesLeft -= i;

		if (inputPacket.bytesLeft > 0) { return; }

		if (mode == Mode::Header) {
			ptr = headerBuffer.data() + kPacketTypeSize;
			size = get32bit(&ptr);

			if (size > 0) {
				if (size > kMaxPacketSize) {
					safs_pretty_syslog(LOG_WARNING,"Master packet too long (%" PRIu32 "/%u)",size,kMaxPacketSize);
					killSession();
					return;
				}

				inputPacket.packet.resize(size);
				passert(inputPacket.packet.data());
				inputPacket.bytesLeft = size;
				inputPacket.startPtr = inputPacket.packet.data();
				mode = Mode::Data;
				continue;
			}

			mode = Mode::Data;
		}

		if (mode == Mode::Data) {
			ptr = headerBuffer.data();
			type = get32bit(&ptr);
			size = get32bit(&ptr);

			mode = Mode::Header;
			inputPacket.bytesLeft = kHeaderSize;
			inputPacket.startPtr = headerBuffer.data();

			gotPacket(type, inputPacket.packet.data(), size);
			inputPacket.packet.clear();
		}

		if (watchdog.expired()) {
			break;
		}
	}
}

void MasterConn::writeToSocket() {
	SignalLoopWatchdog watchdog;
	PacketStruct *pack = nullptr;
	int32_t writtenBytes = 0;

	watchdog.start();

	for (;;) {
		if (outputQueue.empty()) { return; }

		pack = outputQueue.front().get();

		writtenBytes = ::write(sock, pack->startPtr, pack->bytesLeft);

		if (writtenBytes < 0) {
			if (errno != EAGAIN) {
				safs_silent_errlog(LOG_NOTICE, "write to Master error");
				mode = Mode::Kill;
			}
			return;
		}

		pack->startPtr += writtenBytes;
		pack->bytesLeft -= writtenBytes;

		if (pack->bytesLeft > 0) { return; }

		outputQueue.pop();

		if (watchdog.expired()) {
			break;
		}
	}
}

void MasterConn::pollDesc(std::vector<pollfd> &pdesc) {
	pollDescPos = kInvalidPollDescPos;

	if (mode == Mode::Free || sock < 0) {
		return;
	}

	if (mode == Mode::Header || mode == Mode::Data) {
		pdesc.push_back({sock, POLLIN, 0});
		pollDescPos = static_cast<int32_t>(pdesc.size() - 1);
	}

	if (((mode == Mode::Header || mode == Mode::Data) &&
	     !outputQueue.empty()) ||
	    (mode == Mode::Connecting)) {
		if (pollDescPos >= 0) {
			pdesc[pollDescPos].events |= POLLOUT;
		} else {
			pdesc.push_back({sock, POLLOUT, 0});
			pollDescPos = static_cast<int32_t>(pdesc.size() - 1);
		}
	}
}

void MasterConn::serve(const std::vector<pollfd> &pdesc) {
	uint32_t now = eventloop_time();

	if (pollDescPos>=0 && (pdesc[pollDescPos].revents & (POLLHUP | POLLERR))) {
		if (mode == Mode::Connecting) {
			connectTest();
		} else {
			mode = Mode::Kill;
		}
	}

	if (mode == Mode::Connecting) {
		if (sock >= 0 && pollDescPos >= 0 && (pdesc[pollDescPos].revents & POLLOUT)) { // FD_ISSET(eptr->sock,wset)) {
			connectTest();
		}
	} else {
		if (pollDescPos >= 0) {
			if ((mode == Mode::Header || mode == Mode::Data) && (pdesc[pollDescPos].revents & POLLIN)) { // FD_ISSET(eptr->sock,rset)) {
				lastRead = now;
				readFromSocket();
			}
			if ((mode == Mode::Header || mode == Mode::Data) && (pdesc[pollDescPos].revents & POLLOUT)) { // FD_ISSET(eptr->sock,wset)) {
				lastWrite = now;
				writeToSocket();
			}
			if ((mode == Mode::Header || mode == Mode::Data) && lastRead + masterTimeout < now) {
				mode = Mode::Kill;
			}
			if ((mode == Mode::Header || mode == Mode::Data) && lastWrite + (masterTimeout / 3) < now && outputQueue.empty()) {
				createPacket(ANTOAN_NOP, 0);
			}
		}
	}

	if (mode == Mode::Kill) {
		beforeClose();
		tcpclose(sock);
		sock = kInvalidFD;

		inputPacket.packet.clear();

		while (!outputQueue.empty()) {
			outputQueue.pop();
		}

		mode = Mode::Free;
		masterVersion = kInvalidMasterVersion;
	}
}

void MasterConn::reconnect() {
	if (mode == Mode::Free && gExitingStatus == ExitingStatus::kRunning) {
		initConnect();
	}

	if ((mode == Mode::Header || mode == Mode::Data) && state == State::kLimbo) {
		if (changelogApplyErrorTimeout.expired()) {
			requestMetadataDump();
		}
	}
}

void MasterConn::becomeMaster() {
	eventloop_timeunregister(sessionsDownloadInitHandle);
	eventloop_timeunregister(changelogFlushHandle);
}

void MasterConn::loadConfig() {
	MasterHost = cfg_getstring("MASTER_HOST","sfsmaster");
	MasterPort = cfg_getstring("MASTER_PORT","9419");
	BindHost = cfg_getstring("BIND_HOST","*");
	masterTimeout = cfg_getuint32("MASTER_TIMEOUT",60);
	BackMetaCopies = cfg_getuint32("BACK_META_KEEP_PREVIOUS",3);

	masterTimeout = std::min(masterTimeout, kMaxMasterTimeout);
	masterTimeout = std::max<uint32_t>(masterTimeout, kMinMasterTimeout);
}

void MasterConn::reload() {
	std::string newMasterHost = cfg_getstring("MASTER_HOST","sfsmaster");
	std::string newMasterPort = cfg_getstring("MASTER_PORT","9419");
	std::string newBindHost = cfg_getstring("BIND_HOST","*");

	if (newMasterHost != MasterHost || newMasterPort != MasterPort || newBindHost != BindHost) {
		MasterHost = newMasterHost;
		MasterPort = newMasterPort;
		BindHost = newBindHost;
		isMasterAddressValid = 0;
		if (mode != Mode::Free) {
			mode = Mode::Kill;
		}
	}

	masterTimeout = cfg_getuint32("MASTER_TIMEOUT",60);
	BackMetaCopies = cfg_getuint32("BACK_META_KEEP_PREVIOUS",3);
	auto reconnectionDelay = cfg_getuint32("MASTER_RECONNECTION_DELAY",1);

	masterTimeout = std::min(masterTimeout, MasterConn::kMaxMasterTimeout);
	masterTimeout = std::max<uint32_t>(masterTimeout, MasterConn::kMinMasterTimeout);
	BackMetaCopies = std::min(BackMetaCopies, MasterConn::kMaxBackMetaCopies);

#ifdef METALOGGER
	auto metadataDownloadFreq = cfg_getuint32("META_DOWNLOAD_FREQ", 24);
	if (metadataDownloadFreq > (changelog_get_back_logs_config_value() / 2)) {
		metadataDownloadFreq = (changelog_get_back_logs_config_value() / 2);
	}
#endif /* #ifdef METALOGGER */

	eventloop_timechange(reconnect_hook,TIMEMODE_RUN_LATE,reconnectionDelay,0);
#ifdef METALOGGER
	eventloop_timechange(download_hook,TIMEMODE_RUN_LATE,metadataDownloadFreq*3600,630);
#endif /* #ifndef METALOGGER */

#ifndef METALOGGER
	sendMatoClPort();
#endif /* #ifndef METALOGGER */
}

namespace {  // Make clang-tidy happy

void masterconn_sessionsdownloadinit(void) {
	if (gMasterConn->state == MasterConn::State::kSynchronized) {
		gMasterConn->downloadInit(DOWNLOAD_SESSIONS_SFS);
	}
}

void masterconn_wantexit(void) {
	if (gMasterConn != nullptr) {
		gMasterConn->killSession();
	}
}

int masterconn_canexit(void) {
	return static_cast<int>(gMasterConn != nullptr ||
	                        gMasterConn->mode == MasterConn::Mode::Free);
}

void masterconn_desc(std::vector<pollfd> &pdesc) {
	if (gMasterConn != nullptr) {
		gMasterConn->pollDesc(pdesc);
	}
}

void masterconn_serve(const std::vector<pollfd> &pdesc) {
	if (gMasterConn != nullptr) {
		gMasterConn->serve(pdesc);
	}
}

void masterconn_reconnect(void) {
	if (gMasterConn != nullptr) {
		gMasterConn->reconnect();
	}
}

void masterconn_term(void) {
	if (gMasterConn != nullptr) {
		gMasterConn->terminate();
		delete gMasterConn;
		gMasterConn = nullptr;
	}
}

#ifndef METALOGGER

void masterconn_become_master() {
	if (gMasterConn != nullptr) {
		gMasterConn->becomeMaster();
		masterconn_term();
	}
}

#else  // #ifndef METALOGGER

void masterconn_metadownloadinit(void) {
	if (gMasterConn != nullptr) {
		gMasterConn->downloadInit(DOWNLOAD_METADATA_SFS);
	}
}

#endif  // #else #ifndef METALOGGER

void masterconn_reload(void) {
	if (gMasterConn != nullptr) {
		gMasterConn->reload();
	}
}

}  // namespace

int masterconn_init(void) {
#ifndef METALOGGER
	if (metadataserver::getPersonality() != metadataserver::Personality::kShadow) {
		return 0;
	}
#endif /* #ifndef METALOGGER */

	// Create the instance for Shadows and Metaloggers only
	gMasterConn = new MasterConn();
	passert(gMasterConn);

	// Could be multiple connections in the future, so let's use a pointer
	auto *eptr = gMasterConn;

	auto reconnectionDelay = cfg_getuint32("MASTER_RECONNECTION_DELAY", 1);
	eptr->loadConfig();

#ifdef METALOGGER
	changelog_init(kChangelogMlFilename, 5, 1000); // may throw
	changelog_disable_flush(); // metalogger does it once a second
	uint32_t metadataDownloadFreq;
	metadataDownloadFreq = cfg_getuint32("META_DOWNLOAD_FREQ",24);
	if (metadataDownloadFreq > (changelog_get_back_logs_config_value() / 2)) {
		metadataDownloadFreq = (changelog_get_back_logs_config_value() / 2);
	}
#endif /* #ifdef METALOGGER */

#ifdef METALOGGER
	gMetadataBackend->changelogsMigrateFrom_1_6_29("changelog_ml");
	eptr->lastLogVersion = gMetadataBackend->findLastLogVersion();
#endif /* #ifdef METALOGGER */
	if (eptr->initConnect() < 0) {
		return -1;
	}
	eventloop_pollregister(masterconn_desc, masterconn_serve);
	eptr->reconnect_hook = eventloop_timeregister(TIMEMODE_RUN_LATE,reconnectionDelay,0,masterconn_reconnect);
#ifdef METALOGGER
	eptr->download_hook = eventloop_timeregister(TIMEMODE_RUN_LATE,metadataDownloadFreq*3600,630,masterconn_metadownloadinit);
#endif /* #ifdef METALOGGER */
	eventloop_destructregister(masterconn_term);
	eventloop_reloadregister(masterconn_reload);
	eventloop_wantexitregister(masterconn_wantexit);
	eventloop_canexitregister(masterconn_canexit);
#ifndef METALOGGER
	metadataserver::registerFunctionCalledOnPromotion(masterconn_become_master);
#endif
	eptr->sessionsDownloadInitHandle = eventloop_timeregister(TIMEMODE_RUN_LATE,60,0,masterconn_sessionsdownloadinit);
	eptr->changelogFlushHandle = eventloop_timeregister(TIMEMODE_RUN_LATE,1,0,changelog_flush);
	return 0;
}

bool masterconn_is_connected() {
	MasterConn *eptr = gMasterConn;
	return (eptr != nullptr
			&& (eptr->mode == MasterConn::Mode::Header || eptr->mode == MasterConn::Mode::Data) // socket is connected
			&& eptr->masterVersion > MasterConn::kInvalidMasterVersion // registration was successful
	);
}
