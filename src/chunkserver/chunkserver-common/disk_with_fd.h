#pragma once

#include "common/platform.h"

#include "chunkserver-common/disk_interface.h"

inline uint32_t gEmptyBlockCrc;

// forward declaration
void initializeEmptyBlockCrcForDisks();

/**
 * FDDisk is a specialization of IDisk for Disks with IO operations based on
 * file descriptors.
 *
 * FDDisk contains common attributes and functions needed to perform IO
 * operations using file descriptors.
 **/
class FDDisk : public IDisk {
public:
	/// Constructs a Disk with previous information from hdd.cfg and assigns
	/// default values.
	explicit FDDisk(const std::string &_metaPath, const std::string &_dataPath,
	                bool _isMarkedForRemoval, bool _isZonedDevice);

	/// Constructs a Disk from a Configuration object read from hdd.cfg and
	/// assigns default values.
	explicit FDDisk(const disk::Configuration &configuration);

	// No need to copy or move them so far

	FDDisk(const FDDisk &) = delete;
	FDDisk(FDDisk &&) = delete;
	FDDisk &operator=(const FDDisk &) = delete;
	FDDisk &operator=(FDDisk &&) = delete;

	/// Virtual destructor needed for correct polymorphism
	virtual ~FDDisk() = default;

	/// Tells if this Disk is 'logically' marked for deletion.
	///
	/// Disks are considered marked for deletion, from the master's
	/// perspective, if they are explicitly marked for removal in the hdd.cfg
	/// file or if it is on a read-only file system.
	bool isMarkedForDeletion() const override;

	/// Tells if this Disk is a Zoned device
	bool isZonedDevice() const override;

	/// Tells if this Disk is suitable for storing new chunks,
	/// according to its general state
	bool isSelectableForNewChunk() const override;

	/// Returns a DiskInfo object from the information inside this Disk
	DiskInfo toDiskInfo() const override;

	/// Retrieves the paths in the format [*]metaPath[ | dataPath]
	///
	/// [] means optional, depending on current configuration.
	std::string getPaths() const override;

	/// Internal function to actually create the lock files
	void createLockFile(bool isLockNeeded, const std::string &lockFilename,
	                    bool isForMetadata,
	                    std::vector<std::unique_ptr<IDisk>> &allDisks);

	// IO

	/// Writes the crcData in the correct offset of the Chunks' file descriptor
	ssize_t writeCrc(IChunk *chunk, uint8_t *crcData) override;

	/// Synchronize the Chunk state with the storage device
	int fsyncChunk(IChunk *chunk) override;

	/// lseeks the metadata file descriptor
	///
	/// Should be possible for all Disk types if the metadata is stored in CMR
	/// drives.
	off64_t lseekMetadata(IChunk *chunk, off64_t offset, int whence) override;

	/// lseeks the data file descriptor if possible
	off64_t lseekData(IChunk *chunk, off64_t offset, int whence) override;

	/// Reads the CRC into the gOpenChunks entry for this Chunk
	int readChunkCrc(IChunk *chunk, uint32_t chunkVersion,
	                 uint8_t *buffer) override;

	/// Writes the Chunk header into the device
	///
	/// Assumes that the thread local header buffer was filled with correct
	/// in memory information (header + CRC).
	int writeChunkHeader(IChunk *chunk) override;

	/// Returns the path of the metadata directory
	const std::string &metaPath() const override;
	/// Sets the path of the metadata directory
	void setMetaPath(const std::string &newMetaPath);

	/// Returns the path of the data directory
	const std::string &dataPath() const override;
	/// Sets the path of the data directory
	void setDataPath(const std::string &newDataPath);

	/// Returns the reserved space in bytes defined by configuration
	uint64_t leaveFreeSpace() const override;
	/// Setter for leaveFreeSpace_
	void setLeaveFreeSpace(uint64_t newLeaveFreeSpace) override;

	/// Returns the available space of the disk in bytes
	uint64_t availableSpace() const override;
	/// Setter for availableSpace_
	void setAvailableSpace(uint64_t newAvailableSpace) override;

	// Return the total usable space of the disk in bytes
	uint64_t totalSpace() const override;
	/// Setter for totalSpace_
	void setTotalSpace(uint64_t newTotalSpace) override;

	/// Returns true if a read-only file system is detected in the Disk
	bool isReadOnly() const override;
	/// Setter for isReadOnly_
	void setIsReadOnly(bool newIsReadOnly) override;

	/// Returns true if the Disk usage needs to be updated
	bool needRefresh() const override;
	/// Setter for needRefresh_
	void setNeedRefresh(bool newNeedRefresh) override;

	/// Returns timestamp in seconds of the last time the Disk was refreshed
	uint32_t lastRefresh() const override;
	/// Setter for lastRefresh_
	void setLastRefresh(uint32_t newLastRefresh) override;

	/// Getter for currentStats
	HddAtomicStatistics &getCurrentStats() override;
	/// Getter for chunks in this Disk
	DiskChunks &chunks() override;

	/// Returns the position for rotating the stats
	uint32_t statsPos() const override;
	/// Setter for statsPos_
	void setStatsPos(uint32_t newStatsPos) override;

	/// Getter for stats_
	std::array<HddStatistics, disk::kStatsHistoryIn24Hours> &stats() override;

	/// Returns the carry value for this Disk
	double carry() const override;
	/// Setter for carry_
	void setCarry(double newCarry) override;

	/// Returns true if the Disk was removed from the configuration after
	/// reloading
	bool wasRemovedFromConfig() const override;
	/// Setter for wasRemovedFromConfig_
	void setWasRemovedFromConfig(bool newWasRemovedFromConfig) override;

	/// Returns true if the Disk is marked for removal from config file
	bool isMarkedForRemoval() const override;
	/// Setter for isMarkedForRemoval_
	void setIsMarkedForRemoval(bool newIsMarkedForRemoval) override;

	/// Returns the scanning state of the Disk
	ScanState scanState() const override;
	/// Setter for scanState_
	void setScanState(ScanState newScanState) override;

	/// Returns the scanning progress percent of the Disk
	uint8_t scanProgress() const override;
	/// Setter for scanProgress_
	void setScanProgress(uint8_t newScanProgress) override;

	/// Returns true if the Disk is reported as damaged
	bool isDamaged() const override;
	/// Setter for isDamaged_
	void setIsDamaged(bool newIsDamaged) override;

	/// Returns the scanning thread of the Disk
	std::thread &scanThread() override;
	/// Setter for scanThread_
	void setScanThread(std::thread &&newScanThread) override;

	/// Returns the history of the last errors
	std::array<disk::IoError, disk::kLastErrorSize> &lastErrorTab() override;

	/// Returns the index of the last error
	uint32_t lastErrorIndex() const override;
	/// Setter for lastErrorIndex_
	void setLastErrorIndex(uint32_t newLastErrorIndex) override;

private:
	/// Internal helper to sync both FDs (metadata and data)
	int fsyncFD(IChunk *chunk, bool isForMetadata);

	std::string metaPath_;  ///< Metadata directory
	std::string dataPath_;  ///< Data directory
	static std::string trashDir_;  ///< Trash directory name

	ScanState scanState_ = ScanState::kNeeded;  ///< Scanning status
	uint8_t scanProgress_ = 0;                  ///< Scan progress percentage

	std::atomic_bool needRefresh_ = true;  ///< Tells if the disk usage related
	                                       ///< fields need to be recalculated
	uint32_t lastRefresh_ = 0;  ///< Timestamp in seconds storing the last time
	                            ///< this Disk was refreshed

	bool wasRemovedFromConfig_ = false;  ///< Tells if this Disk is missing in
	                                     ///< the config file after reloading
	bool isMarkedForRemoval_ = false;    ///< Marked with * in the hdd.cfg file
	bool isReadOnly_ = false;  ///< A read-only file system was detected

	/// Reserved for future usage. Tells if this Disk is zoned, probably SMR
	bool isZonedDevice_ = false;

	/// Tells if this Disk contains important errors
	///
	/// Most common errors are related to:
	/// * Being unable to create a needed lock file
	/// * Read/write errors greater than the specified limit
	bool isDamaged_ = false;

	uint64_t leaveFreeSpace_ = 0;  ///< Reserved space in bytes (from config)
	uint64_t availableSpace_ = 0;  ///< Total - Used space - LeaveFree (bytes)
	uint64_t totalSpace_ = 0;  ///< Total usable space in bytes in this device

	HddAtomicStatistics currentStat_;  ///< Updated with every operation
	/// History of stats from last 24 hours
	std::array<HddStatistics, disk::kStatsHistoryIn24Hours> stats_;
	uint32_t statsPos_ = 0;  ///< Used to rotate the stats in the stats array

	/// History with last kLastErrorSize errors
	std::array<disk::IoError, disk::kLastErrorSize> lastErrorTab_;
	uint32_t lastErrorIndex_ = 0;  ///< Index of the last error

	/// Lock file for the metadata directory.
	/// Used to prevent the same Disk from being used by different chunkservers
	std::unique_ptr<const disk::LockFile> metaLockFile_ = nullptr;

	/// Lock file for the data directory.
	/// Used to prevent the same Disk from being used by different chunkservers
	std::unique_ptr<const disk::LockFile> dataLockFile_ = nullptr;

	double carry_;  ///< Assists at selecting the Disk for a new Chunk

	/// Holds the thread for scanning this Disk,
	/// so far hddDiskScanThread on hddspacemgr
	std::thread scanThread_;

	/// A collection of chunks_ located in this Disk.
	///
	/// The collection is ordered into a near-random sequence in which
	/// the chunks_ will be tested for checksum correctness.
	/// The collection is guarded by `gTestsMutex`, which should be locked
	/// when the collection is accessed for reading or modifying.
	DiskChunks chunks_;
};
