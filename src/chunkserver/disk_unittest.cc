#include <filesystem>
#include <fstream>
#include <future>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "chunkserver-common/cmr_chunk.h"
#include "chunkserver-common/cmr_disk.h"
#include "chunkserver-common/default_disk_manager.h"
#include "chunkserver-common/global_shared_resources.h"
#include "chunkserver-common/hdd_utils.h"
#include "common/exceptions.h"
#include "common/slice_traits.h"
#include "config/cfg.h"
#include "errors/leil_error_codes.h"
#include "unittests/TemporaryDirectory.h"

namespace {

// Fails with SAUNAFS_ERROR_IO on every call before succeedOnAttempt, then
// succeeds; the chunk pointer is never dereferenced.
class FlakyAttributesDisk : public CmrDisk {
public:
	using CmrDisk::CmrDisk;

	int updateChunkAttributes(IChunk * /*chunk*/, bool /*isFromScan*/) override {
		++callCount;
		return callCount < succeedOnAttempt ? SAUNAFS_ERROR_IO : SAUNAFS_STATUS_OK;
	}

	int unlinkChunk(IChunk * /*chunk*/) override {
		++unlinkCount;
		return SAUNAFS_STATUS_OK;
	}

	int callCount = 0;
	int succeedOnAttempt = 1;
	int unlinkCount = 0;
};

// True if another process could lock the file at path.
bool isLockFree(const std::string &path) {
	const pid_t child = fork();
	if (child == 0) {
		const int fd = ::open(path.c_str(), O_RDWR);
		_exit(fd >= 0 && ::lockf(fd, F_TEST, 0) == 0 ? 0 : 1);
	}
	int status = 0;
	waitpid(child, &status, 0);
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

}  // namespace

TEST(DiskTests, ParseSingleHddLine) {
	const std::string hddCfgLine {"/mnt/hdd_22/"};
	const disk::Configuration diskConfig(hddCfgLine);

	ASSERT_TRUE(diskConfig.isValid);
	ASSERT_FALSE(diskConfig.isComment);
	ASSERT_FALSE(diskConfig.isEmpty);
	ASSERT_FALSE(diskConfig.isZoned);

	const CmrDisk disk(diskConfig);

	ASSERT_EQ(disk.metaPath(), hddCfgLine);
	ASSERT_EQ(disk.metaPath(), disk.dataPath());
}

TEST(DiskTests, ParseCompoundHddLine) {
	const std::string hddCfgLine {"/mnt/hdd_35/meta | /mnt/hdd_35/data"};
	const disk::Configuration diskConfig(hddCfgLine);

	ASSERT_TRUE(diskConfig.isValid);
	ASSERT_FALSE(diskConfig.isComment);
	ASSERT_FALSE(diskConfig.isEmpty);
	ASSERT_FALSE(diskConfig.isZoned);

	const CmrDisk disk(diskConfig);

	ASSERT_EQ(disk.metaPath(), "/mnt/hdd_35/meta/");
	ASSERT_EQ(disk.dataPath(), "/mnt/hdd_35/data/");
}

TEST(DiskTests, ParseCommentedHddLine) {
	const disk::Configuration diskConfig("#/mnt/hdd_35/meta");

	ASSERT_FALSE(diskConfig.isValid);
	ASSERT_TRUE(diskConfig.isComment);
}

TEST(DiskTests, ParseEmptyHddLine) {
	const disk::Configuration diskConfig("   ");

	ASSERT_FALSE(diskConfig.isValid);
	ASSERT_TRUE(diskConfig.isEmpty);
}

TEST(DiskTests, ParseStartingAndTrailingSpacesHddLine) {
	const disk::Configuration diskConfig("   /mnt/hdd_35/meta   ");

	ASSERT_TRUE(diskConfig.isValid);
	ASSERT_FALSE(diskConfig.isEmpty);
}

TEST(DiskTests, ParseZonedHddLine) {
	std::string hddCfgLine {"zonefs:/mnt/hdd_35/meta | /mnt/hdd_35/data"};
	const disk::Configuration diskConfig(hddCfgLine);

	ASSERT_TRUE(diskConfig.isValid);
	ASSERT_TRUE(diskConfig.isZoned);

	const CmrDisk disk(diskConfig);

	ASSERT_TRUE(disk.isZonedDevice());

	hddCfgLine = "zonefs:/mnt/hdd_35/meta";
	const disk::Configuration diskConfig2(hddCfgLine);

	ASSERT_TRUE(diskConfig2.isZoned);
	ASSERT_FALSE(diskConfig2.isValid);
}

TEST(DiskTests, ParsePluginPrefixHddLine) {
	// A non-zonefs prefix selects a plugin serving a conventional device, so a
	// single path is enough.
	const disk::Configuration diskConfig("leil_cmr:/mnt/hdd_35");

	ASSERT_TRUE(diskConfig.isValid);
	ASSERT_FALSE(diskConfig.isZoned);
	ASSERT_EQ(diskConfig.prefix, "leil_cmr");
	ASSERT_EQ(diskConfig.metaPath, "/mnt/hdd_35/");
	ASSERT_EQ(diskConfig.dataPath, "/mnt/hdd_35/");

	// The removal marker is stripped before the prefix is read.
	const disk::Configuration removedDisk("*leil_cmr:/mnt/hdd_35");

	ASSERT_TRUE(removedDisk.isValid);
	ASSERT_TRUE(removedDisk.isMarkedForRemoval);
	ASSERT_EQ(removedDisk.prefix, "leil_cmr");
}

TEST(DiskTests, ParseHddLineWithoutPluginPrefix) {
	// A bare path has no prefix and is served by the built-in CmrDisk.
	const disk::Configuration plainPath("/mnt/hdd_35");

	ASSERT_TRUE(plainPath.isValid);
	ASSERT_TRUE(plainPath.prefix.empty());
	ASSERT_FALSE(plainPath.isZoned);

	// A colon inside a path is not a prefix separator: the path starts with a
	// character that cannot appear in a prefix.
	const disk::Configuration colonInPath("/mnt/hdd:35/meta");

	ASSERT_TRUE(colonInPath.isValid);
	ASSERT_TRUE(colonInPath.prefix.empty());
	ASSERT_EQ(colonInPath.metaPath, "/mnt/hdd:35/meta/");

	// A relative path with no colon at all is not a prefix either.
	const disk::Configuration relativePath("hdd_35");

	ASSERT_TRUE(relativePath.isValid);
	ASSERT_TRUE(relativePath.prefix.empty());

	// A relative path whose first component holds a colon needs the "./"
	// escape, since '.' cannot start a prefix.
	const disk::Configuration escapedPath("./archive:01");

	ASSERT_TRUE(escapedPath.isValid);
	ASSERT_TRUE(escapedPath.prefix.empty());
	ASSERT_EQ(escapedPath.metaPath, "./archive:01/");

	// Without the escape its first component is read as a plugin name.
	const disk::Configuration ambiguousPath("archive:01");

	ASSERT_TRUE(ambiguousPath.isValid);
	ASSERT_EQ(ambiguousPath.prefix, "archive");
}

TEST(DiskTests, ParseHddLineWithoutPaths) {
	// A selector with nothing after it is invalid, not an empty path.
	const disk::Configuration onlyPrefix("leil_cmr:");

	ASSERT_FALSE(onlyPrefix.isValid);
	ASSERT_FALSE(onlyPrefix.isComment);
	ASSERT_FALSE(onlyPrefix.isEmpty);

	// Same for a line holding nothing but the removal marker.
	const disk::Configuration onlyRemovalMarker("*");

	ASSERT_FALSE(onlyRemovalMarker.isValid);

	// A delimiter with nothing before it leaves no metadata path.
	const disk::Configuration emptyMetaPath("leil_cmr: | /mnt/data");

	ASSERT_FALSE(emptyMetaPath.isValid);
}

TEST(DiskTests, UpdateChunkAttributesRetrySucceedsAfterTransientErrors) {
	const disk::Configuration diskConfig("/mnt/hdd_22/");
	FlakyAttributesDisk disk(diskConfig);
	disk.succeedOnAttempt = kOpenRetryCount;

	ASSERT_EQ(hddUpdateChunkAttributesWithRetry(&disk, nullptr, false),
	          SAUNAFS_STATUS_OK);
	ASSERT_EQ(disk.callCount, kOpenRetryCount);
}

TEST(DiskTests, UpdateChunkAttributesRetryGivesUpAfterMaxAttempts) {
	const disk::Configuration diskConfig("/mnt/hdd_22/");
	FlakyAttributesDisk disk(diskConfig);
	disk.succeedOnAttempt = kOpenRetryCount + 1;  // never succeeds in time

	ASSERT_EQ(hddUpdateChunkAttributesWithRetry(&disk, nullptr, false),
	          SAUNAFS_ERROR_IO);
	ASSERT_EQ(disk.callCount, kOpenRetryCount);
}

TEST(DiskTests, ReloadKeepsDiskLock) {
	TemporaryDirectory dir("/tmp", "disk-reload-lock");
	const std::string line = dir.name() + "/";
	std::vector<std::unique_ptr<IDisk>> disks;
	disks.emplace_back(new CmrDisk(disk::Configuration(line)));
	disks.front()->createLockFiles(true, disks);
	ASSERT_FALSE(isLockFree(line + ".lock"));

	// A reload checks the line with a throwaway Disk.
	{
		CmrDisk reloaded{disk::Configuration(line)};
		reloaded.createLockFiles(false, disks);
	}

	EXPECT_FALSE(isLockFree(line + ".lock"));
}

TEST(DiskTests, RejectedSharedLockFileKeepsDiskLock) {
	TemporaryDirectory dir("/tmp", "disk-shared-lock");
	std::filesystem::create_directory(dir.name() + "/disk");
	std::filesystem::create_directory_symlink("disk", dir.name() + "/alias");
	std::vector<std::unique_ptr<IDisk>> disks;
	disks.emplace_back(new CmrDisk(disk::Configuration(dir.name() + "/disk/")));
	disks.front()->createLockFiles(true, disks);

	CmrDisk alias{disk::Configuration(dir.name() + "/alias/")};
	EXPECT_THROW(alias.createLockFiles(true, disks), InitializeException);

	EXPECT_FALSE(isLockFree(dir.name() + "/disk/.lock"));
}

// What locking the disk throws, or empty.
std::string lockFailure(CmrDisk &disk, std::vector<std::unique_ptr<IDisk>> &disks) {
	try {
		disk.createLockFiles(true, disks);
	} catch (const InitializeException &exception) { return exception.what(); }
	return "";
}

TEST(DiskTests, LockFileSharedWithinTheProcessIsNamed) {
	TemporaryDirectory dir("/tmp", "disk-own-lock");
	for (const char *name : {"/a", "/b", "/c"}) {
		std::filesystem::create_directory(dir.name() + name);
	}
	std::filesystem::create_directory_symlink("a", dir.name() + "/alias");
	std::vector<std::unique_ptr<IDisk>> disks;

	// One disk whose metadata and data paths are the same directory.
	{
		CmrDisk itself{disk::Configuration(dir.name() + "/a/ | " + dir.name() + "/alias/")};
		EXPECT_NE(lockFailure(itself, disks).find("same lockfile"), std::string::npos);
	}

	// One disk's metadata lock file is another's data lock file.
	disks.emplace_back(
	    new CmrDisk(disk::Configuration(dir.name() + "/a/ | " + dir.name() + "/b/")));
	disks.front()->createLockFiles(true, disks);
	CmrDisk crossed{disk::Configuration(dir.name() + "/b/ | " + dir.name() + "/c/")};
	EXPECT_NE(lockFailure(crossed, disks).find("same lockfile"), std::string::npos);
}

TEST(DiskTests, DiskLockedByAnotherProcessIsRefused) {
	TemporaryDirectory dir("/tmp", "disk-foreign-lock");
	const std::string line = dir.name() + "/";
	int ready[2];
	int release[2];
	ASSERT_EQ(pipe(ready), 0);
	ASSERT_EQ(pipe(release), 0);

	const pid_t child = fork();
	if (child == 0) {
		const int fd = ::open((line + ".lock").c_str(), O_RDWR | O_CREAT, 0640);
		char locked = fd >= 0 && ::lockf(fd, F_TLOCK, 0) == 0 ? 1 : 0;
		(void)!write(ready[1], &locked, 1);
		(void)!read(release[0], &locked, 1);
		_exit(0);
	}

	char locked = 0;
	ASSERT_EQ(read(ready[0], &locked, 1), 1);
	ASSERT_EQ(locked, 1);

	std::vector<std::unique_ptr<IDisk>> disks;
	CmrDisk disk{disk::Configuration(line)};
	EXPECT_THROW(disk.createLockFiles(true, disks), InitializeException);

	(void)!write(release[1], &locked, 1);
	waitpid(child, nullptr, 0);
	for (const int fd : {ready[0], ready[1], release[0], release[1]}) { close(fd); }
}

TEST(DiskTests, AttributesReadErrorKeepsChunk) {
	const disk::Configuration diskConfig("/mnt/hdd_22/");
	FlakyAttributesDisk disk(diskConfig);
	disk.succeedOnAttempt = kOpenRetryCount + 1;  // never succeeds in time
	const ChunkPartType type = slice_traits::standard::ChunkPartType();

	IChunk *chunk = hddChunkFindOrCreatePlusLock(&disk, 1, type, disk::ChunkGetMode::kCreateOnly);
	ASSERT_NE(chunk, nullptr);
	{
		const std::lock_guard lock(gTestsMutex);
		disk.chunks().insert(chunk);
	}
	hddChunkRelease(chunk);

	EXPECT_EQ(hddChunkFindAndLock(1, type), nullptr);
	EXPECT_EQ(disk.unlinkCount, 0);

	// Still registered, and released.
	disk.succeedOnAttempt = 1;
	disk.callCount = 0;
	EXPECT_EQ(hddChunkFindAndLock(1, type), chunk);
	EXPECT_EQ(disk.unlinkCount, 0);

	const std::lock_guard lock(gChunksMapMutex);
	hddRemoveChunkFromContainers(chunk);
}

TEST(DiskTests, OtherErrorsDoNotPushOutDamagingOnes) {
	CmrDisk disk("/mnt/", "/mnt", false, false);
	CmrChunk chunk(1, slice_traits::standard::ChunkPartType(), ChunkState::Locked);
	chunk.setOwner(&disk);

	errno = EIO;
	hddAddErrorAndPreserveErrno(&chunk);
	errno = EMFILE;
	for (int i = 0; i < 2 * disk::kLastErrorSize; ++i) { hddAddErrorAndPreserveErrno(&chunk); }
	EXPECT_FALSE(disk.damagingErrors().reachedLimit(disk::DamagingErrors::Clock::now()));
	errno = EIO;
	hddAddErrorAndPreserveErrno(&chunk);

	EXPECT_TRUE(disk.damagingErrors().reachedLimit(disk::DamagingErrors::Clock::now()));
}

TEST(DiskTests, DamagingErrorsCountWithinTheirWindow) {
	using std::chrono::seconds;
	const auto start = disk::DamagingErrors::Clock::now();
	disk::DamagingErrors errors;

	errors.add(start);
	errors.add(start + seconds(disk::kLastErrorTime + 1));
	EXPECT_FALSE(errors.reachedLimit(start + seconds(disk::kLastErrorTime + 1)));

	errors.add(start + seconds(disk::kLastErrorTime + 2));
	EXPECT_TRUE(errors.reachedLimit(start + seconds(disk::kLastErrorTime + 2)));
	EXPECT_FALSE(errors.reachedLimit(start + seconds(3 * disk::kLastErrorTime)));

	errors.clear();
	EXPECT_FALSE(errors.reachedLimit(start + seconds(disk::kLastErrorTime + 2)));
}

TEST(DiskTests, DiskListThatIsNotAFileIsRefused) {
	TemporaryDirectory dir("/tmp", "disk-list-fifo");
	const std::string list = dir.name() + "/hdd.cfg";
	ASSERT_EQ(mkfifo(list.c_str(), 0600), 0);
	struct CfgTermGuard {
		~CfgTermGuard() { cfg_term(); }
	} const guard;
	std::ofstream(dir.name() + "/test.cfg") << "HDD_CONF_FILENAME = " << list << "\n";
	ASSERT_EQ(cfg_load((dir.name() + "/test.cfg").c_str(), 0), 0);

	DefaultDiskManager manager;
	auto refused = std::async(std::launch::async, [&manager] {
		try {
			manager.reloadDisksFromCfg();
		} catch (const InitializeException &) { return true; }
		return false;
	});
	if (refused.wait_for(std::chrono::seconds(10)) == std::future_status::timeout) {
		// Unblock the open with a writer.
		::close(::open(list.c_str(), O_WRONLY));
		ADD_FAILURE() << "reading the disk list blocked on a pipe";
	}
	EXPECT_TRUE(refused.get());
}
