timeout_set 10 minutes

master_cfg="METADATA_DUMP_PERIOD_SECONDS = 0"

CHUNKSERVERS=1 \
	MOUNTS=2 \
	USE_RAMDISK="YES" \
	MOUNT_0_EXTRA_CONFIG="sfscachemode=NEVER,sfsreportreservedperiod=1,sfsdirentrycacheto=0" \
	MOUNT_1_EXTRA_CONFIG="sfsmeta" \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota,ignoregid" \
	SFSEXPORTS_META_EXTRA_OPTIONS="nonrootmeta" \
	MASTER_EXTRA_CONFIG="$master_cfg" \
	setup_local_empty_saunafs info

# Save path of meta-mount in SFS_META_MOUNT_PATH for metadata generators
export SFS_META_MOUNT_PATH=${info[mount1]}

# Save path of changelog.sfs in CHANGELOG to make it possible to verify generated changes
export CHANGELOG="${info[master_data_path]}/changelog.sfs"

for generator in $(metadata_get_all_generators); do
	export MESSAGE="Testing generator $generator"

	# Remember version of the metadata file. We expect it not to change when generating data.
	metadata_version=$(metadata_get_version "${info[master_data_path]}"/metadata.sfs)

	# Generate some content using the current generator and remember its metadata
	cd "${info[mount0]}"
	eval "$generator"
	metadata=$(metadata_print)
	cd

	# Simulate crash of the master server
	saunafs_master_daemon kill

	# Make sure changes are in changelog only (ie. that metadata wasn't dumped)
	assert_equals "$metadata_version" "$(metadata_get_version "${info[master_data_path]}"/metadata.sfs)"

	# Restore the filesystem from changelog by starting master server and check it
	assert_failure saunafs_master_daemon start # Should fail without -o auto-recovery!
	assert_success saunafs_master_daemon start -o auto-recovery
	saunafs_wait_for_all_ready_chunkservers
	assert_no_diff "$metadata" "$(metadata_print "${info[mount0]}")"
done

# Check if we can read files
cd "${info[mount0]}"
metadata_validate_files
