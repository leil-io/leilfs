timeout_set 4 minutes

# A master killed while a write start waits for one chunkserver's lock reply leaves its chunk
# locks on the other chunkservers, with the connection they would report to gone. The next
# master's lock of the same parts must adopt those locks, so that the end of the write under
# them is reported to it. Taken for retransmissions, the write end was lost and the chunk stayed
# locked on the master once the client unlocked it (LS-1185).
CHUNKSERVERS=4 \
	USE_RAMDISK=YES \
	CHUNKSERVER_EXTRA_CONFIG="MAX_BLOCKS_PER_HDD_WRITE_JOB = 1|LOG_FLUSH_ON = TRACE" \
	CHUNKSERVER_0_EXTRA_CONFIG="MAGIC_DEBUG_LOG = ${TEMP_DIR}/chunkserver_0.log" \
	CHUNKSERVER_1_EXTRA_CONFIG="MAGIC_DEBUG_LOG = ${TEMP_DIR}/chunkserver_1.log" \
	CHUNKSERVER_2_EXTRA_CONFIG="MAGIC_DEBUG_LOG = ${TEMP_DIR}/chunkserver_2.log" \
	CHUNKSERVER_0_DISK_0="${RAMDISK_DIR}/pwrite_only_slow_hdd_0" \
	CHUNKSERVER_1_DISK_0="${RAMDISK_DIR}/pwrite_only_slow_hdd_1" \
	CHUNKSERVER_2_DISK_0="${RAMDISK_DIR}/pwrite_only_slow_hdd_2" \
	CHUNKSERVER_3_DISK_0="${RAMDISK_DIR}/pwrite_only_slow_hdd_3" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_CUSTOM_GOALS="10 ec31: \$ec(3,1)" \
	MASTER_EXTRA_CONFIG="OPERATIONS_DELAY_INIT = 0|OPERATIONS_DELAY_DISCONNECT = 0|AUTO_RECOVERY = 1" \
	setup_local_empty_saunafs info

# Slow disks keep the write going long enough to gate on the part files
for csid in 0 1 2 3; do
	LD_PRELOAD="${SAUNAFS_INSTALL_FULL_LIBDIR}/libchunk_operations_eio.so" \
		assert_success saunafs_chunkserver_daemon ${csid} restart
done
saunafs_wait_for_all_ready_chunkservers

cd "${info[mount0]}"
mkdir ec31
saunafs setgoal ec31 ec31

# Hold one chunkserver, so that its lock reply, and with it the write start reply, never come
held_pid=$(saunafs_chunkserver_daemon 3 test | sed 's/.*pid: //')
assert_success test -n "${held_pid}"
kill -STOP "${held_pid}"

FILE_SIZE=$((SAUNAFS_BLOCK_SIZE * 1024)) file-generate ec31/file &
writer=$!

# Each of the other three chunkservers holds its part on disk and has installed the master's
# lock on it: the lock follows the create status in the same callback, so the part file alone
# is too early; the chunkserver traces the installed lock to its magic debug log.
cs_log() {
	cat "${TEMP_DIR}/chunkserver_$1.log" 2>/dev/null
}
peer_holds_locked_part() {
	[[ $(find "${RAMDISK_DIR}/pwrite_only_slow_hdd_$1" -name 'chunk_*.dat' 2>/dev/null | wc -l) -ge 1 ]] &&
		cs_log "$1" | grep -q 'chunk lock installed'
}
for csid in 0 1 2; do
	assert_success wait_for "peer_holds_locked_part ${csid}" '60 seconds'
done

cd "${TEMP_DIR}"
saunafs_master_daemon kill
kill -CONT "${held_pid}"
assert_success saunafs_master_daemon start
saunafs_wait_for_all_ready_chunkservers

# The writer waits out the lock the dead master took (120 s), then writes under the next
# master's lock and finishes.
assert_success wait_for "! kill -0 ${writer} 2>/dev/null" '300 seconds'
writer_status=0
wait "${writer}" || writer_status=$?
assert_equals 0 "${writer_status}"
assert_success file-validate "${info[mount0]}/ec31/file"

# The chunk is not locked on the master any more: a new write on the file goes through
file-overwrite "${info[mount0]}/ec31/file" &
writer=$!
assert_success wait_for "! kill -0 ${writer} 2>/dev/null" '60 seconds'
writer_status=0
wait "${writer}" || writer_status=$?
assert_equals 0 "${writer_status}"
assert_success file-validate "${info[mount0]}/ec31/file"

# Each live chunkserver adopted the lock the dead master left on its part
for csid in 0 1 2; do
	assert_awk_finds '/chunk lock of chunkId .* adopted/' "$(cs_log "${csid}")"
done
