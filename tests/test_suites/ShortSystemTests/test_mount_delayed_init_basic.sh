timeout_set 30 seconds

CHUNKSERVERS=1 \
  MOUNTS=2 \
  USE_RAMDISK=YES \
  MOUNT_0_EXTRA_CONFIG="sfscachemode=NEVER" \
  MOUNT_1_EXTRA_CONFIG="sfscachemode=NEVER,sfsdelayedinit" \
  setup_local_empty_saunafs info

# Stop master and clients and check which client starts faster
assert_success saunafs_mount_unmount 0
assert_success saunafs_mount_unmount 1
saunafs_master_daemon stop

# Start client with delayed init first
assert_eventually "saunafs_mount_start 1" "1 second"

saunafs_mount_start 0 &
pid=$!

# Wait a while to check if mount 0 starts
sleep 10

if ps -p $pid > /dev/null; then
	echo "Process still running after 10 seconds, starting master"
	saunafs_master_daemon start
	saunafs_wait_for_all_ready_chunkservers
else
	test_fail "Mount 0 started without master, but it should not have started yet"
fi

# Now mount 0 should start
sleep 1
if ps -p $pid > /dev/null; then
	test_fail "Process still running after 1 seconds while master is available"
fi

# Both mounts should be available now, let's perform some basic checks
cd "${info[mount0]}"
BLOCK_SIZE=456789 FILE_SIZE=123456789 file-generate file0
assert_success file-validate file0

cd "${info[mount1]}"
BLOCK_SIZE=567890 FILE_SIZE=234567890 file-generate file1
assert_success file-validate file0
assert_success file-validate file1

cd "${info[mount0]}"
assert_success file-validate file1
