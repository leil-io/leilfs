CHUNKSERVERS=1 \
	USE_RAMDISK="YES" \
	MASTER_EXTRA_CONFIG="METADATA_CHECKSUM_FREQUENCY = 1" \
	setup_local_empty_saunafs info

# Changelog filename is now dynamic.
# For operations that need to modify the single current LIVE log:
# Assume info[cluster_id] and info[hostname] are available or use defaults
CLUSTER_ID=${info[cluster_id]:-testcluster}
HOSTNAME=${info[hostname]:-$(hostname -s)}
# The FIRST_ID and BEGIN_EPOCH_UTC parts of a live file are dynamic. Use wildcard for them.
live_cl_pattern="${info[master_data_path]}/changelog.sfs.${CLUSTER_ID}.*.UNDEF.*.LIVE.${HOSTNAME}"

# Create some metadata
cd ${info[mount0]}
touch file{00..99}
cd

# Make all CHECKSUM entries in changelog incorrect
saunafs_master_daemon kill

current_live_file=$(ls ${live_cl_pattern} 2>/dev/null | head -n 1)
if [ -n "$current_live_file" ] && [ -f "$current_live_file" ]; then
	echo "Modifying timestamps in: $current_live_file"
	sed -i -e 's/: ./: /' "$current_live_file" # Remove first digit from all timestamps
else
	echo "WARNING: Could not find unique .LIVE changelog file matching pattern ${live_cl_pattern} to modify for timestamp test."
	# If the live file isn't found, the test might not behave as expected,
	# but we avoid erroring on the sed command itself.
	# The subsequent sfsmetarestore calls will reveal if the setup is incorrect.
fi

# Make sure ordinary sfsmetarestore fails, and with disabled checksums succeeds.
assert_failure sfsmetarestore -a -d "${info[master_data_path]}"
assert_success sfsmetarestore -z -a -d "${info[master_data_path]}"
