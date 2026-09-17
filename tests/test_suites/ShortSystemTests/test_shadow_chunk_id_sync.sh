timeout_set 3 minutes

# Regression test for the chunk-ID generator during shadow synchronization. A chunk allocated
# after the last saved metadata state must be recreated with the same ID when the shadow applies
# the subsequent changelog entries.

master_cfg="METADATA_DUMP_PERIOD_SECONDS = 0"

CHUNKSERVERS=3 \
	MASTERSERVERS=2 \
	MOUNTS=1 \
	USE_RAMDISK="YES" \
	MOUNT_0_EXTRA_CONFIG="sfscachemode=NEVER,sfsreportreservedperiod=1,sfsdirentrycacheto=0" \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota,ignoregid" \
	MASTER_EXTRA_CONFIG="$master_cfg" \
	MASTER_0_EXTRA_CONFIG="MAGIC_DEBUG_LOG = ${TEMP_DIR}/master0.log|LOG_FLUSH_ON=DEBUG" \
	setup_local_empty_saunafs info

# Save the metadata state before allocating the chunk exercised by this test.
assert_success saunafs_admin_master save-metadata

cd "${info[mount0]}"
FILE_SIZE=1M file-generate post_checkpoint_chunk
cd

# Allow pending metadata updates to become visible before starting the shadow. Synchronization
# must still replay the later chunk allocation with the same ID assigned by the primary.
sleep 1

saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"

# A replay-time chunk-ID mismatch can trigger automatic resynchronization and hide the original
# failure. Reject any changelog application error instead of allowing the test to self-heal.
assert_file_exists "${TEMP_DIR}/master0.log"
log=$(cat "${TEMP_DIR}/master0.log")
assert_awk_finds_no '/SAU_MLTOMA_CHANGELOG_APPLY_ERROR/' "$log"

saunafs_master_daemon kill
saunafs_make_conf_for_master 1
saunafs_master_daemon reload
saunafs_wait_for_all_ready_chunkservers

cd "${info[mount0]}"
file-validate post_checkpoint_chunk
