timeout_set 6 minutes

chunkserver_count=4

CHUNKSERVERS=${chunkserver_count} \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_EXTRA_CONFIG="CHUNKS_LOOP_MIN_TIME = 1`
			`|CHUNKS_LOOP_MAX_CPU = 90`
			`|OPERATIONS_DELAY_INIT = 0" \
	CHUNKSERVER_EXTRA_CONFIG="MAGIC_DEBUG_LOG = ${TEMP_DIR}/log|LOG_FLUSH_ON=DEBUG`
			`|CHUNK_TRASH_ENABLED=0`
			`|CHUNK_TRASH_EXPIRATION_SECONDS=1000" \
	USE_RAMDISK="YES" \
	setup_local_empty_saunafs info

create_test_file() {
	local file_path="$1"
	local goal="$2"

	touch "${file_path}"
	saunafs setgoal "${goal}" "${file_path}"
	dd if=/dev/zero of="${file_path}" bs=1MiB count=130
	saunafs settrashtime 0 "${file_path}"
}

reload_chunkserver_config() {
	local chunkserver_cfg="$1"
	# Send SIGHUP to the chunkserver to reload the config
	pgrep -a 'sfschunkserver' | grep "${chunkserver_cfg}" | awk '{print $1}' | \
		xargs -r kill -HUP
}

etcdir="${TEMP_DIR}/saunafs/etc"

######################################################################
#### Change the config to enable chunk trashing and reload it
######################################################################
# truncate the log file
truncate -s 0 "${TEMP_DIR}/log"
for chunkserver_id in $(seq 0 "$((chunkserver_count-1))"); do
	chunkserver_cfg="${etcdir}/sfschunkserver_${chunkserver_id}.cfg"
	sed -E -i 's/^CHUNK_TRASH_ENABLED=.*/CHUNK_TRASH_ENABLED=1/' "${chunkserver_cfg}"
	sed -E -i 's/^CHUNK_TRASH_EXPIRATION_SECONDS=.*/CHUNK_TRASH_EXPIRATION_SECONDS=50/' \
	"${chunkserver_cfg}"
	reload_chunkserver_config "${chunkserver_cfg}"
done
assert_eventually_prints 1 "cat '${TEMP_DIR}/log' | \
	grep -m1 'CHUNK_TRASH_EXPIRATION_SECONDS=50' | wc -l"

######################################################################
#### Test that the trashing is enabled and chunks are trashed
######################################################################

# Create a file consisting of a couple of chunks and remove it
create_test_file "${info[mount0]}/file" 3
create_test_file "${info[mount0]}/xorfile" xor3

# Count chunks before file removal
chunks_count_before_files_removal="$(find_all_chunks | wc -l)"
echo "Chunks count before files removal: ${chunks_count_before_files_removal}"
trash_chunks_count_before_files_removal="$(find_all_trashed_chunks | wc -l)"
echo "Trash chunks count before files removal: ${trash_chunks_count_before_files_removal}"

# Remove the files
rm -f "${info[mount0]}/file" "${info[mount0]}/xorfile"

# Wait for all chunks to be trashed
waiting_timeout="3 minutes"
if ! wait_for '[[ $(find_all_chunks | wc -l) == 0 ]]' "${waiting_timeout}"; then
	test_add_failure $'The following chunks were not removed:\n'"$(find_all_chunks)"
fi

# Verify the trashed chunks count
chunks_count_after_files_removal=$(find_all_chunks | wc -l)
echo "Chunks count after files removal: ${chunks_count_after_files_removal}"
trashed_chunks_count=$(find_all_trashed_chunks | wc -l)
echo "Trashed chunks count: ${trashed_chunks_count}"

MESSAGE="The trashed chunks count is lower than expected" \
assert_success test "${trashed_chunks_count}" -ge "${chunks_count_before_files_removal}"

# Wait for trashed chunks to expire
sleep 60

# Ensure trashed chunks are removed after expiration
if [ "$(find_all_trashed_chunks | wc -l)" -ne 0 ]; then
	test_add_failure $'The trashed chunks were not removed after the trashing time'
fi

######################################################################
#### Test that chunks already present in trash survive config reload
#### with trash disabled and are later removed by garbage collector
######################################################################

# Create a new file consisting of a couple of chunks and remove it
create_test_file "${info[mount0]}/file_reload_case" 3
create_test_file "${info[mount0]}/xorfile_reload_case" xor3
rm -f "${info[mount0]}/file_reload_case" "${info[mount0]}/xorfile_reload_case"

# Wait for all chunks to be trashed
if ! wait_for '[[ $(find_all_chunks | wc -l) == 0 ]]' "90 seconds"; then
	test_add_failure $'The following chunks were not removed before reload:\n'"$(find_all_chunks)"
fi

# Verify that chunks are present in trash before reload
trashed_chunks_before_reload=$(find_all_trashed_chunks | wc -l)
echo "Trashed chunks before reload: ${trashed_chunks_before_reload}"

MESSAGE="Expected trashed chunks before reload" \
assert_success test "${trashed_chunks_before_reload}" -gt 0

# Change the config to disable chunk trashing and reload it
truncate -s 0 "${TEMP_DIR}/log"
for chunkserver_id in $(seq 0 "$((chunkserver_count-1))"); do
	chunkserver_cfg="${etcdir}/sfschunkserver_${chunkserver_id}.cfg"
	sed -E -i 's/^CHUNK_TRASH_ENABLED=.*/CHUNK_TRASH_ENABLED=0/' "${chunkserver_cfg}"
	sed -E -i 's/^CHUNK_TRASH_EXPIRATION_SECONDS=.*/CHUNK_TRASH_EXPIRATION_SECONDS=50/' \
	"${chunkserver_cfg}"
	reload_chunkserver_config "${chunkserver_cfg}"
done

# Check that trashed chunks still exist immediately after reload
trashed_chunks_after_reload=$(find_all_trashed_chunks | wc -l)
echo "Trashed chunks immediately after reload: ${trashed_chunks_after_reload}"

MESSAGE="Expected trashed chunks to still exist immediately after reload" \
assert_success test "${trashed_chunks_after_reload}" -gt 0

# Ensure trashed chunks are removed later by garbage collector
if ! wait_for '[[ $(find_all_trashed_chunks | wc -l) == 0 ]]' "90 seconds"; then
	test_add_failure $'The trashed chunks were not removed after reload with trash disabled'
fi

######################################################################
##### Change the config to disable chunk trashing and reload it
######################################################################
truncate -s 0 "${TEMP_DIR}/log"
for chunkserver_id in $(seq 0 "$((chunkserver_count-1))"); do
	chunkserver_cfg="${etcdir}/sfschunkserver_${chunkserver_id}.cfg"
	sed -E -i 's/^CHUNK_TRASH_ENABLED=.*/CHUNK_TRASH_ENABLED=0/' "${chunkserver_cfg}"
	sed -E -i 's/^CHUNK_TRASH_EXPIRATION_SECONDS=.*/CHUNK_TRASH_EXPIRATION_SECONDS=90/' "${chunkserver_cfg}"
	reload_chunkserver_config "${chunkserver_cfg}"
done
assert_eventually_prints 1 "cat '${TEMP_DIR}/log' | \
	grep -m1 'CHUNK_TRASH_EXPIRATION_SECONDS=90' | wc -l"

######################################################################
#### Test that the trashing is disabled and chunks are removed immediately
######################################################################
# Create a file consisting of a couple of chunks and remove it
create_test_file "${info[mount0]}/file" 3
create_test_file "${info[mount0]}/xorfile" xor3

rm -f "${info[mount0]}/file" "${info[mount0]}/xorfile"

# Wait for removing all the chunks
timeout="3 minutes"
if ! wait_for '[[ $(find_all_chunks | wc -l) == 0 ]]' "$timeout"; then
	test_add_failure $'The following chunks were not removed:\n'"$(find_all_chunks)"
fi

# Verify that no chunks were trashed
trashed_chunks_count_after_disable=$(find_all_trashed_chunks | wc -l)
if [ "${trashed_chunks_count_after_disable}" -ne 0 ]; then
	test_add_failure $'Chunks were found in trash after disabling trashing'
fi
