timeout_set 1 minute

master_cfg="METADATA_DUMP_PERIOD_SECONDS = 0"
master_cfg+="|OPERATIONS_DELAY_INIT = 1"
master_cfg+="|CHUNKS_LOOP_MIN_TIME = 1|CHUNKS_LOOP_MAX_CPU = 90"
master_cfg+="|BACK_META_KEEP_PREVIOUS = 0"

CHUNKSERVERS=3 \
	MASTERSERVERS=2 \
	MOUNTS=2 \
	USE_RAMDISK="YES" \
	MOUNT_0_EXTRA_CONFIG="sfscachemode=NEVER,sfsreportreservedperiod=1,sfsdirentrycacheto=0" \
	MOUNT_1_EXTRA_CONFIG="sfsmeta" \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota,ignoregid" \
	SFSEXPORTS_META_EXTRA_OPTIONS="nonrootmeta" \
	MASTER_0_EXTRA_CONFIG="$master_cfg" \
	DEBUG_LOG_FAIL_ON="master.matoml_changelog_apply_error" \
	setup_local_empty_saunafs info

# Save SFS_META_MOUNT_PATH for metadata generators
export SFS_META_MOUNT_PATH=${info[mount1]}
# CHANGELOG env variable pointing to a single file is no longer valid with new dynamic names.
# Scripts using it (like metadata_generate_trash_ops) will need to be adapted,
# or this script needs to find the current .LIVE or latest finalized log to point to.
# For now, removing the export and will adapt consumers.
# export CHANGELOG="${info[master0_data_path]}"/changelog.sfs

# Generate a lot of different changes
# Pass necessary info for metadata_generate_all to find changelogs if needed.
export SAUNAFS_MASTER0_DATAPATH="${info[master0_data_path]}"
# Assuming CLUSTER_ID and HOSTNAME for master0 might be available in info array or use defaults
export SAUNAFS_MASTER0_CLUSTER_ID="${info[master0_cluster_id]:-testcluster}"
export SAUNAFS_MASTER0_HOSTNAME="${info[master0_hostname]:-$(hostname -s)}"

cd "${info[mount0]}"
metadata_generate_all
cd

# Dump metadata in the master server and wait for it to finish
assert_success saunafs_admin_master save-metadata

# There should be no other metadata files (BACK_META_KEEP_PREVIOUS = 0)
assert_equals 1 $(ls "${info[master_data_path]}" | grep -v lock | grep -c metadata)

# Verify if we can start shadow master from the freshly dumped metadata file
saunafs_master_n 1 start
assert_eventually 'saunafs_shadow_synchronized 1'

# Verify if we can modify the filesystem and all the changes would be applied by the shadow master
cd "${info[mount0]}"
rm -rf * || true
# Grep for CHECKSUM in any of master0's changelog files.
# Using SAUNAFS_MASTER0_DATAPATH which was set above.
assert_success grep -q CHECKSUM "${SAUNAFS_MASTER0_DATAPATH}"/changelog.sfs.*
assert_eventually 'saunafs_shadow_synchronized 1'
