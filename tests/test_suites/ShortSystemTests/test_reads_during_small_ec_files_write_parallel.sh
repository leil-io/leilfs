CHUNKSERVERS=6 \
	MOUNTS=2 \
	USE_RAMDISK=YES \
	MASTER_CUSTOM_GOALS="1 ec42: \$ec(4,2)" \
	AUTO_SHADOW_MASTER="NO" \
	MOUNT_0_EXTRA_CONFIG="sfscachemode=NEVER" \
	MOUNT_1_EXTRA_CONFIG="sfscachemode=NEVER" \
	CHUNKSERVER_EXTRA_CONFIG="GARBAGE_COLLECTION_FREQ_MS = 0`
			`|HDD_TEST_FREQ = 100000`
			`|MAGIC_DEBUG_LOG = $TEMP_DIR/log`
			`|LOG_FLUSH_ON=DEBUG" \
	setup_local_empty_saunafs info

# This script tests an optimization in handling 
# small files during Erasure Coding (EC) writes. 
# It addresses a previous issue where the client 
# would unnecessarily request missing data parts 
# from chunk servers, even beyond the file size, 
# adding overhead. The script creates a small 
# 1-byte file with known content and writes this 
# content at specific positions (0, 64K, 128K, 192K) 
# on two mount points. After each write operation, 
# it reads back the data from these positions and 
# checks that the read data is the same as the 
# known content. It also checks that there were 
# no extra reads during these operations. 
# This test ensures that remaining blocks are not 
# requested, eliminating unnecessary requests, 
# reducing overhead, and improving efficiency, 
# especially for small files.

# Array of positions to write to
positions=(0 64K 128K 192K)
expected_reads_one_mountpoint=(0 1 3 6)
total_expected_reads=24

# Write known content from one mountpoint
for index in ${!positions[@]}; do
	position=${positions[$index]}

	# Write known content at positions 0, 64K, 128K, and 192K on mount0
	echo -n "1" | dd of="${info[mount0]}/file0" seek=$position bs=1 count=1 oflag=direct status=progress

	# Check the number of extra reads
	number_of_extra_reads=$(grep "hddRead" $TEMP_DIR/log | wc -l);

	# Assert that the number of extra reads is as expected
	assert_equals ${expected_reads_one_mountpoint[$index]} "${number_of_extra_reads}"
done

# Write known content from two mountpoints in parallel 
for index in ${!positions[@]}; do
	position=${positions[$index]}

	# Write known content at positions 0, 64K, 128K, and 192K on mount0
	echo -n "1" | dd of="${info[mount0]}/file1" seek=$position bs=1 count=1 oflag=direct status=progress &

	# In parallel, write known content at the same positions on mount1
	echo -n "1" | dd of="${info[mount1]}/file1" seek=$position bs=1 count=1 oflag=direct status=progress &

	# Wait for the writes to finish
	wait
done

# Check the number of extra reads
number_of_extra_reads=$(grep "hddRead" $TEMP_DIR/log | wc -l);

# Assert that the number of extra reads is as expected
assert_less_or_equal "${number_of_extra_reads}" "${total_expected_reads}"

# Check that all data was correctly written
for position in ${positions[@]}; do
	# Read back the data from mount0 and mount1
	data_mount0_file0=$(dd if="${info[mount0]}/file0" bs=1 count=1 skip=$position status=none)
	data_mount0_file1=$(dd if="${info[mount0]}/file1" bs=1 count=1 skip=$position status=none)
	data_mount1_file1=$(dd if="${info[mount1]}/file1" bs=1 count=1 skip=$position status=none)

	# Assert that the data read back is the same as the known content
	assert_equals "1" "$data_mount0_file0"
	assert_equals "1" "$data_mount0_file1"
	assert_equals "1" "$data_mount1_file1"
done
