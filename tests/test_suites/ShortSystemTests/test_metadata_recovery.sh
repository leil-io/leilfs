timeout_set 3 minutes

master_cfg="METADATA_DUMP_PERIOD_SECONDS = 0"
master_cfg+="|AUTO_RECOVERY = 1"

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

# Save path of changelog.sfs in CHANGELOG to make it possible to verify generated changes
export CHANGELOG="${info[master_data_path]}"/changelog.sfs

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
rm ${info[master_data_path]}/{changelog,metadata,sessions}.*
sfsmetarestore -a -d "${info[master_data_path]}"
saunafs_master_daemon start

# check restored filesystem
cd "${info[mount0]}"
assert_no_diff "$metadata" "$(metadata_print)"
saunafs_wait_for_all_ready_chunkservers
metadata_validate_files
