timeout_set 3 minutes

# A cluster whose master starts from metadata.leil.empty, as a new installation does: the master,
# its metadata dumps and recovery all stay on the new names (*.leil). The shadow starts from an
# empty data directory, so it keeps the pre-rebrand names (*.sfs).

MASTER_DATA_FILE_EXTENSION=leil \
	CHUNKSERVERS=1 \
	MASTERSERVERS=2 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_EXTRA_CONFIG="METADATA_DUMP_PERIOD_SECONDS = 0|BACK_META_KEEP_PREVIOUS = 5" \
	setup_local_empty_saunafs info

master_path="${info[master0_data_path]}"
shadow_path="${info[master1_data_path]}"

write_files() {
	for ((i = $1; i <= $2; i++)); do
		echo "content $i" > "${info[mount0]}/file$i"
	done
}

check_contents() {
	local last="$1"
	assert_eventually "grep -qx 'content $last' '${info[mount0]}/file$last'"
	for ((i = 1; i <= last; i++)); do
		assert_equals "content $i" "$(cat "${info[mount0]}/file$i")"
	done
}

# Lists the data files in $1 under the names other than the expected ones ($2)
unexpected_files() {
	local other=leil
	[[ $2 == leil ]] && other=sfs
	find "$1" -regextype posix-egrep \
		-regex ".*/(metadata|changelog|sessions|stats)\.${other}([.].*)?" ! -name '*.empty'
}

master_version() {
	saunafs_admin_master_no_password metadataserver-status | cut -f3
}

# A metadata dump writes the new names
write_files 1 10
assert_success saunafs_admin_master save-metadata
assert_file_exists "$master_path/metadata.leil"
assert_file_exists "$master_path/metadata.leil.1"
assert_file_exists "$master_path/changelog.leil.1"
assert_equals "" "$(unexpected_files "$master_path" leil)"

# A shadow synchronizes; it creates its files under the old names in its empty directory
assert_success saunafs_master_n 1 start
assert_eventually 'saunafs_shadow_synchronized 1'
write_files 11 20
assert_eventually 'saunafs_shadow_synchronized 1'
assert_success saunafs_master_n 1 stop
assert_file_exists "$shadow_path/metadata.sfs"
assert_equals "" "$(unexpected_files "$shadow_path" sfs)"

# metarestore -a after an unclean shutdown restores under the new names
write_files 21 30
version=$(master_version)
assert_success saunafs_master_daemon kill
assert_equals "$version" "$(sfsmetarestore -g -d "$master_path")"
assert_success sfsmetarestore -a -d "$master_path"
assert_success saunafs_master_daemon start
assert_equals "$version" "$(master_version)"
check_contents 30
assert_equals "" "$(unexpected_files "$master_path" leil)"

# What the uRaft helper does on promotion: the on-disk version from metarestore -g, then a start
# with auto-recovery, which must reach that version
write_files 31 40
version=$(master_version)
assert_success saunafs_master_daemon kill
assert_equals "$version" "$(sfsmetarestore -g -d "$master_path")"
assert_success saunafs_master_daemon start -o auto-recovery
assert_equals "$version" "$(master_version)"
check_contents 40
assert_equals "" "$(unexpected_files "$master_path" leil)"
