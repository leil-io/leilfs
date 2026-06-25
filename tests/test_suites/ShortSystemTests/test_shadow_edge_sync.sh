timeout_set 3 minutes

# Regression test for shadow-master edge (directory topology) synchronization.
#
# Forces a shadow master to rebuild the whole namespace from the saved metadata
# image and then converge through changelog replay, and proves the directory edges
# survive a shadow promotion unchanged. Focuses on edge/topology operations
# (creates, renames, unlinks, hardlinks, snapshots).

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

# Save path of meta-mount in SFS_META_MOUNT_PATH for metadata generators (trash ops)
export SFS_META_MOUNT_PATH=${info[mount1]}

# Save path of changelog.sfs in CHANGELOG so the generators can verify generated changes
export CHANGELOG="${info[master_data_path]}"/changelog.sfs

# Generate namespace topology plus the node/xattr/chunk operations that shape edges.
cd "${info[mount0]}"
metadata_generate_files
metadata_generate_funny_inodes
metadata_generate_unlink
metadata_generate_trash_ops
metadata_generate_setgoal
metadata_generate_settrashtime
metadata_generate_seteattr
metadata_generate_chunks
metadata_generate_snapshot
metadata_generate_xattrs
metadata_generate_renames
metadata_generate_uids_gids
metadata_generate_touch
metadata_generate_truncate
cd

# Dump the metadata image. The shadow will later load this image and replay only
# the changelog entries produced after it.
assert_success saunafs_admin_master save-metadata

# Churn the topology AFTER the dump, so the shadow cannot simply load the image: it
# must reconcile the saved image with the post-dump changelog (new files, a rename,
# unlinks, a hardlink). This is what makes the load + changelog-replay path
# load-bearing for the assertion below.
cd "${info[mount0]}"
mkdir post_dump_dir
touch post_dump_dir/file{1..20}
mkdir post_dump_rename_src
mv post_dump_rename_src post_dump_rename_dst
touch post_dump_unlink{1..10}
rm -f post_dump_unlink{1..10}
ln post_dump_dir/file1 post_dump_dir/file1_hardlink
cd

# Start the shadow AFTER the churn so it must rebuild the namespace: load the saved
# metadata image, then converge through changelog replay.
saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"

# Capture the namespace as served by the original master.
cd "${info[mount0]}"
metadata=$(metadata_print)
cd

# Simulate master server failure and recover from the shadow.
saunafs_master_daemon kill
saunafs_make_conf_for_master 1
saunafs_master_daemon reload
saunafs_wait_for_all_ready_chunkservers

# The promoted shadow must serve an identical namespace: edges, nodes, xattrs, chunks.
cd "${info[mount0]}"
assert_no_diff "$metadata" "$(metadata_print)"
metadata_validate_files
