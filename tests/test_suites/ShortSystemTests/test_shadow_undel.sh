timeout_set 1 minutes

master_cfg="METADATA_DUMP_PERIOD_SECONDS = 0"
master_cfg+="|AUTO_RECOVERY = 1"

CHUNKSERVERS=1 \
	MASTERSERVERS=2 \
	MOUNTS=2 \
	USE_RAMDISK="YES" \
	MOUNT_0_EXTRA_CONFIG="sfscachemode=NEVER,sfsreportreservedperiod=1" \
	MOUNT_1_EXTRA_CONFIG="sfsmeta" \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota, ignoregid" \
	SFSEXPORTS_META_EXTRA_OPTIONS="nonrootmeta" \
	MASTER_EXTRA_CONFIG="$master_cfg" \
	setup_local_empty_saunafs info

stat_basic_info() {
	stat --format="%A %u %g %i %s" "$@"
}

only_file_in_trash() {
	assert_equals "1" "$(ls "$trash" | grep -v undel | wc -l)"
	echo "$trash/$(ls "$trash" | grep -v undel)"
}

trash="${info[mount1]}/trash"
# changelog_file variable is removed; use glob pattern for accessing logs.
# master_cl_glob="${info[master_data_path]}/changelog.sfs.*" # Define if used multiple times, or inline
# Assuming CLUSTER_ID and HOSTNAME are available or can be defaulted for master0 (implicit target)
CLUSTER_ID=${info[cluster_id]:-testcluster}
HOSTNAME=${info[hostname]:-$(hostname -s)}
master_cl_pattern_specific="${info[master_data_path]}/changelog.sfs.${CLUSTER_ID}.*${HOSTNAME}"


saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"

# Generate file for removal
cd "${info[mount0]}"
mkdir -p a/b/c
FILE_SIZE=1M file-generate a/b/c/test

stat_before_rm=$(stat_basic_info a/b/c/test)

# remove all files and directories
rm -rf a

# undelete file "a/b/c/test"
file_in_trash=$(only_file_in_trash)
assert_success mv "$file_in_trash" "$trash/undel/"

# check if path was recreated
assert_success file-validate a/b/c/test
stat_after_recovery=$(stat_basic_info a/b/c/test)
assert_equals "$stat_before_rm" "$stat_after_recovery"

# check if creating files was issued before undelete in changelog
# Use the pattern that matches all changelogs for this specific master instance
create_count=$(cat ${master_cl_pattern_specific}* 2>/dev/null | grep -B 3 UNDEL | grep CREATE | wc -l)
assert_equals "$create_count" "3"

assert_eventually "saunafs_shadow_synchronized 1"
