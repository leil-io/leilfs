# Test filesystem consistency after client crash

CHUNKSERVERS=4 \
	USE_RAMDISK=YES \
	MOUNTS=1 \
	setup_local_empty_saunafs info

FILE_NAME="${info[mount0]}/crash_test_file"

# Start write operation
( dd if=/dev/urandom of=${FILE_NAME} bs=1M count=1024 status=none ) &

# Allow write to start
sleep 1

# Simulate client crash
MOUNT_PID=$(pgrep -f "sfsmount.*${info[mount0]}")
pgrep -fa "sfsmount.*${info[mount0]}"
# Write operation crashes here
kill -9 ${MOUNT_PID}

sleep 1
umount "${info[mount0]}"

echo "Client process killed to simulate crash."

# Restart client
saunafs_mount_start 0

ACTUAL_SIZE=$(stat -c %s ${FILE_NAME})

# Verify file exists, something was written
if [ ${ACTUAL_SIZE} -gt 0 ]; then
	echo "File exists after client restart."
else
	test_add_failure "File missing after client restart."
fi
