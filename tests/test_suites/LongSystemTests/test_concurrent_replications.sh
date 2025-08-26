timeout_set 15 minutes

# Create an installation with 7 chunkservers, 2 disks each.
USE_RAMDISK=YES \
	MOUNTS=1
	CHUNKSERVERS=7 \
	DISK_PER_CHUNKSERVER=2 \
	CHUNKSERVER_EXTRA_CONFIG="HDD_TEST_FREQ = 10000|`
		`MASTER_REPLICATION_NR_OF_WORKERS=10|`
		`MAX_BLOCKS_PER_HDD_READ_JOB=1" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_CUSTOM_GOALS="10 ec_5_2: \$ec(5,2)" \
	MASTER_EXTRA_CONFIG="CHUNKS_LOOP_MIN_TIME = 1`
			`|CHUNKS_LOOP_MAX_CPU = 90`
			`|CHUNKS_LOOP_PERIOD = 10`
			`|ACCEPTABLE_DIFFERENCE = 1.0`
			`|CHUNKS_WRITE_REP_LIMIT = 50`
			`|CHUNKS_READ_REP_LIMIT = 50`
			`|OPERATIONS_DELAY_INIT = 0`
			`|OPERATIONS_DELAY_DISCONNECT = 0" \
	setup_local_empty_saunafs info

chunkserver_count=7

# The expected state of the system is that each file has a part in each chunkserver.
check_expected_state() {
	check_nr_part_per_cs=$1
	checked_files=0
	for file in *; do
		if [[ check_nr_part_per_cs -eq 1 ]]; then
			# Check that each chunkserver has a part of the file
			for ((csid=0; csid < chunkserver_count; csid++)); do
				assert_eventually_prints 1 "saunafs fileinfo '${file}' | grep ':${info[chunkserver${csid}_port]}' | wc -l" "3 minutes"
			done
		fi
		assert_eventually_prints ${chunkserver_count} "saunafs fileinfo '${file}' | grep copy | wc -l" "3 minutes"

		checked_files=$((checked_files + 1))
		if (( checked_files % 100 == 0 )); then
			echo "Successfully checked ${checked_files} files"
		fi
	done
}

cd "${info[mount0]}"
mkdir ec52_dir
saunafs setgoal ec_5_2 ec52_dir

cd ec52_dir
# Create 1000 files 14 blocks long
file_size=$((14 * SAUNAFS_BLOCK_SIZE))
for i in {1..1000}; do
	FILE_SIZE=${file_size} file-generate "file_${i}"
done

disable_chunkserver_disk 0 0 &
disable_chunkserver_disk 1 0 &
wait

# Replication should start immediately, so we can read the files.
check_expected_state 1

reenable_chunkserver_disk 0 0 &
reenable_chunkserver_disk 1 0 &
wait

# Wait for the deletion of the extra files to finish.
# While deleting the extra parts, the number of parts per chunkserver may not be exactly one.
check_expected_state 0

# Validate the files to ensure they are correct.
for i in {1..1000}; do
	file-validate "file_${i}"
done

sfschunkserver_check_no_buffer_in_use
