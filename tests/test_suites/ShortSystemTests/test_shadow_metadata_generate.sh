timeout_set 2 minutes

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

# Save path of meta-mount in SFS_META_MOUNT_PATH for metadata generators
export SFS_META_MOUNT_PATH=${info[mount1]}

# CHANGELOG env variable pointing to a single file is no longer used.
# Instead, set specific variables for metadata_generate_all if it needs to find changelogs.
export SAUNAFS_MASTER0_DATAPATH="${info[master_data_path]}"
export SAUNAFS_MASTER0_CLUSTER_ID="${info[cluster_id]:-testcluster}"
export SAUNAFS_MASTER0_HOSTNAME="${info[hostname]:-$(hostname -s)}"

saunafs_master_n 1 start

# Generate some metadata and remember it
cd "${info[mount0]}"
metadata_generate_all
metadata=$(metadata_print)
cd

# simulate master server failure and recovery from shadow
assert_eventually "saunafs_shadow_synchronized 1"
saunafs_master_daemon kill

saunafs_make_conf_for_master 1
saunafs_master_daemon reload
saunafs_wait_for_all_ready_chunkservers

# check restored filesystem
cd "${info[mount0]}"
assert_no_diff "$metadata" "$(metadata_print)"
metadata_validate_files
