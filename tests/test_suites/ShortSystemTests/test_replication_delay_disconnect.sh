timeout_set "1 minute"

# Start an installation with:
#   7 unlabeled servers
#   the default goal 5
USE_RAMDISK=YES \
	CHUNKSERVERS=7 \
	MASTER_CUSTOM_GOALS="1 default: _ _ _ _ _" \
	MASTER_EXTRA_CONFIG="CHUNKS_LOOP_MIN_TIME = 1`
			`|CHUNKS_LOOP_MAX_CPU = 90`
			`|ACCEPTABLE_DIFFERENCE = 1.0`
			`|CHUNKS_WRITE_REP_LIMIT = 5`
			`|OPERATIONS_DELAY_INIT = 0`
			`|OPERATIONS_DELAY_DISCONNECT = 30" \
	setup_local_empty_saunafs info

# Leave only two servers
saunafs_chunkserver_daemon 0 stop
saunafs_chunkserver_daemon 1 stop
saunafs_chunkserver_daemon 2 stop
saunafs_chunkserver_daemon 3 stop
saunafs_chunkserver_daemon 4 stop
saunafs_wait_for_ready_chunkservers 2

# Create 20 files. Expect that for each file there are 2 chunk copies.
FILE_SIZE=1K file-generate "${info[mount0]}"/file{1..20}
assert_equals 20 $(saunafs checkfile "${info[mount0]}"/* | grep 'with 2 copies:' | wc -l)

# Stop one server with valid copy and start four new servers.
saunafs_chunkserver_daemon 5 stop
saunafs_chunkserver_daemon 0 start
saunafs_chunkserver_daemon 1 start
saunafs_chunkserver_daemon 2 start
saunafs_chunkserver_daemon 3 start
saunafs_wait_for_ready_chunkservers 5

# All chunks has 4 missing copies but 2 chunkservers are disconnected,
# so only two new copies should be created
if is_windows_system; then
	assert_eventually_prints 20 'saunafs checkfile "${info[mount0]}"/* | grep "with 3 copies:" | wc -l' '15 seconds'
else
	assert_eventually_prints 20 'saunafs checkfile "${info[mount0]}"/* | grep "with 3 copies:" | wc -l' '5 seconds'
fi

# Replication shouldn't be started for few more seconds.
sleep 10
assert_equals 20 $(saunafs checkfile "${info[mount0]}"/* | grep 'with 3 copies:' | wc -l)

# Expect two more copies of each chunk to migrate to the two empty servers
if is_windows_system; then
	assert_eventually_prints 20 'saunafs checkfile "${info[mount0]}"/* | grep "with 3 copies:" | wc -l' '60 seconds'
else
	assert_eventually_prints 20 'saunafs checkfile "${info[mount0]}"/* | grep "with 3 copies:" | wc -l' '20 seconds'
fi
