timeout_set 5 minutes

# A master killed while a chunkserver still flushes a write leaves that write's session open. The
# next master's write must wait for that session instead of sharing its lock, and its own flush
# must then keep a third write waiting.
CHUNKSERVERS=3 \
	USE_RAMDISK=YES \
	CHUNKSERVER_0_DISK_0="${RAMDISK_DIR}/pwrite_very_slow_hdd_0" \
	CHUNKSERVER_1_DISK_0="${RAMDISK_DIR}/pwrite_very_slow_hdd_1" \
	CHUNKSERVER_2_DISK_0="${RAMDISK_DIR}/pwrite_very_slow_hdd_2" \
	CHUNKSERVER_EXTRA_CONFIG="MAX_BLOCKS_PER_HDD_WRITE_JOB = 1|MASTER_RECONNECTION_DELAY = 1|LOG_FLUSH_ON = TRACE" \
	CHUNKSERVER_0_EXTRA_CONFIG="MAGIC_DEBUG_LOG = ${TEMP_DIR}/chunkserver_0.log" \
	CHUNKSERVER_1_EXTRA_CONFIG="MAGIC_DEBUG_LOG = ${TEMP_DIR}/chunkserver_1.log" \
	CHUNKSERVER_2_EXTRA_CONFIG="MAGIC_DEBUG_LOG = ${TEMP_DIR}/chunkserver_2.log" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_CUSTOM_GOALS="10 ec21: \$ec(2,1)" \
	MASTER_EXTRA_CONFIG="OPERATIONS_DELAY_INIT = 0|OPERATIONS_DELAY_DISCONNECT = 0|AUTO_RECOVERY = 1" \
	setup_local_empty_saunafs info

# Slow disks keep the last part of a write flushing for a few seconds after the client is done,
# and the hold file slows every flush to a block per second for as long as it exists. A held
# write still releases its chunk between two blocks, as the registration with a new master waits
# for every chunk a job holds.
hold_file="${TEMP_DIR}/pwrite_hold"
for csid in 0 1 2; do
	CHUNK_OPERATIONS_PWRITE_HOLD="${hold_file}" \
		LD_PRELOAD="${SAUNAFS_INSTALL_FULL_LIBDIR}/libchunk_operations_eio.so" \
		assert_success saunafs_chunkserver_daemon "${csid}" restart
done
saunafs_wait_for_all_ready_chunkservers

cd "${info[mount0]}"
mkdir dir
saunafs setgoal ec21 dir

# Each write covers half of a chunk, so each of the three parts grows by a quarter of a chunk
half=$((SAUNAFS_CHUNK_SIZE / 2))
half_mb=$((half / 1024 / 1024))

### Helpers

hold_flushes() {
	touch "${hold_file}"
}

release_flushes() {
	rm -f "${hold_file}"
}

chunkserver_log() {
	cat "${TEMP_DIR}/chunkserver_$1.log" 2>/dev/null
}

# "<id>_<version>" of the file's first chunk, as its chunk files are named on the chunkservers
first_chunk_and_version_of() {
	saunafs fileinfo "$1" | awk '/chunk 0:/{print $3}'
}

# Size of the data file of chunk part "<id>_<version>" on a chunkserver, empty without one
part_data_size() {
	local part
	part=$(find_chunkserver_chunks "$1" -name "*_$2${chunk_data_extension}" | head -1)
	[[ -n "${part}" ]] && stat -c %s "${part}" || true
}

# Whether every chunkserver still has its part of chunk $1 below $2 bytes
all_parts_flushing() {
	local csid size
	for csid in 0 1 2; do
		size=$(part_data_size "${csid}" "$1")
		[[ -n "${size}" ]] && ((size < $2)) || return 1
	done
}

# Whether every chunkserver still has at least $3 blocks to flush to bring its part of chunk $1 to
# $2 bytes: under the hold, enough to outlast what the test waits for
blocks_left_to_flush() {
	local csid size
	for csid in 0 1 2; do
		size=$(part_data_size "${csid}" "$1")
		[[ -n "${size}" ]] && (( ($2 - size) / SAUNAFS_BLOCK_SIZE >= $3 )) || return 1
	done
}

# Writes half a chunk of new random data, kept in ${TEMP_DIR}/$1, to ${file} at offset $2 MiB in
# the background; the writer's pid goes to ${writer}
write_half_in_background() {
	head -c "${half}" /dev/urandom > "${TEMP_DIR}/$1"
	dd if="${TEMP_DIR}/$1" of="${file}" bs=1M seek="$2" conv=fsync,notrunc status=none &
	writer=$!
}

# Checks that the background writer $1 is still running for $2 seconds
writer_still_running_for() {
	local i
	for ((i = 0; i < $2 * 2; ++i)); do
		kill -0 "$1" 2>/dev/null || test_fail "${3} ended while the flush it waits for was held"
		sleep 0.5
	done
}

# Waits for the background writer $1 and checks it succeeded
wait_for_writer_success() {
	local status=0
	assert_success wait_for "! kill -0 $1 2>/dev/null" '60 seconds'
	wait "$1" || status=$?
	assert_equals 0 "${status}"
}

# Writes the first half of file<n> and holds its flush once the client is done, then kills and
# restarts the master. Fails, to be tried again with a new file, unless every part still had
# enough of the flush left, under the hold, to outlast the restart and the second write's wait.
kill_master_during_flush() {
	local file=dir/file$1 chunk
	release_flushes
	head -c "${half}" /dev/urandom > "${TEMP_DIR}/a"
	assert_success dd if="${TEMP_DIR}/a" of="${file}" bs=1M conv=fsync status=none
	hold_flushes
	chunk=$(first_chunk_and_version_of "${file}")
	blocks_left_to_flush "${chunk}" $((half / 2)) 40 || return 1

	# The client's unlock is in the changelog, so the next master finds the chunk unlocked and
	# only the held session can keep the next write back
	assert_eventually "grep -q 'UNLOCK($((16#${chunk%%_*})))' '${info[master_data_path]}/changelog.sfs'"
	assert_success saunafs_master_daemon kill
	assert_success saunafs_master_daemon start
	saunafs_wait_for_all_ready_chunkservers
}

### First write: the master is killed while its flush is held

attempt=1
until kill_master_during_flush "${attempt}"; do
	if ((attempt == 3)); then
		# The runner was too slow to catch the end of a flush: no verdict on the product.
		echo "INCONCLUSIVE: the first write had almost flushed when its flush was held, 3 times"
		test_end
	fi
	((++attempt))
done
file=dir/file${attempt}
chunk_a=$(first_chunk_and_version_of "${file}")

### Second write: fills the second half of the chunk, and waits for the first write's session

# The second write fills the second half of the chunk. It waits for the first write's session,
# whose flush is held: the chunkservers hold back the version change the new master sends with
# its lock until that session ends
write_half_in_background b "${half_mb}"
writer_still_running_for "${writer}" 15 "the second write"

# The parts still have the first write's version and size: its session never ended
assert_success all_parts_flushing "${chunk_a}" $((half / 2))

release_flushes
wait_for_writer_success "${writer}"

### Third write: overwrites the first half, and is refused while the second write's flush is held

hold_flushes
chunk_b=$(first_chunk_and_version_of "${file}")

# The second write raised the version of the same chunk, the change the chunkservers held back
# behind the first write's session
assert_equals "${chunk_a%%_*}" "${chunk_b%%_*}"
assert_equals $((16#${chunk_a##*_} + 1)) $((16#${chunk_b##*_}))

if ! blocks_left_to_flush "${chunk_b}" "${half}" 20; then
	release_flushes
	echo "INCONCLUSIVE: the second write had almost flushed when its flush was held"
	test_end
fi

# Write start refusals the mount has logged so far
locked_before=$(grep -c 'Chunk locked' /var/log/syslog || true)

# The third write overwrites the first half of the chunk. It waits for the second write's session,
# whose flush is held: the new master knows that session, so it refuses the third write's start
# with "Chunk locked"
write_half_in_background c 0
writer_still_running_for "${writer}" 10 "the third write"
assert_less_than "${locked_before}" "$(grep -c 'Chunk locked' /var/log/syslog || true)"

release_flushes
wait_for_writer_success "${writer}"

### Final checks

cat "${TEMP_DIR}/c" "${TEMP_DIR}/b" > "${TEMP_DIR}/expected"
assert_files_equal "${TEMP_DIR}/expected" "${file}"

# The new master's locks were installed after each old session ended, never adopted from one
for csid in 0 1 2; do
	assert_awk_finds '/chunk lock installed/' "$(chunkserver_log "${csid}")"
	assert_awk_finds_no '/chunk lock of chunkId .* adopted/' "$(chunkserver_log "${csid}")"
done
