USE_RAMDISK=YES \
MASTERSERVERS=2 \
	setup_local_empty_saunafs info

list_metadata_servers() {
	saunafs_admin_master_no_password list-metadataservers --porcelain
}

nr="[0-9]+"
ip="($nr.){3}$nr"
version="$SAUNAFS_VERSION"
meta="$nr"
host="$(hostname)"
master_expected_state="^$ip ${info[matocl]} $host master running $meta $version\$"
shadow_expected_state="^$ip ${info[master1_matocl]} $host shadow connected $meta $version\$"

assert_matches "$master_expected_state" "$(list_metadata_servers)"

saunafs_master_n 1 start
assert_eventually_prints 2 'list_metadata_servers | wc -l'
assert_matches "$master_expected_state" "$(list_metadata_servers | grep -w master)"
assert_eventually_matches "$shadow_expected_state" 'list_metadata_servers | grep -w shadow'

saunafs_master_n 1 stop
assert_eventually_matches "$master_expected_state" 'list_metadata_servers'

saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"
shadow_version=$(list_metadata_servers | awk '/shadow/{print $6}')
saunafs_master_n 0 stop
master_version=$(metadata_get_version "${info[master_data_path]}/metadata.leil")
assert_equals "$master_version" "$shadow_version"
