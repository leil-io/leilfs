USE_RAMDISK=YES \
	MASTERSERVERS=2 \
	setup_local_empty_saunafs info

# Make sure admin stop will fail without connected shadow server
saunafs_master_n 1 stop
assert_failure saunafs-admin stop-master-without-saving-metadata \
		localhost "${info[matocl]}" <<< "${info[admin_password]}"
saunafs_master_n 1 start

# Remember version of metadata before stopping the master server
last_metadata_version=$(saunafs_admin_master_no_password \
		metadataserver-status | cut -f3)

# Make sure admin stop will succeed with connected shadow server
assert_success saunafs-admin stop-master-without-saving-metadata \
		localhost "${info[matocl]}" <<< "${info[admin_password]}"

# Wait for master server to actually shut down
assert_eventually "! saunafs_master_daemon isalive"

# Verify if a proper lock file was left by the master server
lockfile="${info[master_data_path]}/metadata.leil.lock"
assert_file_exists "$lockfile"
assert_equals "quick_stop: $last_metadata_version" "$(cat "$lockfile")"

# Make sure that there is no possibility to start the master server normally
assert_failure saunafs_master_n 0 start

# Make sure that "-o auto-recovery" makes it possible to start the master server
assert_success saunafs_master_n 0 start -o auto-recovery
assert_eventually "saunafs_shadow_synchronized 1"
