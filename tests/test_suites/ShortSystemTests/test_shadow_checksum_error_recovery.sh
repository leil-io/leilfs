CHUNKSERVERS=1 \
	MASTERSERVERS=2 \
	USE_RAMDISK="YES" \
	MASTER_EXTRA_CONFIG="METADATA_DUMP_PERIOD_SECONDS = 0 | MAGIC_DEBUG_LOG = ${TEMP_DIR}/log`
	`|LOG_FLUSH_ON=DEBUG" \
	setup_local_empty_saunafs info

touch "${info[mount0]}"/file

# Define pattern for the .LIVE changelog file
# Assume info[cluster_id] and info[hostname] are available or use defaults for master0
CLUSTER_ID=${info[cluster_id]:-testcluster}
HOSTNAME=${info[hostname]:-$(hostname -s)} # This should be master0's hostname
live_cl_pattern="${info[master_data_path]}/changelog.sfs.${CLUSTER_ID}.*.UNDEF.*.LIVE.${HOSTNAME}"

# Corrupt the changelog, start a shadow master and see if it can deal with it.
current_live_file=$(ls ${live_cl_pattern} 2>/dev/null | head -n 1)
if [ -n "$current_live_file" ] && [ -f "$current_live_file" ]; then
	echo "Corrupting changelog: $current_live_file"
	sed -i 's/file/fool/g' "$current_live_file"
else
	echo "ERROR: Could not find unique .LIVE changelog file matching pattern ${live_cl_pattern} to corrupt."
	# This is a critical failure for this test's logic, so exit
	exit 1
fi
saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"

# Check if the shadow stays synchronized after the error recovery.
touch "${info[mount0]}"/filefilefile
assert_eventually "saunafs_shadow_synchronized 1"

# Verify that it worked the way we expected, not due to bugs or a blind luck.
assert_success awk '
	/master.mismatch/ && i == 0 {i++; next;}
	/master.mltoma_changelog_apply_error/ && i == 1 {i++; next;}
	/master.matoml_changelog_apply_error/ && i == 2 {i++; next;}
	END {if (i != 3) {exit 1}}
	' ${TEMP_DIR}/log
