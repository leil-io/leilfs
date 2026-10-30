CHUNKSERVERS=1 \
	USE_RAMDISK="YES" \
	MASTER_EXTRA_CONFIG="METADATA_DUMP_PERIOD_SECONDS = 0|BACK_META_KEEP_PREVIOUS = 5" \
	setup_local_empty_saunafs info

changelog_file="${info[master_data_path]}/changelog.leil"

# Create some metadata
for i in {1..5} ; do
	FILE_SIZE=1K assert_success file-generate "${info[mount0]}"/file_${i}_{1..10}
	assert_success saunafs_admin_master save-metadata
done

# Test fresh metadata + changelogs
latest_metadata_version=$(saunafs_admin_master_no_password metadataserver-status | cut -f3)
on_disk_metadata_version=$(sfsmetarestore -g -d "${info[master_data_path]}")
assert_equals "$latest_metadata_version" "$on_disk_metadata_version"

# Create more metadata
FILE_SIZE=1K assert_success file-generate "${info[mount0]}"/file_{6..10}_{1..10}
# and don't save it

# Test old metadata + changelogs
latest_metadata_version=$(saunafs_admin_master_no_password metadataserver-status | cut -f3)
saunafs_master_daemon kill
on_disk_metadata_version=$(sfsmetarestore -g -d "${info[master_data_path]}")
assert_equals "$latest_metadata_version" "$on_disk_metadata_version"
assert_success saunafs_master_daemon start -o auto-recovery
assert_equals "$(saunafs_admin_master_no_password metadataserver-status | cut -f3)" "$on_disk_metadata_version"

# Test broken changelogs fail
rm "${info[mount0]}"/file_*
saunafs_master_daemon kill

ls "${info[master_data_path]}"
mv "${info[master_data_path]}"/metadata.leil.2 "${info[master_data_path]}"/metadata.leil
mv "${info[master_data_path]}"/changelog.leil.1 "${info[master_data_path]}"/changelog.leil
mv "${info[master_data_path]}"/changelog.leil.2 "${info[master_data_path]}"/changelog.leil.2.tmp

on_disk_metadata_version=$(sfsmetarestore -g -d "${info[master_data_path]}")
assert_equals "0" "$on_disk_metadata_version"

# Test with changelog and metadata missing (but we should still be able to load old version)
mv "${info[master_data_path]}"/changelog.leil.2.tmp "${info[master_data_path]}"/changelog.leil.2
on_disk_metadata_version=$(sfsmetarestore -g -d "${info[master_data_path]}")
assert_success saunafs_master_daemon start -o auto-recovery
assert_equals "$(saunafs_admin_master_no_password metadataserver-status | cut -f3)" "$on_disk_metadata_version"

# Test sfsmetarestore on clean installation
assert_success saunafs_master_daemon kill
echo "SFSM NEW" > "${info[master_data_path]}"/metadata.leil
on_disk_metadata_version=$(sfsmetarestore -g -d "${info[master_data_path]}")
assert_equals "1" "$on_disk_metadata_version"
