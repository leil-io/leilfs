timeout_set 2 minutes

# Data files were named *.sfs before the rebrand. A daemon keeps using those names where its data
# directory only has them, and switches to *.leil once an administrator renamed its files with the
# daemon stopped. Master and metalogger share the data directory here and choose apart.

CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

data_path="${info[master_data_path]}"
saunafs_metalogger_daemon start

cd "${info[mount0]}"
for i in {1..10}; do
	echo "content $i" > file$i
done
cd "$TEMP_DIR"
assert_eventually 'test -e "${data_path}/changelog_ml.leil"'

# Rename every data file in the directory to its pre-rebrand name
to_extension() {
	local from="$1" to="$2" file name
	shift 2
	for family in "$@"; do
		for file in "$data_path/$family.$from" "$data_path/$family.$from".*; do
			[[ -e $file ]] || continue
			name=$(basename "$file")
			mv "$file" "$data_path/${name/.$from/.$to}"
		done
	done
}
master_files=(metadata changelog sessions stats)
metalogger_files=(metadata_ml changelog_ml sessions_ml)

check_contents() {
	local last="$1"
	assert_eventually "grep -qx 'content $last' '${info[mount0]}/file$last'"
	for ((i = 1; i <= last; i++)); do
		assert_equals "content $i" "$(cat "${info[mount0]}/file$i")"
	done
}

saunafs_metalogger_daemon stop
assert_success saunafs_master_daemon stop
to_extension leil sfs "${master_files[@]}" "${metalogger_files[@]}"
assert_equals "" "$(ls "$data_path" | grep '\.leil')"

# A pre-rebrand directory: both daemons keep reading and writing the old names
assert_success saunafs_master_daemon start
saunafs_metalogger_daemon start
check_contents 10
for i in {11..20}; do
	echo "content $i" > "${info[mount0]}/file$i"
done
assert_eventually 'grep -q file20 "${data_path}/changelog_ml.sfs"'
saunafs_metalogger_daemon stop
assert_success saunafs_master_daemon stop
assert_file_exists "$data_path/metadata.sfs"
assert_not_equal "" "$(ls "$data_path" | grep '^changelog\.sfs')"
assert_equals "" "$(ls "$data_path" | grep '\.leil')"

# Renamed by hand, with the master stopped: it switches to the new names, the metalogger does not
to_extension sfs leil "${master_files[@]}"
assert_success saunafs_master_daemon start
saunafs_metalogger_daemon start
check_contents 20
for i in {21..30}; do
	echo "content $i" > "${info[mount0]}/file$i"
done
assert_eventually 'grep -q file30 "${data_path}/changelog_ml.sfs"'
assert_file_exists "$data_path/metadata.leil"
assert_file_not_exists "$data_path/metadata.sfs"
assert_file_exists "$data_path/changelog.leil"
assert_file_not_exists "$data_path/changelog_ml.leil"
