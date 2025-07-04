timeout_set '1 minute'

master_cfg="METADATA_DUMP_PERIOD_SECONDS = 0"
master_cfg+="|MAGIC_DEBUG_LOG = ${TEMP_DIR}/log|LOG_FLUSH_ON=DEBUG"
master_cfg+="|METADATA_SAVE_REQUEST_MIN_PERIOD = $(timeout_rescale_seconds 10)"

CHUNKSERVERS=1 \
	MASTERSERVERS=2 \
	USE_RAMDISK="YES" \
	MASTER_EXTRA_CONFIG="$master_cfg" \
	DEBUG_LOG_DISABLE_FAIL_ON="master.mismatch" \
	setup_local_empty_saunafs info

# Define pattern for the .LIVE changelog file for master0
CLUSTER_ID=${info[cluster_id]:-testcluster}
HOSTNAME=${info[hostname]:-$(hostname -s)} # This should be master0's hostname
live_cl_pattern="${info[master_data_path]}/changelog.sfs.${CLUSTER_ID}.*.UNDEF.*.LIVE.${HOSTNAME}"

# Do some change and corrupt the changelog
touch "${info[mount0]}"/file
current_live_file=$(ls ${live_cl_pattern} 2>/dev/null | head -n 1)
if [ -n "$current_live_file" ] && [ -f "$current_live_file" ]; then
	echo "Corrupting changelog (1st time): $current_live_file"
	sed -i 's/file/fool/g' "$current_live_file"
else
	echo "ERROR: Could not find unique .LIVE changelog file (1st time) matching pattern ${live_cl_pattern} to corrupt."
	exit 1
fi

# Start a shadow master and see if it can deal with the corrupted changelog
saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"

# Check if the shadow stays synchronized after the error recovery
touch "${info[mount0]}"/filefilefile
assert_eventually "saunafs_shadow_synchronized 1"

# Verify that it worked the way we expected, not due to bugs or a blind luck.
log=$(cat "${TEMP_DIR}/log")
truncate -s0 "${TEMP_DIR}/log"
assert_awk_finds '/master.mismatch/' "$log"
assert_awk_finds '/master.mltoma_changelog_apply_error: do/' "$log"
assert_awk_finds_no '/master.mltoma_changelog_apply_error: delay/' "$log"

# Stop the shadow master, generate new changes, break them.
saunafs_master_n 1 stop
touch "${info[mount0]}"/file2 # This creates a new entry in master0's changelog
# The .LIVE file might have rotated or changed due to previous operations or time. Re-find it.
current_live_file=$(ls ${live_cl_pattern} 2>/dev/null | head -n 1)
if [ -n "$current_live_file" ] && [ -f "$current_live_file" ]; then
	echo "Corrupting changelog (2nd time): $current_live_file"
	sed -i 's/file2/fool2/g' "$current_live_file"
else
	echo "ERROR: Could not find unique .LIVE changelog file (2nd time) matching pattern ${live_cl_pattern} to corrupt."
	# Adding ls output for debugging in case of failure
	ls -la "${info[master_data_path]}"
	exit 1
fi

# Start the shadow master again; it shouldn't synchronize within first seconds because of
# METADATA_SAVE_REQUEST_MIN_PERIOD. Let's verify if the synchronization needs at least 7 seconds.
saunafs_master_n 1 start
sleep $(timeout_rescale_seconds 7)
assert_failure saunafs_shadow_synchronized 1
assert_eventually "saunafs_shadow_synchronized 1" "15 seconds"

# Verify if the METADATA_SAVE_REQUEST_MIN_PERIOD mechanism was used
log=$(cat "${TEMP_DIR}/log")
assert_awk_finds '/master.mismatch/' "$log"
assert_awk_finds '/master.mltoma_changelog_apply_error: delay/' "$log"
assert_awk_finds '/master.mltoma_changelog_apply_error: do/' "$log"
