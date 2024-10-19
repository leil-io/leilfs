chunks_replicated_count() {
	saunafs-admin chunks-health \
		--porcelain \
		--replication \
		localhost "${info[matocl]}" | awk '$3!=0{sum+=$3}END{print sum}'
}

timeout_set 4 minutes
CHUNKSERVERS=4 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_EXTRA_CONFIG="CHUNKS_LOOP_MIN_TIME = 1`
			`|CHUNKS_LOOP_MAX_CPU = 90`
			`|OPERATIONS_DELAY_INIT = 0" \
	CHUNKSERVER_EXTRA_CONFIG="CHUNK_TRASH_ENABLED=1`
			`|CHUNK_TRASH_EXPIRATION_SECONDS=55" \
	USE_RAMDISK="YES" \
	setup_local_empty_saunafs info

# Create a file consising of a couple of chunks and remove it
file="${info[mount0]}/file"
xorfile="${info[mount0]}/xorfile"
touch "$file" "$xorfile"
saunafs setgoal 3 "$file"
saunafs setgoal xor3 "$xorfile"
dd if=/dev/zero of="$file" bs=1MiB count=130
dd if=/dev/zero of="$xorfile" bs=1MiB count=130
saunafs settrashtime 0 "$file" "$xorfile"

waiting_timeout="3 minutes"

# Wait for the chunks to be replicated
## TODO: Remove magic number 6
if ! wait_for '[ $(chunks_replicated_count) -eq 6 ]' "${waiting_timeout}"; then
	test_add_failure $'The chunks replication timed out'
fi

chunks_count_before_files_removal="$(find_all_chunks | wc -l)"
echo "Chunks count before files removal: ${chunks_count_before_files_removal}"

rm -f "${file}" "${xorfile}"

# Wait for removing all the chunks
if ! wait_for '[[ $(find_all_chunks | wc -l) == 0 ]]' "${waiting_timeout}"; then
	test_add_failure $'The following chunks were not removed:\n'"$(find_all_chunks)"
fi

# Ensure the "unlinked" files are trashed
trashed_chunks_count=$(find_all_trashed_chunks | wc -l)
## TODO: Remove the below debug call
find_all_trashed_chunks
echo "Trashed chunks count: ${trashed_chunks_count}"
if [ "${trashed_chunks_count}" -eq 0 ]; then
	test_add_failure $'The removed chunks were not moved to the trash folder'
fi

if [ "${trashed_chunks_count}" -ne  "${chunks_count_before_files_removal}" ]; then
	test_add_failure $'The removed chunks do not match the chunks number in the trash folder'
fi

sleep 60

# Ensure the trashed chunks are removed after the trashing time
trashed_chunks_count=$(find_all_trashed_chunks | wc -l)
if [ "${trashed_chunks_count}" -ne 0 ]; then
	test_add_failure $'The trashed chunks were not removed after the trashing time'
fi
