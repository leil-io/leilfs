# Most of the runtime is a fixed wall-clock floor (75s buffer-expiry wait,
# slowed preads); the budget needs headroom for the ~6 GB of real I/O on top.
timeout_set 5 minutes

cacheexpirationtime_ms=15000
readbuffersexpirationtime_ms=60000

CHUNKSERVERS=1 \
	DISK_PER_CHUNKSERVER=1 \
	CHUNKSERVER_0_DISK_0="$RAMDISK_DIR/pread_only_slow_hdd_0" \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER|maxreadaheadrequests=4`
		`|cacheexpirationtime=${cacheexpirationtime_ms}|readworkers=1`
		`|readaheadmaxwindowsize=16384`
		`|readbuffersexpirationtime=${readbuffersexpirationtime_ms}" \
	MASTER_EXTRA_CONFIG="CHUNKS_LOOP_MIN_TIME = 1`
			`|CHUNKS_LOOP_MAX_CPU = 90`
			`|CHUNKS_LOOP_PERIOD = 10`
			`|OPERATIONS_DELAY_INIT = 0`
			`|OPERATIONS_DELAY_DISCONNECT = 0" \
	setup_local_empty_saunafs info

cd ${info[mount0]}

# Create a file with 30 chunks, and read it to make sure client creates all necessary read buffers
dd if=/dev/zero of=file bs=1M count=$((30 * 64)) status=none
dd if=file of=/dev/null bs=1M count=$((30 * 64)) status=none

saunafs settrashtime 0 file
rm file

# The next file needs the ramdisk space; a restart mid-deletion would make the
# chunkserver rejoin with the leftover chunks still on disk and fail the
# writes below with ENOSPC.
assert_eventually '[[ $(find_chunkserver_chunks 0 | wc -l) == 0 ]]' "2 minutes"

# Restart the first chunkserver preloading pread with slow version of reads
LD_PRELOAD="${SAUNAFS_INSTALL_FULL_LIBDIR}/libchunk_operations_eio.so" \
		assert_success saunafs_chunkserver_daemon 0 restart
saunafs_wait_for_all_ready_chunkservers

create-and-reread-file "file" $(( (cacheexpirationtime_ms + readbuffersexpirationtime_ms) / 1000)) &
helper_pid=$!

# The helper keeps the notify file around for only one second before removing
# it and exiting, so a clean helper exit is an equivalent success signal; a
# failed exit fails the test fast instead of spinning until the timeout.
while ! [[ -f notify_file_reread ]]; do
	if ! kill -0 "${helper_pid}" 2>/dev/null; then
		helper_status=0
		wait "${helper_pid}" || helper_status=$?
		assert_equals 0 "${helper_status}"
		break
	fi
	sleep 0.1
done

# At least 75s must have passed since the last read of the file (see create_and_reread_file.cc),
# so all read buffers should have been expired by now. Check that sfsmount is not using too much 
# memory, which would indicate that it is keeping all read buffers in memory instead of expiring
# them.

sfsmount_pid="$(pgrep -f "sfsmount.*${info[mount0]}" | head -n1)"
assert_success test -n "${sfsmount_pid}"

rss_kb="$(ps -o rss= -p "${sfsmount_pid}" | tr -d '[:space:]')"
vsz_kb="$(ps -o vsz= -p "${sfsmount_pid}" | tr -d '[:space:]')"

echo "sfsmount pid=${sfsmount_pid} rss=${rss_kb}KB vsz=${vsz_kb}KB"

# 1 GB RSS is a very high limit that should be never reached if read buffers are expired properly
assert_less_than "${rss_kb}" $((1024 * 1024)) 

wait
