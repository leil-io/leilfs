#include "disk_with_fd.h"

#include <bitset>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstdio>

#include "chunkserver-common/chunk_interface.h"
#include "chunkserver-common/hdd_stats.h"
#include "common/crc.h"
#include "common/exceptions.h"
#include "devtools/TracePrinter.h"

void initializeEmptyBlockCrcForDisks() {
	uint8_t *emptyBlockCrcBuffer = reinterpret_cast<uint8_t *>(&gEmptyBlockCrc);
	put32bit(&emptyBlockCrcBuffer, mycrc32_zeroblock(0, SFSBLOCKSIZE));
}

FDDisk::FDDisk(const std::string &_metaPath, const std::string &_dataPath,
               bool _isMarkedForRemoval, bool _isZonedDevice)
    : metaPath_(_metaPath),
      dataPath_(_dataPath),
      isMarkedForRemoval_(_isMarkedForRemoval),
      isZonedDevice_(_isZonedDevice),
      leaveFreeSpace_(disk::gLeaveFree),
      carry_(random() / static_cast<double>(RAND_MAX)) {}

FDDisk::FDDisk(const disk::Configuration &configuration)
    : metaPath_(configuration.metaPath),
      dataPath_(configuration.dataPath),
      isMarkedForRemoval_(configuration.isMarkedForRemoval),
      isZonedDevice_(configuration.isZoned),
      leaveFreeSpace_(disk::gLeaveFree),
      carry_(random() / static_cast<double>(RAND_MAX)) {}

bool FDDisk::isMarkedForDeletion() const {
	return isMarkedForRemoval_ || isReadOnly_;
}

bool FDDisk::isZonedDevice() const { return isZonedDevice_; }

bool FDDisk::isSelectableForNewChunk() const {
	return !isDamaged_ && !isMarkedForDeletion() && totalSpace_ != 0 &&
	       availableSpace_ != 0 && scanState_ == IDisk::ScanState::kWorking;
}

DiskInfo FDDisk::toDiskInfo() const {
	DiskInfo diskInfo;

	diskInfo.path = dataPath_;
	if (diskInfo.path.length() > LegacyString<uint8_t>::maxLength()) {
		const std::string dots("(...)");
		const uint32_t substrSize =
		    LegacyString<uint8_t>::maxLength() - dots.length();
		diskInfo.path =
		    dots + diskInfo.path.substr(diskInfo.path.length() - substrSize,
		                                substrSize);
	}

	diskInfo.entrySize = static_cast<uint16_t>(
	    serializedSize(diskInfo) - serializedSize(diskInfo.entrySize));

	std::bitset<8> flags;
	flags.set(disk::kMarkedForDeletionFlagIndex, isMarkedForDeletion());
	flags.set(disk::kIsDamageFlagIndex, isDamaged_);
	flags.set(disk::kScanInProgressFlagIndex,
	          scanState_ == ScanState::kInProgress);
	diskInfo.flags = static_cast<uint8_t>(flags.to_ulong());

	const uint32_t errorIndex =
	    (lastErrorIndex_ + (disk::kLastErrorSize - 1)) % disk::kLastErrorSize;
	diskInfo.errorChunkId = lastErrorTab_[errorIndex].chunkid;
	diskInfo.errorTimeStamp = lastErrorTab_[errorIndex].timestamp;

	if (scanState_ == ScanState::kInProgress) {
		diskInfo.used = scanProgress_;
		diskInfo.total = 0;
	} else {
		diskInfo.used = totalSpace_ - availableSpace_;
		diskInfo.total = totalSpace_;
	}

	diskInfo.chunksCount = chunks_.size();

	// Statistics: last minute
	HddStatistics hddStats = stats_[statsPos_];
	diskInfo.lastMinuteStats = hddStats;

	// last hour
	for (auto pos = 1U; pos < disk::kMinutesInOneHour; pos++) {
		hddStats.add(stats_[(statsPos_ + pos) % disk::kStatsHistoryIn24Hours]);
	}
	diskInfo.lastHourStats = hddStats;

	// last day
	for (auto pos = disk::kMinutesInOneHour; pos < disk::kStatsHistoryIn24Hours;
	     pos++) {
		hddStats.add(stats_[(statsPos_ + pos) % disk::kStatsHistoryIn24Hours]);
	}
	diskInfo.lastDayStats = hddStats;

	return diskInfo;
}

std::string FDDisk::getPaths() const {
	std::stringstream result;

	if (isMarkedForRemoval_) {
		result << "*";
	}

	result << metaPath_;

	if (dataPath_ != metaPath_) {
		result << " | " << dataPath_;
	}

	return result.str();
}

void FDDisk::createLockFile(bool isLockNeeded, const std::string &lockFilename,
                            bool isForMetadata,
                            std::vector<std::unique_ptr<IDisk>> &allDisks) {
	int lockFD = ::open(lockFilename.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0640);
	struct stat lockStat {};

	if (lockFD < 0 && errno == EROFS) {
		isReadOnly_ = true;
	}

	if (isReadOnly_ && isMarkedForRemoval_) {
		// Nothing to do, we can use a read only file system if it was
		// marked for removal.
	} else if (lockFD < 0) {
		safs_pretty_errlog(LOG_WARNING,
		                   "can't create lock file %s, marking hdd as damaged",
		                   lockFilename.c_str());
		isDamaged_ = true;
	} else if (isLockNeeded && ::lockf(lockFD, F_TLOCK, 0) < 0) {
		const int err = errno;
		close(lockFD);
		if (err == EAGAIN) {
			throw InitializeException("disk " + metaPath() +
			                          " already locked by another process");
		}
		safs_pretty_syslog(LOG_WARNING,
		                   "lockf(%s) failed, marking disk as damaged: %s",
		                   lockFilename.c_str(), strerr(err));
		isDamaged_ = true;
	} else if (fstat(lockFD, &lockStat) < 0) {
		const int err = errno;
		close(lockFD);
		safs_pretty_syslog(LOG_WARNING,
		                   "fstat(%s) failed, marking hdd as damaged: %s",
		                   lockFilename.c_str(), strerr(err));
		isDamaged_ = true;
	} else if (isLockNeeded) {
		for (const auto &disk : allDisks) {
			auto *fdDisk = dynamic_cast<FDDisk *>(disk.get());

			if (fdDisk == nullptr) {
				continue;
			}

			if (isForMetadata) {
				if (fdDisk->metaLockFile_ &&
				    fdDisk->metaLockFile_->isInTheSameDevice(lockStat.st_dev)) {
					if (fdDisk->metaLockFile_->isTheSameFile(lockStat.st_dev,
					                                         lockStat.st_ino)) {
						const std::string diskPath = fdDisk->metaPath_;
						close(lockFD);
						throw InitializeException(
						    "Metadata disks '" + metaPath() + "' and '" +
						    diskPath + "' have the same lockfile");
					}  // else: No problem, it is expected to have multiple
					// metadata directories in the same device (probably NVMe)
				}
			} else {
				if (fdDisk->dataLockFile_ &&
				    fdDisk->dataLockFile_->isInTheSameDevice(lockStat.st_dev)) {
					if (fdDisk->dataLockFile_->isTheSameFile(lockStat.st_dev,
					                                         lockStat.st_ino)) {
						const std::string diskPath = fdDisk->dataPath_;
						close(lockFD);
						throw InitializeException("Data disks '" + metaPath() +
						                          "' and '" + diskPath +
						                          "' have the same lockfile");
					}
					safs_pretty_syslog(
					    LOG_WARNING,
					    "Data disks '%s' and '%s' are on the same "
					    "physical device (could lead to "
					    "unexpected behaviours)",
					    dataPath().c_str(), fdDisk->dataPath_.c_str());
				}
			}
		}
	}

	if (!isDamaged_) {
		if (isForMetadata) {
			metaLockFile_ = std::unique_ptr<const disk::LockFile>(
			    new disk::LockFile(lockFD, lockStat.st_dev, lockStat.st_ino));
		} else {
			dataLockFile_ = std::unique_ptr<const disk::LockFile>(
			    new disk::LockFile(lockFD, lockStat.st_dev, lockStat.st_ino));
		}
	}
}

ssize_t FDDisk::writeCrc(IChunk *chunk, uint8_t *crcData) {
	return pwrite(chunk->metaFD(), crcData, chunk->getCrcBlockSize(),
	              chunk->getCrcOffset());
}

int FDDisk::fsyncChunk(IChunk *chunk) {
	const int metaResult = fsyncFD(chunk, true);
	const int dataResult = fsyncFD(chunk, false);

	if (metaResult != SAUNAFS_STATUS_OK || dataResult != SAUNAFS_STATUS_OK) {
		return SAUNAFS_ERROR_IO;
	}

	return SAUNAFS_STATUS_OK;
}

off64_t FDDisk::lseekMetadata(IChunk *chunk, off64_t offset, int whence) {
	return ::lseek(chunk->metaFD(), offset, whence);
}

off64_t FDDisk::lseekData(IChunk *chunk, off64_t offset, int whence) {
	return ::lseek(chunk->dataFD(), offset, whence);
}

int FDDisk::readChunkCrc(IChunk *chunk, uint32_t chunkVersion,
                         uint8_t *buffer) {
	TRACETHIS();
	assert(chunk);

	std::unique_ptr<ChunkSignature> chunkSignature = createChunkSignature();

	if (!chunkSignature->readFromDescriptor(this, chunk->metaFD(),
	                                        chunk->getSignatureOffset())) {
		const int errmem = errno;
		safs_silent_errlog(LOG_WARNING, "readChunkCrc: file:%s - read error",
		                   chunk->fullMetaFilename().c_str());
		errno = errmem;
		return SAUNAFS_ERROR_IO;
	}

	if (!chunkSignature->hasValidSignatureId()) {
		safs_pretty_syslog(LOG_WARNING, "readChunkCrc: file:%s - wrong header",
		                   chunk->fullMetaFilename().c_str());
		errno = 0;
		return SAUNAFS_ERROR_IO;
	}

	if (chunkVersion == disk::kMaxUInt32Number) {
		chunkVersion = chunk->version();
	}

	if (chunk->id() != chunkSignature->chunkId() ||
	    chunkVersion != chunkSignature->chunkVersion() ||
	    chunk->type().getId() != chunkSignature->chunkType().getId()) {
		safs_pretty_syslog(
		    LOG_WARNING,
		    "readChunkCrc: file:%s - wrong id/version/type in header "
		    "(%016" PRIX64 "_%08" PRIX32 ", typeId %" PRIu8 ")",
		    chunk->fullMetaFilename().c_str(), chunkSignature->chunkId(),
		    chunkSignature->chunkVersion(),
		    chunkSignature->chunkType().getId());
		errno = 0;
		return SAUNAFS_ERROR_IO;
	}

#ifndef ENABLE_CRC /* if NOT defined */
	for (int i = 0; i < SFSBLOCKSINCHUNK; ++i) {
		memcpy(buffer + i * crcSize, &emptyblockcrc, crcSize);
	}
#else  /* if ENABLE_CRC defined */
	{
		DiskReadStatsUpdater updater(chunk->owner(), chunk->getCrcBlockSize());
		ssize_t ret = ::pread(chunk->metaFD(), buffer, chunk->getCrcBlockSize(),
		                      chunk->getCrcOffset());
		if (ret != static_cast<ssize_t>(chunk->getCrcBlockSize())) {
			const int errmem = errno;
			safs_silent_errlog(LOG_WARNING,
			                   "readChunkCrc: file:%s - read error",
			                   chunk->fullMetaFilename().c_str());
			errno = errmem;
			updater.markReadAsFailed();
			return SAUNAFS_ERROR_IO;
		}
	}

	HddStats::overheadRead(chunk->getCrcBlockSize());
#endif /* ENABLE_CRC */
	errno = 0;

	return SAUNAFS_STATUS_OK;
}

int FDDisk::writeChunkHeader(IChunk *chunk) {
	auto writtenBytes = ::write(chunk->metaFD(), chunk->getChunkHeaderBuffer(),
	                            chunk->getHeaderSize());

	if (writtenBytes == static_cast<ssize_t>(chunk->getHeaderSize())) {
		return SAUNAFS_STATUS_OK;
	}

	return SAUNAFS_ERROR_IO;
}

int FDDisk::fsyncFD(IChunk *chunk, bool isForMetadata) {
	int result = -1;

	const auto fileDescriptor =
	    isForMetadata ? chunk->metaFD() : chunk->dataFD();
	const auto filename =
	    isForMetadata ? chunk->fullMetaFilename() : chunk->fullDataFilename();

	if (filename.empty() || fileDescriptor < 0) {
		return SAUNAFS_STATUS_OK;
	}

#ifdef F_FULLFSYNC
	result = fcntl(fileDescriptor, F_FULLFSYNC);

	if (result < 0) {
		int errmem = errno;
		safs_silent_errlog(LOG_WARNING,
		                   "fsyncFD: file:%s - fsync (via fcntl) error",
		                   filename.c_str());
		errno = errmem;
		return SAUNAFS_ERROR_IO;
	}
#else
	result = ::fsync(fileDescriptor);

	if (result < 0) {
		const int errmem = errno;
		safs_silent_errlog(LOG_WARNING,
		                   "fsyncFD: file:%s - fsync (direct call) error",
		                   filename.c_str());
		errno = errmem;
		return SAUNAFS_ERROR_IO;
	}
#endif

	return result;
}

uint32_t FDDisk::lastErrorIndex() const { return lastErrorIndex_; }

void FDDisk::setLastErrorIndex(uint32_t newLastErrorIndex) {
	lastErrorIndex_ = newLastErrorIndex;
}

std::array<disk::IoError, disk::kLastErrorSize> &FDDisk::lastErrorTab() {
	return lastErrorTab_;
}

uint32_t FDDisk::lastRefresh() const { return lastRefresh_; }

void FDDisk::setLastRefresh(uint32_t newLastRefresh) {
	lastRefresh_ = newLastRefresh;
}

std::thread &FDDisk::scanThread() { return scanThread_; }

void FDDisk::setScanThread(std::thread &&newScanThread) {
	scanThread_ = std::move(newScanThread);
}

bool FDDisk::isDamaged() const { return isDamaged_; }

void FDDisk::setIsDamaged(bool newIsDamaged) { isDamaged_ = newIsDamaged; }

uint8_t FDDisk::scanProgress() const { return scanProgress_; }

void FDDisk::setScanProgress(uint8_t newScanProgress) {
	scanProgress_ = newScanProgress;
}

IDisk::ScanState FDDisk::scanState() const { return scanState_; }

void FDDisk::setScanState(ScanState newScanState) { scanState_ = newScanState; }

bool FDDisk::isMarkedForRemoval() const { return isMarkedForRemoval_; }

void FDDisk::setIsMarkedForRemoval(bool newIsMarkedForRemoval) {
	isMarkedForRemoval_ = newIsMarkedForRemoval;
}

bool FDDisk::wasRemovedFromConfig() const { return wasRemovedFromConfig_; }

void FDDisk::setWasRemovedFromConfig(bool newWasRemovedFromConfig) {
	wasRemovedFromConfig_ = newWasRemovedFromConfig;
}

double FDDisk::carry() const { return carry_; }

void FDDisk::setCarry(double newCarry) { carry_ = newCarry; }

std::array<HddStatistics, disk::kStatsHistoryIn24Hours> &FDDisk::stats() {
	return stats_;
}

uint32_t FDDisk::statsPos() const { return statsPos_; }

void FDDisk::setStatsPos(uint32_t newStatsPos) { statsPos_ = newStatsPos; }

bool FDDisk::needRefresh() const { return needRefresh_.load(); }

void FDDisk::setNeedRefresh(bool newNeedRefresh) {
	needRefresh_.store(newNeedRefresh);
}

HddAtomicStatistics &FDDisk::getCurrentStats() { return currentStat_; }

DiskChunks &FDDisk::chunks() { return chunks_; }

bool FDDisk::isReadOnly() const { return isReadOnly_; }

void FDDisk::setIsReadOnly(bool newIsReadOnly) { isReadOnly_ = newIsReadOnly; }

uint64_t FDDisk::totalSpace() const { return totalSpace_; }

void FDDisk::setTotalSpace(uint64_t newTotalSpace) {
	totalSpace_ = newTotalSpace;
}

uint64_t FDDisk::availableSpace() const { return availableSpace_; }

void FDDisk::setAvailableSpace(uint64_t newAvailableSpace) {
	availableSpace_ = newAvailableSpace;
}

uint64_t FDDisk::leaveFreeSpace() const { return leaveFreeSpace_; }

void FDDisk::setLeaveFreeSpace(uint64_t newLeaveFreeSpace) {
	leaveFreeSpace_ = newLeaveFreeSpace;
}

const std::string &FDDisk::dataPath() const { return dataPath_; }

void FDDisk::setDataPath(const std::string &newDataPath) {
	dataPath_ = newDataPath;
}

const std::string &FDDisk::metaPath() const { return metaPath_; }

void FDDisk::setMetaPath(const std::string &newMetaPath) {
	metaPath_ = newMetaPath;
}
