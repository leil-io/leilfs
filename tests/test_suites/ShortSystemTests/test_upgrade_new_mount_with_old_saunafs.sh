timeout_set 1 minute

# Test checks if both legacy, and new SaunaFS mount
# work with legacy versions of master and chunkservers

export SAFS_MOUNT_COMMAND="sfsmount"

CHUNKSERVERS=2 \
	MOUNTS=2 \
	START_WITH_LEGACY_SAUNAFS=YES \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_EXTRA_CONFIG="CHUNKS_LOOP_TIME = 1|OPERATIONS_DELAY_INIT = 0" \
	setup_local_empty_saunafs info

# Start test with master, 2 chunkservers and 2 mounts running legacy SaunaFS code
# Ensure that we work on legacy version
assert_equals 1 $(saunafs_old_admin_master info | grep $SAUNAFSXX_TAG | wc -l)
assert_equals 2 $(saunafs_old_admin_master list-chunkservers | grep $SAUNAFSXX_TAG | wc -l)
assert_equals 2 $(saunafs_old_admin_master list-mounts | grep $SAUNAFSXX_TAG | wc -l)

cd "${info[mount0]}"
mkdir dir0
assert_success saunafsXX saunafs setgoal 2 dir0
cd dir0

function generate_file {
	FILE_SIZE=123456789 BLOCK_SIZE=56789 file-generate $1
}

# Test if reading and writing on old SaunaFS works:
assert_success generate_file file0
assert_success file-validate file0

# Unmount old SaunaFS client 1:
assert_success saunafs_mount_unmount 1
# Mount SaunaFS client 1:
assert_success saunafs_mount_start 1

cd "${info[mount1]}/dir0"

# Test if file created on legacy version is readable
assert_success file-validate file0

cd ..
mkdir dir1
assert_success saunafsXX saunafs setgoal 2 dir1
cd dir1
# Test if reading and writing on new SaunaFS mount works:
assert_success generate_file file1
assert_success file-validate file1

# Test if all files produced so far are readable on legacy mount:
cd "${info[mount0]}/dir0"
assert_success file-validate file0
cd ../dir1
assert_success file-validate file1
