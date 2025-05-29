# Test exception handling during writes

CHUNKSERVERS=4 \
    USE_RAMDISK=YES \
    setup_local_empty_saunafs info

cd "${info[mount0]}"

# Create read-only file
FILE_NAME="exception_test_file"
touch ${FILE_NAME}
chmod 444 ${FILE_NAME}

# Attempt to write
assert_failure dd if=/dev/urandom of=${FILE_NAME} bs=1M count=1 conv=notrunc status=none

echo "File is read-only, write failed as expected."

# Check locks released
assert_success rm ${FILE_NAME} <<< y
