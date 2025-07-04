timeout_set 3 minutes

master_cfg="METADATA_DUMP_PERIOD_SECONDS = 0"
master_cfg+="|AUTO_RECOVERY = 1"
master_cfg+="|EMPTY_TRASH_PERIOD = 1"
master_cfg+="|EMPTY_RESERVED_INODES_PERIOD = 1"

CHUNKSERVERS=3 \
	MOUNTS=2 \
	USE_RAMDISK="YES" \
	MOUNT_0_EXTRA_CONFIG="sfscachemode=NEVER,sfsreportreservedperiod=1,sfsdirentrycacheto=0" \
	MOUNT_1_EXTRA_CONFIG="sfsmeta" \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota,ignoregid" \
	SFSEXPORTS_META_EXTRA_OPTIONS="nonrootmeta" \
	MASTER_EXTRA_CONFIG="$master_cfg" \
	DEBUG_LOG_FAIL_ON="master.fs.checksum.mismatch" \
	setup_local_empty_saunafs info

# Save path of meta-mount in SFS_META_MOUNT_PATH for metadata generators
export SFS_META_MOUNT_PATH=${info[mount1]}

# CHANGELOG env variable pointing to a single file is no longer used.
# Instead, set specific variables for metadata_generate_all if it needs to find changelogs.
export SAUNAFS_MASTER0_DATAPATH="${info[master_data_path]}"
export SAUNAFS_MASTER0_CLUSTER_ID="${info[cluster_id]:-testcluster}"
export SAUNAFS_MASTER0_HOSTNAME="${info[hostname]:-$(hostname -s)}"

saunafs_metalogger_daemon start

# Generate some metadata and remember it
cd "${info[mount0]}"
metadata_generate_all
metadata=$(metadata_print)

# Check if the metadata checksum is fine.
# Possible checksum mismatch will be reported at the end of the test.
assert_success saunafs_admin_master magic-recalculate-metadata-checksum

# simulate master server failure and recovery
sleep 3
cd
saunafs_master_daemon kill
# leave only files written by metalogger
# This means removing master's metadata, sessions, and specific master changelogs.
# Metalogger files (e.g. metadata_ml.sfs, changelog_ml.sfs.*) should remain.
rm -f ${info[master_data_path]}/metadata.sfs* \
      ${info[master_data_path]}/sessions.sfs* \
      ${info[master_data_path]}/changelog.sfs.*
sfsmetarestore -a -d "${info[master_data_path]}"
saunafs_master_daemon start

# check restored filesystem
cd "${info[mount0]}"
assert_no_diff "$metadata" "$(metadata_print)"
saunafs_wait_for_all_ready_chunkservers
metadata_validate_files
