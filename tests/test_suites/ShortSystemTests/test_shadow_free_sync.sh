timeout_set 3 minutes

# Regression test for shadow-master free-inode (detained inode pool) synchronization.
#
# Forces a shadow master to rebuild the detained inode pool from the saved metadata image
# and then converge through changelog replay, and proves the free-inode state survives a
# shadow promotion unchanged.
#
# When a file is removed its inode is released into the detained inode pool (held for the
# reuse delay); allocating an inode takes it back out. Deleting files AFTER the metadata
# dump makes the live pool drift from the dumped image, which the shadow must reconcile on
# load.

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

# Generate namespace operations; the unlink and trash generators already detain inodes
# (free-inode state captured by the dump below).
cd "${info[mount0]}"
metadata_generate_files
metadata_generate_funny_inodes
metadata_generate_unlink
metadata_generate_trash_ops
metadata_generate_chunks
metadata_generate_renames
metadata_generate_touch
metadata_generate_truncate
# Files that exist at the dump and will be deleted afterwards.
mkdir free_churn
touch free_churn/file{1..60}
cd

# Dump the metadata image: the detained inode pool is captured at this point.
assert_success saunafs_admin_master save-metadata

# Churn the inode pool AFTER the dump. Deleting these files detains their inodes (the pool
# grows); the create+delete batch detains more. The live pool drifts from the dumped image,
# which is what the shadow must reconcile on load.
cd "${info[mount0]}"
rm -f free_churn/file{1..60}
touch free_extra{1..40}
rm -f free_extra{1..40}
cd

# Start the shadow AFTER the churn so it must rebuild the detained inode pool: load the saved
# metadata image, then converge through changelog replay.
saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"

# Capture the namespace as served by the original master.
cd "${info[mount0]}"
metadata=$(metadata_print)
cd

# Simulate master failure and recover from the shadow.
saunafs_master_daemon kill
saunafs_make_conf_for_master 1
saunafs_master_daemon reload
saunafs_wait_for_all_ready_chunkservers

# The promoted shadow must serve an identical namespace.
cd "${info[mount0]}"
assert_no_diff "$metadata" "$(metadata_print)"
metadata_validate_files
