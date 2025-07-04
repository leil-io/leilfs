USE_RAMDISK="YES" \
	MASTER_EXTRA_CONFIG="MAGIC_DEBUG_LOG=${TEMP_DIR}/reloads|LOG_FLUSH_ON=DEBUG" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

# Define a glob for master changelogs in this specific data path.
# Assuming CLUSTER_ID and HOSTNAME for master0 (implicit target) are available from 'info' array or use defaults.
CLUSTER_ID=${info[cluster_id]:-testcluster}
HOSTNAME=${info[hostname]:-$(hostname -s)} # Or specific info[master0_hostname] if available
# This pattern needs to be general enough for .LIVE and finalized files.
master_cl_glob="${info[master_data_path]}/changelog.sfs.${CLUSTER_ID}.*" # Simplified glob for this test

count_accesses() {
	grep -c -w ACCESS ${master_cl_glob}* 2>/dev/null || true
}

# Verify if atime updates are detectable in changelogs by count_accesses
N=2
echo "file content" > "${info[mount0]}/file"
for i in $(seq $N); do
	sleep 1.1
	ls "${info[mount0]}" > /dev/null
	cat "${info[mount0]}/file" > /dev/null
	assert_equals $((2 * i)) $(count_accesses)
done

# Disable updating atime
echo "NO_ATIME = 1" >> "${info[master_cfg]}"
saunafs_master_daemon reload
assert_eventually_matches main.reload 'cat "${TEMP_DIR}/reloads"'

# Verify if atime updates are no longer detectable in changelogs by count_accesses
ls "${info[mount0]}"
cat "${info[mount0]}/file"
assert_equals $((2 * N)) $(count_accesses)
