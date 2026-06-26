timeout_set 3 minutes

# Regression test for shadow-master quota synchronization.
#
# Forces a shadow master to rebuild quota limits from the saved metadata image and then
# converge through changelog replay, and proves the quota state survives a shadow promotion
# unchanged.
#
# Quota limits are not coupled to node create/delete, so this changes limits on pre-existing
# owners AFTER the metadata dump: the drift is isolated to the quota state (no node or edge
# changes), and the shadow must reconcile the dumped image with the post-dump changelog.

# METADATA_DUMP_PERIOD_SECONDS = 0 disables metadata periodical dumping, preventing the master
# from auto-re-dumping a fresh image with the drifted quota limits. This ensures the shadow
# must rebuild the quotas from the saved metadata image.
master_cfg="METADATA_DUMP_PERIOD_SECONDS = 0"

CHUNKSERVERS=3 \
	MASTERSERVERS=2 \
	MOUNTS=2 \
	USE_RAMDISK="YES" \
	MOUNT_0_EXTRA_CONFIG="sfscachemode=NEVER,sfsreportreservedperiod=1,sfsdirentrycacheto=0" \
	MOUNT_1_EXTRA_CONFIG="sfsmeta" \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota,ignoregid" \
	SFSEXPORTS_META_EXTRA_OPTIONS="nonrootmeta" \
	MASTER_EXTRA_CONFIG="$master_cfg" \
	setup_local_empty_saunafs info

export SFS_META_MOUNT_PATH=${info[mount1]}
export CHANGELOG="${info[master_data_path]}"/changelog.sfs

# Generate namespace operations, then set quota limits for a set of user and group owners. These
# owners exist at the dump and will have their limits changed afterwards.
cd "${info[mount0]}"
metadata_generate_files
metadata_generate_funny_inodes
metadata_generate_chunks
for owner in {2001..2020}; do
	saunafs setquota -u "$owner" 10GB 30GB 100 200 .
	saunafs setquota -g "$owner" 5GB 15GB 50 100 .
done
cd

# Dump the metadata image: the quota limits are captured at this point.
assert_success saunafs_admin_master save-metadata

# Churn quota limits AFTER the dump on the same (pre-existing) owners, so the drift is purely in
# the quota state (no node/edge changes). The shadow must reconcile the dumped image with the
# post-dump changelog.
cd "${info[mount0]}"
for owner in {2001..2020}; do
	saunafs setquota -u "$owner" 50GB 90GB 500 800 .
	saunafs setquota -g "$owner" 40GB 70GB 400 600 .
done
cd

# Start the shadow AFTER the churn so it must rebuild the quotas: load the saved metadata image,
# then converge through changelog replay.
saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"

# Capture the namespace (including quotas via repquota) as served by the original master.
cd "${info[mount0]}"
metadata=$(metadata_print)
cd

# Simulate master failure and recover from the shadow.
saunafs_master_daemon kill
saunafs_make_conf_for_master 1
saunafs_master_daemon reload
saunafs_wait_for_all_ready_chunkservers

# The promoted shadow must serve identical quotas.
cd "${info[mount0]}"
assert_no_diff "$metadata" "$(metadata_print)"
metadata_validate_files
