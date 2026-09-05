#include <gtest/gtest.h>

#include "chunkserver-common/cmr_disk.h"
#include "chunkserver-common/global_shared_resources.h"
#include "chunkserver-common/hdd_utils.h"
#include "errors/saunafs_error_codes.h"

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

	int callCount = 0;
	int succeedOnAttempt = 1;
};

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
