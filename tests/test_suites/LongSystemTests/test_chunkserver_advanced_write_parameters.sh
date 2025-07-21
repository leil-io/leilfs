# Verifies that chunkservers are able to write using different block sizes per write job
# (MAX_BLOCKS_PER_HDD_WRITE_JOB).

assert_program_installed fio

timeout_set 15 minutes

CHUNKSERVERS=3 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,sfswritewindowsize=64" \
	CHUNKSERVER_EXTRA_CONFIG="MAGIC_DEBUG_LOG=${TEMP_DIR}/log|`
		`LOG_FLUSH_ON=DEBUG" \
	MASTER_CUSTOM_GOALS="1 ec21: \$ec(2,1)" \
	MASTER_EXTRA_CONFIG="CHUNKS_LOOP_MIN_TIME = 1`
		`|CHUNKS_LOOP_MAX_CPU = 90`
		`|ACCEPTABLE_DIFFERENCE = 1.0`
		`|CHUNKS_SOFT_DEL_LIMIT = 50`
		`|CHUNKS_HARD_DEL_LIMIT = 100`
		`|OPERATIONS_DELAY_INIT = 0`
		`|OPERATIONS_DELAY_DISCONNECT = 0" \
	setup_local_empty_saunafs info

apply_chunkserver_config() {
	local parameter=$1
	local value=$2
	for i in $(seq 0 $(( ${info[chunkserver_count]} - 1 ))); do
		sed -i -re "s/^$parameter = .*/$parameter = $value/" "${info[chunkserver${i}_cfg]}"
	done
}

restartChunkserversAndWait() {
	for i in $(seq 0 $(( ${info[chunkserver_count]} - 1 ))); do
		saunafs_chunkserver_daemon ${i} restart
	done
	saunafs_wait_for_all_ready_chunkservers
}

jobSize=$(( 4 * SAUNAFS_CHUNK_SIZE ))
# Block sizes from 64 KiB to 4 MiB
blocksToTest=(1 2 4 8 16 32 64)
# Test basic ec goal and a chain goal
goalsToTest=(3 ec21)
fioNumJobs=8

cd "${info[mount0]}"
for goal in ${goalsToTest[@]}; do
	mkdir "dir_${goal}" && saunafs setgoal ${goal} "dir_${goal}"
	saunafs settrashtime 0 "dir_${goal}"
	testDir="${info[mount0]}/dir_${goal}"

	for blocks in ${blocksToTest[@]}; do
		apply_chunkserver_config MAX_BLOCKS_PER_HDD_WRITE_JOB ${blocks}
		restartChunkserversAndWait

		echo "Testing with MAX_BLOCKS_PER_HDD_WRITE_JOB set to ${blocks} blocks"

		assert_success fio --directory=${testDir} --name=fio_bs_${blocks}_blocks --rw=write --bs=1M \
			--size=${jobSize} --numjobs=${fioNumJobs} --direct=1 --group_reporting --ioengine=libaio

		# Remove the chunks for the next iteration
		rm -rf ${testDir}/fio*
		echo "Waiting for chunks to be deleted"
		assert_eventually_prints "0" "find_all_chunks | wc -l" "2 minutes"

		# Check the log does not contain any related errors
		assert_failure grep -i "InputBuffer.*inconsistent" ${TEMP_DIR}/log
		# Clear the log for the next iteration
		echo > ${TEMP_DIR}/log
	done
done
