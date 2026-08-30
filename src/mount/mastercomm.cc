/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2019 Skytechnology sp. z o.o.
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

#include "mount/mastercomm.h"

#include <limits.h>
#include <cstdint>
#if _WIN32
#include <winsock2.h>
#else
#include <netinet/in.h>
#endif
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifndef _WIN32
  #include <arpa/inet.h>
  #include <grp.h>
  #include <pwd.h>
#endif

#include "common/datapack.h"
#include "common/exception.h"
#include "common/exit_status.h"
#include "common/goal.h"
#include "common/md5.h"
#include "common/saunafs_version.h"
#include "common/sockets.h"
#include "common/tls_session.h"
#include "common/type_defs.h"
#include "errors/sfserr.h"
#include "mount/g_io_limiters.h"
#include "mount/exports.h"
#include "mount/notification_area_logging.h"
#include "mount/stats.h"
#include "protocol/SFSCommunication.h"
#include "protocol/cltoma.h"
#include "protocol/matocl.h"
#include "protocol/packet.h"
#include "slogger/slogger.h"

struct threc {
	pthread_t thid;
	std::mutex mutex;
	std::condition_variable condition;
	MessageBuffer outputBuffer;
	MessageBuffer inputBuffer;
	uint8_t status;         // receive status
	bool sent;              // packet was sent
	bool received;          // packet was received
	bool waiting;           // thread is waiting for answer

	uint32_t receivedType;

	uint32_t packetId;      // thread number
	threc *next;

	~threc() {
		pthread_mutex_destroy(mutex.native_handle());
	}
};

/// Context of the TLS channel used for communication with SFS master.
///
/// If no TLS is used, this is `nullptr`.
std::unique_ptr<TlsSession> tlsSession;
int lastHandshakeError = 0;

/// TLS related values
/// These are usually initialized as TlsSession::kNoFile on client startup (see
/// FsInitParams)
std::string tlsConfigFile = "";
std::mutex tlsConfigFileMutex;

#define DEFAULT_OUTPUT_BUFFSIZE 0x1000
#define DEFAULT_INPUT_BUFFSIZE 0x10000
#define NO_DATA_RECEIVED_FROM_MASTER NULL
#define RECEIVE_TIMEOUT 10

constexpr size_t kDefaultTcpCommTimeoutMSeconds = 1000;
constexpr uint32_t kFuseRegisterBlobAclTotalPacketLength = REGISTER_BLOB_SIZE + 1;
constexpr uint32_t kFuseRegisterBlobAclLength = REGISTER_BLOB_SIZE;
constexpr uint32_t kMasterResponseRegisterPacketLength = 32U;
constexpr uint32_t kFuseRegisterBlobAclPacketSizeValueLength =
    sizeof(kFuseRegisterBlobAclTotalPacketLength);
constexpr uint32_t kFuseRegisterSessionTypeLength = 1U;

constexpr uint32_t kSecondsPerMinute = 60;
constexpr uint32_t kSecondsPerHour = 60 * kSecondsPerMinute;
constexpr uint32_t kSecondsPerDay = 24 * kSecondsPerHour;
constexpr uint32_t kSecondsPerWeek = 7 * kSecondsPerDay;

constexpr size_t kSaunafsVersionMajorLength = 2U;
constexpr size_t kSaunafsVersionMinorLength = 1U;
constexpr size_t kSaunafsVersionMicroLength = 1U;

const uint8_t cltomaFuseRegisterHeaderLength = sizeof(CLTOMA_FUSE_REGISTER);
const uint32_t registerTotalHeaderInfoLength =
    cltomaFuseRegisterHeaderLength + kFuseRegisterBlobAclPacketSizeValueLength;

static threc *threchead=NULL;

static int fd;
static bool disconnect;
static time_t lastwrite;
static int sessionlost;

// This constant means no master side mapping of uid/gid coming from client
#define DEFAULT_UID_GID_MAPPING 999

#ifdef _WIN32
uint8_t *sessionFlags = nullptr;
int *mountingUid  = nullptr;
int *mountingGid = nullptr;
#endif

static std::atomic<uint32_t> maxretries;
static std::atomic<uint32_t> maxWaitRetryTime;
static std::atomic<uint32_t> mastercommSleepTimeDivisor;

static pthread_t rpthid,npthid;
static std::mutex fdMutex, recMutex;

static uint32_t sessionid;

constexpr uint32_t fuseRegisterReconnectDataLength =
    sizeof(sessionid) + kSaunafsVersionMajorLength + kSaunafsVersionMinorLength +
    kSaunafsVersionMicroLength;
constexpr uint32_t fuseRegisterWithReconnectPacketInfoLength =
    kFuseRegisterSessionTypeLength + fuseRegisterReconnectDataLength;
constexpr uint32_t registerReconnectTotalPacketLength = registerTotalHeaderInfoLength +
                                                        kFuseRegisterBlobAclLength +
                                                        fuseRegisterWithReconnectPacketInfoLength;

constexpr uint32_t fuseRegisterWithClosePacketInfoLength =
    kFuseRegisterSessionTypeLength + sizeof(sessionid);
constexpr uint32_t registerCloseSessionTotalPacketLength = registerTotalHeaderInfoLength +
                                                           kFuseRegisterBlobAclLength +
                                                           fuseRegisterWithClosePacketInfoLength;

static char masterstrip[17];
static uint32_t masterip=0;
static uint16_t masterport=0;
static char srcstrip[17];
static uint32_t srcip=0;

static uint8_t fterm;
static std::atomic<bool> gIsKilled(false);

typedef std::unordered_map<PacketHeader::Type, PacketHandler*> PerTypePacketHandlers;
static PerTypePacketHandlers perTypePacketHandlers;
static std::mutex perTypePacketHandlersLock;

std::string gCfgString = "";

void fs_getmasterlocation(uint8_t loc[14]) {
	put32bit(&loc,masterip);
	put16bit(&loc,masterport);
	put32bit(&loc,sessionid);
	put32bit(&loc,masterVersion.load());
}

uint32_t fs_getsrcip() {
	return srcip;
}

enum {
	MASTER_CONNECTS = 0,
	MASTER_BYTESSENT,
	MASTER_BYTESRCVD,
	MASTER_PACKETSSENT,
	MASTER_PACKETSRCVD,
	STATNODES
};

static std::vector<uint64_t *> statsptr(STATNODES);

struct InitParams {
	std::string bind_host;
	std::string host;
	std::string port;
	bool meta;
	bool do_not_remember_password;
	std::string mountpoint;
	std::string subfolder;
	std::vector<uint8_t> password_digest;
	unsigned report_reserved_period;

	InitParams &operator=(const SaunaClient::FsInitParams &params) {
		bind_host = params.bind_host;
		host = params.host;
		port = params.port;
		meta = params.meta;
		do_not_remember_password = params.do_not_remember_password;
		mountpoint = params.mountpoint;
		subfolder = params.subfolder;
		password_digest = params.password_digest;
		report_reserved_period = params.report_reserved_period;
		return *this;
	}
};

static InitParams gInitParams;

// Check if TLS connection is enabled to be tried,
// at least client certificate and key files should be available.
// Also, TLS config file can be used to configure TLS connection,
// and should be prioritized if provided.
bool isTlsEnabled() {
	std::lock_guard lock(tlsConfigFileMutex);
	return !tlsConfigFile.empty();
}

/// Starts or continues a TLS handshake.
/// \return true if the handshake completed successfully, false otherwise.
bool fs_tlshandshake() {
	sassert(tlsSession != nullptr);

#ifndef _WIN32
	// Temporarily ignore SIGPIPE only during SSL_connect.
	// This is necessary when the master server does not support TLS,
	// as a signal may be sent to the process during the connection attempt.
	struct sigaction old_action, ignore_action;
	ignore_action.sa_handler = SIG_IGN;
	sigemptyset(&ignore_action.sa_mask);
	ignore_action.sa_flags = 0;
	sigaction(SIGPIPE, &ignore_action, &old_action);
#endif

	int ret = SSL_connect(tlsSession->session());

#ifndef _WIN32
	// Restore the previous SIGPIPE handler.
	sigaction(SIGPIPE, &old_action, nullptr);
#endif

	if (ret == 1) {
		// Handshake completed successfully
		return true;
	}

	int err = SSL_get_error(tlsSession->session(), ret);

	lastHandshakeError = err;

	if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
		// Non-fatal, handshake can be retried later
		safs::log_info("TLS handshake in progress (client): {}", opensslErrorString(err));
		return false;
	}

	// Fatal error
	safs::log_warn("TLS handshake failed (client): {}", opensslErrorString(err));
	return false;
}

inline int fs_write(int fd, const void *buf, size_t count, int timeout_ms) {
	if (tlsSession) { return SSL_write(tlsSession->session(), buf, static_cast<int>(count)); }

	return tcptowrite(fd, buf, count, timeout_ms);
}

inline int fs_read(int fd, void *buf, size_t count, int timeout_ms) {
	if (tlsSession) { return SSL_read(tlsSession->session(), buf, static_cast<int>(count)); }

	return tcptoread(fd, buf, count, timeout_ms);
}

void fs_close(void) {
	if (tlsSession != nullptr) {
		int ret = SSL_shutdown(tlsSession->session());
		if (ret < 0) {
			safs::log_warn("TLS shutdown failed: {}",
			               opensslErrorString(SSL_get_error(tlsSession->session(), ret)));
		}
		tlsSession.reset();
		safs::log_info("TLS session closed.");
	}

	tcpclose(fd);
	fd = -1;
}

uint32_t getSafeSleepTimeDivisor() {
	uint32_t safeSleepTimeDivisor = mastercommSleepTimeDivisor.load();
	if (safeSleepTimeDivisor == 0) {
		safeSleepTimeDivisor = 1;
	}
	return safeSleepTimeDivisor;
}

void master_statsptr_init(void) {
	statsnode *s;
	s = stats_get_subnode(NULL,"master",0);
	statsptr[MASTER_PACKETSRCVD] = stats_get_counterptr(stats_get_subnode(s,"packets_received",0));
	statsptr[MASTER_PACKETSSENT] = stats_get_counterptr(stats_get_subnode(s,"packets_sent",0));
	statsptr[MASTER_BYTESRCVD] = stats_get_counterptr(stats_get_subnode(s,"bytes_received",0));
	statsptr[MASTER_BYTESSENT] = stats_get_counterptr(stats_get_subnode(s,"bytes_sent",0));
	statsptr[MASTER_CONNECTS] = stats_get_counterptr(stats_get_subnode(s,"reconnects",0));
}

static inline void setDisconnect(bool value) {
	std::unique_lock<std::mutex> fdLock(fdMutex);
	disconnect = value;
#ifdef _WIN32
	gIsDisconnectedFromMaster.store(value);
#endif
	if(value) {
		SaunaClient::masterDisconnectedCallback();
		safs_pretty_syslog(LOG_WARNING,"master: disconnected");
		addNotificationMessage("SaunaFS master disconnected");
	}
}

void fs_inc_acnt(inode_t inode) {
	std::unique_lock<std::mutex> acquiredFileLock(acquiredFileMutex);
	acquiredFiles[inode]++;
#ifdef _WIN32
	if (gCleanAcquiredFilesPeriod > 0) {
		addOrUpdateAcquiredFileLastTimeUsed(inode);
	}
#endif
}

void fs_dec_acnt(inode_t inode) {
	std::unique_lock<std::mutex> afLock(acquiredFileMutex);
	if (!acquiredFiles.contains(inode)) {
		return;
	}
	auto &cnt = acquiredFiles[inode];
	cnt--;
	if (cnt <= 0) {
#ifdef _WIN32
		if (gCleanAcquiredFilesPeriod > 0) {
			removeAcquiredFileLastTimeUsed(inode);
		}
#endif
		acquiredFiles.erase(inode);
	}
}

threc* fs_get_my_threc() {
	pthread_t mythid = pthread_self();
	threc *rec;
	std::unique_lock<std::mutex> recLock(recMutex);
	for (rec = threchead ; rec ; rec = rec->next) {
		if (pthread_equal(rec->thid, mythid)) {
			return rec;
		}
	}
	rec = new threc;
	rec->thid = mythid;
	rec->sent = false;
	rec->status = 0;
	rec->received = false;
	rec->waiting = 0;
	rec->receivedType = 0;
	if (threchead==NULL) {
		rec->packetId = 1;
	} else {
		rec->packetId = threchead->packetId + 1;
	}
	rec->next = threchead;
	threchead = rec;
	return rec;
}

threc* fs_get_threc_by_id(uint32_t packetId) {
	threc *rec;
	std::unique_lock<std::mutex> recLock(recMutex);
	for (rec = threchead ; rec ; rec=rec->next) {
		if (rec->packetId==packetId) {
			return rec;
		}
	}
	return NULL;
}

uint8_t* fs_createpacket(threc *rec,uint32_t cmd,uint32_t size) {
	uint8_t *ptr;
	uint32_t hdrsize = size + sizeof(cmd);
	std::unique_lock<std::mutex> lock(rec->mutex);
	constexpr uint32_t kExtraSize = sizeof(cmd) + sizeof(hdrsize) + sizeof(threc::packetId);
	rec->outputBuffer.resize(size + kExtraSize);
	ptr = rec->outputBuffer.data();
	put32bit(&ptr,cmd);
	put32bit(&ptr,hdrsize);
	put32bit(&ptr,rec->packetId);
	return ptr;
}

bool fs_saucreatepacket(threc *rec, MessageBuffer message) {
	std::unique_lock<std::mutex> lock(rec->mutex);
	rec->outputBuffer = std::move(message);
	return true;
}

SAUNAFS_CREATE_EXCEPTION_CLASS_MSG(LostSessionException, Exception, "session lost");

static bool fs_threc_flush(threc *rec) {
	std::unique_lock<std::mutex> fdLock(fdMutex);
	if (sessionlost) {
		throw LostSessionException();
	}
	if (fd==-1) {
		return false;
	}
	std::unique_lock<std::mutex> lock(rec->mutex);
	const int32_t size = rec->outputBuffer.size();

	int writtenBytes = fs_write(fd, rec->outputBuffer.data(), size, kDefaultTcpCommTimeoutMSeconds);
	if (writtenBytes != size) {
		if (tlsSession) {
			if (writtenBytes < 0) {
				int err = SSL_get_error(tlsSession->session(), writtenBytes);
				safs::log_warn("tls send error: {}", opensslErrorString(err));
			} else {
				safs::log_warn("tls send error: short write ({} of {})", writtenBytes, size);
			}
		} else {
			safs::log_warn("tcp send error: %s", strerr(tcpgetlasterror()));
		}

		disconnect = true;
		return false;
	}

	rec->received = false;
	rec->sent = true;
	lock.unlock();
	stats_inc(MASTER_BYTESSENT, statsptr, size);
	stats_inc(MASTER_PACKETSSENT, statsptr);
	lastwrite = time(NULL);
	return true;
}

static bool fs_threc_wait(threc *rec, std::unique_lock<std::mutex>& lock) {
	while (!rec->received) {
		rec->waiting = 1;
		rec->condition.wait(lock);
		rec->waiting = 0;
	}
	return rec->status == SAUNAFS_STATUS_OK;
}

static inline uint32_t sleep_time(uint32_t tryNo) {
	uint32_t safeSleepTimeDivisor = getSafeSleepTimeDivisor();
	if (tryNo / safeSleepTimeDivisor < maxWaitRetryTime.load()) {
		return 1 + (tryNo / safeSleepTimeDivisor);
	}
	return maxWaitRetryTime.load();
}

// TODO(jotek): not every request should be retransmitted if recv failed (e.g. snapshot)
static bool fs_threc_send_receive(threc *rec, bool filter, PacketHeader::Type expected_type) {
	try {
		for (uint32_t cnt = 0 ; cnt < maxretries ; cnt++) {
			if (fs_threc_flush(rec)) {
				std::unique_lock<std::mutex> lock(rec->mutex);
				if (fs_threc_wait(rec, lock)) {
					if (!filter || rec->receivedType == expected_type) {
						return true;
					} else {
						lock.unlock();
						setDisconnect(true);
					}
				}
			}
			sleep(sleep_time(cnt));
			continue;
		}
	} catch (LostSessionException&) {
	}
	return false;
}

const uint8_t* fs_sendandreceive(threc *rec, uint32_t expected_cmd, uint32_t *answer_leng) {
	// this function is only for compatibility with Legacy code
	sassert(expected_cmd <= PacketHeader::kMaxOldPacketType);

	if (fs_threc_send_receive(rec, true, expected_cmd)) {
		const uint8_t *answer;
		answer = rec->inputBuffer.data();
		*answer_leng = rec->inputBuffer.size();

		// Legacy code doesn't expect message id, skip it
		answer += 4;
		*answer_leng -= 4;

		return answer;
	}
	return NO_DATA_RECEIVED_FROM_MASTER;
}

bool fs_sausendandreceive(threc *rec, uint32_t expectedCommand, MessageBuffer& messageData) {
	if (fs_threc_send_receive(rec, true, expectedCommand)) {
		std::unique_lock<std::mutex> lock(rec->mutex);
		rec->received = false;  // we steal ownership of the received buffer
		messageData = std::move(rec->inputBuffer);
		return true;
	}
	return false;
}

bool fs_sausend(threc *rec) {
	try {
		for (uint32_t cnt = 0; cnt < maxretries; cnt++) {
			if (fs_threc_flush(rec)) {
				return true;
			}
			sleep(sleep_time(cnt));
		}
	} catch (LostSessionException&) {
	}
	return false;
}

bool fs_saurecv(threc *rec, uint32_t expectedCommand, MessageBuffer& messageData) {
	try {
		std::unique_lock<std::mutex> lock(rec->mutex);
		if (fs_threc_wait(rec, lock)) {
			if (rec->receivedType == expectedCommand) {
				rec->received = false;  // we steal ownership of the received buffer
				messageData = std::move(rec->inputBuffer);
				return true;
			} else {
				lock.unlock();
				setDisconnect(true);
			}
		}
	} catch (LostSessionException&) {
	}
	return false;
}

bool fs_sausendandreceive_any(threc *rec, MessageBuffer& messageData) {
	if (fs_threc_send_receive(rec, false, 0)) {
		std::unique_lock<std::mutex> lock(rec->mutex);
		const MessageBuffer& payload = rec->inputBuffer;
		const PacketHeader header(rec->receivedType, payload.size());
		messageData.clear();
		serialize(messageData, header);
		messageData.insert(messageData.end(), payload.begin(), payload.end());
		return true;
	}
	return false;
}

int fs_resolve(bool verbose, const std::string &bindhostname, const std::string &masterhostname, const std::string &masterportname) {
	if (!bindhostname.empty()) {
		if (tcpresolve(bindhostname.c_str(), nullptr, &srcip, nullptr, 1) < 0) {
			if (verbose) {
				fprintf(stderr,"can't resolve source hostname (%s)\n", bindhostname.c_str());
			} else {
				safs_pretty_syslog(LOG_WARNING,"can't resolve source hostname (%s)", bindhostname.c_str());
			}
			return -1;
		}
	} else {
		srcip = 0;
	}
	snprintf(srcstrip, 17, "%" PRIu32 ".%" PRIu32 ".%" PRIu32 ".%" PRIu32,
	         (srcip>>24)&0xFF, (srcip>>16)&0xFF, (srcip>>8)&0xFF, srcip&0xFF);
	srcstrip[16] = 0;

	if (tcpresolve(masterhostname.c_str(), masterportname.c_str(), &masterip, &masterport, 0) < 0) {
		if (verbose) {
			fprintf(stderr, "can't resolve master hostname and/or portname (%s:%s)\n",
			        masterhostname.c_str(), masterportname.c_str());
		} else {
			safs_pretty_syslog(LOG_WARNING,"can't resolve master hostname and/or portname (%s:%s)",
			                   masterhostname.c_str(), masterportname.c_str());
		}
		return -1;
	}
	snprintf(masterstrip, 17, "%" PRIu32 ".%" PRIu32 ".%" PRIu32 ".%" PRIu32,
	        (masterip>>24)&0xFF, (masterip>>16)&0xFF, (masterip>>8)&0xFF, masterip&0xFF);
	masterstrip[16] = 0;

	return 0;
}

void fs_register_config() {
	if (gCfgString.empty()) {
		return;
	}
	auto message = cltoma::registerConfig::build(gCfgString);
	fs_send_custom(message);
}

// Format and print the master’s goal and trash-time limits in human-readable form.
void fs_trashtime_values_checks(uint8_t mingoal, uint8_t maxgoal, uint32_t &mintrashtime,
                                uint32_t &maxtrashtime) {
	if (mingoal > 0 && maxgoal > 0) {
		std::stringstream infoToPrint;

		if (mingoal > GoalId::kMin || maxgoal < GoalId::kMax) {
			infoToPrint << " ; setgoal limited to (" << static_cast<unsigned>(mingoal) << ":"
			            << static_cast<unsigned>(maxgoal) << ")";
		}

		if (mintrashtime > 0 || maxtrashtime < std::numeric_limits<uint32_t>::max()) {
			infoToPrint << "settrashtime limited to (";
			if (mintrashtime > 0) {
				if (mintrashtime > kSecondsPerWeek) {
					infoToPrint << " ; settrashtime limited to (" << mintrashtime / kSecondsPerWeek
					            << "w";
					mintrashtime %= kSecondsPerWeek;
				}

				if (mintrashtime > kSecondsPerDay) {
					infoToPrint << mintrashtime / kSecondsPerDay << "d";
					mintrashtime %= kSecondsPerDay;
				}

				if (mintrashtime > kSecondsPerHour) {
					infoToPrint << mintrashtime / kSecondsPerHour << "h";
					mintrashtime %= kSecondsPerHour;
				}

				if (mintrashtime > kSecondsPerMinute) {
					infoToPrint << mintrashtime / kSecondsPerMinute << "m";
					mintrashtime %= kSecondsPerMinute;
				}

				if (mintrashtime > 0) { infoToPrint << mintrashtime << "s"; }
			} else {
				infoToPrint << "0s";
			}

			if (maxtrashtime > 0) {
				if (maxtrashtime > kSecondsPerWeek) {
					infoToPrint << maxtrashtime / kSecondsPerWeek << "w";
					maxtrashtime %= kSecondsPerWeek;
				}

				if (maxtrashtime > kSecondsPerDay) {
					infoToPrint << maxtrashtime / kSecondsPerDay << "d";
					maxtrashtime %= kSecondsPerDay;
				}

				if (maxtrashtime > kSecondsPerHour) {
					infoToPrint << maxtrashtime / kSecondsPerHour << "h";
					maxtrashtime %= kSecondsPerHour;
				}

				if (maxtrashtime > kSecondsPerMinute) {
					infoToPrint << maxtrashtime / kSecondsPerMinute << "m";
					maxtrashtime %= kSecondsPerMinute;
				}

				if (maxtrashtime > 0) { infoToPrint << maxtrashtime << "s"; }
			} else {
				infoToPrint << "0s";
			}
			infoToPrint << ")";
		}
		fprintf(stdout, "%s", infoToPrint.str().c_str());
	}
}

// Print the session flags and user/group mappings for the current session.
void fs_session_flags_users_groups_checks(uint8_t sesflags, uint32_t rootuid, uint32_t rootgid,
                                          uint32_t mapalluid, uint32_t mapallgid) {
	const char *sessionFlagsPositiveStrTab[] = {SESFLAG_POS_STRINGS};
	const char *sessionFlagsNegativeStrTab[] = {SESFLAG_NEG_STRINGS};
	bool clientHasSessionFlags = false;
	std::stringstream infoToPrint;

	for (uint32_t sesFlagsIndex = 0; sesFlagsIndex < sizeof(sesflags); sesFlagsIndex++) {
		if (sesflags & (1 << sesFlagsIndex)) {
			infoToPrint << (clientHasSessionFlags ? "," : "")
			            << sessionFlagsPositiveStrTab[sesFlagsIndex];
			clientHasSessionFlags = true;
		} else if (sessionFlagsNegativeStrTab[sesFlagsIndex]) {
			infoToPrint << (clientHasSessionFlags ? "," : "")
			            << sessionFlagsNegativeStrTab[sesFlagsIndex];
			clientHasSessionFlags = true;
		}
	}

	if (!clientHasSessionFlags) { infoToPrint << "-"; }

	if (!gInitParams.meta) {
#ifndef _WIN32
		constexpr uint32_t kUserGroupBufferDefaultSize = 16384;
		struct passwd userEntry, *userInfo;
		struct group groupEntry, *groupInfo;
		std::vector<char> userGroupBuffer(kUserGroupBufferDefaultSize);

		infoToPrint << " ; root mapped to ";
		getpwuid_r(rootuid, &userEntry, userGroupBuffer.data(), kUserGroupBufferDefaultSize,
		           &userInfo);
		if (userInfo) {
			infoToPrint << userInfo->pw_name << ":";
		} else {
			infoToPrint << rootuid << ":";
		}

		getgrgid_r(rootgid, &groupEntry, userGroupBuffer.data(), kUserGroupBufferDefaultSize,
		           &groupInfo);
		if (groupInfo) {
			infoToPrint << groupInfo->gr_name;
		} else {
			infoToPrint << rootgid;
		}

		if (sesflags & SESFLAG_MAPALL) {
			infoToPrint << " ; users mapped to ";
			userInfo = getpwuid(mapalluid);
			if (userInfo) {
				infoToPrint << userInfo->pw_name << ":";
			} else {
				infoToPrint << mapalluid << ":";
			}
			groupInfo = getgrgid(mapallgid);
			if (groupInfo) {
				infoToPrint << groupInfo->gr_name;
			} else {
				infoToPrint << mapallgid;
			}
		}
#else
		infoToPrint << " ; root mapped to " << rootuid << ":" << rootgid;
		// The Windows specific code makes consistent the use of client side
		// uid/gid mappings and master side uid/gid mappings.
		if (mapalluid != DEFAULT_UID_GID_MAPPING || mapallgid != DEFAULT_UID_GID_MAPPING) {
			if (*mountingUid != USE_LOCAL_ID && *mountingGid != USE_LOCAL_ID) {
				infoToPrint << " ; master server overwrote users mapping";
			}
			*mountingUid = mapalluid;
			*mountingGid = mapallgid;
		}

		// At last, the final mapping is shown in the connection parameters
		// line.
		if (*mountingUid == USE_LOCAL_ID && *mountingGid == USE_LOCAL_ID) {
			infoToPrint << " ; users mapped to local IDs";
		} else {
			infoToPrint << " ; users mapped to " << *mountingUid << ":" << *mountingGid;
		}
#endif
	} else {
		// meta
		if (sesflags & SESFLAG_NONROOTMETA) {
			nonRootAllowedToUseMeta() = true;
		} else {
			nonRootAllowedToUseMeta() = false;
		}
	}
	fprintf(stdout, "%s", infoToPrint.str().c_str());
}

int fs_open_master_connection(bool verbose = false) {
	int socket = tcpsocket();

	if (socket < 0) { return -1; }

	if (tcpnodelay(socket) < 0) {
		if (verbose) {
			fprintf(stderr, "can't set TCP_NODELAY\n");
		} else {
			safs::log_warn("can't set TCP_NODELAY");
		}
	}

	if (srcip > 0) {
		if (tcpnumbind(socket, srcip, 0) < 0) {
			if (verbose) {
				fprintf(stderr, "can't bind socket to given ip (\"%s\")\n", srcstrip);
			} else {
				safs::log_warn("can't bind socket to given ip (\"{}\")", srcstrip);
			}
			tcpclose(socket);
			return -1;
		}
	}

	if (tcpnumconnect(socket, masterip, masterport) < 0) {
		if (verbose) {
			fprintf(stderr, "can't connect to sfsmaster (\"%s\":\"%" PRIu16 "\")\n", masterstrip,
			        masterport);
		} else {
			safs::log_warn("can't connect to sfsmaster (\"{}\":\"{}\")", masterstrip, masterport);
		}
		tcpclose(socket);
		return -1;
	}
	return socket;
}

int fs_register_with_get_random(std::vector<std::uint8_t> &registrationMessageBuffer,
                                std::vector<uint8_t> &passwordDigest, bool verbose) {
	md5ctx md5PassCtx;
	uint32_t messageValueIterator;
	uint32_t passwordDigestLength = passwordDigest.size();
	uint8_t *messageToMaster = registrationMessageBuffer.data();
	const uint8_t *messageFromMaster = registrationMessageBuffer.data();

	// Register message with REGISTER_GETRANDOM request and response lengths
	const uint32_t registerWithGetRandomRequestLength =
	    registerTotalHeaderInfoLength + kFuseRegisterBlobAclTotalPacketLength;

	messageToMaster = registrationMessageBuffer.data();
	put32bit(&messageToMaster, CLTOMA_FUSE_REGISTER);
	put32bit(&messageToMaster, kFuseRegisterBlobAclTotalPacketLength);
	memcpy(messageToMaster, FUSE_REGISTER_BLOB_ACL, kFuseRegisterBlobAclLength);
	messageToMaster += kFuseRegisterBlobAclLength;
	put8bit(&messageToMaster, REGISTER_GETRANDOM);

	if (fs_write(fd, registrationMessageBuffer.data(), registerWithGetRandomRequestLength,
	               kDefaultTcpCommTimeoutMSeconds) !=
	    (int32_t)(registerWithGetRandomRequestLength)) {
		if (verbose) {
			fprintf(stderr, "error sending data to sfsmaster\n");
		} else {
			safs::log_warn("error sending data to sfsmaster");
		}

		fs_close();
		return -1;
	}

	if (fs_read(fd, registrationMessageBuffer.data(), registerTotalHeaderInfoLength,
	              kDefaultTcpCommTimeoutMSeconds) != (int32_t)(registerTotalHeaderInfoLength)) {
		if (verbose) {
			fprintf(stderr, "error receiving data from sfsmaster\n");
		} else {
			safs::log_warn("error receiving data from sfsmaster");
		}

		fs_close();
		return -1;
	}

	messageFromMaster = registrationMessageBuffer.data();
	get32bit(&messageFromMaster, messageValueIterator);
	if (messageValueIterator != MATOCL_FUSE_REGISTER) {
		if (verbose) {
			fprintf(stderr, "got incorrect answer from sfsmaster\n");
		} else {
			safs::log_warn("got incorrect answer from sfsmaster");
		}

		fs_close();
		return -1;
	}

	get32bit(&messageFromMaster, messageValueIterator);
	if (messageValueIterator != kMasterResponseRegisterPacketLength) {
		if (verbose) {
			fprintf(stderr, "got incorrect answer from sfsmaster\n");
		} else {
			safs::log_warn("got incorrect answer from sfsmaster");
		}

		fs_close();
		return -1;
	}

	if (fs_read(fd, registrationMessageBuffer.data(), kMasterResponseRegisterPacketLength,
	              kDefaultTcpCommTimeoutMSeconds) != (int32_t)kMasterResponseRegisterPacketLength) {
		if (verbose) {
			fprintf(stderr, "error receiving data from sfsmaster\n");
		} else {
			safs::log_warn("error receiving data from sfsmaster");
		}

		fs_close();
		return -1;
	}

	md5_init(&md5PassCtx);
	md5_update(&md5PassCtx, registrationMessageBuffer.data(), passwordDigestLength);
	md5_update(&md5PassCtx, gInitParams.password_digest.data(), gInitParams.password_digest.size());
	md5_update(&md5PassCtx, registrationMessageBuffer.data() + passwordDigestLength,
	           passwordDigestLength);
	md5_final(passwordDigest.data(), &md5PassCtx);
	return 0;
}

int fs_register_with_new_session(std::vector<std::uint8_t> &registrationMessageBuffer,
                                 std::vector<uint8_t> &passwordDigest,
                                 const uint32_t fuseRegisterPacketInfoLength, uint8_t havepassword,
                                 uint8_t &sesflags, uint32_t &rootuid, uint32_t &rootgid,
                                 uint32_t &mapalluid, uint32_t &mapallgid, uint8_t &mingoal,
                                 uint8_t &maxgoal, uint32_t &mintrashtime, uint32_t &maxtrashtime,
                                 bool verbose) {
	uint32_t messageValueIterator;
	uint8_t *messageToMaster = registrationMessageBuffer.data();
	const uint8_t *messageFromMaster = registrationMessageBuffer.data();
	uint32_t mountPointPathLength = gInitParams.mountpoint.size() + 1;
	uint32_t subfolderPathLength = gInitParams.meta ? 0 : gInitParams.subfolder.size() + 1;
	uint32_t passwordDigestLength = passwordDigest.size();

	// Constants for session parameters registration lengths
	constexpr uint32_t kNewMetaSessionDataLengthLegacy = 5;
	constexpr uint32_t kNewMetaSessionDataLengthWithPackageVersion = 9;
	constexpr uint32_t kNewMetaSessionDataLengthWithGoalsAndTrashtimes = 19;

	constexpr uint32_t kNewSessionDataLengthWithPackageVersion = 13;
	constexpr uint32_t kNewSessionDataLengthWithMapAllUidGid = 21;
	constexpr uint32_t kNewSessionDataLengthGoalsAndTrashtimes = 25;
	constexpr uint32_t kNewSessionDataLengthWithIoLimits = 35;

	// Register message with REGISTER_NEWMETASESSION or REGISTER_NEWSESSION request length
	const uint32_t fuseRegisterPacketInfoLegth =
	    kFuseRegisterBlobAclLength + fuseRegisterPacketInfoLength + mountPointPathLength +
	    subfolderPathLength + (havepassword ? passwordDigestLength : 0);
	const uint32_t fuseRegisterNewSessionRequestLength = cltomaFuseRegisterHeaderLength +
	                                                     kFuseRegisterBlobAclPacketSizeValueLength +
	                                                     fuseRegisterPacketInfoLegth;

	messageToMaster = registrationMessageBuffer.data();
	put32bit(&messageToMaster, CLTOMA_FUSE_REGISTER);
	put32bit(&messageToMaster, fuseRegisterPacketInfoLegth);
	memcpy(messageToMaster, FUSE_REGISTER_BLOB_ACL, kFuseRegisterBlobAclLength);
	messageToMaster += kFuseRegisterBlobAclLength;
	put8bit(&messageToMaster, (gInitParams.meta) ? REGISTER_NEWMETASESSION : REGISTER_NEWSESSION);
	put16bit(&messageToMaster, SAUNAFS_PACKAGE_VERSION_MAJOR);
	put8bit(&messageToMaster, SAUNAFS_PACKAGE_VERSION_MINOR);
	put8bit(&messageToMaster, SAUNAFS_PACKAGE_VERSION_MICRO);
	put32bit(&messageToMaster, mountPointPathLength);
	memcpy(messageToMaster, gInitParams.mountpoint.c_str(), mountPointPathLength);
	messageToMaster += mountPointPathLength;

	if (!gInitParams.meta) {
		put32bit(&messageToMaster, subfolderPathLength);
		memcpy(messageToMaster, gInitParams.subfolder.c_str(), subfolderPathLength);
	}

	if (havepassword) {
		memcpy(messageToMaster + subfolderPathLength, passwordDigest.data(), passwordDigestLength);
	}

	int writtenBytes =
	    fs_write(fd, registrationMessageBuffer.data(), fuseRegisterNewSessionRequestLength,
	             kDefaultTcpCommTimeoutMSeconds);
	if (writtenBytes != (int32_t)(fuseRegisterNewSessionRequestLength)) {
		if (verbose) {
			if (tlsSession) {
				fprintf(stderr, "error sending data to sfsmaster: %s\n",
				        writtenBytes < 0
				            ? opensslErrorString(SSL_get_error(tlsSession->session(), writtenBytes))
				                  .c_str()
				            : "incomplete write");
			} else {
				fprintf(stderr, "error sending data to sfsmaster: %s\n", strerr(tcpgetlasterror()));
			}
		} else {
			if (tlsSession) {
				safs::log_warn("error sending data to sfsmaster: {}",
				               writtenBytes < 0 ? opensslErrorString(SSL_get_error(
				                                      tlsSession->session(), writtenBytes))
				                                : "incomplete write");
			} else {
				safs::log_warn("error sending data to sfsmaster: {}", strerr(tcpgetlasterror()));
			}
		}

		fs_close();
		return -1;
	}

	int readBytes = fs_read(fd, registrationMessageBuffer.data(), registerTotalHeaderInfoLength,
	                        kDefaultTcpCommTimeoutMSeconds);
	if (readBytes != static_cast<int32_t>(registerTotalHeaderInfoLength)) {
		std::string errorMessage = "";
		if (tlsSession) {
			if (readBytes < 0) {
				errorMessage = opensslErrorString(SSL_get_error(tlsSession->session(), readBytes));
			} else {
				errorMessage = "TLS recv error: short read";
			}
		} else {
			int tcplasterr = tcpgetlasterror();
			errorMessage = (tcplasterr != 0) ? strerr(tcplasterr) : strerr(TCPNORESPONSE);
		}

		if (verbose) {
			fprintf(stderr, "error receiving data from sfsmaster: %s\n", errorMessage.c_str());
		} else {
			safs::log_warn("error receiving data from sfsmaster: {}", errorMessage);
		}

		fs_close();
		return -1;
	}

	messageFromMaster = registrationMessageBuffer.data();
	get32bit(&messageFromMaster, messageValueIterator);
	if (messageValueIterator != MATOCL_FUSE_REGISTER) {
		if (verbose) {
			fprintf(stderr, "got incorrect answer from sfsmaster\n");
		} else {
			safs::log_warn("got incorrect answer from sfsmaster");
		}

		fs_close();
		return -1;
	}

	get32bit(&messageFromMaster, messageValueIterator);
	bool validNewMetaSessionDataLegth =
	    messageValueIterator == kNewMetaSessionDataLengthLegacy ||
	    messageValueIterator == kNewMetaSessionDataLengthWithPackageVersion ||
	    messageValueIterator == kNewMetaSessionDataLengthWithGoalsAndTrashtimes;
	bool validNewSessionDataLegth =
	    messageValueIterator == kNewSessionDataLengthWithPackageVersion ||
	    messageValueIterator == kNewSessionDataLengthWithMapAllUidGid ||
	    messageValueIterator == kNewSessionDataLengthGoalsAndTrashtimes ||
	    messageValueIterator == kNewSessionDataLengthWithIoLimits;
	if (!(messageValueIterator == 1 || (gInitParams.meta && validNewMetaSessionDataLegth) ||
	      (gInitParams.meta == 0 && validNewSessionDataLegth))) {
		if (verbose) {
			fprintf(stderr, "got incorrect answer from sfsmaster\n");
		} else {
			safs::log_warn("got incorrect answer from sfsmaster");
		}

		fs_close();
		return -1;
	}

	readBytes = fs_read(fd, registrationMessageBuffer.data(), messageValueIterator,
	                        kDefaultTcpCommTimeoutMSeconds);
	if (readBytes != static_cast<int32_t>(messageValueIterator)) {
		std::string errorMessage = "";
		if (tlsSession) {
			if (readBytes < 0) {
				errorMessage = opensslErrorString(SSL_get_error(tlsSession->session(), readBytes));
			} else {
				errorMessage = "TLS recv error: short read";
			}
		} else {
			int tcplasterr = tcpgetlasterror();
			errorMessage = (tcplasterr != 0) ? strerr(tcplasterr) : strerr(TCPNORESPONSE);
		}

		if (verbose) {
			fprintf(stderr, "error receiving data from sfsmaster: %s\n", errorMessage.c_str());
		} else {
			safs::log_warn("error receiving data from sfsmaster: {}", errorMessage);
		}

		fs_close();
		return -1;
	}

	messageFromMaster = registrationMessageBuffer.data();
	if (messageValueIterator == 1) {
		if (verbose) {
			fprintf(stderr, "sfsmaster register error: %s\n",
			        saunafs_error_string(messageFromMaster[0]));
		} else {
			safs::log_warn("sfsmaster register error: {}",
			               saunafs_error_string(messageFromMaster[0]));
		}

		fs_close();
		return -1;
	}

	if (messageValueIterator == kNewMetaSessionDataLengthWithPackageVersion ||
	    messageValueIterator == kNewMetaSessionDataLengthWithGoalsAndTrashtimes ||
	    messageValueIterator == kNewSessionDataLengthGoalsAndTrashtimes ||
	    messageValueIterator == kNewSessionDataLengthWithIoLimits) {
		get32bit(&messageFromMaster, masterVersion);
	} else {
		masterVersion = 0;
	}

	get32bit(&messageFromMaster, sessionid);
	sesflags = get8bit(&messageFromMaster);
#ifdef _WIN32
	*sessionFlags = sesflags;
#endif
	if (!gInitParams.meta) {
		get32bit(&messageFromMaster, rootuid);
		get32bit(&messageFromMaster, rootgid);
		if (messageValueIterator == kNewSessionDataLengthWithMapAllUidGid ||
		    messageValueIterator == kNewSessionDataLengthGoalsAndTrashtimes ||
		    messageValueIterator == kNewSessionDataLengthWithIoLimits) {
			get32bit(&messageFromMaster, mapalluid);
			get32bit(&messageFromMaster, mapallgid);
		} else {
			mapalluid = 0;
			mapallgid = 0;
		}
	} else {
		rootuid = 0;
		rootgid = 0;
		mapalluid = 0;
		mapallgid = 0;
	}

	if (messageValueIterator == kNewMetaSessionDataLengthWithGoalsAndTrashtimes ||
	    messageValueIterator == kNewSessionDataLengthWithIoLimits) {
		mingoal = get8bit(&messageFromMaster);
		maxgoal = get8bit(&messageFromMaster);
		get32bit(&messageFromMaster, mintrashtime);
		get32bit(&messageFromMaster, maxtrashtime);
	} else {
		mingoal = 0;
		maxgoal = 0;
		mintrashtime = 0;
		maxtrashtime = 0;
	}

	lastwrite = time(nullptr);
	return 0;
}

bool fs_starttls_connection() {
	try {
		std::string tlsConfigFileCopy;
		{
			std::lock_guard<std::mutex> lock(tlsConfigFileMutex);
			tlsConfigFileCopy = tlsConfigFile;
		}

		if (!tlsConfigFileCopy.empty()) {
			TlsSession::TlsConfig tlsCfg = TlsSession::TlsConfig::fromFile(tlsConfigFileCopy);

			if (tlsCfg.expectedHostname.empty()) { tlsCfg.expectedHostname = gInitParams.host; }

			tlsSession = std::make_unique<TlsSession>(fd, tlsCfg);
		} else {
			safs::log_warn(
			    "TLS configuration file not specified, not using TLS for connection to SFS master");
			return false;
		}
		safs::log_info("initiating TLS handshake with SFS master");

		auto startTlsRequest = cltoma::startTls::build();
		ssize_t ret = tcptowrite(fd, startTlsRequest.data(), startTlsRequest.size(),
		                         kDefaultTcpCommTimeoutMSeconds);
		if (ret < 0) {
			safs::log_err("cannot transmit startTls request to SFS master");
			fs_close();
			return false;
		} else if (ret != static_cast<int>(startTlsRequest.size())) {
			safs::log_err(
			    "cannot transmit startTls request to SFS master: send(len={} ) returned {}",
			    startTlsRequest.size(), ret);
			fs_close();
			return false;
		}

		if (!fs_tlshandshake()) {
			fs_close();
			return false;
		}
		safs::log_info("TLS handshake with SFS master completed successfully");
	} catch (const std::exception &e) {
		safs::log_err("TLS setup error: {}", e.what());
		fs_close();
		return false;
	}

	return true;
}

bool fs_endtls_connection() {
	try {
		if (tlsSession) {
			auto endTlsRequest = cltoma::endTls::build();
			ssize_t ret = fs_write(fd, endTlsRequest.data(), endTlsRequest.size(),
			                       kDefaultTcpCommTimeoutMSeconds);
			if (ret < 0) {
				safs::log_warn("Failed to send TLS termination request to master: {}",
				               opensslErrorString(SSL_get_error(tlsSession->session(), ret)));
				return false;
			} else {
				safs::log_info("Sent TLS termination request to master.");
			}
		}

	} catch (const std::exception &e) {
		safs::log_warn("TLS termination error: {}", e.what());
		return false;
	}

	return true;
}

int fs_connect(bool verbose) {
	std::vector<std::uint8_t> registrationMessageBuffer;
	uint32_t passwordDigestLength = gInitParams.password_digest.size();
	std::vector<uint8_t> passwordDigest(passwordDigestLength);
	uint8_t havepassword = !gInitParams.password_digest.empty();
	uint32_t mountPointPathLength = gInitParams.mountpoint.size() + 1;
	uint32_t subfolderPathLength = gInitParams.meta ? 0 : gInitParams.subfolder.size() + 1;
	uint8_t sesflags = 0;
	uint32_t rootuid = 0;
	uint32_t rootgid = 0;
	uint32_t mapalluid = 0;
	uint32_t mapallgid = 0;
	uint8_t mingoal = 0;
	uint8_t maxgoal = 0;
	uint32_t mintrashtime = 0;
	uint32_t maxtrashtime = 0;

	// Base constants for registration message from client to master and
	// response from master to client size calculations
	const uint32_t fuseRegisterSubfolderLength =
	    (gInitParams.meta) ? 0 : sizeof(subfolderPathLength);

	// Constants for total packet size calculation results for registration
	// message from client to master and response from master to client
	const uint32_t fuseRegisterPacketInfoLength =
	    kFuseRegisterSessionTypeLength + sizeof(mountPointPathLength) + kSaunafsVersionMajorLength +
	    kSaunafsVersionMinorLength + kSaunafsVersionMicroLength + fuseRegisterSubfolderLength;
	const uint32_t fuseRegisterTotalPacketLength =
	    registerTotalHeaderInfoLength + kFuseRegisterBlobAclLength + fuseRegisterPacketInfoLength +
	    (gInitParams.meta ? 0 : subfolderPathLength) + mountPointPathLength + passwordDigestLength;

	if (fs_resolve(verbose, gInitParams.bind_host, gInitParams.host, gInitParams.port) < 0) {
		return -1;
	}

	registrationMessageBuffer.resize(fuseRegisterTotalPacketLength);

	fd = fs_open_master_connection(verbose);
	if (fd < 0) { return -1; }

	if (isTlsEnabled() && !fs_starttls_connection()) { return -1; }

	if (havepassword &&
	    fs_register_with_get_random(registrationMessageBuffer, passwordDigest, verbose) < 0) {
		return -1;
	}

	if (fs_register_with_new_session(registrationMessageBuffer, passwordDigest,
	                                 fuseRegisterPacketInfoLength, havepassword, sesflags, rootuid,
	                                 rootgid, mapalluid, mapallgid, mingoal, maxgoal, mintrashtime,
	                                 maxtrashtime, verbose) < 0) {
		return -1;
	}

	if (!verbose) { safs::log_info("registered to master with new session (id #{})", sessionid); }

	if (gInitParams.do_not_remember_password) {
		std::fill(gInitParams.password_digest.begin(), gInitParams.password_digest.end(), 0);
	}

	if (verbose) {
		fprintf(stdout, "sfsmaster accepted connection with parameters: ");
		fs_session_flags_users_groups_checks(sesflags, rootuid, rootgid, mapalluid, mapallgid);
		fs_trashtime_values_checks(mingoal, maxgoal, mintrashtime, maxtrashtime);
		fprintf(stdout, "\n");
	}
	return 0;
}

void fs_reconnect() {
	std::vector<std::uint8_t> registrationMessageBuffer(registerReconnectTotalPacketLength);
	uint32_t currentMessageValue;
	uint8_t *messageToMaster;
	const uint8_t *messageFromMaster;

	if (sessionid == 0) {
		safs::log_warn("can't register: session not created");
		return;
	}

	fd = fs_open_master_connection();
	if (fd < 0) { return; }

	if (isTlsEnabled() && !fs_starttls_connection()) { return; }

	stats_inc(MASTER_CONNECTS, statsptr);
	messageToMaster = registrationMessageBuffer.data();
	put32bit(&messageToMaster, CLTOMA_FUSE_REGISTER);
	put32bit(&messageToMaster,
	         kFuseRegisterBlobAclLength + fuseRegisterWithReconnectPacketInfoLength);
	memcpy(messageToMaster, FUSE_REGISTER_BLOB_ACL, kFuseRegisterBlobAclLength);
	messageToMaster += kFuseRegisterBlobAclLength;
	put8bit(&messageToMaster, REGISTER_RECONNECT);
	put32bit(&messageToMaster, sessionid);
	put16bit(&messageToMaster, SAUNAFS_PACKAGE_VERSION_MAJOR);
	put8bit(&messageToMaster, SAUNAFS_PACKAGE_VERSION_MINOR);
	put8bit(&messageToMaster, SAUNAFS_PACKAGE_VERSION_MICRO);

	int writtenBytes = fs_write(fd, registrationMessageBuffer.data(),
	                            registerReconnectTotalPacketLength, kDefaultTcpCommTimeoutMSeconds);
	if (writtenBytes != static_cast<int32_t>(registerReconnectTotalPacketLength)) {
		if (tlsSession) {
			if (writtenBytes < 0) {
				int err = SSL_get_error(tlsSession->session(), writtenBytes);
				safs::log_warn("register error (TLS write: {})", opensslErrorString(err));
			} else {
				safs::log_warn("register error (TLS short write: {} of {})", writtenBytes,
				               registerReconnectTotalPacketLength);
			}
		} else {
			safs::log_warn("register error (write: {})", strerr(tcpgetlasterror()));
		}

		fs_close();
		return;
	}

	stats_inc(MASTER_BYTESSENT, statsptr, 16 + kFuseRegisterBlobAclLength);
	stats_inc(MASTER_PACKETSSENT, statsptr);

	int readBytes = fs_read(fd, registrationMessageBuffer.data(), registerTotalHeaderInfoLength,
	                        kDefaultTcpCommTimeoutMSeconds);
	if (readBytes != static_cast<int32_t>(registerTotalHeaderInfoLength)) {
		std::string errorMessage = "";
		if (tlsSession) {
			if (readBytes < 0) {
				errorMessage = opensslErrorString(SSL_get_error(tlsSession->session(), readBytes));
			} else {
				errorMessage = "TLS recv error: short read";
			}
		} else {
			int tcplasterr = tcpgetlasterror();
			errorMessage = (tcplasterr != 0) ? strerr(tcplasterr) : strerr(TCPNORESPONSE);
		}

		safs::log_warn("register error (read header: {})", errorMessage);
		fs_close();
		return;
	}

	stats_inc(MASTER_BYTESRCVD, statsptr, registerTotalHeaderInfoLength);
	messageFromMaster = registrationMessageBuffer.data();
	get32bit(&messageFromMaster, currentMessageValue);
	if (currentMessageValue != MATOCL_FUSE_REGISTER) {
		safs::log_warn("register error (bad answer: {})", currentMessageValue);
		fs_close();
		return;
	}

	get32bit(&messageFromMaster, currentMessageValue);
	if (currentMessageValue != 1) {
		safs::log_warn("register error (bad length: {})", currentMessageValue);
		fs_close();
		return;
	}

	readBytes = fs_read(fd, registrationMessageBuffer.data(), currentMessageValue,
	                        kDefaultTcpCommTimeoutMSeconds);
	if (readBytes != static_cast<int32_t>(currentMessageValue)) {
		std::string errorMessage = "";
		if (tlsSession) {
			if (readBytes < 0) {
				errorMessage = opensslErrorString(SSL_get_error(tlsSession->session(), readBytes));
			} else {
				errorMessage = "TLS recv error: short read";
			}
		} else {
			int tcplasterr = tcpgetlasterror();
			errorMessage = (tcplasterr != 0) ? strerr(tcplasterr) : strerr(TCPNORESPONSE);
		}

		safs::log_warn("register error (read data: {})", errorMessage);
		fs_close();
		return;
	}

	stats_inc(MASTER_BYTESRCVD, statsptr, currentMessageValue);
	stats_inc(MASTER_PACKETSRCVD, statsptr);
	messageFromMaster = registrationMessageBuffer.data();
	if (messageFromMaster[0] != 0) {
		sessionlost = 1;
		safs::log_warn("master: register status: {}", saunafs_error_string(messageFromMaster[0]));
		fs_close();
		return;
	}

	lastwrite = time(nullptr);
	safs::log_info("registered to master (session id #{})", sessionid);
}

void fs_close_session() {
	std::vector<std::uint8_t> registrationMessageBuffer;
	uint8_t *messageToMaster;

	if (sessionid == 0) { return; }

	registrationMessageBuffer.resize(registerCloseSessionTotalPacketLength);
	messageToMaster = registrationMessageBuffer.data();
	put32bit(&messageToMaster, CLTOMA_FUSE_REGISTER);
	put32bit(&messageToMaster, kFuseRegisterBlobAclLength + fuseRegisterWithClosePacketInfoLength);
	memcpy(messageToMaster, FUSE_REGISTER_BLOB_ACL, kFuseRegisterBlobAclLength);
	messageToMaster += kFuseRegisterBlobAclLength;
	put8bit(&messageToMaster, REGISTER_CLOSESESSION);
	put32bit(&messageToMaster, sessionid);

	int writtenBytes =
	    fs_write(fd, registrationMessageBuffer.data(), registerCloseSessionTotalPacketLength,
	             kDefaultTcpCommTimeoutMSeconds);
	if (writtenBytes != static_cast<int32_t>(registerCloseSessionTotalPacketLength)) {
		safs::log_warn("master: close session error (write: {})",
		               tlsSession
		                   ? opensslErrorString(SSL_get_error(tlsSession->session(), writtenBytes))
		                   : strerr(tcpgetlasterror()));
	}

	if (tlsSession) { fs_endtls_connection(); }
}

#ifdef ENABLE_EXIT_ON_USR1
static void usr1_handler(int) {
	gIsKilled = true;
}
#endif

void* fs_nop_thread(void *arg) {
	pthread_setname_np(pthread_self(), "nopThread");

	constexpr uint32_t kHeaderSize = sizeof(uint32_t) * 3;
	uint8_t *ptr, hdr[kHeaderSize], *inodespacket;
	int32_t inodesleng;
	int now;
	inode_t inodeswritecnt = 0;
	bool lastDisconnectedStatus = disconnect || (fd < 0);
	(void)arg;

#ifdef ENABLE_EXIT_ON_USR1
	if (signal(SIGUSR1, usr1_handler) == SIG_ERR) {
		mabort("Can't set handler for SIGUSR1");
	}
#endif

	uint64_t lastTweaksGlobalEpoch = gTweaks.getGlobalLastChangeEpoch();
	uint64_t lastIOLimitsEpoch = gTweaks.getVarLastChangeEpochByName("IOLimitsFilePath");
	uint64_t lastTlsConfigFileEpoch = gTweaks.getVarLastChangeEpochByName("TlsConfigFile");

	for (;;) {
		now = time(NULL);
		std::unique_lock<std::mutex> fdLock(fdMutex);
		if (fterm) {
			if (disconnect) {  // setDisconnect was already called async
				fdLock.unlock();
				return nullptr;
			}
			if (fd>=0) {
				fs_close_session();
			}
			fdLock.unlock();
			return NULL;
		}
		if (gIsKilled) {
			safs_pretty_syslog(LOG_NOTICE, "Received SIGUSR1, killing gently...");
			fdLock.unlock();
			exit(SAUNAFS_EXIT_STATUS_GENTLY_KILL);
		}
		if (disconnect == false && fd >= 0) {
			if (lastwrite + 2 < now) {  // NOP
				ptr = hdr;
				put32bit(&ptr, ANTOAN_NOP);  // cmd
				put32bit(&ptr, 4);           // length
				put32bit(&ptr, 0);           // msg id

				if (fs_write(fd, hdr, kHeaderSize, kDefaultTcpCommTimeoutMSeconds) !=
				    kHeaderSize) {
					safs::log_warn("Failed to send ANTOAN_NOP to master");
					disconnect = true;
				} else {
					stats_inc(MASTER_BYTESSENT, statsptr, kHeaderSize);
					stats_inc(MASTER_PACKETSSENT, statsptr);
				}
				lastwrite = now;
			}
			if (++inodeswritecnt >= gInitParams.report_reserved_period && !disconnect) {
				inodeswritecnt = 0;
				constexpr uint32_t kHeaderSizeCmdPlusLeng = sizeof(uint32_t) + sizeof(uint32_t);

				std::unique_lock<std::mutex> asLock(acquiredFileMutex);
				inodesleng = kHeaderSizeCmdPlusLeng + kinode_t_size * acquiredFiles.size();
				inodespacket = (uint8_t*) malloc(inodesleng);
				ptr = inodespacket;
				put32bit(&ptr, CLTOMA_FUSE_RESERVED_INODES);
				put32bit(&ptr, inodesleng - kHeaderSizeCmdPlusLeng);

				for (const auto &[inode, _] : acquiredFiles) {
					putINode(&ptr, inode);
				}

				if (fs_write(fd, inodespacket, inodesleng, kDefaultTcpCommTimeoutMSeconds) !=
				    inodesleng) {
					safs::log_warn("Failed to send CLTOMA_FUSE_RESERVED_INODES to master");
					disconnect = true;
				} else {
					stats_inc(MASTER_BYTESSENT, statsptr, inodesleng);
					stats_inc(MASTER_PACKETSSENT, statsptr);
				}

				free(inodespacket);
			}

			const uint64_t currentTweaksGlobalEpoch = gTweaks.getGlobalLastChangeEpoch();
			if (currentTweaksGlobalEpoch > lastTweaksGlobalEpoch ||
			    lastDisconnectedStatus) {
				if (masterVersion >= kFirstVersionWithMountInfoOnMonitoring && !disconnect) {
					std::string mountInfoStr;
					{
						std::lock_guard lock(gMountInfoMtx);
						gMountInfo.buildMountInfoStr();
						mountInfoStr = gMountInfo.getMountInfoStr();
					}

					auto message = cltoma::updateMountInfo::build(mountInfoStr);
					uint32_t messageLength = message.size();
					std::vector<uint8_t> mountInfoPacket(messageLength);
					std::copy(message.begin(), message.end(), mountInfoPacket.begin());

					if (fs_write(fd, mountInfoPacket.data(), messageLength,
					             kDefaultTcpCommTimeoutMSeconds) != (int32_t)messageLength) {
						safs::log_warn("Failed to send mount info to master");
						disconnect = true;
					} else {
						stats_inc(MASTER_BYTESSENT, statsptr, messageLength);
						stats_inc(MASTER_PACKETSSENT, statsptr);
					}
				}

				if (gIOLimitsInitialized) {
					const uint64_t currentIOLimitsEpoch =
					    gTweaks.getVarLastChangeEpochByName("IOLimitsFilePath");
					if (currentIOLimitsEpoch > lastIOLimitsEpoch) {
						fsLoadMountIoLimits();
						lastIOLimitsEpoch = currentIOLimitsEpoch;
					}
				}

				const uint64_t currentTlsConfigFileEpoch =
				    gTweaks.getVarLastChangeEpochByName("TlsConfigFile");

				if (currentTlsConfigFileEpoch > lastTlsConfigFileEpoch) {
					safs::log_warn("TLS configuration changed, starting reconnection...");
					disconnect = true;
				}

				lastTweaksGlobalEpoch = currentTweaksGlobalEpoch;
				lastTlsConfigFileEpoch = currentTlsConfigFileEpoch;
			}
		}

		lastDisconnectedStatus = disconnect || (fd < 0);
		fdLock.unlock();

		sleep(1);
	}
}

bool fs_append_from_master(MessageBuffer &buffer, uint32_t size) {
	if (size == 0) { return true; }
	const uint32_t oldSize = buffer.size();
	buffer.resize(oldSize + size);
	uint8_t *appendPointer = buffer.data() + oldSize;

	int readBytes =
	    fs_read(fd, appendPointer, size, RECEIVE_TIMEOUT * kDefaultTcpCommTimeoutMSeconds);
	if (readBytes == 0) {
		safs_pretty_syslog(LOG_WARNING, "master: connection lost");
		setDisconnect(true);
		return false;
	}

	if (readBytes != static_cast<int>(size)) {
		std::string errorMessage = "";
		if (tlsSession) {
			if (readBytes < 0) {
				errorMessage = opensslErrorString(SSL_get_error(tlsSession->session(), readBytes));
			} else {
				errorMessage = "TLS recv error: short read";
			}
		} else {
			int tcplasterr = tcpgetlasterror();
			errorMessage = (tcplasterr != 0) ? strerr(tcplasterr) : strerr(TCPNORESPONSE);
		}

		safs_pretty_syslog(LOG_WARNING, "master: recv error: %s", errorMessage.c_str());
		setDisconnect(true);
		return false;
	}

	stats_inc(MASTER_BYTESRCVD, statsptr, size);
	return true;
}

template<class... Args>
bool fs_deserialize_from_master(uint32_t& remainingBytes, Args&... destination) {
	const uint32_t size = serializedSize(destination...);
	if (size > remainingBytes) {
		safs_pretty_syslog(LOG_WARNING,"master: packet too short");
		setDisconnect(true);
		return false;
	}
	MessageBuffer buffer;
	if (!fs_append_from_master(buffer, size)) {
		return false;
	}
	try {
		deserialize(buffer, destination...);
	} catch (IncorrectDeserializationException& e) {
		safs_pretty_syslog(LOG_WARNING,"master: deserialization error: %s", e.what());
		setDisconnect(true);
		return false;
	}
	remainingBytes -= size;
	return true;
}

void* fs_receive_thread(void *) {
	uint32_t initialReconnectSleep_ms = 100;
	uint32_t reconnectSleep_ms = initialReconnectSleep_ms;

	pthread_setname_np(pthread_self(), "recFromMaster");

	for (;;) {
		std::unique_lock<std::mutex>fdLock(fdMutex);
		if (fterm) {
			return NULL;
		}
		if (disconnect) {
			fs_close();
			disconnect = false;
			// send to any threc status error and unlock them
			std::unique_lock<std::mutex>recLock(recMutex);
			for (threc *rec=threchead ; rec ; rec=rec->next) {
				std::unique_lock<std::mutex> lock(rec->mutex);
				if (rec->sent) {
					rec->status = 1;
					rec->received = true;
					if (rec->waiting) {
						rec->condition.notify_one();
					}
				}
			}
		}
		if (fd==-1 && sessionid!=0) {
			fs_reconnect();         // try to register using the same session id
		}
		if (fd==-1) {   // still not connected
			if (sessionlost) {      // if previous session is lost then try to register as a new session
				if (fs_connect(false)==0) {
					sessionlost=0;
					fdLock.unlock();
					fs_register_config();
					fdLock.lock();
				}
			} else {        // if other problem occurred then try to resolve hostname and portname then try to reconnect using the same session id
				if (fs_resolve(false, gInitParams.bind_host, gInitParams.host, gInitParams.port) == 0) {
					fs_reconnect();
				}
			}
		}
		if (fd==-1) {
			fdLock.unlock();
			usleep(reconnectSleep_ms * kDefaultTcpCommTimeoutMSeconds);
			// slowly increase timeout before each retry
			if (reconnectSleep_ms < 5 * initialReconnectSleep_ms) {
				reconnectSleep_ms += initialReconnectSleep_ms / 2;
			} else if (reconnectSleep_ms < 10 * initialReconnectSleep_ms) {
				reconnectSleep_ms += initialReconnectSleep_ms;
			} else {
				reconnectSleep_ms = 20 * initialReconnectSleep_ms;
			}
			continue;
		} else {
			// connecection succeeded -- reset timeout the initial value
#ifdef _WIN32
			gIsDisconnectedFromMaster.store(false);
#endif
			reconnectSleep_ms = initialReconnectSleep_ms;
		}
		fdLock.unlock();

		PacketHeader packetHeader;
		PacketVersion packetVersion = 0;
		uint32_t messageId = 0;
		uint32_t remainingBytes = serializedSize(packetHeader);
		if (!fs_deserialize_from_master(remainingBytes, packetHeader)) {
			continue;
		}
		stats_inc(MASTER_PACKETSRCVD, statsptr);
		remainingBytes = packetHeader.length;

		{
			std::unique_lock<std::mutex> lock(perTypePacketHandlersLock);
			const PerTypePacketHandlers::iterator handler =
					perTypePacketHandlers.find(packetHeader.type);
			if (handler != perTypePacketHandlers.end()) {
				MessageBuffer buffer;
				if (fs_append_from_master(buffer, remainingBytes)) {
					handler->second->handle(std::move(buffer));
				}
				continue;
			}
		}

		if (packetHeader.isSauPacketType()) {
			if (remainingBytes < serializedSize(packetVersion, messageId)) {
				safs_pretty_syslog(LOG_WARNING,"master: packet too short: no msgid");
				setDisconnect(true);
				continue;
			}
			if (!fs_deserialize_from_master(remainingBytes, packetVersion, messageId)) {
				continue;
			}
		} else {
			if (remainingBytes < serializedSize(messageId)) {
				safs_pretty_syslog(LOG_WARNING,"master: packet too short: no msgid");
				setDisconnect(true);
				continue;
			}
			if (!fs_deserialize_from_master(remainingBytes, messageId)) {
				continue;
			}
		}

		if (messageId == 0) {
			if (packetHeader.type == ANTOAN_NOP && remainingBytes == 0) {
				continue;
			}
			if (packetHeader.type == ANTOAN_UNKNOWN_COMMAND ||
					packetHeader.type == ANTOAN_BAD_COMMAND_SIZE) {
				// just ignore these packets with packetId==0
				continue;
			}
		}
		threc *rec = fs_get_threc_by_id(messageId);
		if (rec == NULL) {
			safs_pretty_syslog(LOG_WARNING,"master: got unexpected queryid");
			setDisconnect(true);
			continue;
		}
		std::unique_lock<std::mutex> lock(rec->mutex);
		rec->inputBuffer.clear();
		if (packetHeader.isSauPacketType()) {
			serialize(rec->inputBuffer, packetVersion, messageId);
		} else {
			serialize(rec->inputBuffer, messageId);
		}
		if (!fs_append_from_master(rec->inputBuffer, remainingBytes)) {
			lock.unlock();
			continue;
		}
		rec->sent = false;
		rec->status = 0;
		rec->receivedType = packetHeader.type;
		rec->received = true;
		if (rec->waiting) {
			rec->condition.notify_one();
		}
	}
}

// called before fork
int fs_init_master_connection(SaunaClient::FsInitParams &params
#ifdef _WIN32
, uint8_t &session_flags, int &mounting_uid, int &mounting_gid
#endif
) {
	master_statsptr_init();

	gInitParams = params;
	std::fill(params.password_digest.begin(), params.password_digest.end(), 0);

	fd = -1;
	sessionlost = params.delayed_init;
	sessionid = 0;
	disconnect = false;

    // TLS parameters
	{
		std::lock_guard lock(tlsConfigFileMutex);
		tlsConfigFile = params.tls_config_file;
	}

	if (params.delayed_init) {
		return 1;
	}
#ifdef _WIN32
	sessionFlags = &session_flags;
	mountingUid  = &mounting_uid;
	mountingGid = &mounting_gid;
#endif
	int connectResult = fs_connect(params.verbose);
	if (connectResult == 0) {
		fs_register_config();
	}
	return connectResult;
}

// called after fork
void fs_init_threads(uint32_t retries, uint32_t maxWaitTimeForRetry, uint32_t sleepTimeDivisor) {
	pthread_attr_t thattr;
	maxretries = retries;
	maxWaitRetryTime = maxWaitTimeForRetry;
	mastercommSleepTimeDivisor = sleepTimeDivisor;
	fterm = 0;

	std::unique_lock mountInfoLock(gMountInfoMtx);
	gTweaks.registerVariable("MaxRetriesMasterComm", maxretries, "maxretriesmastercomm");
	gTweaks.registerVariable("MaxWaitRetryTimeMasterComm", maxWaitRetryTime, "maxwaitretrytime");
	gTweaks.registerVariable("MasterCommSleepTimeDivisor", mastercommSleepTimeDivisor, "mastercommsleeptimedivisor");
	gTweaks.registerVariable("TlsConfigFile", tlsConfigFile, tlsConfigFileMutex, "tlsconfigfile");
	mountInfoLock.unlock();

	pthread_attr_init(&thattr);
	pthread_attr_setstacksize(&thattr,0x100000);
	pthread_create(&rpthid,&thattr,fs_receive_thread,NULL);
	pthread_create(&npthid,&thattr,fs_nop_thread,NULL);
	pthread_attr_destroy(&thattr);
}

void fs_term(void) {
	threc *tr,*trn;
	std::unique_lock<std::mutex> fd_lock(fdMutex);
	fterm = 1;
	fd_lock.unlock();
	pthread_join(npthid,NULL);
	pthread_join(rpthid,NULL);
	std::unique_lock<std::mutex> rec_lock(recMutex);
	for (tr = threchead ; tr ; tr = trn) {
		trn = tr->next;
		tr->mutex.lock();  // Make helgrind happy
		tr->outputBuffer.clear();
		tr->inputBuffer.clear();
		tr->mutex.unlock();
		delete tr;
	}
	threchead = nullptr;
	rec_lock.unlock();
	std::unique_lock<std::mutex> af_lock(acquiredFileMutex);
#ifdef _WIN32
	acquiredFilesLastTimeUsed.clear();
	inodeToFileMap.clear();
#endif
	acquiredFiles.clear();
	af_lock.unlock();
	fd_lock.lock();

	if (fd >= 0) { fs_close(); }
}

static void fs_got_inconsistent(const std::string& type, uint32_t size, const std::string& what) {
	safs_pretty_syslog(LOG_NOTICE,
			"Got inconsistent %s message from master (length:%" PRIu32 "): %s",
			type.c_str(), size, what.c_str());
	setDisconnect(true);
}

void fs_statfs(uint64_t *totalspace, uint64_t *availspace, uint64_t *trashspace,
               uint64_t *reservedspace, inode_t *inodes) {
	uint8_t *wptr;
	const uint8_t *rptr;
	uint32_t answerLength;
	threc *rec = fs_get_my_threc();

	wptr = fs_createpacket(rec,CLTOMA_FUSE_STATFS,0);

	auto resetStats = [&]() {
		*totalspace = 0;
		*availspace = 0;
		*trashspace = 0;
		*reservedspace = 0;
		*inodes = 0;
	};

	if (wptr == nullptr) {
		resetStats();
		return;
	}

	rptr = fs_sendandreceive(rec, MATOCL_FUSE_STATFS, &answerLength);

	constexpr uint32_t kExpectedAnswerLength = sizeof(*totalspace) + sizeof(*availspace) +
	                                           sizeof(*trashspace) + sizeof(*reservedspace) +
	                                           sizeof(*inodes);

	if (rptr == nullptr) {
		safs::log_warn("fs_statfs: MATOCL_FUSE_STATFS - no answer");
		resetStats();
		return;
	}

	if (answerLength != kExpectedAnswerLength) {
		safs::log_warn("fs_statfs: MATOCL_FUSE_STATFS - wrong size ({}/{})", kExpectedAnswerLength,
		               answerLength);
		resetStats();
		return;
	}

	*totalspace = get64bit(&rptr);
	*availspace = get64bit(&rptr);
	*trashspace = get64bit(&rptr);
	*reservedspace = get64bit(&rptr);
	inode_t tmpInodes;
	getINode(&rptr, tmpInodes);
	*inodes = tmpInodes;
}

uint8_t fs_access(inode_t inode,uint32_t uid,uint32_t gid,uint8_t modemask) {
	uint8_t *wptr;
	const uint8_t *rptr;
	uint32_t i;
	uint8_t ret;
	threc *rec = fs_get_my_threc();
	constexpr uint32_t kPacketSize = sizeof(inode) + sizeof(uid) + sizeof(gid) + sizeof(modemask);
	wptr = fs_createpacket(rec, CLTOMA_FUSE_ACCESS, kPacketSize);
	if (wptr==NULL) {
		return SAUNAFS_ERROR_IO;
	}
	putINode(&wptr,inode);
	put32bit(&wptr,uid);
	put32bit(&wptr,gid);
	put8bit(&wptr,modemask);
	rptr = fs_sendandreceive(rec,MATOCL_FUSE_ACCESS,&i);
	if (!rptr || i!=1) {
		ret = SAUNAFS_ERROR_IO;
	} else {
		ret = rptr[0];
	}
	return ret;
}

uint8_t fs_lookup(inode_t parent, const std::string &path, uint32_t uid, uint32_t gid, inode_t *inode,
                  Attributes &attr) {
	threc *rec = fs_get_my_threc();
	auto message = cltoma::wholePathLookup::build(rec->packetId, parent, path, uid, gid);
	if (!fs_saucreatepacket(rec, message)) {
		return SAUNAFS_ERROR_IO;
	}
	if (!fs_sausendandreceive(rec, SAU_MATOCL_WHOLE_PATH_LOOKUP, message)) {
		return SAUNAFS_ERROR_IO;
	}
	try {
		uint32_t msgid;
		PacketVersion packet_version;
		deserializePacketVersionNoHeader(message, packet_version);
		if (packet_version == matocl::wholePathLookup::kStatusPacketVersion) {
			uint8_t status;
			matocl::wholePathLookup::deserialize(message, msgid, status);
			if (status == SAUNAFS_STATUS_OK) {
				fs_got_inconsistent("SAU_MATOCL_WHOLE_PATH_LOOKUP", message.size(),
				                    "version 0 and SAUNAFS_STATUS_OK");
				return SAUNAFS_ERROR_IO;
			}
			return status;
		} else if (packet_version == matocl::wholePathLookup::kResponsePacketVersion) {
			matocl::wholePathLookup::deserialize(message, msgid, *inode, attr);
			return SAUNAFS_STATUS_OK;
		} else {
			fs_got_inconsistent("SAU_MATOCL_WHOLE_PATH_LOOKUP", message.size(),
					"unknown version " + std::to_string(packet_version));
			return SAUNAFS_ERROR_IO;
		}
	} catch (Exception& ex) {
		fs_got_inconsistent("SAU_MATOCL_WHOLE_PATH_LOOKUP", message.size(), ex.what());
		return SAUNAFS_ERROR_IO;
	}
}

uint8_t fs_getattr(inode_t inode, uint32_t uid, uint32_t gid, Attributes &attr) {
	uint8_t *wptr;
	const uint8_t *rptr;
	uint32_t answerLength;
	uint8_t ret;
	threc *rec = fs_get_my_threc();
	constexpr uint32_t kPacketSize = sizeof(inode) + sizeof(uid) + sizeof(gid);
	wptr = fs_createpacket(rec, CLTOMA_FUSE_GETATTR, kPacketSize);
	if (!wptr) {
		return SAUNAFS_ERROR_IO;
	}
	putINode(&wptr,inode);
	put32bit(&wptr,uid);
	put32bit(&wptr,gid);
	rptr = fs_sendandreceive(rec, MATOCL_FUSE_GETATTR, &answerLength);
	if (rptr==NULL) {
		ret = SAUNAFS_ERROR_IO;
	} else if (answerLength == 1) {
		ret = rptr[0];
	} else if (answerLength != attr.size()) {
		setDisconnect(true);
		ret = SAUNAFS_ERROR_IO;
	} else {
		memcpy(attr.data(), rptr, attr.size());
		ret = SAUNAFS_STATUS_OK;
	}
	return ret;
}

uint8_t fs_setattr(inode_t inode, uint32_t uid, uint32_t gid, uint8_t setmask, uint16_t attrmode,
                   uint32_t attruid, uint32_t attrgid, uint32_t attratime, uint32_t attrmtime,
                   uint8_t sugidclearmode, Attributes &attr) {
	uint8_t *wptr;
	const uint8_t *rptr;
	uint32_t answerLength;
	uint8_t ret;
	threc *rec = fs_get_my_threc();

	constexpr uint32_t kPacketSize =
	    sizeof(inode) + sizeof(uid) + sizeof(gid) + sizeof(setmask) + sizeof(attrmode) +
	    sizeof(attruid) + sizeof(attrgid) + sizeof(attratime) + sizeof(attrmtime);
	constexpr uint32_t kPacketSizeWithSugid = kPacketSize + sizeof(sugidclearmode);

	if (masterVersion < 0x010619) {
		wptr = fs_createpacket(rec, CLTOMA_FUSE_SETATTR, kPacketSize);
	} else {
		wptr = fs_createpacket(rec, CLTOMA_FUSE_SETATTR, kPacketSizeWithSugid);
	}
	if (wptr == nullptr) {
		return SAUNAFS_ERROR_IO;
	}

	putINode(&wptr,inode);
	put32bit(&wptr,uid);
	put32bit(&wptr,gid);
	put8bit(&wptr,setmask);
	put16bit(&wptr,attrmode);
	put32bit(&wptr,attruid);
	put32bit(&wptr,attrgid);
	put32bit(&wptr,attratime);
	put32bit(&wptr,attrmtime);
	if (masterVersion>=0x010619) {
		put8bit(&wptr,sugidclearmode);
	}

	rptr = fs_sendandreceive(rec, MATOCL_FUSE_SETATTR, &answerLength);

	if (rptr == nullptr) {
		ret = SAUNAFS_ERROR_IO;
	} else if (answerLength == 1) {
		ret = rptr[0];
	} else if (answerLength != attr.size()) {
		setDisconnect(true);
		ret = SAUNAFS_ERROR_IO;
	} else {
		memcpy(attr.data(), rptr, attr.size());
		ret = SAUNAFS_STATUS_OK;
	}

	return ret;
}

uint8_t fs_truncate(inode_t inode, bool opened, uint32_t uid, uint32_t gid, uint64_t length,
                    bool &clientPerforms, Attributes &attr, uint64_t &oldLength, uint32_t &lockId) {
	threc *rec = fs_get_my_threc();
	std::vector<uint8_t> message;
	cltoma::fuseTruncate::serialize(message, rec->packetId, inode, opened, uid, gid, length);
	if (!fs_saucreatepacket(rec, message)) {
		return SAUNAFS_ERROR_IO;
	}

	try {
		if (!fs_sausendandreceive(rec, SAU_MATOCL_FUSE_TRUNCATE, message)) {
			return SAUNAFS_ERROR_IO;
		}

		clientPerforms = false;
		uint32_t messageId;
		PacketVersion packetVersion;
		deserializePacketVersionNoHeader(message, packetVersion);
		if (packetVersion == matocl::fuseTruncate::kStatusPacketVersion) {
			uint8_t status;
			matocl::fuseTruncate::deserialize(message, messageId, status);
			if (status == SAUNAFS_STATUS_OK) {
				safs_pretty_syslog (LOG_NOTICE,
						"Received SAUNAFS_STATUS_OK in message SAU_MATOCL_FUSE_TRUNCATE with version"
						" %d" PRIu32, matocl::fuseTruncate::kStatusPacketVersion);
				setDisconnect(true);
				return SAUNAFS_ERROR_IO;
			}
			return status;
		} else if (packetVersion == matocl::fuseTruncate::kFinishedPacketVersion) {
			matocl::fuseTruncate::deserialize(message, messageId, attr);
		} else if (packetVersion == matocl::fuseTruncate::kInProgressPacketVersion) {
			clientPerforms = true;
			matocl::fuseTruncate::deserialize(message, messageId, oldLength, lockId);
		} else {
			safs_pretty_syslog(LOG_NOTICE, "SAU_MATOCL_FUSE_TRUNCATE - wrong packet version");
			setDisconnect(true);
			return SAUNAFS_ERROR_IO;
		}
	} catch (IncorrectDeserializationException& ex) {
		safs_pretty_syslog(LOG_NOTICE,
				"got inconsistent SAU_MATOCL_FUSE_TRUNCATE message from master "
				"(length:%zu), %s", message.size(), ex.what());
		setDisconnect(true);
		return SAUNAFS_ERROR_IO;
	}

	return SAUNAFS_STATUS_OK;
}

uint8_t fs_truncateend(inode_t inode, uint32_t uid, uint32_t gid, uint64_t length, uint32_t lockId,
                       Attributes &attr) {
	threc *rec = fs_get_my_threc();
	std::vector<uint8_t> message;
	cltoma::fuseTruncateEnd::serialize(message, rec->packetId, inode, uid, gid, length, lockId);
	if (!fs_saucreatepacket(rec, message)) {
		return SAUNAFS_ERROR_IO;
	}

	try {
		if (!fs_sausendandreceive(rec, SAU_MATOCL_FUSE_TRUNCATE_END, message)) {
			return SAUNAFS_ERROR_IO;
		}

		PacketVersion packetVersion;
		deserializePacketVersionNoHeader(message, packetVersion);
		uint32_t messageId;
		if (packetVersion == matocl::fuseTruncateEnd::kStatusPacketVersion) {
			uint8_t status;
			matocl::fuseTruncateEnd::deserialize(message, messageId, status);
			if (status == SAUNAFS_STATUS_OK) {
				safs_pretty_syslog (LOG_NOTICE,
						"Received SAUNAFS_STATUS_OK in message SAU_MATOCL_FUSE_TRUNCATE_END with version"
						" %d" PRIu32, matocl::fuseTruncateEnd::kStatusPacketVersion);
				setDisconnect(true);
				return SAUNAFS_ERROR_IO;
			}
			return status;
		} else if (packetVersion == matocl::fuseTruncateEnd::kResponsePacketVersion) {
			matocl::fuseTruncateEnd::deserialize(message, messageId, attr);
		} else {
			safs_pretty_syslog(LOG_NOTICE, "SAU_MATOCL_FUSE_TRUNCATE_END - wrong packet version");
			setDisconnect(true);
			return SAUNAFS_ERROR_IO;
		}
	} catch (IncorrectDeserializationException& ex) {
		safs_pretty_syslog(LOG_NOTICE,
				"got inconsistent SAU_MATOCL_FUSE_TRUNCATE_END message from master "
				"(length:%zu), %s", message.size(), ex.what());
		setDisconnect(true);
		return SAUNAFS_ERROR_IO;
	}

	return SAUNAFS_STATUS_OK;
}

uint8_t fs_readlink(inode_t inode,const uint8_t **path) {
	uint8_t *wptr;
	const uint8_t *rptr;
	uint32_t answerLength;
	uint32_t pathLeng;
	uint8_t ret;
	threc *rec = fs_get_my_threc();

	constexpr uint32_t kPacketSize = sizeof(inode);
	wptr = fs_createpacket(rec, CLTOMA_FUSE_READLINK, kPacketSize);

	if (wptr == nullptr) { return SAUNAFS_ERROR_IO; }

	putINode(&wptr,inode);

	rptr = fs_sendandreceive(rec, MATOCL_FUSE_READLINK, &answerLength);

	if (rptr == NULL) {
		ret = SAUNAFS_ERROR_IO;
	} else if (answerLength == 1) {  // A status code
		ret = rptr[0];
	} else if (answerLength < kPacketSize) {
		setDisconnect(true);
		ret = SAUNAFS_ERROR_IO;
	} else {
		get32bit(&rptr, pathLeng);
		if (answerLength != sizeof(uint32_t) + pathLeng || pathLeng == 0 ||
		    rptr[pathLeng - 1] != 0) {  // null-terminated string
			setDisconnect(true);
			ret = SAUNAFS_ERROR_IO;
		} else {
			*path = rptr;
			ret = SAUNAFS_STATUS_OK;
		}
	}

	return ret;
}

uint8_t fs_symlink(inode_t parent, uint8_t nleng, const uint8_t *name, const uint8_t *path,
                   uint32_t uid, uint32_t gid, inode_t *inode, Attributes &attr) {
	uint8_t *wptr;
	const uint8_t *rptr;
	uint32_t answerLength;
	uint32_t t32;
	uint8_t ret;
	threc *rec = fs_get_my_threc();
	t32 = strlen((const char *)path) + 1;

	constexpr uint32_t kPacketExtraSize =
	    sizeof(parent) + sizeof(nleng) + sizeof(t32) + sizeof(uid) + sizeof(gid);
	wptr = fs_createpacket(rec, CLTOMA_FUSE_SYMLINK, t32 + nleng + kPacketExtraSize);
	if (wptr == nullptr) { return SAUNAFS_ERROR_IO; }

	putINode(&wptr, parent);
	put8bit(&wptr, nleng);
	memcpy(wptr, name, nleng);
	wptr += nleng;
	put32bit(&wptr, t32);
	memcpy(wptr, path, t32);
	wptr += t32;
	put32bit(&wptr, uid);
	put32bit(&wptr, gid);

	rptr = fs_sendandreceive(rec, MATOCL_FUSE_SYMLINK, &answerLength);

	if (rptr == nullptr) {
		ret = SAUNAFS_ERROR_IO;
	} else if (answerLength == 1) {  // A status code
		ret = rptr[0];
	} else if (answerLength != attr.size() + sizeof(inode_t)) {
		setDisconnect(true);
		ret = SAUNAFS_ERROR_IO;
	} else {
		inode_t tmpInode;
		getINode(&rptr, tmpInode);
		*inode = tmpInode;
		memcpy(attr.data(), rptr, attr.size());
		ret = SAUNAFS_STATUS_OK;
	}

	return ret;
}

uint8_t fs_mknod(inode_t parent, uint8_t nleng, const uint8_t *name, uint8_t type, uint16_t mode,
                 uint16_t umask, uint32_t uid, uint32_t gid, uint32_t rdev, inode_t &inode,
                 Attributes &attr) {
	threc* rec = fs_get_my_threc();
	auto message = cltoma::fuseMknod::build(rec->packetId, parent,
			LegacyString<uint8_t>(reinterpret_cast<const char*>(name), nleng),
			type, mode, umask, uid, gid, rdev);
	if (!fs_saucreatepacket(rec, message)) {
		return SAUNAFS_ERROR_IO;
	}
	if (!fs_sausendandreceive(rec, SAU_MATOCL_FUSE_MKNOD, message)) {
		return SAUNAFS_ERROR_IO;
	}
	try {
		uint32_t messageId;
		PacketVersion packetVersion;
		deserializePacketVersionNoHeader(message, packetVersion);
		if (packetVersion == matocl::fuseMkdir::kStatusPacketVersion) {
			uint8_t status;
			matocl::fuseMknod::deserialize(message, messageId, status);
			if (status == SAUNAFS_STATUS_OK) {
				fs_got_inconsistent("SAU_MATOCL_FUSE_MKNOD", message.size(),
						"version 0 and SAUNAFS_STATUS_OK");
				return SAUNAFS_ERROR_IO;
			}
			return status;
		} else if (packetVersion == matocl::fuseMkdir::kResponsePacketVersion) {
			matocl::fuseMknod::deserialize(message, messageId, inode, attr);
			return SAUNAFS_STATUS_OK;
		} else {
			fs_got_inconsistent("SAU_MATOCL_FUSE_MKNOD", message.size(),
					"unknown version " + std::to_string(packetVersion));
			return SAUNAFS_ERROR_IO;
		}
		return SAUNAFS_ERROR_ENOTSUP;
	} catch (Exception& ex) {
		fs_got_inconsistent("SAU_MATOCL_FUSE_MKNOD", message.size(), ex.what());
		return SAUNAFS_ERROR_IO;
	}
}

uint8_t fs_create(inode_t parent, uint8_t nleng, const uint8_t *name, uint16_t mode, uint16_t umask,
                  uint32_t uid, uint32_t gid, uint8_t flags, inode_t &inode, Attributes &attr) {
	threc* rec = fs_get_my_threc();
	auto message = cltoma::fuseCreate::build(rec->packetId, parent,
			LegacyString<uint8_t>(reinterpret_cast<const char*>(name), nleng),
			mode, umask, uid, gid, flags);
	if (!fs_saucreatepacket(rec, message)) {
		return SAUNAFS_ERROR_IO;
	}
	if (!fs_sausendandreceive(rec, SAU_MATOCL_FUSE_CREATE, message)) {
		return SAUNAFS_ERROR_IO;
	}
	try {
		uint32_t messageId;
		PacketVersion packetVersion;
		deserializePacketVersionNoHeader(message, packetVersion);
		if (packetVersion == matocl::fuseCreate::kStatusPacketVersion) {
			uint8_t status;
			matocl::fuseCreate::deserialize(message, messageId, status);
			if (status == SAUNAFS_STATUS_OK) {
				fs_got_inconsistent("SAU_MATOCL_FUSE_CREATE", message.size(),
						"version 0 and SAUNAFS_STATUS_OK");
				return SAUNAFS_ERROR_IO;
			}
			return status;
		} else if (packetVersion == matocl::fuseCreate::kResponsePacketVersion) {
			matocl::fuseCreate::deserialize(message, messageId, inode, attr);
			// The fused create also opens the file; track the acquire client-side
			// like fs_opencheck does, so it is reported via CLTOMA_FUSE_RESERVED_INODES.
			fs_inc_acnt(inode);
			return SAUNAFS_STATUS_OK;
		} else {
			fs_got_inconsistent("SAU_MATOCL_FUSE_CREATE", message.size(),
					"unknown version " + std::to_string(packetVersion));
			return SAUNAFS_ERROR_IO;
		}
	} catch (Exception& ex) {
		fs_got_inconsistent("SAU_MATOCL_FUSE_CREATE", message.size(), ex.what());
		return SAUNAFS_ERROR_IO;
	}
}

uint8_t fs_mkdir(inode_t parent, uint8_t nleng, const uint8_t *name, uint16_t mode, uint16_t umask,
                 uint32_t uid, uint32_t gid, uint8_t copysgid, inode_t &inode, Attributes &attr) {
	threc* rec = fs_get_my_threc();
	auto message = cltoma::fuseMkdir::build(rec->packetId, parent,
			LegacyString<uint8_t>(reinterpret_cast<const char*>(name), nleng),
			mode, umask, uid, gid, copysgid);
	if (!fs_saucreatepacket(rec, message)) {
		return SAUNAFS_ERROR_IO;
	}
	if (!fs_sausendandreceive(rec, SAU_MATOCL_FUSE_MKDIR, message)) {
		return SAUNAFS_ERROR_IO;
	}
	try {
		uint32_t messageId;
		PacketVersion packetVersion;
		deserializePacketVersionNoHeader(message, packetVersion);
		if (packetVersion == matocl::fuseMkdir::kStatusPacketVersion) {
			uint8_t status;
			matocl::fuseMkdir::deserialize(message, messageId, status);
			if (status == SAUNAFS_STATUS_OK) {
				fs_got_inconsistent("SAU_MATOCL_FUSE_MKDIR", message.size(),
						"version 0 and SAUNAFS_STATUS_OK");
				return SAUNAFS_ERROR_IO;
			}
			return status;
		} else if (packetVersion == matocl::fuseMkdir::kResponsePacketVersion) {
			matocl::fuseMkdir::deserialize(message, messageId, inode, attr);
			return SAUNAFS_STATUS_OK;
		} else {
			fs_got_inconsistent("SAU_MATOCL_FUSE_MKDIR", message.size(),
					"unknown version " + std::to_string(packetVersion));
			return SAUNAFS_ERROR_IO;
		}
		return SAUNAFS_ERROR_ENOTSUP;
	} catch (Exception& ex) {
		fs_got_inconsistent("SAU_MATOCL_FUSE_MKDIR", message.size(), ex.what());
		return SAUNAFS_ERROR_IO;
	}
}

uint8_t fs_unlink(inode_t parent,uint8_t nleng,const uint8_t *name,uint32_t uid,uint32_t gid) {
	uint8_t *wptr;
	const uint8_t *rptr;
	uint32_t answerLength;
	uint8_t ret;
	threc *rec = fs_get_my_threc();

	constexpr uint32_t kPacketSize = sizeof(parent) + sizeof(nleng) + sizeof(uid) + sizeof(gid);
	wptr = fs_createpacket(rec, CLTOMA_FUSE_UNLINK, kPacketSize + nleng);
	if (wptr == nullptr) { return SAUNAFS_ERROR_IO; }

	putINode(&wptr, parent);
	put8bit(&wptr, nleng);
	memcpy(wptr, name, nleng);
	wptr += nleng;
	put32bit(&wptr, uid);
	put32bit(&wptr, gid);

	rptr = fs_sendandreceive(rec, MATOCL_FUSE_UNLINK, &answerLength);

	if (rptr == nullptr) {
		ret = SAUNAFS_ERROR_IO;
	} else if (answerLength == 1) {
		ret = rptr[0];
	} else {
		setDisconnect(true);
		ret = SAUNAFS_ERROR_IO;
	}

	return ret;
}

uint8_t fs_rmdir(inode_t parent,uint8_t nleng,const uint8_t *name,uint32_t uid,uint32_t gid) {
	uint8_t *wptr;
	const uint8_t *rptr;
	uint32_t answerLength;
	uint8_t ret;
	threc *rec = fs_get_my_threc();

	constexpr uint32_t kPacketSize = sizeof(parent) + sizeof(nleng) + sizeof(uid) + sizeof(gid);
	wptr = fs_createpacket(rec, CLTOMA_FUSE_RMDIR, kPacketSize + nleng);
	if (wptr == nullptr) { return SAUNAFS_ERROR_IO; }

	putINode(&wptr, parent);
	put8bit(&wptr, nleng);
	memcpy(wptr, name, nleng);
	wptr += nleng;
	put32bit(&wptr, uid);
	put32bit(&wptr, gid);

	rptr = fs_sendandreceive(rec, MATOCL_FUSE_RMDIR, &answerLength);

	if (rptr == nullptr) {
		ret = SAUNAFS_ERROR_IO;
	} else if (answerLength == 1) {
		ret = rptr[0];
	} else {
		setDisconnect(true);
		ret = SAUNAFS_ERROR_IO;
	}

	return ret;
}

uint8_t fs_rename(inode_t parent_src, uint8_t nleng_src, const uint8_t *name_src, inode_t parent_dst,
                  uint8_t nleng_dst, const uint8_t *name_dst, uint32_t uid, uint32_t gid,
                  inode_t *inode, Attributes &attr) {
	uint8_t *wptr;
	const uint8_t *rptr;
	uint32_t answerLength;
	uint8_t ret;
	threc *rec = fs_get_my_threc();

	constexpr uint32_t kPacketSize = sizeof(parent_src) + sizeof(nleng_src) + sizeof(parent_dst) +
		sizeof(nleng_dst) + sizeof(uid) + sizeof(gid);
	wptr = fs_createpacket(rec, CLTOMA_FUSE_RENAME, kPacketSize + nleng_src + nleng_dst);
	if (wptr == nullptr) { return SAUNAFS_ERROR_IO; }

	putINode(&wptr, parent_src);
	put8bit(&wptr, nleng_src);
	memcpy(wptr, name_src, nleng_src);
	wptr += nleng_src;
	putINode(&wptr, parent_dst);
	put8bit(&wptr, nleng_dst);
	memcpy(wptr, name_dst, nleng_dst);
	wptr += nleng_dst;
	put32bit(&wptr, uid);
	put32bit(&wptr, gid);

	rptr = fs_sendandreceive(rec, MATOCL_FUSE_RENAME, &answerLength);

	if (rptr==NULL) {
		ret = SAUNAFS_ERROR_IO;
	} else if (answerLength==1) {
		ret = rptr[0];
		*inode = 0;
		attr.fill(0);
	} else if (answerLength != (attr.size() + sizeof(*inode))) {
		setDisconnect(true);
		ret = SAUNAFS_ERROR_IO;
	} else {
		inode_t tmpInode;
		getINode(&rptr, tmpInode);
		*inode = tmpInode;
		memcpy(attr.data(), rptr, attr.size());
		ret = SAUNAFS_STATUS_OK;
	}

	return ret;
}

uint8_t fs_link(inode_t inode_src, inode_t parent_dst, uint8_t nleng_dst, const uint8_t *name_dst,
                uint32_t uid, uint32_t gid, inode_t *inode, Attributes &attr) {
	uint8_t *wptr;
	const uint8_t *rptr;
	uint32_t answerLength;
	uint8_t ret;
	threc *rec = fs_get_my_threc();

	constexpr uint32_t kPacketSize = sizeof(inode_src) + sizeof(parent_dst) + sizeof(nleng_dst) +
		sizeof(uid) + sizeof(gid);
	wptr = fs_createpacket(rec, CLTOMA_FUSE_LINK, kPacketSize + nleng_dst);
	if (wptr == nullptr) { return SAUNAFS_ERROR_IO; }

	putINode(&wptr, inode_src);
	putINode(&wptr, parent_dst);
	put8bit(&wptr, nleng_dst);
	memcpy(wptr, name_dst, nleng_dst);
	wptr += nleng_dst;
	put32bit(&wptr, uid);
	put32bit(&wptr, gid);

	rptr = fs_sendandreceive(rec, MATOCL_FUSE_LINK, &answerLength);

	if (rptr == nullptr) {
		ret = SAUNAFS_ERROR_IO;
	} else if (answerLength == 1) {
		ret = rptr[0];
	} else if (answerLength != (attr.size() + sizeof(*inode))) {
		setDisconnect(true);
		ret = SAUNAFS_ERROR_IO;
	} else {
		inode_t tmpInode;
		getINode(&rptr, tmpInode);
		*inode = tmpInode;
		memcpy(attr.data(), rptr, attr.size());
		ret = SAUNAFS_STATUS_OK;
	}

	return ret;
}

uint8_t fs_getdir_plus(inode_t inode, uint32_t uid, uint32_t gid, uint8_t addtocache,
                       const uint8_t **dbuff, uint32_t *dbuffsize) {
	uint8_t *wptr;
	const uint8_t *rptr;
	uint32_t answerLength;
	uint8_t ret;
	uint8_t flags;
	threc *rec = fs_get_my_threc();

	constexpr uint32_t kPacketSize = sizeof(inode) + sizeof(uid) + sizeof(gid) + sizeof(flags);
	wptr = fs_createpacket(rec, CLTOMA_FUSE_GETDIR, kPacketSize);
	if (wptr == nullptr) { return SAUNAFS_ERROR_IO; }

	putINode(&wptr,inode);
	put32bit(&wptr,uid);
	put32bit(&wptr,gid);
	flags = GETDIR_FLAG_WITHATTR;
	if (addtocache) {
		flags |= GETDIR_FLAG_ADDTOCACHE;
	}
	put8bit(&wptr,flags);

	rptr = fs_sendandreceive(rec, MATOCL_FUSE_GETDIR, &answerLength);

	if (rptr == nullptr) {
		ret = SAUNAFS_ERROR_IO;
	} else if (answerLength == 1) {  // A status code
		ret = rptr[0];
	} else {
		*dbuff = rptr;
		*dbuffsize = answerLength;
		ret = SAUNAFS_STATUS_OK;
	}

	return ret;
}

uint8_t fs_getdir(inode_t inode, uint32_t uid, uint32_t gid, uint64_t first_entry,
                  uint64_t max_entries, std::vector<DirectoryEntry> &dir_entries) {
	threc *rec = fs_get_my_threc();
	auto message =
	        cltoma::fuseGetDir::build(rec->packetId, inode, uid, gid, first_entry, max_entries);
	if (!fs_saucreatepacket(rec, message)) {
		return SAUNAFS_ERROR_IO;
	}
	if (!fs_sausendandreceive(rec, SAU_MATOCL_FUSE_GETDIR, message)) {
		return SAUNAFS_ERROR_IO;
	}
	try {
		uint32_t message_id;
		PacketVersion packet_version;
		deserializePacketVersionNoHeader(message, packet_version);
		if (packet_version == matocl::fuseGetDir::kStatus) {
			uint8_t status;
			matocl::fuseGetDir::deserialize(message, message_id, status);
			if (status == SAUNAFS_STATUS_OK) {
				fs_got_inconsistent("SAU_MATOCL_FUSE_GETDIR", message.size(),
				                    "version 0 and SAUNAFS_STATUS_OK");
				return SAUNAFS_ERROR_IO;
			}
			return status;
		} else if (packet_version == matocl::fuseGetDir::kResponseWithDirentIndex) {
			matocl::fuseGetDir::deserialize(message, message_id, first_entry,
			                                dir_entries);
			return SAUNAFS_STATUS_OK;
		} else {
			fs_got_inconsistent("SAU_MATOCL_FUSE_GETDIR", message.size(),
			                    "unknown version " + std::to_string(packet_version));
			return SAUNAFS_ERROR_IO;
		}
	} catch (Exception &ex) {
		fs_got_inconsistent("SAU_MATOCL_FUSE_GETDIR", message.size(), ex.what());
		return SAUNAFS_ERROR_IO;
	}
}

// FUSE - I/O

uint8_t fs_opencheck(inode_t inode, uint32_t uid, uint32_t gid, uint8_t flags, Attributes &attr) {
	uint8_t *wptr;
	const uint8_t *rptr;
	uint32_t answerLength;
	uint8_t ret;
	threc *rec = fs_get_my_threc();

	constexpr uint32_t kPacketSize = sizeof(inode) + sizeof(uid) + sizeof(gid) + sizeof(flags);
	wptr = fs_createpacket(rec, CLTOMA_FUSE_OPEN, kPacketSize);
	if (wptr == nullptr) { return SAUNAFS_ERROR_IO; }

	putINode(&wptr, inode);
	put32bit(&wptr, uid);
	put32bit(&wptr, gid);
	put8bit(&wptr, flags);

	fs_inc_acnt(inode);

	rptr = fs_sendandreceive(rec, MATOCL_FUSE_OPEN, &answerLength);

	if (rptr == nullptr) {
		ret = SAUNAFS_ERROR_IO;
	} else if (answerLength == 1) {
		attr.fill(0);
		ret = rptr[0];
	} else if (answerLength == attr.size()) {
		memcpy(attr.data(), rptr, attr.size());
		ret = SAUNAFS_STATUS_OK;
	} else {
		setDisconnect(true);
		ret = SAUNAFS_ERROR_IO;
	}

	if (ret) {      // release on error
		fs_dec_acnt(inode);
	}

	return ret;
}

uint8_t fs_update_credentials(uint32_t key, const GroupCache::Groups &gids) {
	threc* rec = fs_get_my_threc();
	std::vector<uint8_t> message;
	cltoma::updateCredentials::serialize(message, rec->packetId, key, gids);
	if (!fs_saucreatepacket(rec, message)) {
		return SAUNAFS_ERROR_IO;
	}
	if (!fs_sausendandreceive(rec, SAU_MATOCL_UPDATE_CREDENTIALS, message)) {
		return SAUNAFS_ERROR_IO;
	}
	try {
		uint8_t status;
		uint32_t msgid;
		matocl::updateCredentials::deserialize(message, msgid, status);
		return status;
	} catch (Exception& ex) {
		setDisconnect(true);
		return SAUNAFS_ERROR_IO;
	}
}

void fs_release(inode_t inode) {
	fs_dec_acnt(inode);
}

uint8_t fs_saureadchunk(std::vector<ChunkTypeWithAddress> &chunkservers, uint64_t &chunkId,
                        uint32_t &chunkVersion, uint64_t &fileLength, inode_t inode,
                        uint32_t chunkIndex) {
	threc *rec = fs_get_my_threc();

	std::vector<uint8_t> message;
	cltoma::fuseReadChunk::serialize(message, rec->packetId, inode, chunkIndex);
	if (!fs_saucreatepacket(rec, message)) {
		return SAUNAFS_ERROR_IO;
	}

	try {
		if (!fs_sausendandreceive(rec, SAU_MATOCL_FUSE_READ_CHUNK, message)) {
			return SAUNAFS_ERROR_IO;
		}
		PacketVersion packetVersion;
		deserializePacketVersionNoHeader(message, packetVersion);

		if (packetVersion == matocl::fuseReadChunk::kStatusPacketVersion) {
			uint8_t status;
			matocl::fuseReadChunk::deserialize(message, status);
			return status;
		} else if (packetVersion == matocl::fuseReadChunk::kECChunks_ResponsePacketVersion) {
			matocl::fuseReadChunk::deserialize(message, fileLength,
					chunkId, chunkVersion, chunkservers);
		} else if (packetVersion == matocl::fuseReadChunk::kResponsePacketVersion) {
			std::vector<legacy::ChunkTypeWithAddress> legacy_chunkservers;
			matocl::fuseReadChunk::deserialize(message, fileLength,
					chunkId, chunkVersion, legacy_chunkservers);
			chunkservers.clear();
			for (const auto &part : legacy_chunkservers) {
				chunkservers.push_back(ChunkTypeWithAddress(part.address, ChunkPartType(part.chunkType), kFirstXorVersion));
			}
		} else {
			safs_pretty_syslog(LOG_NOTICE, "SAU_MATOCL_FUSE_READ_CHUNK - wrong packet version");
			setDisconnect(true);
			return SAUNAFS_ERROR_IO;
		}
	} catch (IncorrectDeserializationException&) {
		setDisconnect(true);
		return SAUNAFS_ERROR_IO;
	}
	for (auto& server : chunkservers) {
		if (isLoopbackAddress(server.address.ip)) {
			safs::log_debug("Changing chunkserver ip address {} to {}",
				  htonl(server.address.ip),
				  htonl(masterip));
			server.address.ip = masterip;
		}
	}
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_sauwritechunk(inode_t inode, uint32_t chunkIndex, uint32_t &lockId,
                         uint64_t &grantGeneration, uint64_t &grantRandom, uint64_t &fileLength,
                         uint64_t &chunkId, uint32_t &chunkVersion,
                         std::vector<ChunkTypeWithAddress> &chunkservers) {
	grantGeneration = 0;
	grantRandom = 0;
	threc *rec = fs_get_my_threc();

	std::vector<uint8_t> message;
	cltoma::fuseWriteChunk::serialize(message, rec->packetId, inode, chunkIndex, lockId);
	if (!fs_saucreatepacket(rec, message)) {
		return SAUNAFS_ERROR_IO;
	}

	try {
		if (!fs_sausendandreceive(rec, SAU_MATOCL_FUSE_WRITE_CHUNK, message)) {
			return SAUNAFS_ERROR_IO;
		}

		PacketVersion packetVersion;
		deserializePacketVersionNoHeader(message, packetVersion);
		if (packetVersion == matocl::fuseWriteChunk::kStatusPacketVersion) {
			uint8_t status;
			matocl::fuseWriteChunk::deserialize(message, status);
			if (status == SAUNAFS_STATUS_OK) {
				safs_pretty_syslog (LOG_NOTICE,
						"Received SAUNAFS_STATUS_OK in message SAU_MATOCL_FUSE_WRITE_CHUNK with version"
						" %d" PRIu32, matocl::fuseWriteChunk::kStatusPacketVersion);
				setDisconnect(true);
				return SAUNAFS_ERROR_IO;
			}
			return status;
		} else if (packetVersion ==
		           matocl::fuseWriteChunk::kECChunksWithGrant_ResponsePacketVersion) {
			matocl::fuseWriteChunk::deserialize(message, fileLength, chunkId, chunkVersion,
			                                    lockId, grantGeneration, grantRandom,
			                                    chunkservers);
		} else if (packetVersion == matocl::fuseWriteChunk::kECChunks_ResponsePacketVersion) {
			matocl::fuseWriteChunk::deserialize(message,
					fileLength, chunkId, chunkVersion, lockId, chunkservers);
		} else if (packetVersion == matocl::fuseWriteChunk::kResponsePacketVersion) {
			std::vector<legacy::ChunkTypeWithAddress> legacy_chunkservers;
			matocl::fuseWriteChunk::deserialize(message,
					fileLength, chunkId, chunkVersion, lockId, legacy_chunkservers);
			chunkservers.clear();
			for (const auto &part : legacy_chunkservers) {
				chunkservers.push_back(ChunkTypeWithAddress(part.address, ChunkPartType(part.chunkType), kFirstXorVersion));
			}
		} else {
			safs_pretty_syslog(LOG_NOTICE, "SAU_MATOCL_FUSE_WRITE_CHUNK - wrong packet version");
			setDisconnect(true);
			return SAUNAFS_ERROR_IO;
		}
	} catch (IncorrectDeserializationException& ex) {
		safs_pretty_syslog(LOG_NOTICE,
				"got inconsistent SAU_MATOCL_FUSE_WRITE_CHUNK message from master "
				"(length:%zu), %s", message.size(), ex.what());
		setDisconnect(true);
		return SAUNAFS_ERROR_IO;
	}
	for (auto& server : chunkservers) {
		// If 127.0.0.1, let's assume it's the same as master
		if (isLoopbackAddress(server.address.ip)) {
			safs::log_debug("fs_sauwritechunk: Changing chunkserver ip address {} to {}",
				  htonl(server.address.ip),
				  htonl(masterip));
			server.address.ip = masterip;
		}
	}

	return SAUNAFS_STATUS_OK;
}

uint8_t fs_sauwriteend(uint64_t chunkId, uint32_t lockId, inode_t inode, uint64_t length,
                       uint64_t grantGeneration, uint64_t grantRandom) {
	threc* rec = fs_get_my_threc();
	std::vector<uint8_t> message;
	if (grantRandom != 0) {
		cltoma::fuseWriteChunkEnd::serialize(message, rec->packetId, chunkId, lockId, inode, length,
		                                     grantGeneration, grantRandom);
	} else {
		cltoma::fuseWriteChunkEnd::serialize(message, rec->packetId, chunkId, lockId, inode, length);
	}
	if (!fs_saucreatepacket(rec, message)) {
		return SAUNAFS_ERROR_IO;
	}
	if (!fs_sausendandreceive(rec, SAU_MATOCL_FUSE_WRITE_CHUNK_END, message)) {
		return SAUNAFS_ERROR_IO;
	}
	try {
		uint8_t status;
		matocl::fuseWriteChunkEnd::deserialize(message, status);
		return status;
	} catch (Exception& ex) {
		safs_pretty_syslog(LOG_NOTICE,
				"got inconsistent SAU_MATOCL_FUSE_WRITE_CHUNK_END message from master "
				"(length:%zu), %s", message.size(), ex.what());
		setDisconnect(true);
		return SAUNAFS_ERROR_IO;
	}
}

// FUSE - META

uint8_t fs_getreserved(const uint8_t **dbuff,uint32_t *dbuffsize) {
	uint8_t *wptr;
	const uint8_t *rptr;
	uint32_t answerLength;
	uint8_t ret;
	threc *rec = fs_get_my_threc();
	wptr = fs_createpacket(rec, CLTOMA_FUSE_GETRESERVED, 0);
	if (wptr==NULL) {
		return SAUNAFS_ERROR_IO;
	}
	rptr = fs_sendandreceive(rec, MATOCL_FUSE_GETRESERVED, &answerLength);
	if (rptr==NULL) {
		ret = SAUNAFS_ERROR_IO;
	} else if (answerLength==1) {
		ret = rptr[0];
	} else {
		*dbuff = rptr;
		*dbuffsize = answerLength;
		ret = SAUNAFS_STATUS_OK;
	}
	return ret;
}

uint8_t fs_gettrash(const uint8_t **dbuff,uint32_t *dbuffsize) {
	uint8_t *wptr;
	const uint8_t *rptr;
	uint32_t answerLength;
	uint8_t ret;
	threc *rec = fs_get_my_threc();

	wptr = fs_createpacket(rec, CLTOMA_FUSE_GETTRASH, 0);
	if (wptr == nullptr) { return SAUNAFS_ERROR_IO; }

	rptr = fs_sendandreceive(rec, MATOCL_FUSE_GETTRASH, &answerLength);

	if (rptr == nullptr) {
		ret = SAUNAFS_ERROR_IO;
	} else if (answerLength == 1) {
		ret = rptr[0];
	} else {
		*dbuff = rptr;
		*dbuffsize = answerLength;
		ret = SAUNAFS_STATUS_OK;
	}

	return ret;
}

template <typename OffsetT, typename EntryT>
uint8_t fs_getreserved(OffsetT off, uint32_t max_entries, std::vector<EntryT> &entries) {
	threc *rec = fs_get_my_threc();
	auto message = cltoma::fuseGetReserved::build(rec->packetId, off, max_entries);
	if (!fs_saucreatepacket(rec, message)) { return SAUNAFS_ERROR_IO; }

	if (!fs_sausendandreceive(rec, SAU_MATOCL_FUSE_GETRESERVED, message)) {
		return SAUNAFS_ERROR_IO;
	}

	try {
		PacketVersion dummy_packet_version;
		uint32_t dummy_message_id;
		deserializePacketVersionNoHeader(message, dummy_packet_version);
		matocl::fuseGetReserved::deserialize(message, dummy_message_id, entries);
		return SAUNAFS_STATUS_OK;
	} catch (Exception &ex) {
		fs_got_inconsistent("SAU_MATOCL_FUSE_GETRESERVED", message.size(), ex.what());
		return SAUNAFS_ERROR_IO;
	}
}

template uint8_t fs_getreserved<SaunaClient::NamedInodeOffset, NamedInodeEntry>(
    SaunaClient::NamedInodeOffset, uint32_t, std::vector<NamedInodeEntry> &);
template uint8_t fs_getreserved<uint64_t, HandleInodeEntry>(uint64_t, uint32_t,
                                                            std::vector<HandleInodeEntry> &);

template <typename OffsetT, typename EntryT>
uint8_t fs_gettrash(OffsetT off, uint32_t max_entries, std::vector<EntryT> &entries) {
	threc *rec = fs_get_my_threc();
	auto message = cltoma::fuseGetTrash::build(rec->packetId, off, max_entries);
	if (!fs_saucreatepacket(rec, message)) { return SAUNAFS_ERROR_IO; }
	if (!fs_sausendandreceive(rec, SAU_MATOCL_FUSE_GETTRASH, message)) { return SAUNAFS_ERROR_IO; }
	try {
		PacketVersion dummy_packet_version;
		uint32_t dummy_message_id;
		deserializePacketVersionNoHeader(message, dummy_packet_version);
		matocl::fuseGetTrash::deserialize(message, dummy_message_id, entries);
		return SAUNAFS_STATUS_OK;
	} catch (Exception &ex) {
		fs_got_inconsistent("SAU_MATOCL_FUSE_GETTRASH", message.size(), ex.what());
		return SAUNAFS_ERROR_IO;
	}
}

template uint8_t fs_gettrash<SaunaClient::NamedInodeOffset, NamedInodeEntry>(
    SaunaClient::NamedInodeOffset, uint32_t, std::vector<NamedInodeEntry> &);
template uint8_t fs_gettrash<uint64_t, HandleInodeEntry>(uint64_t, uint32_t,
                                                         std::vector<HandleInodeEntry> &);

uint8_t fs_getdetachedattr(inode_t inode, Attributes &attr) {
	uint8_t *wptr;
	const uint8_t *rptr;
	uint32_t answerLength;
	uint8_t ret;
	threc *rec = fs_get_my_threc();

	wptr = fs_createpacket(rec, CLTOMA_FUSE_GETDETACHEDATTR, sizeof(inode));
	if (wptr == nullptr) { return SAUNAFS_ERROR_IO; }

	putINode(&wptr,inode);

	rptr = fs_sendandreceive(rec, MATOCL_FUSE_GETDETACHEDATTR, &answerLength);

	if (rptr == nullptr) {
		ret = SAUNAFS_ERROR_IO;
	} else if (answerLength == 1) {
		ret = rptr[0];
	} else if (answerLength != attr.size()) {
		setDisconnect(true);
		ret = SAUNAFS_ERROR_IO;
	} else {
		memcpy(attr.data(), rptr, attr.size());
		ret = SAUNAFS_STATUS_OK;
	}

	return ret;
}

uint8_t fs_gettrashpath(inode_t inode,const uint8_t **path) {
	uint8_t *wptr;
	const uint8_t *rptr;
	uint32_t answerLength;
	uint32_t pleng;
	uint8_t ret;
	threc *rec = fs_get_my_threc();

	wptr = fs_createpacket(rec, CLTOMA_FUSE_GETTRASHPATH, sizeof(inode));
	if (wptr == nullptr) { return SAUNAFS_ERROR_IO; }

	putINode(&wptr, inode);

	rptr = fs_sendandreceive(rec, MATOCL_FUSE_GETTRASHPATH, &answerLength);

	if (rptr == nullptr) {
		ret = SAUNAFS_ERROR_IO;
	} else if (answerLength == 1) {
		ret = rptr[0];
	} else if (answerLength < sizeof(uint32_t)) {
		setDisconnect(true);
		ret = SAUNAFS_ERROR_IO;
	} else {
		get32bit(&rptr, pleng);
		if (answerLength != sizeof(uint32_t) + pleng || pleng == 0 || rptr[pleng - 1] != 0) {
			setDisconnect(true);
			ret = SAUNAFS_ERROR_IO;
		} else {
			*path = rptr;
			ret = SAUNAFS_STATUS_OK;
		}
	}

	return ret;
}

uint8_t fs_settrashpath(inode_t inode,const uint8_t *path) {
	uint8_t *wptr;
	const uint8_t *rptr;
	uint32_t answerLength;
	uint32_t t32;
	uint8_t ret;
	threc *rec = fs_get_my_threc();
	t32 = strlen((const char *)path) + 1;

	wptr = fs_createpacket(rec, CLTOMA_FUSE_SETTRASHPATH, sizeof(inode) + sizeof(t32) + t32);
	if (wptr == nullptr) { return SAUNAFS_ERROR_IO; }

	putINode(&wptr, inode);
	put32bit(&wptr, t32);
	memcpy(wptr, path, t32);

	rptr = fs_sendandreceive(rec, MATOCL_FUSE_SETTRASHPATH, &answerLength);

	if (rptr == nullptr) {
		ret = SAUNAFS_ERROR_IO;
	} else if (answerLength == 1) {
		ret = rptr[0];
	} else {
		setDisconnect(true);
		ret = SAUNAFS_ERROR_IO;
	}

	return ret;
}

uint8_t fs_undel(inode_t inode) {
	uint8_t *wptr;
	const uint8_t *rptr;
	uint32_t answerLength;
	uint8_t ret;
	threc *rec = fs_get_my_threc();

	wptr = fs_createpacket(rec, CLTOMA_FUSE_UNDEL, sizeof(inode));
	if (wptr == nullptr) { return SAUNAFS_ERROR_IO; }

	putINode(&wptr, inode);

	rptr = fs_sendandreceive(rec, MATOCL_FUSE_UNDEL, &answerLength);

	if (rptr == nullptr) {
		ret = SAUNAFS_ERROR_IO;
	} else if (answerLength == 1) {
		ret = rptr[0];
	} else {
		setDisconnect(true);
		ret = SAUNAFS_ERROR_IO;
	}

	return ret;
}

uint8_t fs_purge(inode_t inode) {
	uint8_t *wptr;
	const uint8_t *rptr;
	uint32_t answerLength;
	uint8_t ret;
	threc *rec = fs_get_my_threc();

	wptr = fs_createpacket(rec, CLTOMA_FUSE_PURGE, sizeof(inode));
	if (wptr == nullptr) { return SAUNAFS_ERROR_IO; }

	putINode(&wptr, inode);

	rptr = fs_sendandreceive(rec, MATOCL_FUSE_PURGE, &answerLength);

	if (rptr == nullptr) {
		ret = SAUNAFS_ERROR_IO;
	} else if (answerLength == 1) {
		ret = rptr[0];
	} else {
		setDisconnect(true);
		ret = SAUNAFS_ERROR_IO;
	}
	return ret;
}

uint8_t fs_getxattr(inode_t inode, uint8_t opened, uint32_t uid, uint32_t gid, uint8_t nleng,
                    const uint8_t *name, uint8_t mode, const uint8_t **vbuff, uint32_t *vleng) {
	uint8_t *wptr;
	const uint8_t *rptr;
	uint32_t answerLength;
	uint8_t ret;
	threc *rec = fs_get_my_threc();

	if (masterVersion < saunafsVersion(1, 6, 29)) { return SAUNAFS_ERROR_ENOTSUP; }

	constexpr uint32_t kPacketHeaderSize =
	    sizeof(inode) + sizeof(opened) + sizeof(uid) + sizeof(gid) + sizeof(nleng) + sizeof(mode);

	wptr = fs_createpacket(rec, CLTOMA_FUSE_GETXATTR, kPacketHeaderSize + nleng);
	if (wptr == nullptr) { return SAUNAFS_ERROR_IO; }

	putINode(&wptr, inode);
	put8bit(&wptr, opened);
	put32bit(&wptr, uid);
	put32bit(&wptr, gid);
	put8bit(&wptr, nleng);
	memcpy(wptr, name, nleng);
	wptr += nleng;
	put8bit(&wptr, mode);

	rptr = fs_sendandreceive(rec, MATOCL_FUSE_GETXATTR, &answerLength);

	if (rptr == nullptr) {
		ret = SAUNAFS_ERROR_IO;
	} else if (answerLength == 1) {
		ret = rptr[0];
	} else if (answerLength < sizeof(uint32_t)) {
		setDisconnect(true);
		ret = SAUNAFS_ERROR_IO;
	} else {
		uint32_t tempVLeng;
		get32bit(&rptr, tempVLeng);
		*vleng = tempVLeng;
		*vbuff = (mode == XATTR_GMODE_GET_DATA) ? rptr : nullptr;

		if ((mode == XATTR_GMODE_GET_DATA && answerLength != (*vleng) + sizeof(uint32_t)) ||
		    (mode == XATTR_GMODE_LENGTH_ONLY && answerLength != sizeof(uint32_t))) {
			setDisconnect(true);
			ret = SAUNAFS_ERROR_IO;
		} else {
			ret = SAUNAFS_STATUS_OK;
		}
	}

	return ret;
}

uint8_t fs_listxattr(inode_t inode, uint8_t opened, uint32_t uid, uint32_t gid, uint8_t mode,
                     const uint8_t **dbuff, uint32_t *dleng) {
	uint8_t *wptr;
	const uint8_t *rptr;
	uint32_t answerLength;
	uint8_t ret;
	threc *rec = fs_get_my_threc();

	if (masterVersion < saunafsVersion(1, 6, 29)) { return SAUNAFS_ERROR_ENOTSUP; }

	constexpr uint32_t kPacketSize =
	    sizeof(inode) + sizeof(opened) + sizeof(uid) + sizeof(gid) + sizeof(uint8_t) + sizeof(mode);

	wptr = fs_createpacket(rec, CLTOMA_FUSE_GETXATTR, kPacketSize);
	if (wptr == nullptr) { return SAUNAFS_ERROR_IO; }

	putINode(&wptr, inode);
	put8bit(&wptr, opened);
	put32bit(&wptr, uid);
	put32bit(&wptr, gid);
	put8bit(&wptr, 0);
	put8bit(&wptr, mode);

	rptr = fs_sendandreceive(rec, MATOCL_FUSE_GETXATTR, &answerLength);

	if (rptr == nullptr) {
		ret = SAUNAFS_ERROR_IO;
	} else if (answerLength == 1) {
		ret = rptr[0];
	} else if (answerLength < sizeof(uint32_t)) {
		setDisconnect(true);
		ret = SAUNAFS_ERROR_IO;
	} else {
		uint32_t tmpDLeng;
		get32bit(&rptr, tmpDLeng);
		*dleng = tmpDLeng;
		*dbuff = (mode == XATTR_GMODE_GET_DATA) ? rptr : nullptr;
		if ((mode == XATTR_GMODE_GET_DATA && answerLength != (*dleng) + sizeof(uint32_t)) ||
		    (mode == XATTR_GMODE_LENGTH_ONLY && answerLength != sizeof(uint32_t))) {
			setDisconnect(true);
			ret = SAUNAFS_ERROR_IO;
		} else {
			ret = SAUNAFS_STATUS_OK;
		}
	}

	return ret;
}

uint8_t fs_setxattr(inode_t inode, uint8_t opened, uint32_t uid, uint32_t gid, uint8_t nleng,
                    const uint8_t *name, uint32_t vleng, const uint8_t *value, uint8_t mode) {
	uint8_t *wptr;
	const uint8_t *rptr;
	uint32_t answerLength;
	uint8_t ret;
	threc *rec = fs_get_my_threc();

	if (masterVersion < saunafsVersion(1, 6, 29)) { return SAUNAFS_ERROR_ENOTSUP; }

	if (mode >= XATTR_SMODE_REMOVE) { return SAUNAFS_ERROR_EINVAL; }

	constexpr uint32_t kPacketHeaderSize = sizeof(inode) + sizeof(opened) + sizeof(uid) +
	                                       sizeof(gid) + sizeof(nleng) + sizeof(vleng) +
	                                       sizeof(mode);

	wptr = fs_createpacket(rec, CLTOMA_FUSE_SETXATTR, kPacketHeaderSize + nleng + vleng);
	if (wptr == nullptr) { return SAUNAFS_ERROR_IO; }

	putINode(&wptr, inode);
	put8bit(&wptr, opened);
	put32bit(&wptr, uid);
	put32bit(&wptr, gid);
	put8bit(&wptr, nleng);
	memcpy(wptr, name, nleng);
	wptr += nleng;
	put32bit(&wptr, vleng);
	memcpy(wptr, value, vleng);
	wptr += vleng;
	put8bit(&wptr, mode);

	rptr = fs_sendandreceive(rec, MATOCL_FUSE_SETXATTR, &answerLength);

	if (rptr == nullptr) {
		ret = SAUNAFS_ERROR_IO;
	} else if (answerLength == 1) {
		ret = rptr[0];
	} else {
		setDisconnect(true);
		ret = SAUNAFS_ERROR_IO;
	}

	return ret;
}

uint8_t fs_removexattr(inode_t inode, uint8_t opened, uint32_t uid, uint32_t gid, uint8_t nleng,
                       const uint8_t *name) {
	uint8_t *wptr;
	const uint8_t *rptr;
	uint32_t answerLength;
	uint8_t ret;
	threc *rec = fs_get_my_threc();

	if (masterVersion < saunafsVersion(1, 6, 29)) { return SAUNAFS_ERROR_ENOTSUP; }

	constexpr uint32_t kPacketHeaderSize = sizeof(inode) + sizeof(opened) + sizeof(uid) +
	                                       sizeof(gid) + sizeof(nleng) + sizeof(uint32_t) +
	                                       sizeof(uint8_t);

	wptr = fs_createpacket(rec, CLTOMA_FUSE_SETXATTR, kPacketHeaderSize + nleng);
	if (wptr == nullptr) { return SAUNAFS_ERROR_IO; }

	putINode(&wptr, inode);
	put8bit(&wptr, opened);
	put32bit(&wptr, uid);
	put32bit(&wptr, gid);
	put8bit(&wptr, nleng);
	memcpy(wptr, name, nleng);
	wptr += nleng;
	put32bit(&wptr, 0);
	put8bit(&wptr, XATTR_SMODE_REMOVE);

	rptr = fs_sendandreceive(rec, MATOCL_FUSE_SETXATTR, &answerLength);

	if (rptr == nullptr) {
		ret = SAUNAFS_ERROR_IO;
	} else if (answerLength == 1) {
		ret = rptr[0];
	} else {
		setDisconnect(true);
		ret = SAUNAFS_ERROR_IO;
	}

	return ret;
}

uint8_t fs_deletacl(inode_t inode, uint32_t uid, uint32_t gid, AclType type) {
	threc* rec = fs_get_my_threc();
	auto message = cltoma::fuseDeleteAcl::build(rec->packetId, inode, uid, gid, type);
	if (!fs_saucreatepacket(rec, message)) {
		return SAUNAFS_ERROR_IO;
	}
	if (!fs_sausendandreceive(rec, SAU_MATOCL_FUSE_DELETE_ACL, message)) {
		return SAUNAFS_ERROR_IO;
	}
	try {
		uint8_t status;
		uint32_t dummyMessageId;
		matocl::fuseDeleteAcl::deserialize(message.data(), message.size(), dummyMessageId, status);
		return status;
	} catch (Exception& ex) {
		fs_got_inconsistent("SAU_MATOCL_DELETE_ACL", message.size(), ex.what());
		return SAUNAFS_ERROR_IO;
	}
}

uint8_t fs_getacl(inode_t inode, uint32_t uid, uint32_t gid, RichACL& acl, uint32_t &owner_id) {
	threc* rec = fs_get_my_threc();
	auto message = cltoma::fuseGetAcl::build(rec->packetId, inode, uid, gid, AclType::kRichACL);
	if (!fs_saucreatepacket(rec, message)) {
		return SAUNAFS_ERROR_IO;
	}
	if (!fs_sausendandreceive(rec, SAU_MATOCL_FUSE_GET_ACL, message)) {
		return SAUNAFS_ERROR_IO;
	}
	try {
		PacketVersion packetVersion;
		deserializePacketVersionNoHeader(message, packetVersion);
		if (packetVersion == matocl::fuseGetAcl::kStatusPacketVersion) {
			uint8_t status;
			uint32_t dummy_msg_id;
			matocl::fuseGetAcl::deserialize(message.data(), message.size(), dummy_msg_id,
					status);
			if (status == SAUNAFS_STATUS_OK) {
				fs_got_inconsistent("SAU_MATOCL_GET_ACL", message.size(),
						"version 0 and SAUNAFS_STATUS_OK");
				return SAUNAFS_ERROR_IO;
			}
			return status;
		} else if (packetVersion == matocl::fuseGetAcl::kRichACLResponsePacketVersion) {
			uint32_t dummy_msg_id;
			matocl::fuseGetAcl::deserialize(message.data(), message.size(), dummy_msg_id, owner_id, acl);
			return SAUNAFS_STATUS_OK;
		} else {
			fs_got_inconsistent("SAU_MATOCL_GET_ACL", message.size(),
					"unknown version " + std::to_string(packetVersion));
			return SAUNAFS_ERROR_IO;
		}
	} catch (Exception& ex) {
		fs_got_inconsistent("SAU_MATOCL_GET_ACL", message.size(), ex.what());
		return SAUNAFS_ERROR_IO;
	}
}

uint8_t fs_setacl(inode_t inode, uint32_t uid, uint32_t gid, const RichACL& acl) {
	threc* rec = fs_get_my_threc();
	auto message = cltoma::fuseSetAcl::build(rec->packetId, inode, uid, gid, acl);
	if (!fs_saucreatepacket(rec, message)) {
		return SAUNAFS_ERROR_IO;
	}
	if (!fs_sausendandreceive(rec, SAU_MATOCL_FUSE_SET_ACL, message)) {
		return SAUNAFS_ERROR_IO;
	}
	try {
		uint8_t status;
		uint32_t dummy_msg_id;
		matocl::fuseSetAcl::deserialize(message.data(), message.size(), dummy_msg_id, status);
		return status;
	} catch (Exception& ex) {
		fs_got_inconsistent("SAU_MATOCL_SET_ACL", message.size(), ex.what());
		return SAUNAFS_ERROR_IO;
	}
}

uint8_t fs_setacl(inode_t inode, uint32_t uid, uint32_t gid, AclType type,
                  const AccessControlList &acl) {
	threc* rec = fs_get_my_threc();
	auto message = cltoma::fuseSetAcl::build(rec->packetId, inode, uid, gid, type, acl);
	if (!fs_saucreatepacket(rec, message)) {
		return SAUNAFS_ERROR_IO;
	}
	if (!fs_sausendandreceive(rec, SAU_MATOCL_FUSE_SET_ACL, message)) {
		return SAUNAFS_ERROR_IO;
	}
	try {
		uint8_t status;
		uint32_t dummy_msg_id;
		matocl::fuseSetAcl::deserialize(message.data(), message.size(), dummy_msg_id, status);
		return status;
	} catch (Exception& ex) {
		fs_got_inconsistent("SAU_MATOCL_SET_ACL", message.size(), ex.what());
		return SAUNAFS_ERROR_IO;
	}
}

uint8_t fs_fullpath(inode_t inode, uint32_t uid, uint32_t gid, std::string &fullPath) {
	threc *rec = fs_get_my_threc();
	if (masterVersion < kFirstVersionWithPathByInodeHiddenFile) {
		safs::log_warn(
		    "fs_fullpath: Operation not supported for current master version: {}, for this operation "
		    "master version should be {} or higher",
		    saunafsVersionToString(masterVersion),
		    saunafsVersionToString(kFirstVersionWithPathByInodeHiddenFile));
		return SAUNAFS_ERROR_ENOTSUP;
	}
	auto message =
	    cltoma::fullPathByInode::build(rec->packetId, inode, uid, gid);
	if (!fs_saucreatepacket(rec, message)) {
		return SAUNAFS_ERROR_IO;
	}
	if (!fs_sausendandreceive(rec, SAU_MATOCL_FULL_PATH_BY_INODE, message)) {
		return SAUNAFS_ERROR_IO;
	}
	try {
		uint32_t msgid;
		PacketVersion packet_version;
		deserializePacketVersionNoHeader(message, packet_version);
		if (packet_version == matocl::fullPathByInode::kStatusPacketVersion) {
			uint8_t status;
			matocl::fullPathByInode::deserialize(message, msgid, status);
			if (status == SAUNAFS_STATUS_OK) {
				fs_got_inconsistent("SAU_MATOCL_FULL_PATH_BY_INODE",
				                    message.size(),
				                    "version 0 and SAUNAFS_STATUS_OK");
				return SAUNAFS_ERROR_IO;
			}
			return status;
		} else if (packet_version ==
		           matocl::fullPathByInode::kResponsePacketVersion) {
			matocl::fullPathByInode::deserialize(message, msgid, fullPath);
			return SAUNAFS_STATUS_OK;
		} else {
			fs_got_inconsistent(
			    "SAU_MATOCL_FULL_PATH_BY_INODE", message.size(),
			    "unknown version " + std::to_string(packet_version));
			return SAUNAFS_ERROR_IO;
		}
	} catch (Exception &ex) {
		fs_got_inconsistent("SAU_MATOCL_FULL_PATH_BY_INODE", message.size(),
		                    ex.what());
		return SAUNAFS_ERROR_IO;
	}
}

static uint32_t* msgIdPtr(const MessageBuffer& buffer) {
	PacketHeader header;
	deserializePacketHeader(buffer, header);
	uint32_t msgIdOffset = 0;
	if (header.isOldPacketType()) {
		msgIdOffset = serializedSize(PacketHeader());
	} else if (header.isSauPacketType()) {
		msgIdOffset = serializedSize(PacketHeader(), PacketVersion());
	} else {
		sassert(!"unrecognized packet header");
	}
	if (msgIdOffset + serializedSize(uint32_t()) > buffer.size()) {
		return nullptr;
	}
	return (uint32_t*) (buffer.data() + msgIdOffset);
}

uint8_t fs_custom(MessageBuffer& buffer) {
	threc *rec = fs_get_my_threc();
	uint32_t *ptr = nullptr;
	ptr = msgIdPtr(buffer);
	if (!ptr) {
		// packet too short
		return SAUNAFS_ERROR_EINVAL;
	}
	const uint32_t origMsgIdBigEndian = *ptr;
	*ptr = htonl(rec->packetId);
	if (!fs_saucreatepacket(rec, std::move(buffer))) {
		return SAUNAFS_ERROR_IO;
	}
	if (!fs_sausendandreceive_any(rec, buffer)) {
		return SAUNAFS_ERROR_IO;
	}
	ptr = msgIdPtr(buffer);
	if (!ptr) {
		// reply too short
		return SAUNAFS_ERROR_EINVAL;
	}
	*ptr = origMsgIdBigEndian;
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_raw_sendandreceive(MessageBuffer& buffer, PacketHeader::Type expectedType) {
	threc *rec = fs_get_my_threc();
	uint32_t *ptr = nullptr;
	ptr = msgIdPtr(buffer);
	if (!ptr) {
		// packet too short
		return SAUNAFS_ERROR_EINVAL;
	}
	*ptr = htonl(rec->packetId);
	if (!fs_saucreatepacket(rec, std::move(buffer))) {
		return SAUNAFS_ERROR_IO;
	}
	if (!fs_sausendandreceive(rec, expectedType, buffer)) {
		return SAUNAFS_ERROR_IO;
	}
	return SAUNAFS_STATUS_OK;
}

uint8_t fs_send_custom(MessageBuffer buffer) {
	threc *rec = fs_get_my_threc();
	if (!fs_saucreatepacket(rec, std::move(buffer))) {
		return SAUNAFS_ERROR_IO;
	}
	if (!fs_threc_flush(rec)) {
		return SAUNAFS_ERROR_IO;
	}
	return SAUNAFS_STATUS_OK;
}

bool fs_register_packet_type_handler(PacketHeader::Type type, PacketHandler *handler) {
	std::unique_lock<std::mutex> lock(perTypePacketHandlersLock);
	if (perTypePacketHandlers.count(type) > 0) {
		return false;
	}
	perTypePacketHandlers[type] = handler;
	return true;
}

bool fs_unregister_packet_type_handler(PacketHeader::Type type, PacketHandler *handler) {
	std::unique_lock<std::mutex> lock(perTypePacketHandlersLock);
	PerTypePacketHandlers::iterator it = perTypePacketHandlers.find(type);
	if (it == perTypePacketHandlers.end()) {
		return false;
	}
	if (it->second != handler) {
		return false;
	}
	perTypePacketHandlers.erase(it);
	return true;
}

void fs_saurenewwritegrant(uint64_t chunkId, uint64_t grantGeneration, uint64_t grantRandom) {
	threc *rec = fs_get_my_threc();
	std::vector<uint8_t> message;
	cltoma::writeGrantRenew::serialize(message, rec->packetId, chunkId, grantGeneration,
	                                   grantRandom);
	// Advisory: nothing to do if the send fails; the grant then lapses and revocation owns it.
	fs_saucreatepacket(rec, message);
	fs_sausend(rec);
}

void fs_flock_interrupt(const safs_locks::InterruptData &data) {
	threc *rec = fs_get_my_threc();
	auto message = cltoma::fuseFlock::build(rec->packetId, data);
	// is there anything we can do if send fails?
	fs_saucreatepacket(rec, message);
	fs_sausend(rec);
}

void fs_setlk_interrupt(const safs_locks::InterruptData &data) {
	threc *rec = fs_get_my_threc();
	auto message = cltoma::fuseSetlk::build(rec->packetId, data);
	// is there anything we can do if send fails?
	fs_saucreatepacket(rec, message);
	fs_sausend(rec);
}

uint8_t fs_getlk(inode_t inode, uint64_t owner, safs_locks::FlockWrapper &lock) {
	threc *rec = fs_get_my_threc();

	auto message = cltoma::fuseGetlk::build(rec->packetId, inode, owner, lock);

	if (!fs_saucreatepacket(rec, message)) {
		return SAUNAFS_ERROR_IO;
	}

	if (!fs_sausendandreceive(rec, SAU_MATOCL_FUSE_GETLK, message)) {
		return SAUNAFS_ERROR_IO;
	}

	try {
		PacketVersion packet_version;
		deserializePacketVersionNoHeader(message, packet_version);
		if (packet_version == matocl::fuseGetlk::kStatusPacketVersion) {
			uint8_t status;
			uint32_t message_id;
			matocl::fuseGetlk::deserialize(message, message_id, status);
			return status;
		} else if (packet_version == matocl::fuseGetlk::kResponsePacketVersion) {
			uint32_t message_id;
			matocl::fuseGetlk::deserialize(message, message_id, lock);
			return SAUNAFS_STATUS_OK;
		} else {
			fs_got_inconsistent(
				"SAU_MATOCL_GETLK",
				message.size(),
				"unknown version " + std::to_string(packet_version));
			return SAUNAFS_ERROR_IO;
		}
	} catch (Exception& ex) {
		fs_got_inconsistent("SAU_MATOCL_GETLK", message.size(), ex.what());
		return SAUNAFS_ERROR_IO;
	}
}

uint8_t fs_setlk_send(inode_t inode, uint64_t owner, uint32_t reqid,
                      const safs_locks::FlockWrapper &lock) {
	threc *rec = fs_get_my_threc();

	auto message = cltoma::fuseSetlk::build(rec->packetId, inode, owner, reqid, lock);

	if (!fs_saucreatepacket(rec, message)) {
		return SAUNAFS_ERROR_IO;
	}

	if (!fs_sausend(rec)) {
		return SAUNAFS_ERROR_IO;
	}

	return SAUNAFS_STATUS_OK;
}

uint8_t fs_setlk_recv() {
	MessageBuffer message;
	threc* rec = fs_get_my_threc();

	if (!fs_saurecv(rec, SAU_MATOCL_FUSE_SETLK, message)) {
		return SAUNAFS_ERROR_IO;
	}

	try {
		PacketVersion packetVersion;
		deserializePacketVersionNoHeader(message, packetVersion);
		if (packetVersion == 0) {
			uint8_t status;
			uint32_t dummyMessageId;
			matocl::fuseSetlk::deserialize(message, dummyMessageId, status);
			return status;
		} else {
			fs_got_inconsistent(
				"SAU_MATOCL_SETLK",
				message.size(),
				"unknown version " + std::to_string(packetVersion));
			return SAUNAFS_ERROR_IO;
		}
	} catch (Exception& ex) {
		fs_got_inconsistent("SAU_MATOCL_SETLK", message.size(), ex.what());
		return SAUNAFS_ERROR_IO;
	}
}

uint8_t fs_flock_send(inode_t inode, uint64_t owner, uint32_t reqid, uint16_t op) {
	threc *rec = fs_get_my_threc();

	auto message = cltoma::fuseFlock::build(rec->packetId, inode, owner, reqid, op);

	if (!fs_saucreatepacket(rec, message)) {
		return SAUNAFS_ERROR_IO;
	}

	if (!fs_sausend(rec)) {
		return SAUNAFS_ERROR_IO;
	}

	return SAUNAFS_STATUS_OK;
}

uint8_t fs_flock_recv() {
	MessageBuffer message;
	threc* rec = fs_get_my_threc();

	if (!fs_saurecv(rec, SAU_MATOCL_FUSE_FLOCK, message)) {
		return SAUNAFS_ERROR_IO;
	}

	try {
		PacketVersion packetVersion;
		deserializePacketVersionNoHeader(message, packetVersion);
		if (packetVersion == 0) {
			uint8_t status;
			uint32_t dummyMessageId;
			matocl::fuseFlock::deserialize(message, dummyMessageId, status);
			return status;
		} else {
			fs_got_inconsistent(
				"SAU_MATOCL_FLOCK",
				message.size(),
				"unknown version " + std::to_string(packetVersion));
			return SAUNAFS_ERROR_IO;
		}
	} catch (Exception& ex) {
		fs_got_inconsistent("SAU_MATOCL_FLOCK", message.size(), ex.what());
		return SAUNAFS_ERROR_IO;
	}
}

uint8_t fs_makesnapshot(inode_t src_inode, inode_t dst_inode, const std::string &dst_parent,
                        uint32_t uid, uint32_t gid, uint8_t can_overwrite,
                        SaunaClient::JobId &job_id) {
	static const int kBatchSize = 1024;
	threc *rec = fs_get_my_threc();
	job_id = 0;

	uint32_t msg_id;
	MessageBuffer response;
	auto request = cltoma::requestTaskId::build(rec->packetId);
	if (!fs_saucreatepacket(rec, request)) {
		return SAUNAFS_ERROR_IO;
	}
	if (!fs_sausendandreceive(rec, SAU_MATOCL_REQUEST_TASK_ID, response)) {
		return SAUNAFS_ERROR_IO;
	}

	try {
		matocl::requestTaskId::deserialize(response, msg_id, job_id);
	} catch (Exception& ex) {
		fs_got_inconsistent("SAU_MATOCL_FUSE_REQUEST_TASK_ID", request.size(), ex.what());
		job_id = 0;
		return SAUNAFS_ERROR_IO;
	}

	request = cltoma::snapshot::build(rec->packetId, job_id, src_inode, dst_inode, dst_parent,
	                                  uid, gid, can_overwrite, true, kBatchSize);

	if (!fs_saucreatepacket(rec, request)) {
		job_id = 0;
		return SAUNAFS_ERROR_IO;
	}

	response.clear();
	if (!fs_sausendandreceive(rec, SAU_MATOCL_FUSE_SNAPSHOT, response)) {
		job_id = 0;
		return SAUNAFS_ERROR_IO;
	}

	try {
		uint8_t status;
		matocl::snapshot::deserialize(response, msg_id, status);
		return status;
	} catch (Exception& ex) {
		fs_got_inconsistent("SAU_MATOCL_FUSE_SNAPSHOT", request.size(), ex.what());
		job_id = 0;
		return SAUNAFS_ERROR_IO;
	}
}

uint8_t fs_get_self_quota(uint32_t uid, uint32_t gid, inode_t inode,
                          std::vector<QuotaEntry> &quotaEntries) {
	threc *rec = fs_get_my_threc();
	if (masterVersion < kFirstVersionWithUseQuotaInVolumeSize) {
		safs::log_warn(
		    "fs_get_self_quota: Operation not supported for current master version: {}, for this operation "
		    "master version should be {} or higher",
		    saunafsVersionToString(masterVersion),
		    saunafsVersionToString(kFirstVersionWithUseQuotaInVolumeSize));
		return SAUNAFS_ERROR_ENOTSUP;
	}
	auto message = cltoma::fuseGetSelfQuota::build(rec->packetId, uid, gid, inode);

	if (!fs_saucreatepacket(rec, message)) {
		return SAUNAFS_ERROR_IO;
	}

	if (!fs_sausendandreceive(rec, SAU_MATOCL_FUSE_GET_SELF_QUOTA, message)) {
		return SAUNAFS_ERROR_IO;
	}

	try {
		PacketVersion packet_version;
		deserializePacketVersionNoHeader(message, packet_version);
		if (packet_version == matocl::fuseGetSelfQuota::kStatusPacketVersion) {
			uint8_t status;
			uint32_t dummy_message_id;
			matocl::fuseGetSelfQuota::deserialize(message, dummy_message_id, status);
			return status;
		} else if (packet_version == matocl::fuseGetSelfQuota::kResponsePacketVersion) {
			uint32_t messageId;
			matocl::fuseGetSelfQuota::deserialize(message, messageId, quotaEntries);
			return SAUNAFS_STATUS_OK;
		} else {
			return SAUNAFS_ERROR_EINVAL;
		}
	} catch (Exception& ex) {
		fs_got_inconsistent("SAU_MATOCL_FUSE_GET_SELF_QUOTA", message.size(), ex.what());
		return SAUNAFS_ERROR_IO;
	}
}

uint8_t fs_getgoal(inode_t inode, std::string &goal) {
	threc *rec = fs_get_my_threc();

	goal.clear();
	auto message = cltoma::fuseGetGoal::build(rec->packetId, inode, GMODE_NORMAL);

	if (!fs_saucreatepacket(rec, message)) {
		return SAUNAFS_ERROR_IO;
	}

	if (!fs_sausendandreceive(rec, SAU_MATOCL_FUSE_GETGOAL, message)) {
		return SAUNAFS_ERROR_IO;
	}

	try {
		PacketVersion packet_version;
		deserializePacketVersionNoHeader(message, packet_version);
		if (packet_version == matocl::fuseGetGoal::kStatusPacketVersion) {
			uint8_t status;
			uint32_t dummy_message_id;
			matocl::fuseGetGoal::deserialize(message, dummy_message_id, status);
			return status;
		} else if (packet_version == matocl::fuseGetGoal::kResponsePacketVersion) {
			std::vector<FuseGetGoalStats> goalsStats;
			uint32_t messageId;
			matocl::fuseGetGoal::deserialize(message, messageId, goalsStats);
			if (goalsStats.size() != 1) {
				return SAUNAFS_ERROR_EINVAL;
			}
			goal = goalsStats[0].goalName;
			return SAUNAFS_STATUS_OK;
		} else {
			return SAUNAFS_ERROR_EINVAL;
		}
	} catch (Exception& ex) {
		fs_got_inconsistent("SAU_MATOCL_FUSE_GETGOAL", message.size(), ex.what());
		return SAUNAFS_ERROR_IO;
	}
}

uint8_t fs_setgoal(inode_t inode, uint32_t uid, const std::string &goal_name, uint8_t smode) {
	threc *rec = fs_get_my_threc();

	auto message = cltoma::fuseSetGoal::build(rec->packetId, inode, uid, goal_name, smode);

	if (!fs_saucreatepacket(rec, message)) {
		return SAUNAFS_ERROR_IO;
	}

	if (!fs_sausendandreceive(rec, SAU_MATOCL_FUSE_SETGOAL, message)) {
		return SAUNAFS_ERROR_IO;
	}

	try {
		PacketVersion packet_version;
		deserializePacketVersionNoHeader(message, packet_version);
		if (packet_version == matocl::fuseSetGoal::kStatusPacketVersion) {
			uint8_t status;
			uint32_t dummy_message_id;
			matocl::fuseSetGoal::deserialize(message, dummy_message_id, status);
			return status;
		} else if (packet_version == matocl::fuseSetGoal::kResponsePacketVersion) {
			return SAUNAFS_STATUS_OK;
		} else {
			return SAUNAFS_ERROR_EINVAL;
		}
	} catch (Exception& ex) {
		fs_got_inconsistent("SAU_MATOCL_FUSE_SETGOAL", message.size(), ex.what());
		return SAUNAFS_ERROR_IO;
	}
}

uint8_t fs_getchunksinfo(uint32_t uid, uint32_t gid, inode_t inode, uint32_t chunk_index,
                         uint32_t chunk_count, std::vector<ChunkWithAddressAndLabel> &chunks) {
	threc *rec = fs_get_my_threc();

	auto message = cltoma::chunksInfo::build(rec->packetId, uid, gid, inode, chunk_index, chunk_count);

	if (!fs_saucreatepacket(rec, message)) {
		return SAUNAFS_ERROR_IO;
	}

	if (!fs_sausendandreceive(rec, SAU_MATOCL_CHUNKS_INFO, message)) {
		return SAUNAFS_ERROR_IO;
	}

	try {
		PacketVersion packet_version;
		deserializePacketVersionNoHeader(message, packet_version);
		if (packet_version == matocl::fuseSetGoal::kStatusPacketVersion) {
			uint8_t status;
			uint32_t message_id;
			matocl::chunksInfo::deserialize(message, message_id, status);
			return status;
		} else if (packet_version == matocl::fuseSetGoal::kResponsePacketVersion) {
			uint32_t message_id;
			matocl::chunksInfo::deserialize(message, message_id, chunks);
			return SAUNAFS_STATUS_OK;
		} else {
			return SAUNAFS_ERROR_EINVAL;
		}
	} catch (Exception& ex) {
		fs_got_inconsistent("SAU_MATOCL_CHUNKS_INFO", message.size(), ex.what());
		return SAUNAFS_ERROR_IO;
	}
}

uint8_t fs_getchunkservers(std::vector<ChunkserverListEntry> &chunkservers) {
	threc *rec = fs_get_my_threc();
	uint32_t message_id;

	auto message = cltoma::cservList::build(rec->packetId, true);

	if (!fs_saucreatepacket(rec, message)) {
		return SAUNAFS_ERROR_IO;
	}

	if (!fs_sausendandreceive(rec, SAU_MATOCL_CSERV_LIST, message)) {
		return SAUNAFS_ERROR_IO;
	}

	chunkservers.clear();
	matocl::cservList::deserialize(message, message_id, chunkservers);
	for (auto& server : chunkservers) {
		// If 127.0.0.1, let's assume chunkserver is the same as master host
		if (isLoopbackAddress(server.servip)) {
			server.servip = masterip;
			safs::log_debug("fs_getchunkservers: changing chunkserver ip address {} to {}",
				  htonl(server.servip),
				  htonl(masterip));
			safs::log_warn("localhost chunkserver ip addresses are experimental, consider assigning an IP address to chunkserver (via /etc/hosts or some other way)");
		}
	}
	return SAUNAFS_STATUS_OK;
}
