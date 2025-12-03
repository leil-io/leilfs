timeout_set 120 seconds
rebalancing_timeout=90

CHUNKSERVERS=5 \
	USE_LOOP_DISKS=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	CHUNKSERVER_EXTRA_CONFIG="HDD_TEST_FREQ = 10000|HDD_LEAVE_SPACE_DEFAULT = 0MiB" \
	MASTER_EXTRA_CONFIG="CHUNKS_LOOP_MIN_TIME = 1`
			`|CHUNKS_LOOP_MAX_CPU = 90`
			`|CHUNKS_WRITE_REP_LIMIT = 1`
			`|CHUNKS_LOOP_MAX_CPS = 2`
			`|CHUNKS_READ_REP_LIMIT = 2`
			`|OPERATIONS_DELAY_INIT = 0`
			`|OPERATIONS_DELAY_DISCONNECT = 0`
			`|POLL_TIMEOUT_MS = 10`
			`|ACCEPTABLE_DIFFERENCE = 0.0015" \
	setup_local_empty_saunafs info

# Create some chunks on three out of five chunkservers
saunafs_chunkserver_daemon 0 stop
saunafs_chunkserver_daemon 1 stop
saunafs_wait_for_ready_chunkservers 3
cd "${info[mount0]}"
mkdir dir dirxor
saunafs setgoal 2 dir
saunafs setgoal xor2 dirxor
for i in {1..10}; do
	 # Each loop creates 2 standard chunks and 3 xor chunks, ~1 MB each
	( FILE_SIZE=1M expect_success file-generate "dir/file_$i" ) &
	( FILE_SIZE=2M expect_success file-generate "dirxor/file_$i" ) &
done
wait
saunafs_chunkserver_daemon 0 start
saunafs_chunkserver_daemon 1 start

echo "Waiting for rebalancing..."
expected_rebalancing_status="10 10 10 10 10"
status=
end_time=$((rebalancing_timeout + $(date +%s)))
while [[ $status != $expected_rebalancing_status ]] && (( $(date +%s) < end_time )); do
	sleep 1
	status=$(saunafs_rebalancing_status | awk '{print $2}' | xargs echo)
	echo "Rebalancing status: $status"
done
MESSAGE="Chunks are not rebalanced properly" assert_equals "$expected_rebalancing_status" "$status"

for csid in {0..4}; do
	saunafs_chunkserver_daemon $csid stop
	MESSAGE="Validating files without chunkserver $csid" expect_success file-validate dir/*
	MESSAGE="Validating files without chunkserver $csid" expect_success file-validate dirxor/*
	saunafs_chunkserver_daemon $csid start
	saunafs_wait_for_all_ready_chunkservers
done
