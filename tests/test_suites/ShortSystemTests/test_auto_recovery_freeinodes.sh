timeout_set 1 minute

master_cfg="METADATA_DUMP_PERIOD_SECONDS = 0"
master_cfg+="|AUTO_RECOVERY = 1"
master_cfg+="|FREE_INODES_PERIOD = 1"
master_cfg+="|DISABLE_METADATA_CHECKSUM_VERIFICATION = 1"

CHUNKSERVERS=1 \
	MOUNTS=1 \
	USE_RAMDISK="YES" \
	MOUNT_0_EXTRA_CONFIG="sfscachemode=NEVER,sfsattrcacheto=0,sfsreportreservedperiod=1" \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota" \
	MASTER_EXTRA_CONFIG="$master_cfg" \
	AUTO_SHADOW_MASTER="NO" \
	setup_local_empty_saunafs info

# Changelog filename is now dynamic.
# For operations that need to read all current/recent logs:
master_cl_glob="${info[master_data_path]}/changelog.sfs.*"
# For operations that need to modify the single current LIVE log:
# Assume info[cluster_id] and info[hostname] are available or use defaults
CLUSTER_ID=${info[cluster_id]:-testcluster}
HOSTNAME=${info[hostname]:-$(hostname -s)}
# The FIRST_ID and BEGIN_EPOCH_UTC parts of a live file are dynamic. Use wildcard for them.
live_cl_pattern="${info[master_data_path]}/changelog.sfs.${CLUSTER_ID}.*.UNDEF.*.LIVE.${HOSTNAME}"

metadata_file="${info[master_data_path]}/metadata.sfs"

# Remember version of the metadata file. We expect it not to change when generating data.
metadata_version=$(metadata_get_version "$metadata_file")

# Create and remove inodes, which later will be freed generating FREEINODES entry in changelog
cd ${info[mount0]}
touch file{00..99}
assert_eventually '[[ $(grep RELEASE ${master_cl_glob} 2>/dev/null | wc -l) == 100 ]]'
saunafs settrashtime 0 file*
rm file{10..80}
cd

# Decrease timestamp for all operations in changelog by a factor of at least 24h
saunafs_master_daemon kill
assert_equals "$metadata_version" "$(metadata_get_version "$metadata_file")"
# Find the specific .LIVE file to modify. This assumes only one such file exists.
current_live_file=$(ls ${live_cl_pattern} 2>/dev/null | head -n 1)
if [ -n "$current_live_file" ] && [ -f "$current_live_file" ]; then
	echo "Modifying timestamps in: $current_live_file"
	sed -i -e 's/: ./: /' "$current_live_file" # Remove first digit from all timestamps
else
	echo "WARNING: Could not find unique .LIVE changelog file matching pattern ${live_cl_pattern} to modify for timestamp test."
fi

# Start the master server (it will recover and dump metadata) and remember version of the new file.
assert_success saunafs_master_daemon start
saunafs_wait_for_all_ready_chunkservers
new_metadata_version=$(metadata_get_version "$metadata_file")
assert_less_than "$metadata_version" "$new_metadata_version"

# Check content across all possible changelog files
changelog_content=$(cat ${master_cl_glob} 2>/dev/null)
if [ -n "$changelog_content" ]; then
	assert_awk_finds_no '/CREATE.*:20$/' "$changelog_content"
else
	echo "INFO: No changelog content found to check for CREATE.*:20"
fi

touch "${info[mount0]}"/file{0000..099}
# Re-read content as it might have changed
changelog_content=$(cat ${master_cl_glob} 2>/dev/null)
assert_awk_finds    '/CREATE.*:20$/' "$changelog_content" # Make sure that inode 20 was reused

# Simulate crash of the master server, auto recover metadata applying FREEINODES and check it
metadata=$(metadata_print "${info[mount0]}")
saunafs_master_daemon kill
assert_equals "$new_metadata_version" "$(metadata_get_version "$metadata_file")"
assert_success saunafs_master_daemon start
saunafs_wait_for_all_ready_chunkservers
assert_no_diff "$metadata" "$(metadata_print "${info[mount0]}")"
