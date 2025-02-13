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
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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

/// Block size for metadata download (1 MB).
constexpr uint32_t kMetadataDownloadBlocksize = 1000000U;

/// Safety measure about expected max packet size.
constexpr uint32_t kMaxPacketSize = 1500000U;

struct PacketStruct {
	struct PacketStruct *next;
	uint8_t *startPtr;
	uint32_t bytesLeft;
	uint8_t *packet;
};

/// Represents a connection to the master server.
/// Holds the connection state and the data that is being sent or received.
struct MasterConn {
	static constexpr uint8_t kHeaderSize = 8;
	static constexpr uint8_t kChangeLogApplyErrorTimeout = 10;
	static constexpr int kInvalidFD = -1;
	static constexpr uint32_t kInvalidMasterVersion = 0;

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

	Mode mode{};  ///< Current connection mode.
	State state{State::kNone};  ///< Current synchonization state.

	int sock{kInvalidFD};  ///< Socket descriptor.
	int32_t pollDescPos{};  ///< Position in the poll descriptors array.
	uint32_t lastRead{};  ///< Timestamp of the last read operation.
	uint32_t lastWrite{};  ///< Timestamp of the last write operation.

	/// Master version in the other end. Known after registration.
	uint32_t masterVersion{kInvalidMasterVersion};

	std::array<uint8_t, kHeaderSize> headerBuffer{};  ///< Buffer for headers.
	PacketStruct inputPacket{};  ///< Structure for the input packet.
	// To be replaced by a container.
	PacketStruct *outputHead{};  ///< Head of the output packet queue.
	PacketStruct **outputTail{};  ///< Tail of the output packet queue.

	uint32_t bindIP{};  ///< IP address to bind the socket.
	uint32_t masterIP{};  ///< IP address of the master server.
	uint16_t masterPort{};  ///< Port of the master server.
	uint8_t isMasterAddressValid{};  /// Known after (re)connections.

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
};

static MasterConn *gMasterConn = nullptr;

// from config
static uint32_t BackMetaCopies;
static std::string MasterHost;
static std::string MasterPort;
static std::string BindHost;
static uint32_t Timeout;
static void* reconnect_hook;
#ifdef METALOGGER
static void* download_hook;
#endif /* #ifdef METALOGGER */
static uint64_t lastlogversion=0;

static uint32_t stats_bytesout=0;
static uint32_t stats_bytesin=0;

void masterconn_stats(uint32_t *bin,uint32_t *bout) {
	*bin = stats_bytesin;
	*bout = stats_bytesout;
	stats_bytesin = 0;
	stats_bytesout = 0;
}

#ifdef METALOGGER
void masterconn_findlastlogversion(void) {
	struct stat st;
	uint8_t buff[32800];    // 32800 = 32768 + 32
	uint64_t size;
	uint32_t buffpos;
	uint64_t lastnewline;
	int fd;
	lastlogversion = 0;

	if ((stat(kMetadataMlFilename, &st) < 0) || (st.st_size == 0) || ((st.st_mode & S_IFMT) != S_IFREG)) {
		return;
	}

	fd = open(kChangelogMlFilename, O_RDWR);
	if (fd<0) {
		return;
	}
	fstat(fd,&st);
	size = st.st_size;
	memset(buff,0,32);
	lastnewline = 0;
	while (size>0 && size+200000>(uint64_t)(st.st_size)) {
		if (size>32768) {
			memcpy(buff+32768,buff,32);
			size-=32768;
			lseek(fd,size,SEEK_SET);
			if (read(fd,buff,32768)!=32768) {
				lastlogversion = 0;
				close(fd);
				return;
			}
			buffpos = 32768;
		} else {
			memmove(buff+size,buff,32);
			lseek(fd,0,SEEK_SET);
			if (read(fd,buff,size)!=(ssize_t)size) {
				lastlogversion = 0;
				close(fd);
				return;
			}
			buffpos = size;
			size = 0;
		}
		// size = position in file of first byte in buff
		// buffpos = position of last byte in buff to search
		while (buffpos>0) {
			buffpos--;
			if (buff[buffpos]=='\n') {
				if (lastnewline==0) {
					lastnewline = size + buffpos;
				} else {
					if (lastnewline+1 != (uint64_t)(st.st_size)) {  // garbage at the end of file - truncate
						if (ftruncate(fd,lastnewline+1)<0) {
							lastlogversion = 0;
							close(fd);
							return;
						}
					}
					buffpos++;
					while (buffpos<32800 && buff[buffpos]>='0' && buff[buffpos]<='9') {
						lastlogversion *= 10;
						lastlogversion += buff[buffpos]-'0';
						buffpos++;
					}
					if (buffpos==32800 || buff[buffpos]!=':') {
						lastlogversion = 0;
					}
					close(fd);
					return;
				}
			}
		}
	}
	close(fd);
	return;
}
#endif /* #ifdef METALOGGER */

uint8_t* masterconn_createpacket(MasterConn *eptr,uint32_t type,uint32_t size) {
	PacketStruct *outpacket;
	uint8_t *ptr;
	uint32_t psize;

	outpacket=(PacketStruct*)malloc(sizeof(PacketStruct));
	passert(outpacket);
	psize = size+8;
	outpacket->packet= (uint8_t*) malloc(psize);
	passert(outpacket->packet);
	outpacket->bytesLeft = psize;
	ptr = outpacket->packet;
	put32bit(&ptr,type);
	put32bit(&ptr,size);
	outpacket->startPtr = (uint8_t*)(outpacket->packet);
	outpacket->next = NULL;
	*(eptr->outputTail) = outpacket;
	eptr->outputTail = &(outpacket->next);
	return ptr;
}

void masterconn_createpacket(MasterConn *eptr, std::vector<uint8_t> data) {
	PacketStruct *outpacket = (PacketStruct*) malloc(sizeof(PacketStruct));
	passert(outpacket);
	outpacket->packet = (uint8_t*) malloc(data.size());
	passert(outpacket->packet);
	memcpy(outpacket->packet, data.data(), data.size());
	outpacket->bytesLeft = data.size();
	outpacket->startPtr = outpacket->packet;
	outpacket->next = nullptr;
	*(eptr->outputTail) = outpacket;
	eptr->outputTail = &(outpacket->next);
}

void masterconn_sendregister(MasterConn *eptr) {
	uint8_t *buff;

	eptr->downloadingFileNum = 0;
	eptr->downloadFD = MasterConn::kInvalidFD;

#ifndef METALOGGER
	// shadow master registration
	uint64_t metadataVersion = 0;
	if (eptr->state == MasterConn::State::kSynchronized) {
		metadataVersion = fs_getversion();
	}
	auto request = mltoma::registerShadow::build(SAUNAFS_VERSHEX, Timeout * 1000, metadataVersion);
	masterconn_createpacket(eptr, std::move(request));
	return;
#endif

	if (lastlogversion>0) {
		buff = masterconn_createpacket(eptr,MLTOMA_REGISTER,1+4+2+8);
		put8bit(&buff,2);
		put16bit(&buff,SAUNAFS_PACKAGE_VERSION_MAJOR);
		put8bit(&buff,SAUNAFS_PACKAGE_VERSION_MINOR);
		put8bit(&buff,SAUNAFS_PACKAGE_VERSION_MICRO);
		put16bit(&buff,Timeout);
		put64bit(&buff,lastlogversion);
	} else {
		buff = masterconn_createpacket(eptr,MLTOMA_REGISTER,1+4+2);
		put8bit(&buff,1);
		put16bit(&buff,SAUNAFS_PACKAGE_VERSION_MAJOR);
		put8bit(&buff,SAUNAFS_PACKAGE_VERSION_MINOR);
		put8bit(&buff,SAUNAFS_PACKAGE_VERSION_MICRO);
		put16bit(&buff,Timeout);
	}
}

void masterconn_send_metalogger_config(MasterConn *eptr) {
	std::string config = cfg_yaml_string();
	auto request = mltoma::dumpConfiguration::build(config);
	masterconn_createpacket(eptr, std::move(request));
}

namespace {
#ifdef METALOGGER
	const std::string metadataFilename = kMetadataMlFilename;
	const std::string metadataTmpFilename = kMetadataMlTmpFilename;
	const std::string changelogFilename = kChangelogMlFilename;
	const std::string changelogTmpFilename = kChangelogMlTmpFilename;
	const std::string sessionsFilename = kSessionsMlFilename;
	const std::string sessionsTmpFilename = kSessionsMlTmpFilename;
#else /* #ifdef METALOGGER */
	const std::string metadataFilename = kMetadataFilename;
	const std::string metadataTmpFilename = kMetadataTmpFilename;
	const std::string changelogFilename = kChangelogFilename;
	const std::string changelogTmpFilename = kChangelogTmpFilename;
	const std::string sessionsFilename = kSessionsFilename;
	const std::string sessionsTmpFilename = kSessionsTmpFilename;
#endif /* #else #ifdef METALOGGER */
}

void masterconn_kill_session(MasterConn* eptr) {
	if (eptr->mode != MasterConn::Mode::Free) {
		eptr->mode = MasterConn::Mode::Kill;
	}
}

void masterconn_force_metadata_download(MasterConn* eptr) {
#ifndef METALOGGER
	eptr->state = MasterConn::State::kNone;
	fs_unload();
	restore_reset();
#endif
	lastlogversion = 0;
	masterconn_kill_session(eptr);
}

void masterconn_request_metadata_dump(MasterConn* eptr) {
	masterconn_createpacket(eptr, mltoma::changelogApplyError::build(eptr->errorStatus));
	eptr->state = MasterConn::State::kDumpRequestPending;
	eptr->changelogApplyErrorTimeout.reset();
}

void masterconn_handle_changelog_apply_error(MasterConn* eptr, uint8_t status) {
	if (eptr->masterVersion <= saunafsVersion(2, 5, 0)) {
		safs_pretty_syslog(LOG_NOTICE, "Dropping in-memory metadata and starting download from master");
		masterconn_force_metadata_download(eptr);
	} else {
		safs_pretty_syslog(LOG_NOTICE, "Waiting for master to produce up-to-date metadata image");
		eptr->errorStatus = status;
		masterconn_request_metadata_dump(eptr);
	}
}

#ifndef METALOGGER
void masterconn_int_send_matoclport(MasterConn* eptr) {
	static std::string previousPort = "";
	if (eptr->masterVersion < SAUNAFS_VERSION(2, 5, 5)) {
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
	masterconn_createpacket(eptr, mltoma::matoclport::build(port));
}

void masterconn_registered(MasterConn *eptr, const uint8_t *data, uint32_t length) {
	PacketVersion responseVersion;
	deserializePacketVersionNoHeader(data, length, responseVersion);
	if (responseVersion == matoml::registerShadow::kStatusPacketVersion) {
		uint8_t status;
		matoml::registerShadow::deserialize(data, length, status);
		safs_pretty_syslog(LOG_NOTICE, "Cannot register to master: %s", saunafs_error_string(status));
		eptr->mode = MasterConn::Mode::Kill;
	} else if (responseVersion == matoml::registerShadow::kResponsePacketVersion) {
		uint32_t masterVersion;
		uint64_t masterMetadataVersion;
		matoml::registerShadow::deserialize(data, length, masterVersion, masterMetadataVersion);
		eptr->masterVersion = masterVersion;
		masterconn_int_send_matoclport(eptr);
		if ((eptr->state == MasterConn::State::kSynchronized) && (fs_getversion() != masterMetadataVersion)) {
			masterconn_force_metadata_download(eptr);
		}
	} else {
		safs_pretty_syslog(LOG_NOTICE, "Unknown register response: #%u", unsigned(responseVersion));
	}
}
#endif

void masterconn_metachanges_log(MasterConn *eptr,const uint8_t *data,uint32_t length) {
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
		eptr->mode = MasterConn::Mode::Kill;
		return;
	}
	if (data[0]!=0xFF) {
		safs_pretty_syslog(LOG_NOTICE,"MATOML_METACHANGES_LOG - wrong packet");
		eptr->mode = MasterConn::Mode::Kill;
		return;
	}
	if (data[length-1]!='\0') {
		safs_pretty_syslog(LOG_NOTICE,"MATOML_METACHANGES_LOG - invalid string");
		eptr->mode = MasterConn::Mode::Kill;
		return;
	}

	data++;
	uint64_t version = get64bit(&data);
	const char* changelogEntry = reinterpret_cast<const char*>(data);

	if ((lastlogversion > 0) && (version != (lastlogversion + 1))) {
		safs_pretty_syslog(LOG_WARNING, "some changes lost: [%" PRIu64 "-%" PRIu64 "], download metadata again",lastlogversion,version-1);
		masterconn_handle_changelog_apply_error(eptr, SAUNAFS_ERROR_METADATAVERSIONMISMATCH);
		return;
	}

#ifndef METALOGGER
	if (eptr->state == MasterConn::State::kSynchronized) {
		std::string buf(": ");
		buf.append(changelogEntry);
		static char const network[] = "network";
		uint8_t status;
		if ((status = restore(network, version, buf.c_str(),
				RestoreRigor::kDontIgnoreAnyErrors)) != SAUNAFS_STATUS_OK) {
			safs_pretty_syslog(LOG_WARNING, "malformed changelog sent by the master server, can't apply it. status: %s",
					saunafs_error_string(status));
			masterconn_handle_changelog_apply_error(eptr, status);
			return;
		}
	}
#endif /* #ifndef METALOGGER */
	changelog(version, changelogEntry);
	lastlogversion = version;
}

void masterconn_end_session(MasterConn *eptr, const uint8_t* data, uint32_t length) {
	matoml::endSession::deserialize(data, length); // verify the empty packet
	safs_pretty_syslog(LOG_NOTICE, "Master server is terminating; closing the connection...");
	masterconn_kill_session(eptr);
}

int masterconn_download_end(MasterConn *eptr) {
	eptr->downloadingFileNum=0;
	masterconn_createpacket(eptr,MLTOMA_DOWNLOAD_END,0);
	if (eptr->downloadFD>=0) {
		if (close(eptr->downloadFD)<0) {
			safs_silent_errlog(LOG_NOTICE,"error closing metafile");
			eptr->downloadFD = MasterConn::kInvalidFD;
			return -1;
		}
		eptr->downloadFD = MasterConn::kInvalidFD;
	}
	return 0;
}

void masterconn_download_init(MasterConn *eptr,uint8_t filenum) {
	uint8_t *ptr;
	if ((eptr->mode==MasterConn::Mode::Header || eptr->mode==MasterConn::Mode::Data) && eptr->downloadingFileNum==0) {
		ptr = masterconn_createpacket(eptr,MLTOMA_DOWNLOAD_START,1);
		put8bit(&ptr,filenum);
		eptr->downloadingFileNum = filenum;
		if (filenum == DOWNLOAD_METADATA_SFS) {
			gMasterConn->state = MasterConn::State::kDownloading;
		}
	}
}

void masterconn_metadownloadinit() {
	masterconn_download_init(gMasterConn, DOWNLOAD_METADATA_SFS);
}

void masterconn_sessionsdownloadinit(void) {
	if (gMasterConn->state == MasterConn::State::kSynchronized) {
		masterconn_download_init(gMasterConn, DOWNLOAD_SESSIONS_SFS);
	}
}

int masterconn_metadata_check(const std::string& name) {
	try {
		gMetadataBackend->getVersion(name);
		return 0;
	} catch (MetadataCheckException& ex) {
		safs_pretty_syslog(LOG_NOTICE, "Verification of the downloaded metadata file failed: %s", ex.what());
		return -1;
	}
}

void masterconn_download_next(MasterConn *eptr) {
	uint8_t *ptr;
	uint8_t filenum;
	int64_t dltime;
	if (eptr->downloadOffset>=eptr->fileSize) {   // end of file
		filenum = eptr->downloadingFileNum;
		if (masterconn_download_end(eptr)<0) {
			return;
		}
		dltime = eventloop_utime()-eptr->downloadStartTimeInMicroSeconds;
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
				eptr->fileSize, dltime/1000000, (uint32_t)(dltime%1000000),
				(double)(eptr->fileSize) / (double)(dltime));
		if (filenum == DOWNLOAD_METADATA_SFS) {
			if (masterconn_metadata_check(metadataTmpFilename) == 0) {
				if (BackMetaCopies>0) {
					rotateFiles(metadataFilename, BackMetaCopies, 1);
				}
				if (rename(metadataTmpFilename.c_str(), metadataFilename.c_str()) < 0) {
					safs_pretty_syslog(LOG_NOTICE,"can't rename downloaded metadata - do it manually before next download");
				}
			}
			masterconn_download_init(eptr, DOWNLOAD_CHANGELOG_SFS);
		} else if (filenum == DOWNLOAD_CHANGELOG_SFS) {
			if (rename(changelogTmpFilename.c_str(), changelogFilename_1.c_str()) < 0) {
				safs_pretty_syslog(LOG_NOTICE,"can't rename downloaded changelog - do it manually before next download");
			}
			masterconn_download_init(eptr, DOWNLOAD_CHANGELOG_SFS_1);
		} else if (filenum == DOWNLOAD_CHANGELOG_SFS_1) {
			if (rename(changelogTmpFilename.c_str(), changelogFilename_2.c_str()) < 0) {
				safs_pretty_syslog(LOG_NOTICE,"can't rename downloaded changelog - do it manually before next download");
			}
			masterconn_download_init(eptr, DOWNLOAD_SESSIONS_SFS);
		} else if (filenum == DOWNLOAD_SESSIONS_SFS) {
			if (rename(sessionsTmpFilename.c_str(), sessionsFilename.c_str()) < 0) {
				safs_pretty_syslog(LOG_NOTICE,"can't rename downloaded sessions - do it manually before next download");
			} else {
#ifndef METALOGGER
				/*
				 * We can have other state if we are synchronized or we got changelog apply error
				 * during independent sessions download session.
				 */
				if (eptr->state == MasterConn::State::kDownloading) {
					try {
						fs_loadall();
						lastlogversion = fs_getversion() - 1;
						safs_pretty_syslog(LOG_NOTICE, "synced at version = %" PRIu64, lastlogversion);
						eptr->state = MasterConn::State::kSynchronized;
					} catch (Exception& ex) {
						safs_pretty_syslog(LOG_WARNING, "can't load downloaded metadata and changelogs: %s",
								ex.what());
						uint8_t status = ex.status();
						if (status == SAUNAFS_STATUS_OK) {
							// unknown error - tell the master to apply changelogs and hope that
							// all will be good
							status = SAUNAFS_ERROR_CHANGELOGINCONSISTENT;
						}
						masterconn_handle_changelog_apply_error(eptr, status);
					}
				}
#else /* #ifndef METALOGGER */
				eptr->state = MasterConn::State::kSynchronized;
#endif /* #else #ifndef METALOGGER */
			}
		}
	} else {        // send request for next data packet
		ptr = masterconn_createpacket(eptr,MLTOMA_DOWNLOAD_DATA,12);
		put64bit(&ptr,eptr->downloadOffset);
		if (eptr->fileSize-eptr->downloadOffset>kMetadataDownloadBlocksize) {
			put32bit(&ptr,kMetadataDownloadBlocksize);
		} else {
			put32bit(&ptr,eptr->fileSize-eptr->downloadOffset);
		}
	}
}

void masterconn_download_start(MasterConn *eptr,const uint8_t *data,uint32_t length) {
	if (length!=1 && length!=8) {
		safs_pretty_syslog(LOG_NOTICE,"MATOML_DOWNLOAD_START - wrong size (%" PRIu32 "/1|8)",length);
		eptr->mode = MasterConn::Mode::Kill;
		return;
	}
	passert(data);
	if (length==1) {
		eptr->downloadingFileNum=0;
		safs_pretty_syslog(LOG_NOTICE,"download start error");
		return;
	}
#ifndef METALOGGER
	// We are a shadow master and we are going to do some changes in the data dir right now
	fs_erase_message_from_lockfile();
#endif
	eptr->fileSize = get64bit(&data);
	eptr->downloadOffset = 0;
	eptr->downloadRetryCnt = 0;
	eptr->downloadStartTimeInMicroSeconds = eventloop_utime();
	if (eptr->downloadingFileNum == DOWNLOAD_METADATA_SFS) {
		eptr->downloadFD = open(metadataTmpFilename.c_str(), O_WRONLY | O_TRUNC | O_CREAT, 0666);
	} else if (eptr->downloadingFileNum == DOWNLOAD_SESSIONS_SFS) {
		eptr->downloadFD = open(sessionsTmpFilename.c_str(), O_WRONLY | O_TRUNC | O_CREAT, 0666);
	} else if ((eptr->downloadingFileNum == DOWNLOAD_CHANGELOG_SFS)
			|| (eptr->downloadingFileNum == DOWNLOAD_CHANGELOG_SFS_1)) {
		eptr->downloadFD = open(changelogTmpFilename.c_str(), O_WRONLY | O_TRUNC | O_CREAT, 0666);
	} else {
		safs_pretty_syslog(LOG_NOTICE,"unexpected MATOML_DOWNLOAD_START packet");
		eptr->mode = MasterConn::Mode::Kill;
		return;
	}
	if (eptr->downloadFD<0) {
		safs_silent_errlog(LOG_NOTICE,"error opening metafile");
		masterconn_download_end(eptr);
		return;
	}
	masterconn_download_next(eptr);
}

void masterconn_download_data(MasterConn *eptr,const uint8_t *data,uint32_t length) {
	uint64_t offset;
	uint32_t leng;
	uint32_t crc;
	ssize_t ret;
	if (eptr->downloadFD<0) {
		safs_pretty_syslog(LOG_NOTICE,"MATOML_DOWNLOAD_DATA - file not opened");
		eptr->mode = MasterConn::Mode::Kill;
		return;
	}
	if (length<16) {
		safs_pretty_syslog(LOG_NOTICE,"MATOML_DOWNLOAD_DATA - wrong size (%" PRIu32 "/16+data)",length);
		eptr->mode = MasterConn::Mode::Kill;
		return;
	}
	passert(data);
	offset = get64bit(&data);
	leng = get32bit(&data);
	crc = get32bit(&data);
	if (leng+16!=length) {
		safs_pretty_syslog(LOG_NOTICE,"MATOML_DOWNLOAD_DATA - wrong size (%" PRIu32 "/16+%" PRIu32 ")",length,leng);
		eptr->mode = MasterConn::Mode::Kill;
		return;
	}
	if (offset!=eptr->downloadOffset) {
		safs_pretty_syslog(LOG_NOTICE,"MATOML_DOWNLOAD_DATA - unexpected file offset (%" PRIu64 "/%" PRIu64 ")",offset,eptr->downloadOffset);
		eptr->mode = MasterConn::Mode::Kill;
		return;
	}
	if (offset+leng>eptr->fileSize) {
		safs_pretty_syslog(LOG_NOTICE,"MATOML_DOWNLOAD_DATA - unexpected file size (%" PRIu64 "/%" PRIu64 ")",offset+leng,eptr->fileSize);
		eptr->mode = MasterConn::Mode::Kill;
		return;
	}
#ifdef SAUNAFS_HAVE_PWRITE
	ret = pwrite(eptr->downloadFD,data,leng,offset);
#else /* SAUNAFS_HAVE_PWRITE */
	lseek(eptr->metafd,offset,SEEK_SET);
	ret = write(eptr->metafd,data,leng);
#endif /* SAUNAFS_HAVE_PWRITE */
	if (ret!=(ssize_t)leng) {
		safs_silent_errlog(LOG_NOTICE,"error writing metafile");
		if (eptr->downloadRetryCnt>=5) {
			masterconn_download_end(eptr);
		} else {
			eptr->downloadRetryCnt++;
			masterconn_download_next(eptr);
		}
		return;
	}
	if (crc!=mycrc32(0,data,leng)) {
		safs_pretty_syslog(LOG_NOTICE,"metafile data crc error");
		if (eptr->downloadRetryCnt>=5) {
			masterconn_download_end(eptr);
		} else {
			eptr->downloadRetryCnt++;
			masterconn_download_next(eptr);
		}
		return;
	}
	if (fsync(eptr->downloadFD)<0) {
		safs_silent_errlog(LOG_NOTICE,"error syncing metafile");
		if (eptr->downloadRetryCnt>=5) {
			masterconn_download_end(eptr);
		} else {
			eptr->downloadRetryCnt++;
			masterconn_download_next(eptr);
		}
		return;
	}
	eptr->downloadOffset+=leng;
	eptr->downloadRetryCnt=0;
	masterconn_download_next(eptr);
}

void masterconn_changelog_apply_error(MasterConn *eptr, const uint8_t *data, uint32_t length) {
	uint8_t status;
	matoml::changelogApplyError::deserialize(data, length, status);
	safs_silent_syslog(LOG_DEBUG, "master.matoml_changelog_apply_error status: %u", status);
	if (status == SAUNAFS_STATUS_OK) {
		masterconn_force_metadata_download(eptr);
	} else if (status == SAUNAFS_ERROR_DELAYED) {
		eptr->state = MasterConn::State::kLimbo;
		safs_pretty_syslog(LOG_NOTICE, "Master temporarily refused to produce a new metadata image");
	} else {
		eptr->state = MasterConn::State::kLimbo;
		safs_pretty_syslog(LOG_NOTICE, "Master failed to produce a new metadata image: %s", saunafs_error_string(status));
	}
}

void masterconn_beforeclose(MasterConn *eptr) {
	if (eptr->downloadFD>=0) {
		close(eptr->downloadFD);
		eptr->downloadFD = MasterConn::kInvalidFD;
		unlink(metadataTmpFilename.c_str());
		unlink(sessionsTmpFilename.c_str());
		unlink(changelogTmpFilename.c_str());
	}
}

void masterconn_gotpacket(MasterConn *eptr,uint32_t type,const uint8_t *data,uint32_t length) {
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
				masterconn_registered(eptr,data,length);
				break;
#endif
			case MATOML_METACHANGES_LOG:
				masterconn_metachanges_log(eptr,data,length);
				break;
			case SAU_MATOML_END_SESSION:
				masterconn_end_session(eptr,data,length);
				break;
			case MATOML_DOWNLOAD_START:
				masterconn_download_start(eptr,data,length);
				break;
			case MATOML_DOWNLOAD_DATA:
				masterconn_download_data(eptr,data,length);
				break;
			case SAU_MATOML_CHANGELOG_APPLY_ERROR:
				masterconn_changelog_apply_error(eptr, data, length);
				break;
			default:
				safs_pretty_syslog(LOG_NOTICE,"got unknown message (type:%" PRIu32 ")",type);
				eptr->mode = MasterConn::Mode::Kill;
				break;
		}
	} catch (IncorrectDeserializationException& ex) {
		safs_pretty_syslog(LOG_NOTICE, "Packet 0x%" PRIX32 " - can't deserialize: %s", type, ex.what());
		eptr->mode = MasterConn::Mode::Kill;
	}
}

void masterconn_term(void) {
	if (!gMasterConn) {
		return;
	}
	PacketStruct *pptr,*paptr;
	MasterConn *eptr = gMasterConn;

	if (eptr->mode!=MasterConn::Mode::Free) {
		tcpclose(eptr->sock);
		if (eptr->mode!=MasterConn::Mode::Connecting) {
			if (eptr->inputPacket.packet) {
				free(eptr->inputPacket.packet);
			}
			pptr = eptr->outputHead;
			while (pptr) {
				if (pptr->packet) {
					free(pptr->packet);
				}
				paptr = pptr;
				pptr = pptr->next;
				free(paptr);
			}
		}
	}

	delete gMasterConn;
	gMasterConn = NULL;
}

void masterconn_connected(MasterConn *eptr) {
	tcpnodelay(eptr->sock);
	eptr->mode = MasterConn::Mode::Header;
	eptr->masterVersion = MasterConn::kInvalidMasterVersion;
	eptr->inputPacket.next = NULL;
	eptr->inputPacket.bytesLeft = 8;
	eptr->inputPacket.startPtr = eptr->headerBuffer.data();
	eptr->inputPacket.packet = NULL;
	eptr->outputHead = NULL;
	eptr->outputTail = &(eptr->outputHead);

	masterconn_sendregister(eptr);
#ifdef METALOGGER
	masterconn_send_metalogger_config(eptr);
#endif
	if (lastlogversion==0) {
		masterconn_metadownloadinit();
	} else if (eptr->state == MasterConn::State::kDumpRequestPending) {
		masterconn_request_metadata_dump(eptr);
	}
	eptr->lastRead = eptr->lastWrite = eventloop_time();
}

int masterconn_initconnect(MasterConn *eptr) {
	int status;
	if (eptr->isMasterAddressValid==0) {
		uint32_t mip,bip;
		uint16_t mport;
		if (tcpresolve(BindHost.c_str(), NULL, &bip, NULL, 1)>=0) {
			eptr->bindIP = bip;
		} else {
			eptr->bindIP = 0;
		}
		if (tcpresolve(MasterHost.c_str(), MasterPort.c_str(), &mip, &mport, 0)>=0) {
			eptr->masterIP = mip;
			eptr->masterPort = mport;
			eptr->isMasterAddressValid = 1;
		} else {
			safs_pretty_syslog(LOG_WARNING,
					"can't resolve master host/port (%s:%s)",
					MasterHost.c_str(), MasterPort.c_str());
			return -1;
		}
	}
	eptr->sock=tcpsocket();
	if (eptr->sock<0) {
		safs_pretty_errlog(LOG_WARNING,"create socket, error");
		return -1;
	}
	if (tcpnonblock(eptr->sock)<0) {
		safs_pretty_errlog(LOG_WARNING,"set nonblock, error");
		tcpclose(eptr->sock);
		eptr->sock = MasterConn::kInvalidFD;
		return -1;
	}
	if (eptr->bindIP>0) {
		if (tcpnumbind(eptr->sock,eptr->bindIP,0)<0) {
			safs_pretty_errlog(LOG_WARNING,"can't bind socket to given ip");
			tcpclose(eptr->sock);
			eptr->sock = MasterConn::kInvalidFD;
			return -1;
		}
	}
	status = tcpnumconnect(eptr->sock,eptr->masterIP,eptr->masterPort);
	if (status<0) {
		safs_pretty_errlog(LOG_WARNING,"connect failed, error");
		tcpclose(eptr->sock);
		eptr->sock = MasterConn::kInvalidFD;
		eptr->isMasterAddressValid = 0;
		return -1;
	}
	if (status==0) {
		safs_pretty_syslog(LOG_NOTICE,"connected to Master immediately");
		masterconn_connected(eptr);
	} else {
		eptr->mode = MasterConn::Mode::Connecting;
		safs_pretty_syslog_attempt(LOG_NOTICE,"connecting to Master");
	}
	return 0;
}

void masterconn_connecttest(MasterConn *eptr) {
	int status;

	status = tcpgetstatus(eptr->sock);
	if (status) {
		safs_silent_errlog(LOG_WARNING,"connection failed, error");
		tcpclose(eptr->sock);
		eptr->sock = MasterConn::kInvalidFD;
		eptr->mode = MasterConn::Mode::Free;
		eptr->isMasterAddressValid = 0;
		eptr->masterVersion = MasterConn::kInvalidMasterVersion;
	} else {
		safs_pretty_syslog(LOG_NOTICE,"connected to Master");
		masterconn_connected(eptr);
	}
}

void masterconn_read(MasterConn *eptr) {
	SignalLoopWatchdog watchdog;
	int32_t i;
	uint32_t type,size;
	const uint8_t *ptr;

	watchdog.start();
	while (eptr->mode != MasterConn::Mode::Kill) {
		i=read(eptr->sock,eptr->inputPacket.startPtr,eptr->inputPacket.bytesLeft);
		if (i==0) {
			safs_pretty_syslog(LOG_NOTICE,"connection was reset by Master");
			masterconn_kill_session(eptr);
			return;
		}
		if (i<0) {
			if (errno!=EAGAIN) {
				safs_silent_errlog(LOG_NOTICE,"read from Master error");
				masterconn_kill_session(eptr);
			}
			return;
		}
		stats_bytesin+=i;
		eptr->inputPacket.startPtr+=i;
		eptr->inputPacket.bytesLeft-=i;

		if (eptr->inputPacket.bytesLeft>0) {
			return;
		}

		if (eptr->mode==MasterConn::Mode::Header) {
			ptr = eptr->headerBuffer.data() + 4;
			size = get32bit(&ptr);

			if (size>0) {
				if (size>kMaxPacketSize) {
					safs_pretty_syslog(LOG_WARNING,"Master packet too long (%" PRIu32 "/%u)",size,kMaxPacketSize);
					masterconn_kill_session(eptr);
					return;
				}
				eptr->inputPacket.packet = (uint8_t*) malloc(size);
				passert(eptr->inputPacket.packet);
				eptr->inputPacket.bytesLeft = size;
				eptr->inputPacket.startPtr = eptr->inputPacket.packet;
				eptr->mode = MasterConn::Mode::Data;
				continue;
			}
			eptr->mode = MasterConn::Mode::Data;
		}

		if (eptr->mode==MasterConn::Mode::Data) {
			ptr = eptr->headerBuffer.data();
			type = get32bit(&ptr);
			size = get32bit(&ptr);

			eptr->mode=MasterConn::Mode::Header;
			eptr->inputPacket.bytesLeft = 8;
			eptr->inputPacket.startPtr = eptr->headerBuffer.data();

			masterconn_gotpacket(eptr,type,eptr->inputPacket.packet,size);

			if (eptr->inputPacket.packet) {
				free(eptr->inputPacket.packet);
			}
			eptr->inputPacket.packet=NULL;
		}

		if (watchdog.expired()) {
			break;
		}
	}
}

void masterconn_write(MasterConn *eptr) {
	SignalLoopWatchdog watchdog;
	PacketStruct *pack;
	int32_t i;

	watchdog.start();
	for (;;) {
		pack = eptr->outputHead;
		if (pack==NULL) {
			return;
		}
		i=write(eptr->sock,pack->startPtr,pack->bytesLeft);
		if (i<0) {
			if (errno!=EAGAIN) {
				safs_silent_errlog(LOG_NOTICE,"write to Master error");
				eptr->mode = MasterConn::Mode::Kill;
			}
			return;
		}
		stats_bytesout+=i;
		pack->startPtr+=i;
		pack->bytesLeft-=i;
		if (pack->bytesLeft>0) {
			return;
		}
		free(pack->packet);
		eptr->outputHead = pack->next;
		if (eptr->outputHead==NULL) {
			eptr->outputTail = &(eptr->outputHead);
		}
		free(pack);

		if (watchdog.expired()) {
			break;
		}
	}
}

void masterconn_wantexit(void) {
	if (gMasterConn) {
		masterconn_kill_session(gMasterConn);
	}
}

int masterconn_canexit(void) {
	return !gMasterConn || gMasterConn->mode == MasterConn::Mode::Free;
}

void masterconn_desc(std::vector<pollfd> &pdesc) {
	if (!gMasterConn) {
		return;
	}
	MasterConn *eptr = gMasterConn;

	eptr->pollDescPos = -1;
	if (eptr->mode==MasterConn::Mode::Free || eptr->sock<0) {
		return;
	}
	if (eptr->mode==MasterConn::Mode::Header || eptr->mode==MasterConn::Mode::Data) {
		pdesc.push_back({eptr->sock,POLLIN,0});
		eptr->pollDescPos = pdesc.size() - 1;
	}
	if (((eptr->mode==MasterConn::Mode::Header || eptr->mode==MasterConn::Mode::Data) && eptr->outputHead!=NULL) || eptr->mode==MasterConn::Mode::Connecting) {
		if (eptr->pollDescPos>=0) {
			pdesc[eptr->pollDescPos].events |= POLLOUT;
		} else {
			pdesc.push_back({eptr->sock,POLLOUT,0});
			eptr->pollDescPos = pdesc.size() - 1;
		}
	}
}

void masterconn_serve(const std::vector<pollfd> &pdesc) {
	if (!gMasterConn) {
		return;
	}
	uint32_t now=eventloop_time();
	PacketStruct *pptr,*paptr;
	MasterConn *eptr = gMasterConn;

	if (eptr->pollDescPos>=0 && (pdesc[eptr->pollDescPos].revents & (POLLHUP | POLLERR))) {
		if (eptr->mode==MasterConn::Mode::Connecting) {
			masterconn_connecttest(eptr);
		} else {
			eptr->mode = MasterConn::Mode::Kill;
		}
	}
	if (eptr->mode==MasterConn::Mode::Connecting) {
		if (eptr->sock>=0 && eptr->pollDescPos>=0 && (pdesc[eptr->pollDescPos].revents & POLLOUT)) { // FD_ISSET(eptr->sock,wset)) {
			masterconn_connecttest(eptr);
		}
	} else {
		if (eptr->pollDescPos>=0) {
			if ((eptr->mode==MasterConn::Mode::Header || eptr->mode==MasterConn::Mode::Data) && (pdesc[eptr->pollDescPos].revents & POLLIN)) { // FD_ISSET(eptr->sock,rset)) {
				eptr->lastRead = now;
				masterconn_read(eptr);
			}
			if ((eptr->mode==MasterConn::Mode::Header || eptr->mode==MasterConn::Mode::Data) && (pdesc[eptr->pollDescPos].revents & POLLOUT)) { // FD_ISSET(eptr->sock,wset)) {
				eptr->lastWrite = now;
				masterconn_write(eptr);
			}
			if ((eptr->mode==MasterConn::Mode::Header || eptr->mode==MasterConn::Mode::Data) && eptr->lastRead+Timeout<now) {
				eptr->mode = MasterConn::Mode::Kill;
			}
			if ((eptr->mode==MasterConn::Mode::Header || eptr->mode==MasterConn::Mode::Data) && eptr->lastWrite+(Timeout/3)<now && eptr->outputHead==NULL) {
				masterconn_createpacket(eptr,ANTOAN_NOP,0);
			}
		}
	}
	if (eptr->mode == MasterConn::Mode::Kill) {
		masterconn_beforeclose(eptr);
		tcpclose(eptr->sock);
		eptr->sock = MasterConn::kInvalidFD;
		if (eptr->inputPacket.packet) {
			free(eptr->inputPacket.packet);
			eptr->inputPacket.packet = NULL;
		}
		pptr = eptr->outputHead;
		while (pptr) {
			if (pptr->packet) {
				free(pptr->packet);
			}
			paptr = pptr;
			pptr = pptr->next;
			free(paptr);
		}
		eptr->outputHead = NULL;
		eptr->mode = MasterConn::Mode::Free;
		eptr->masterVersion = MasterConn::kInvalidMasterVersion;
	}
}

void masterconn_reconnect(void) {
	if (!gMasterConn) {
		return;
	}
	MasterConn *eptr = gMasterConn;
	if (eptr->mode == MasterConn::Mode::Free && gExitingStatus == ExitingStatus::kRunning) {
		masterconn_initconnect(eptr);
	}
	if ((eptr->mode == MasterConn::Mode::Header || eptr->mode == MasterConn::Mode::Data) && eptr->state == MasterConn::State::kLimbo) {
		if (eptr->changelogApplyErrorTimeout.expired()) {
			masterconn_request_metadata_dump(eptr);
		}
	}
}

void masterconn_become_master() {
	if (!gMasterConn) {
		return;
	}
	MasterConn *eptr = gMasterConn;
	eventloop_timeunregister(eptr->sessionsDownloadInitHandle);
	eventloop_timeunregister(eptr->changelogFlushHandle);
	masterconn_term();
}

void masterconn_reload(void) {
	if (!gMasterConn) {
		return;
	}
	MasterConn *eptr = gMasterConn;
	uint32_t ReconnectionDelay;

	std::string newMasterHost = cfg_getstring("MASTER_HOST","sfsmaster");
	std::string newMasterPort = cfg_getstring("MASTER_PORT","9419");
	std::string newBindHost = cfg_getstring("BIND_HOST","*");

	if (newMasterHost != MasterHost || newMasterPort != MasterPort || newBindHost != BindHost) {
		MasterHost = newMasterHost;
		MasterPort = newMasterPort;
		BindHost = newBindHost;
		eptr->isMasterAddressValid = 0;
		if (eptr->mode != MasterConn::Mode::Free) {
			eptr->mode = MasterConn::Mode::Kill;
		}
	}

	Timeout = cfg_getuint32("MASTER_TIMEOUT",60);
	BackMetaCopies = cfg_getuint32("BACK_META_KEEP_PREVIOUS",3);
	ReconnectionDelay = cfg_getuint32("MASTER_RECONNECTION_DELAY",1);

	if (Timeout>65536) {
		Timeout=65535;
	}
	if (Timeout<10) {
		Timeout=10;
	}
	if (BackMetaCopies>99) {
		BackMetaCopies=99;
	}

#ifdef METALOGGER
	uint32_t metadataDownloadFreq;
	metadataDownloadFreq = cfg_getuint32("META_DOWNLOAD_FREQ",24);
	if (metadataDownloadFreq > (changelog_get_back_logs_config_value() / 2)) {
		metadataDownloadFreq = (changelog_get_back_logs_config_value() / 2);
	}
#endif /* #ifdef METALOGGER */

	eventloop_timechange(reconnect_hook,TIMEMODE_RUN_LATE,ReconnectionDelay,0);
#ifdef METALOGGER
	eventloop_timechange(download_hook,TIMEMODE_RUN_LATE,metadataDownloadFreq*3600,630);
#endif /* #ifndef METALOGGER */

#ifndef METALOGGER
	masterconn_int_send_matoclport(gMasterConn);
#endif /* #ifndef METALOGGER */
}

int masterconn_init(void) {
	uint32_t ReconnectionDelay;
#ifndef METALOGGER
	if (metadataserver::getPersonality() != metadataserver::Personality::kShadow) {
		return 0;
	}
#endif /* #ifndef METALOGGER */
	MasterConn *eptr;

	ReconnectionDelay = cfg_getuint32("MASTER_RECONNECTION_DELAY", 1);
	MasterHost = cfg_getstring("MASTER_HOST","sfsmaster");
	MasterPort = cfg_getstring("MASTER_PORT","9419");
	BindHost = cfg_getstring("BIND_HOST","*");
	Timeout = cfg_getuint32("MASTER_TIMEOUT",60);
	BackMetaCopies = cfg_getuint32("BACK_META_KEEP_PREVIOUS",3);

	if (Timeout>65536) {
		Timeout=65535;
	}
	if (Timeout<10) {
		Timeout=10;
	}

#ifdef METALOGGER
	changelog_init(kChangelogMlFilename, 5, 1000); // may throw
	changelog_disable_flush(); // metalogger does it once a second
	uint32_t metadataDownloadFreq;
	metadataDownloadFreq = cfg_getuint32("META_DOWNLOAD_FREQ",24);
	if (metadataDownloadFreq > (changelog_get_back_logs_config_value() / 2)) {
		metadataDownloadFreq = (changelog_get_back_logs_config_value() / 2);
	}
#endif /* #ifdef METALOGGER */

	eptr = gMasterConn = new MasterConn();
	passert(eptr);

	eptr->isMasterAddressValid = 0;
	eptr->mode = MasterConn::Mode::Free;
	eptr->pollDescPos = -1;
	eptr->downloadFD = MasterConn::kInvalidFD;
	eptr->masterVersion = MasterConn::kInvalidMasterVersion;
	eptr->sock  = MasterConn::kInvalidFD;
	eptr->state = MasterConn::State::kNone;
#ifdef METALOGGER
	gMetadataBackend->changelogsMigrateFrom_1_6_29("changelog_ml");
	masterconn_findlastlogversion();
#endif /* #ifdef METALOGGER */
	if (masterconn_initconnect(eptr)<0) {
		return -1;
	}
	reconnect_hook = eventloop_timeregister(TIMEMODE_RUN_LATE,ReconnectionDelay,0,masterconn_reconnect);
#ifdef METALOGGER
	download_hook = eventloop_timeregister(TIMEMODE_RUN_LATE,metadataDownloadFreq*3600,630,masterconn_metadownloadinit);
#endif /* #ifdef METALOGGER */
	eventloop_destructregister(masterconn_term);
	eventloop_pollregister(masterconn_desc,masterconn_serve);
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
