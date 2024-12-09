# Test concurrent overlapping writes to the same file
timeout_set 2 minutes

CHUNKSERVERS=4 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

# Define number of concurrent writers
NUM_WRITERS=10
# Define block size
BS=1024
# Define overlapping regions
# All writers write to overlapping regions starting at offset 16M
SLICE_SIZE=$((16 * 1024))
LENGTH=$((2 * SLICE_SIZE))

# Function for a writer process
write_overlap() {
	local id=$1
	local offset=$2
	local length=$3
	local pattern=$(printf "%02d" ${id})
	# Write the pattern to the file at the specified offset
	yes "${pattern}" | tr -d '\n' | head -c $((length * BS)) | dd of=concurrent_file bs=${BS} count=${length} seek=${offset} conv=notrunc status=none
}


# Start concurrent writes
for i in $(seq 1 ${NUM_WRITERS}); do
	write_overlap ${i} $((SLICE_SIZE * i)) $((LENGTH)) &
done

wait

# Read back the overlapping region
dd if=concurrent_file bs=${BS} skip=$((SLICE_SIZE)) count=$((SLICE_SIZE * 11)) status=none > read_back_data

# Since multiple writes overlapped, the final data should correspond to a mix of the 10 patterns
unique_patterns=$(tr -d '\n' < read_back_data | fold -w2 | sort | uniq)
if [ $(echo "${unique_patterns}" | wc -l) -le 10 ]; then
	echo "Test passed: Overlapping writes resulted in consistent data."
else
	test_add_failure "Data corruption detected in overlapping writes."
fi
