CHUNKSERVERS=1 \
	MASTERSERVERS=2 \
	USE_RAMDISK="YES" \
	MASTER_EXTRA_CONFIG="METADATA_DUMP_PERIOD_SECONDS = 0" \
	setup_local_empty_saunafs info

assert_eventually_prints $'master\trunning' \
		"saunafs-admin metadataserver-status --porcelain localhost ${info[matocl]} | cut -f-2"

version=$(saunafs-admin metadataserver-status --porcelain localhost ${info[matocl]} | cut -f3)
# Version of last changelog entry.
# New changelog names are dynamic. Cat all potential current changelog files for this master
# (both .LIVE and finalized) and get the last entry from the combined output.
# The glob should ideally include CLUSTER_ID and HOSTNAME if available to be more specific.
# Using a simpler glob for now, assuming test environment isolation.
changelog_version=$(cat "${info[master_data_path]}"/changelog.sfs.*.*.LIVE.* "${info[master_data_path]}"/changelog.sfs.*.*[0-9]Z 2>/dev/null | tail -1 | grep -o '^[0-9]*' || echo 0)

# Make sure admin returned correct metadata version.
# If changelog_version is 0 (e.g. no logs yet or error), this assertion might need adjustment.
# The metadata version starts at 1 for an empty filesystem.
# If there are no changelog entries, metadataserver-status might return 1.
# The original logic was (last_changelog_id + 1).
# If changelog_version is 0 (no entries), then admin should report 1.
if [ "$changelog_version" -eq 0 ]; then
  expected_admin_version=1
else
  expected_admin_version=$((changelog_version + 1))
fi
assert_equals "$version" "$expected_admin_version"

saunafs_master_n 1 start
assert_equals $version "$((changelog_version + 1))"

saunafs_master_n 1 start

assert_eventually_prints $'shadow\tconnected\t'$version \
		"saunafs-admin metadataserver-status --porcelain localhost ${info[master1_matocl]}"

saunafs_master_n 0 stop

assert_eventually_prints $'shadow\tdisconnected\t'$version \
		"saunafs-admin metadataserver-status --porcelain localhost ${info[master1_matocl]}"
