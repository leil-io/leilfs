CHUNKSERVERS=1 \
	MASTERSERVERS=2 \
	USE_RAMDISK="YES" \
	MASTER_EXTRA_CONFIG="METADATA_DUMP_PERIOD_SECONDS = 0" \
	setup_local_empty_saunafs info

assert_eventually_prints $'master\trunning' \
		"saunafs-admin metadataserver-status --porcelain localhost ${info[matocl]} | cut -f-2"

version=$(saunafs-admin metadataserver-status --porcelain localhost ${info[matocl]} | cut -f3)
# Version of last changelog entry.
changelog_version=$(tail -1 "${info[master_data_path]}"/changelog.leil | grep -o '^[0-9]*')

# Make sure admin returned correct metadata version.
assert_equals $version "$((changelog_version + 1))"

saunafs_master_n 1 start

assert_eventually_prints $'shadow\tconnected\t'$version \
		"saunafs-admin metadataserver-status --porcelain localhost ${info[master1_matocl]}"

saunafs_master_n 0 stop

assert_eventually_prints $'shadow\tdisconnected\t'$version \
		"saunafs-admin metadataserver-status --porcelain localhost ${info[master1_matocl]}"
