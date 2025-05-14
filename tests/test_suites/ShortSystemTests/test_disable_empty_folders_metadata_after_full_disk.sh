timeout_set 1 minutes

CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	MASTER_EXTRA_CONFIG="CREATE_EMPTY_FOLDERS_WHEN_SPACE_DEPLETED = 0" \
	CHUNKSERVER_EXTRA_CONFIG="READ_AHEAD_KB = 1024|MAX_READ_BEHIND_KB = 2048`
			`|HDD_LEAVE_SPACE_DEFAULT = 0MiB" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,sfsioretries=10" \
	setup_local_empty_saunafs info

cd ${info[mount0]}

# Write a 2G file, remaining 48 MiB of available space
assert_success dd if=/dev/zero of="${info[mount0]}/largefile" bs=1M count=2000 \
	oflag=direct conv=notrunc status=none

# Create a 48 MiB file to deplete the available space
assert_failure dd if=/dev/zero of="${info[mount0]}/48MiBfile" bs=1M count=48 oflag=direct status=none

# Try to create a folder, which should fail
assert_failure mkdir "${info[mount0]}/empty.folder"

# Try to create a 1 MiB file, which should fail
assert_failure dd if=/dev/zero of="${info[mount0]}/1MiBfile" bs=1M count=1 oflag=direct status=none

# Validate empty file was successfully created
assert_success ls -lh "${info[mount0]}/1MiBfile"

# Validate empty folder was not created
assert_failure ls -lh "${info[mount0]}/empty.folder"

assert_success ls -lh "${info[mount0]}"
