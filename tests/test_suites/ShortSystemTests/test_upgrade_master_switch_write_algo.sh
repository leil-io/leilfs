timeout_set 50 seconds

# Test checks if new SaunaFS mount works with legacy versions of master and chunkservers
# and if it can read and write files created on legacy SaunaFS mount.

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

# Unmount old SaunaFS client 1:
assert_success saunafs_mount_unmount 1
# Mount SaunaFS client 1:
assert_success saunafs_mount_start 1

cd "${info[mount0]}"
mkdir from_old_mount
cd from_old_mount

function generate_file {
  FILE_SIZE=234567890 BLOCK_SIZE=56789 file-generate $1
}

# Test if reading and writing on old SaunaFS works:
assert_success generate_file old_mount_old_master_file
assert_success file-validate old_mount_old_master_file

cd "${info[mount1]}/from_old_mount"

# Test if file created on legacy version is readable
assert_success file-validate old_mount_old_master_file

cd ..
mkdir from_new_mount
cd from_new_mount
# Test if reading and writing on new SaunaFS mount works:
assert_success generate_file new_mount_old_master_file
assert_success file-validate new_mount_old_master_file

# Test if all files produced so far are readable on legacy mount:
cd "${info[mount0]}/from_new_mount"
assert_success file-validate new_mount_old_master_file

saunafsXX_master_daemon stop

# Force the master to create new sessions (and clients to connect instead of reconnect)
rm $(get_current_master_sessions_file)

# Don't know why, but we have to bypass the saunafs_master_daemon function
assert_success sfsmaster -c "${info[master0_cfg]}" start

saunafs_wait_for_all_ready_chunkservers

# Ensure that we can still read and write files from new and old SaunaFS mounts

# We were at "${info[mount0]}/from_new_mount"
assert_success file-validate new_mount_old_master_file

cd "../from_old_mount"
assert_success file-validate old_mount_old_master_file
assert_success generate_file old_mount_new_master_file
assert_success file-validate old_mount_new_master_file

cd "${info[mount1]}/from_old_mount"
assert_success file-validate old_mount_old_master_file
assert_success file-validate old_mount_new_master_file

cd "../from_new_mount"
assert_success file-validate new_mount_old_master_file
assert_success generate_file new_mount_new_master_file
assert_success file-validate new_mount_new_master_file

cd "${info[mount0]}/from_new_mount"
assert_success file-validate new_mount_new_master_file
