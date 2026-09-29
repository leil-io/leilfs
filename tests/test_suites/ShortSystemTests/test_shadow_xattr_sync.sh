timeout_set 3 minutes
assert_program_installed attr

# Regression test for shadow-master extended-attribute (xattr) synchronization.
#
# Forces a shadow master to rebuild the extended attributes from the saved metadata image
# and then converge through changelog replay, and proves the xattr state survives a shadow
# promotion unchanged.
#
# Xattrs are not coupled to node create/delete, so this churns xattrs on pre-existing files
# AFTER the metadata dump: the drift is isolated to the xattr state (no node or edge
# changes), and the shadow must reconcile the dumped image with the post-dump changelog.

# METADATA_DUMP_PERIOD_SECONDS = 0 disables metadata periodical dumping, preventing the master
# from auto-re-dumping a fresh image with the drifted xattrs. This ensures the shadow must
# rebuild the extended attributes from the saved metadata image.
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
	MASTER_0_EXTRA_CONFIG="MAGIC_DEBUG_LOG = ${TEMP_DIR}/master0.log|LOG_FLUSH_ON=DEBUG" \
	setup_local_empty_saunafs info

export SFS_META_MOUNT_PATH=${info[mount1]}
export CHANGELOG="${info[master_data_path]}"/changelog.sfs

# Generate files (with xattrs) plus the node/chunk operations around them.
cd "${info[mount0]}"
metadata_generate_files
metadata_generate_funny_inodes
metadata_generate_chunks
metadata_generate_xattrs
# Files with xattrs that exist at the dump and will have their xattrs churned afterwards.
mkdir xattr_churn
touch xattr_churn/file{1..30}
for i in {1..30}; do
	attr -qs predump -V "predump_value_$i" xattr_churn/file$i
done
cd

# Dump the metadata image: the xattr state is captured at this point.
assert_success saunafs_admin_master save-metadata

# Churn xattrs AFTER the dump on files that already exist, so the drift is purely in the
# xattr state (no node/edge changes): add new xattrs, remove some of the earlier ones.
cd "${info[mount0]}"
for i in {1..30}; do
	attr -qs postdump -V "postdump_value_$i" xattr_churn/file$i
done
for i in {1..15}; do
	attr -r predump xattr_churn/file$i
done
attr -qs churn -V churn_value xattr_file
cd

# Start the shadow AFTER the churn so it must rebuild the xattrs: load the saved metadata
# image, then converge through changelog replay.
saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"

# The shadow must converge from the metadata image saved above. A shadow that fails to apply the
# changelog asks the master for a fresh image (SAU_MLTOMA_CHANGELOG_APPLY_ERROR); the master then
# re-dumps and the shadow resynchronizes from already-current state, so the assertions below would
# pass without the xattr rebuild ever being exercised.
assert_file_exists "${TEMP_DIR}/master0.log"
log=$(cat "${TEMP_DIR}/master0.log")
assert_awk_finds_no '/SAU_MLTOMA_CHANGELOG_APPLY_ERROR/' "$log"

# Capture the namespace (including xattrs via getfattr) as served by the original master.
cd "${info[mount0]}"
metadata=$(metadata_print)
cd

# Simulate master failure and recover from the shadow.
saunafs_master_daemon kill
saunafs_make_conf_for_master 1
saunafs_master_daemon reload
saunafs_wait_for_all_ready_chunkservers

# The promoted shadow must serve identical xattrs.
cd "${info[mount0]}"
assert_no_diff "$metadata" "$(metadata_print)"
metadata_validate_files
