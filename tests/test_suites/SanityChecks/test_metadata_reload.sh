timeout_set 2 minutes

master_cfg="METADATA_DUMP_PERIOD_SECONDS = 0"

CHUNKSERVERS=3 \
	MOUNTS=2 \
	USE_RAMDISK="YES" \
	MOUNT_0_EXTRA_CONFIG="sfscachemode=NEVER,sfsreportreservedperiod=1,sfsdirentrycacheto=0" \
	MOUNT_1_EXTRA_CONFIG="sfsmeta" \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota,ignoregid" \
	SFSEXPORTS_META_EXTRA_OPTIONS="nonrootmeta" \
	MASTER_EXTRA_CONFIG="${master_cfg}" \
	DEBUG_LOG_FAIL_ON="master.fs.checksum.mismatch" \
	setup_local_empty_saunafs info

# Generate some metadata and remember it
cd "${info[mount0]}"
metadata_generate_all
metadata="$(metadata_print)"

# simulate master server restart
cd
assert_success saunafs_master_daemon restart
saunafs_wait_for_all_ready_chunkservers

# check restored filesystem
cd "${info[mount0]}"
assert_no_diff "${metadata}" "$(metadata_print)"
metadata_validate_files
