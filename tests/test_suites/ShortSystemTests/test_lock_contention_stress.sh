# Stress test write locking mechanism

CHUNKSERVERS=4 \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

# Create file.
FILE_NAME="lock_stress_file"
FILE_NAME_CPY="lock_stress_file_cpy"
NUM_PROCESSES=400
KILOBYTES_PER_PROCESS=100
FILE_SIZE=$((NUM_PROCESSES * KILOBYTES_PER_PROCESS * 1024)) file-generate ${FILE_NAME}

# High concurrency writes

for i in $(seq 1 ${NUM_PROCESSES}); do
	(
		OFFSET=$(((i - 1) * KILOBYTES_PER_PROCESS))
		dd if=${FILE_NAME} of=${FILE_NAME_CPY} bs=1K count=${KILOBYTES_PER_PROCESS} \
			seek=${OFFSET} skip=${OFFSET} conv=notrunc status=none
	) &
done

wait

echo "All concurrent writes completed."

# Validate copy file
assert_success file-validate ${FILE_NAME_CPY}
