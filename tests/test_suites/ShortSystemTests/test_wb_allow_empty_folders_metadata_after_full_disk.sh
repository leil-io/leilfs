timeout_set 2 minutes

CHUNKSERVERS=4 \
	USE_RAMDISK=YES \
	CHUNKSERVER_EXTRA_CONFIG="READ_AHEAD_KB = 1024|MAX_READ_BEHIND_KB = 2048 \
		|HDD_LEAVE_SPACE_DEFAULT = 0MiB|WRITE_BUFFERING_SIZE_MB = 64" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,sfsioretries=10" \
	MASTER_CUSTOM_GOALS="8 ec22: \$ec(2,2)" \
	setup_local_empty_saunafs info

cd ${info[mount0]}

mkdir dir_ec22
saunafs setgoal -r ec22 dir_ec22
cd dir_ec22

# Write a 1020 MiB file in goal ec22, remaining 8 MiB of available space
assert_success dd if=/dev/zero of=largefile bs=1M count=1020 \
	oflag=direct conv=notrunc status=none

# Let it flush the data
sleep 2

# Create a 4 MiB file to deplete the available space
assert_failure dd if=/dev/zero of=4MiBfile bs=1M count=4 oflag=direct status=none

# Try to create a file, which should fail
assert_failure dd if=/dev/zero of=1MiBfile.1 bs=1M count=1
assert_failure dd if=/dev/zero of=1MiBfile.2 bs=1M count=1

# Try to create a folder, which should be successful
assert_success mkdir empty.folder

# Validate empty files were created
assert_success ls -lh 1MiBfile.1
assert_success ls -lh 1MiBfile.2
