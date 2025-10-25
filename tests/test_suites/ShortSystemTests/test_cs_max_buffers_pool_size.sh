timeout_set 30 seconds

CHUNKSERVERS=6 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	CHUNKSERVER_EXTRA_CONFIG="MAX_BUFFERS_POOL_SIZE_MB = 0`
		`|MAX_BLOCKS_PER_HDD_WRITE_JOB = 1`
		`|MAX_BLOCKS_PER_HDD_READ_JOB = 1`
		`|MAX_PARALLEL_HDD_READ_JOBS_PER_CS_ENTRY = 1`
		`|LOG_LEVEL = trace|LOG_FLUSH_ON = TRACE|MAGIC_DEBUG_LOG = ${TEMP_DIR}/log" \
	MASTER_CUSTOM_GOALS="8 ec42: \$ec(4,2)"
	setup_local_empty_saunafs info

apply_chunkserver_config() {
	local parameter=$1
	local value=$2
	for i in $(seq 0 $(( ${info[chunkserver_count]} - 1 ))); do
		sed -i -re "s/^$parameter = .*/$parameter = $value/" "${info[chunkserver${i}_cfg]}"
	done
}

file_size=$(( 4 * SAUNAFS_CHUNK_SIZE ))

mkdir -p "${info[mount0]}/data"
saunafs setgoal -r ec42 "${info[mount0]}/data"
saunafs settrashtime 0 "${info[mount0]}/data"
cd "${info[mount0]}/data"

current_iteration=0
for maxBuffersPoolSize_mb in 0 64 512 4096; do
	apply_chunkserver_config MAX_BUFFERS_POOL_SIZE_MB ${maxBuffersPoolSize_mb}
	# To apply the new configuration, we just need to reload the chunkservers.
	for i in $(seq 0 $(( ${info[chunkserver_count]} - 1 ))); do
		saunafs_chunkserver_daemon $i reload
	done
	saunafs_wait_for_all_ready_chunkservers

	echo "Testing with MAX_BUFFERS_POOL_SIZE_MB=${maxBuffersPoolSize_mb}..."
	FILE_SIZE=${file_size} BLOCK_SIZE=65536 file-generate file_${maxBuffersPoolSize_mb}
	if ! file-validate file_${maxBuffersPoolSize_mb}; then
		test_add_failure "File validation failed for MAX_BUFFERS_POOL_SIZE_MB=${maxBuffersPoolSize_mb}"
	fi

	echo "File validation succeeded for MAX_BUFFERS_POOL_SIZE_MB=${maxBuffersPoolSize_mb}"
	rm -f file_${maxBuffersPoolSize_mb}
	sleep 1

	expectedNumberOfBlocks=$((maxBuffersPoolSize_mb * 1024 * 1024 / SAUNAFS_BLOCK_SIZE))
	if ! grep "maxNumberOfBlocks_ = ${expectedNumberOfBlocks}" "${TEMP_DIR}/log"; then
		test_add_failure "Log does not contain expected buffer pool size: maxNumberOfBlocks_ = ${expectedNumberOfBlocks}"
	fi
done
