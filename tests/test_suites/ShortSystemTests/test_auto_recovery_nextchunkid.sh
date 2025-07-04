master_cfg="AUTO_RECOVERY = 1"
master_cfg+="|METADATA_DUMP_PERIOD_SECONDS = 0"
master_cfg+="|DISABLE_METADATA_CHECKSUM_VERIFICATION = 1"

CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota" \
	MASTER_EXTRA_CONFIG="$master_cfg" \
	AUTO_SHADOW_MASTER="NO" \
	setup_local_empty_saunafs info

# Define patterns for changelog files
CLUSTER_ID=${info[cluster_id]:-testcluster}
HOSTNAME=${info[hostname]:-$(hostname -s)}
# Pattern for the specific .LIVE file (may need adjustment if FIRST_ID/BEGIN_EPOCH change significantly)
# For robustly finding the *current* live file, `ls -t` might be needed if multiple could exist (should not normally)
live_cl_pattern="${info[master_data_path]}/changelog.sfs.${CLUSTER_ID}.*.UNDEF.*.LIVE.${HOSTNAME}"
# Glob for reading any relevant changelog for this master
master_cl_glob="${info[master_data_path]}/changelog.sfs.${CLUSTER_ID}.*${HOSTNAME}*"


# Create 6 chunks, saving the changelog after generating 3 of them
cd "${info[mount0]}"
FILE_SIZE=1K file-generate {1..3}
# Find the current live changelog to back it up
current_live_file_path_backup_candidate=$(ls ${live_cl_pattern} 2>/dev/null | head -n 1)
if [ -z "$current_live_file_path_backup_candidate" ] || [ ! -f "$current_live_file_path_backup_candidate" ]; then
	echo "ERROR: Could not find .LIVE changelog to backup for test_auto_recovery_nextchunkid.sh"
	exit 1
fi
echo "Backing up changelog: $current_live_file_path_backup_candidate"
cp "$current_live_file_path_backup_candidate" "$TEMP_DIR/changelog.sfs.backup"
# Store the exact name of the backed-up file to restore it later
backed_up_live_filename=$(basename "$current_live_file_path_backup_candidate")

FILE_SIZE=1K file-generate {4..6} # These entries go into a potentially new .LIVE file if rotation occurred, or same one
cd

# Lose information about metadata of chunks 4..6
saunafs_master_daemon kill
# Remove any current .LIVE file(s) for this master
rm -f ${live_cl_pattern}
# Restore the backed-up changelog to its original dynamic name
mv "$TEMP_DIR/changelog.sfs.backup" "${info[master_data_path]}/$backed_up_live_filename"
echo "Restored changelog to: ${info[master_data_path]}/$backed_up_live_filename"

# Start the master - expect it to generate NEXTCHUNKID when chunks 4..6 are registered
saunafs_master_daemon start
saunafs_wait_for_all_ready_chunkservers

# Create a new chunk and check if it's number is as high as expected
cd "${info[mount0]}"
FILE_SIZE=1K file-generate 7
assert_awk_finds 0000000000000007 "$(saunafs fileinfo 7)"
metadata=$(metadata_print)
cd

# Simulate crash of the master server, auto recover metadata applying NEXTCHUNKID and check it
saunafs_master_daemon kill
assert_awk_finds '/NEXTCHUNKID/' "$(cat "${info[master_data_path]}"/changelog.sfs)"
assert_success saunafs_master_daemon start
saunafs_wait_for_all_ready_chunkservers
assert_no_diff "$metadata" "$(metadata_print "${info[mount0]}")"
