# Test writes that cross block and chunk boundaries.

CHUNKSERVERS=4 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"
FILE_NAME="boundary_test_file"
FILE_NAME_SAVED="${FILE_NAME}_saved"

# Get block and chunk sizes.
BLOCK_SIZE=${SAUNAFS_BLOCK_SIZE}
CHUNK_SIZE=${SAUNAFS_CHUNK_SIZE}

# Create a file large enough to span multiple chunks.
SIZE=$((CHUNK_SIZE * 2 + BLOCK_SIZE * 2))
FILE_SIZE=${SIZE} file-generate ${FILE_NAME}
FILE_SIZE=${SIZE} file-generate ${FILE_NAME_SAVED}

# Corrupt the data in different places
BLOCK_GAP=128
BLOCK_WRITE_BEGIN=$((BLOCK_SIZE - BLOCK_GAP))
# note that: BLOCK_WRITE_BEGIN = 128 * (2^9 - 1) = 128 * 511, the first
# write crosses a block boundary
dd if=/dev/urandom of=${FILE_NAME} bs=$((BLOCK_WRITE_BEGIN / BLOCK_GAP)) \
	count=10 seek=$((BLOCK_GAP)) conv=notrunc status=none

CHUNK_GAP=1024
CHUNK_WRITE_BEGIN=$((CHUNK_SIZE - CHUNK_GAP))
# note that: CHUNK_WRITE_BEGIN = 1024 * (BLOCK_SIZE - 1) = 1024 * 65535, the 
# first write crosses a chunk boundary
dd if=/dev/urandom of=${FILE_NAME} bs=$((CHUNK_WRITE_BEGIN / CHUNK_GAP)) \
	count=10 seek=$((CHUNK_GAP)) conv=notrunc status=none

# note that: SIZE = 2050 * BLOCK_SIZE
UNALIGNED_BS=2050
# It will write using unaligned block size the end of the file
dd if=/dev/urandom of=${FILE_NAME} bs=$((UNALIGNED_BS)) count=50 \
	seek=$((BLOCK_SIZE - 50)) conv=notrunc status=none

# Verify file is broken
assert_failure file-validate ${FILE_NAME}

# Though readable
assert_success pv ${FILE_NAME} > /dev/null

# Fix the file
# Fix the cross-block-boundary write
dd if=${FILE_NAME_SAVED} of=${FILE_NAME} bs=$((BLOCK_WRITE_BEGIN / BLOCK_GAP)) \
	count=10 seek=$((BLOCK_GAP)) skip=$((BLOCK_GAP)) conv=notrunc status=none

# Fix the chunk-block-boundary write
dd if=${FILE_NAME_SAVED} of=${FILE_NAME} bs=$((CHUNK_WRITE_BEGIN / CHUNK_GAP)) \
	count=10 seek=$((CHUNK_GAP)) skip=$((CHUNK_GAP)) conv=notrunc status=none

# Fix the unaligned block size write
dd if=${FILE_NAME_SAVED} of=${FILE_NAME} bs=$((UNALIGNED_BS)) count=50 \
	seek=$((BLOCK_SIZE - 50)) skip=$((BLOCK_SIZE - 50)) conv=notrunc status=none

# Verify file size
ACTUAL_SIZE=$(stat -c %s ${FILE_NAME})

if [ ${ACTUAL_SIZE} -ne ${SIZE} ]; then
	test_add_failure "File size mismatch after cross-boundary writes. Expected ${SIZE}, got ${ACTUAL_SIZE}."
else
	echo "File size is correct after cross-boundary writes."
fi

# Verify file is fixed
assert_success file-validate ${FILE_NAME}
