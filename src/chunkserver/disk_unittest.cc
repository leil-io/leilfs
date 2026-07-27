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
