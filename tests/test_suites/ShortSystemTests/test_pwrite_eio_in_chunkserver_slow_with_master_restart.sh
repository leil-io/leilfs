timeout_set 1 minute

# Create an installation with 4 chunkservers, 1 disk each.
# CS 0 has a disk which will fail on the first write and has a 2s delay per operation.
USE_RAMDISK=YES \
	CHUNKSERVERS=4 \
	CHUNKSERVER_EXTRA_CONFIG="HDD_TEST_FREQ = 100000|MAX_BLOCKS_PER_HDD_WRITE_JOB = 1" \
	CHUNKSERVER_0_DISK_0="$RAMDISK_DIR/pwrite_slow_and_one_EIO_hdd_0" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_EXTRA_CONFIG="CHUNKS_LOOP_MIN_TIME = 1`
			`|CHUNKS_LOOP_MAX_CPU = 90`
			`|ACCEPTABLE_DIFFERENCE = 1.0`
			`|CHUNKS_WRITE_REP_LIMIT = 20`
			`|OPERATIONS_DELAY_INIT = 0`
			`|OPERATIONS_DELAY_DISCONNECT = 0" \
	MASTER_CUSTOM_GOALS="8 ec31: \$ec(3,1)" \
	setup_local_empty_saunafs info

# Restart the first chunkserver preloading pwrite with EIO-throwing version
LD_PRELOAD="${SAUNAFS_INSTALL_FULL_LIBDIR}/libchunk_operations_eio.so" \
		assert_success saunafs_chunkserver_daemon 0 restart
saunafs_wait_for_all_ready_chunkservers

# Create a directory with goal ec(3,1)
cd "${info[mount0]}"
mkdir dir
saunafs setgoal ec31 dir
cd dir
# The file is two stripes long
FILE_SIZE=$((SAUNAFS_BLOCK_SIZE * 6)) assert_success file-generate file &
cd "${TEMP_DIR}"

# Give some time for the writes to begin and trigger the EIO on CS 0, then restart the master server
# while the chunkserver is still handling the write job.
sleep 1
saunafs_master_daemon restart

wait
# The part in the first chunkserver should have been removed, and should have been reported as lost
# chunk to the master

saunafs_wait_for_all_ready_chunkservers

# Only 3 copies should be available
assert_equals $(saunafs fileinfo "${info[mount0]}/dir/file" | tail -n1 | grep -c "copy 3:") 1

# The missing part should be replicated to the first, which now won't fail, and the file should be
# healthy again
assert_eventually_prints "4:" "saunafs fileinfo '${info[mount0]}/dir/file' | tail -n1 | awk \
	'{print \$2}'" "30 seconds"

saunafs_chunkserver_daemon 3 stop
saunafs_wait_for_ready_chunkservers 3
# Check the file is still healthy with 3 copies, this time verifying the part in the 1st CS
assert_success file-validate "${info[mount0]}/dir/file"
