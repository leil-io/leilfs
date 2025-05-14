timeout_set 2 minutes

CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	CHUNKSERVER_EXTRA_CONFIG="READ_AHEAD_KB = 1024|MAX_READ_BEHIND_KB = 2048" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,sfsioretries=10" \
	setup_local_empty_saunafs info

cd ${info[mount0]}

# Write a big file to deplete the space of 2 GiB
assert_failure dd if=/dev/zero of="${info[mount0]}/largefile" bs=1M count=2048 \
	oflag=direct conv=notrunc status=none

# Try to create a file, which should fail
assert_failure dd if=/dev/zero of="${info[mount0]}/1MiBfile.1" bs=1M count=1
assert_failure dd if=/dev/zero of="${info[mount0]}/1MiBfile.2" bs=1M count=1

# Try to create a folder, which should be successful
assert_success mkdir "${info[mount0]}/empty.folder"

# Validate empty files were created
assert_success ls -lh "${info[mount0]}/1MiBfile.1"
assert_success ls -lh "${info[mount0]}/1MiBfile.2"

ls -lh "${info[mount0]}"
