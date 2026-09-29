timeout_set 3 minutes
assert_program_installed fdbcli
assert_program_installed getfattr
assert_program_installed setfattr

# An initialized FDB store may legitimately have an empty section. A stale metadata.sfs must not
# be used to repopulate that section on restart once META_HEADER identifies FDB as authoritative.
AUTO_SHADOW_MASTER=NO \
	CHUNKSERVERS=1 \
	METADATA_BACKEND=FILE \
	MASTER_EXTRA_CONFIG="METADATA_DUMP_PERIOD_SECONDS = 0" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

live_xattr_count() {
	fdbcli --exec 'getrangekeys XATR_ XATS_ 100' \
		--cluster-file "${workspace}/conf/fdb.cluster" |
		awk '/^`XATR_/ { count++ } END { print count+0 }'
}

cd "${info[mount0]}"
touch bootstrap_xattr
setfattr -n user.stale -v stale_value bootstrap_xattr

# Produce the source image for the initial FILE-to-FORKLESS bootstrap and keep an immutable copy
# that still contains the xattr. The copy is restored before the final restart so the test does
# not depend on whether the forkless backend leaves metadata.sfs untouched.
assert_success saunafs_admin_master save-metadata
stale_metadata="${TEMP_DIR}/stale-metadata.sfs"
cp "${info[master_data_path]}/metadata.sfs" "${stale_metadata}"

cd "${TEMP_DIR}"
assert_success saunafs_master_daemon stop
start_fdb_cluster
sed -i 's/^METADATA_BACKEND = FILE$/METADATA_BACKEND = FORKLESS/' "${info[master_cfg]}"
echo "FDB_CLUSTER_FILE = /tmp/saunafs-fdb-test/conf/fdb.cluster" >>"${info[master_cfg]}"
assert_success saunafs_master_daemon start
saunafs_wait_for_all_ready_chunkservers
cd "${info[mount0]}"
assert_equals stale_value "$(getfattr --only-values -n user.stale bootstrap_xattr)"

# Make XATTR_ legitimately empty in the initialized store and seal that state as the latest
# checkpoint. This is the state that prefix-emptiness must not confuse with a missing section.
setfattr -x user.stale bootstrap_xattr
assert_eventually_prints 0 live_xattr_count '30 seconds'
assert_success saunafs_admin_master save-metadata
assert_equals 0 "$(live_xattr_count)"
assert_failure getfattr --only-values -n user.stale bootstrap_xattr

# Present the stale source image again. Since META_HEADER marks FDB as initialized, restart must
# keep its empty XATTR_ state authoritative and must not restore user.stale from metadata.sfs.
cd "${TEMP_DIR}"
assert_success saunafs_master_daemon stop
cp "${stale_metadata}" "${info[master_data_path]}/metadata.sfs"
assert_success saunafs_master_daemon start
saunafs_wait_for_all_ready_chunkservers
cd "${info[mount0]}"
assert_failure getfattr --only-values -n user.stale bootstrap_xattr
