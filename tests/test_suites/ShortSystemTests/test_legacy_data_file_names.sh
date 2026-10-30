timeout_set 2 minutes

# Data files were named *.sfs before the rebrand. A daemon keeps using those names, also when it
# creates them, and switches to *.leil once an administrator renamed its files with the daemon
# stopped. Master and metalogger share the data directory here and choose apart.

CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

data_path="${info[master_data_path]}"
saunafs_metalogger_daemon start

check_contents() {
	local last="$1"
	assert_eventually "grep -qx 'content $last' '${info[mount0]}/file$last'"
	for ((i = 1; i <= last; i++)); do
		assert_equals "content $i" "$(cat "${info[mount0]}/file$i")"
	done
}

write_files() {
	for ((i = $1; i <= $2; i++)); do
		echo "content $i" > "${info[mount0]}/file$i"
	done
}

# Renames the data files of the given families from extension $1 to $2
rename_files() {
	local from="$1" to="$2" family file name
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

# Without any *.leil files both daemons read and write the old names
write_files 1 10
assert_eventually 'grep -q file10 "${data_path}/changelog_ml.sfs"'
saunafs_metalogger_daemon stop
assert_success saunafs_master_daemon stop
assert_file_exists "$data_path/metadata.sfs"
assert_file_exists "$data_path/metadata_ml.sfs"
assert_not_equal "" "$(ls "$data_path" | grep '^changelog\.sfs')"
assert_equals "" "$(find "$data_path" -name '*.leil*')"

# Master files renamed by hand, with it stopped: it switches to the new names, the metalogger does not
rename_files sfs leil "${master_files[@]}"
assert_success saunafs_master_daemon start
saunafs_metalogger_daemon start
check_contents 10
write_files 11 20
assert_eventually 'grep -q file20 "${data_path}/changelog_ml.sfs"'
assert_file_exists "$data_path/metadata.leil"
assert_file_not_exists "$data_path/metadata.sfs"
assert_file_exists "$data_path/changelog.leil"
assert_file_not_exists "$data_path/changelog_ml.leil"

# Metalogger files renamed by hand too: it switches as well
saunafs_metalogger_daemon stop
rename_files sfs leil "${metalogger_files[@]}"
saunafs_metalogger_daemon start
write_files 21 30
assert_eventually 'grep -q file30 "${data_path}/changelog_ml.leil"'
check_contents 30
assert_equals "" "$(find "$data_path" -regextype posix-egrep \
	-regex '.*/(metadata|changelog|sessions)(_ml)?\.sfs([.].*)?')"
