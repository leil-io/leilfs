CHUNKSERVERS=0 \
	USE_RAMDISK=YES \
	MASTER_EXTRA_CONFIG="AUTO_RECOVERY = 1" \
	MOUNTS=0 \
	setup_local_empty_saunafs info

# Generate empty metadata file by stopping the master server and generate a long changelog.
saunafs_master_daemon stop
assert_equals 1 $(metadata_get_version "${info[master_data_path]}/metadata.leil")
generate_changelog > "${info[master_data_path]}/changelog.leil"

# Start sfsmetarestore in background and wait until it starts to apply
# the changelog. This process will then last for a couple of seconds.
sfsmetarestore -a -d "${info[master_data_path]}" &
assert_eventually 'test -e "${info[master_data_path]}/metadata.leil.lock"'
sleep 1

# Try to start the master server. This should fail, because sfsmetarestore holds the lock.
expect_failure saunafs_master_daemon start

# Expect that sfsmetarestore still applies the changelog
expect_equals 1 $(metadata_get_version "${info[master_data_path]}/metadata.leil")
