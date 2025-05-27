USE_RAMDISK=YES \
	MASTER_EXTRA_CONFIG="METADATA_DUMP_PERIOD_SECONDS = 0" \
	setup_local_empty_saunafs info
saunafs_metalogger_daemon start

# Prints md5 hashes of all master's and metalogger's changelog files.
# changelog_ml.sfs.1 and changelog_ml.sfs.2 are omitted, because metalogger
# overwrites these two when starting and we don't want a race in:
#   saunafs_metalogger_daemon start
#   assert_no_diff "$expected_changelogs" "$(changelog_checksums)"
changelog_checksums() {
	md5sum changelog*.sfs* | grep -v '_ml[.]sfs[.][12]' | sort
}

metadata_file="${info[master_data_path]}/metadata.sfs"
cd "${info[mount0]}"
# Generate some changelog files in master and metalogger
for n in {1..10}; do
	touch file_${n}.{1..10}
	prev_version=$(metadata_get_version "$metadata_file")
	assert_success saunafs_admin_master save-metadata
	assert_less_than "$prev_version" "$(metadata_get_version "$metadata_file")"
	assert_file_exists "${info[master_data_path]}/changelog.sfs.$n"
done

cd ${info[master_data_path]}
saunafs_master_daemon stop
saunafs_metalogger_daemon stop
echo 111 > changelog.sfs
echo kazik > changelog_ml.sfs
expected_changelogs=$(changelog_checksums)

# Rename changelog files so they simulate old version
for i in {1..99}; do
	if [[ -e changelog.sfs.$i ]]; then
		mv changelog.sfs.$i changelog.${i}.sfs
	fi
	if [[ -e changelog_ml.sfs.$i ]]; then
		mv changelog_ml.sfs.$i changelog_ml.${i}.sfs
	fi
done
mv changelog.sfs changelog.0.sfs
mv changelog_ml.sfs changelog_ml.0.sfs
assert_not_equal "$expected_changelogs" "$(changelog_checksums)"

# Start master and metalogger, and make sure they properly rename changelog files
saunafs_master_daemon start
saunafs_metalogger_daemon start

sleep 1
assert_no_diff "$expected_changelogs" "$(changelog_checksums)"
