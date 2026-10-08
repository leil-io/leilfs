timeout_set 5 minutes

# A chunkserver acknowledges buffered writes of ec and xor parts before they reach its disk, and
# reads of the part already return them. The master keeps the chunk locked until the flush ends,
# but a restarted master no longer knows about it and may rebuild a part from those sources. The
# rebuilt part must hold every acknowledged block: it is published as a full part, so a missing
# tail would read as zeros.
master_cfg="OPERATIONS_DELAY_INIT = 0|OPERATIONS_DELAY_DISCONNECT = 0"
master_cfg+="|CHUNKS_LOOP_MIN_TIME = 1|CHUNKS_LOOP_MAX_CPU = 90|PRIORITIZE_DATA_PARTS = 1"
CHUNKSERVERS=3 \
	USE_RAMDISK=YES \
	CHUNKSERVER_0_DISK_0="${RAMDISK_DIR}/pwrite_very_slow_hdd_0" \
	CHUNKSERVER_1_DISK_0="${RAMDISK_DIR}/pwrite_very_slow_hdd_1" \
	CHUNKSERVER_EXTRA_CONFIG="MAX_BLOCKS_PER_HDD_WRITE_JOB = 1|MASTER_RECONNECTION_DELAY = 1" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_EXTRA_CONFIG="${master_cfg}" \
	setup_local_empty_saunafs info

# With PRIORITIZE_DATA_PARTS the data parts take the two servers that are up, cs0 and cs1, whose
# slow disks keep acknowledged blocks in the write buffer; the parity is the part left without a
# server, and is rebuilt from them once cs2 starts.
for csid in 0 1; do
	LD_PRELOAD="${SAUNAFS_INSTALL_FULL_LIBDIR}/libchunk_operations_eio.so" \
		assert_success saunafs_chunkserver_daemon "${csid}" restart
done
saunafs_wait_for_all_ready_chunkservers

cd "${info[mount0]}"
mkdir dir
saunafs setgoal ec21 dir
full_size=$((SAUNAFS_CHUNK_SIZE / 2))

# Size of the data file of a chunk ("<id>_<version>") on a chunkserver.
part_size() {
	local part
	part=$(find_chunkserver_chunks "${1}" -name "*_${2}${chunk_data_extension}" | head -1)
	[[ -n "${part}" ]] && stat -c %s "${part}" || echo 0
}
both_flushing() {
	(($(part_size 0 "${1}") < full_size && $(part_size 1 "${1}") < full_size))
}

# Writes file<n> while cs2 is down, restarts the master and lets it rebuild the parity on cs2.
# Fails, to be tried again with a new file, unless both data parts were still being flushed when
# the rebuilt parity was published.
rebuild_during_flush() {
	local file=dir/file${1} chunk
	if saunafs_chunkserver_daemon 2 isalive; then
		assert_success saunafs_chunkserver_daemon 2 stop
	fi
	head -c "${SAUNAFS_CHUNK_SIZE}" /dev/urandom >"${TEMP_DIR}/data${1}"
	assert_success dd if="${TEMP_DIR}/data${1}" of="${file}" bs=1M conv=fsync status=none
	chunk=$(saunafs fileinfo "${file}" | awk '/chunk 0:/{print $3}')
	both_flushing "${chunk}" || return 1

	assert_success saunafs_master_daemon restart
	saunafs_wait_for_ready_chunkservers 2
	assert_success saunafs_chunkserver_daemon 2 start
	assert_eventually_prints 3 "saunafs fileinfo ${file} | grep -c copy"
	both_flushing "${chunk}"
}

attempt=1
until rebuild_during_flush "${attempt}"; do
	if ((attempt == 3)); then
		# The runner was too slow to overlap the rebuild with a flush: no verdict on the product.
		echo "INCONCLUSIVE: data parts flushed before the parity was rebuilt, 3 times"
		test_end
	fi
	((++attempt))
done
file=dir/file${attempt}
chunk=$(saunafs fileinfo "${file}" | awk '/chunk 0:/{print $3}')

# The published parity must hold every acknowledged block, and serve them in place of a data part.
assert_equals "${full_size}" "$(part_size 2 "${chunk}")"
assert_success saunafs_chunkserver_daemon 0 kill
saunafs_wait_for_ready_chunkservers 2
assert_files_equal "${TEMP_DIR}/data${attempt}" "${file}"
