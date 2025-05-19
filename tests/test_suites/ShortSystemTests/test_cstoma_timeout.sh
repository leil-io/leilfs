CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_TIMEOUT = 10" \
	setup_local_empty_saunafs info

saunafs_wait_for_all_ready_chunkservers
assert_equals 1 $(saunafs_ready_chunkservers_count)

# Make the chunkserver hang and check if master disconnects if not earlier than
# 2/3 * MASTER_TIMEOUT and not later than after the MASTER_TIMEOUT is expired
pid=$(saunafs_chunkserver_daemon 0 test | sed 's/.*pid: //')
kill -STOP $pid
assert_equals 1 $(saunafs_ready_chunkservers_count)
sleep 6
assert_equals 1 $(saunafs_ready_chunkservers_count)
sleep 4
assert_equals 0 $(saunafs_ready_chunkservers_count)
