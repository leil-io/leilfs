# Test switching between inode and chunk-based write algorithms

CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

# Generate original file
FILE_NAME="${info[mount0]}/original_file"
FILE_CPY="${FILE_NAME}_cpy"
FILE_SIZE=10M file-generate ${FILE_NAME}

# Write initial data using default: chunk-based algorithm
dd if=${FILE_NAME} of=${FILE_CPY} bs=1M count=5 conv=notrunc status=none

# Switch to inode-based algorithm.
saunafs_mount_unmount 0
echo "sfsuseinodebasedwritealgorithm=1" >> "${info[mount0_cfg]}"
saunafs_mount_start 0

# Continue writing.
dd if=${FILE_NAME} of=${FILE_CPY} bs=1M count=5 seek=5 skip=5 conv=notrunc status=none

# Verify data integrity.
if file-validate ${FILE_CPY}; then
	echo "Data integrity maintained after algorithm switch."
else
	test_add_failure "Data corruption after algorithm switch."
fi
