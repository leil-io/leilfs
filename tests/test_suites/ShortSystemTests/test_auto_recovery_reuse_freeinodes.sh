timeout_set 1 minute

master_cfg="METADATA_DUMP_PERIOD_SECONDS = 0"
master_cfg+="|AUTO_RECOVERY = 1"
master_cfg+="|DISABLE_METADATA_CHECKSUM_VERIFICATION = 1"

CHUNKSERVERS=1 \
	MOUNTS=1 \
	USE_RAMDISK="YES" \
	MOUNT_0_EXTRA_CONFIG="sfscachemode=NEVER,sfsattrcacheto=0,sfsreportreservedperiod=1" \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota" \
	MASTER_EXTRA_CONFIG="$master_cfg" \
	AUTO_SHADOW_MASTER="NO" \
	setup_local_empty_saunafs info

changelog_file="${info[master_data_path]}/changelog.sfs"
metadata_file="${info[master_data_path]}/metadata.sfs"

# Remember version of the metadata file. We expect it not to change when generating data.
metadata_version=$(metadata_get_version "$metadata_file")

# Create and remove inodes to test that inode IDs are reused after deletion
cd ${info[mount0]}
touch file{00..99}
assert_eventually '[[ $(grep RELEASE "$changelog_file" | wc -l) == 100 ]]'
saunafs settrashtime 0 file*
rm file{10..80}
cd

# Decrease timestamp for all operations in changelog by a factor of at least 24h
saunafs_master_daemon kill
assert_equals "$metadata_version" "$(metadata_get_version "$metadata_file")"
sed -i -e 's/: ./: /' "$changelog_file" # Remove first digit from all timestamps (subtract 37 years)

# Start the master server (it will recover and dump metadata) and remember version of the new file.
assert_success saunafs_master_daemon start
saunafs_wait_for_all_ready_chunkservers
new_metadata_version=$(metadata_get_version "$metadata_file")
assert_less_than "$metadata_version" "$new_metadata_version"

if [ -f "$changelog_file" ]; then
	assert_awk_finds_no '/CREATE.*:20$/' "$(cat "$changelog_file")"
fi

touch "${info[mount0]}"/file{0000..099}
assert_awk_finds    '/CREATE.*:20$/' "$(cat "$changelog_file")" # Make sure that inode 20 was reused

# Simulate crash in master server, auto recover metadata and verify that there are no changes
metadata=$(metadata_print "${info[mount0]}")
saunafs_master_daemon kill
assert_equals "$new_metadata_version" "$(metadata_get_version "$metadata_file")"
assert_success saunafs_master_daemon start
saunafs_wait_for_all_ready_chunkservers
assert_no_diff "$metadata" "$(metadata_print "${info[mount0]}")"
