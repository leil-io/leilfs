timeout_set 4 minutes

CHUNKSERVERS=4 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_EXTRA_CONFIG="CHUNKS_LOOP_MIN_TIME = 1`
			`|CHUNKS_LOOP_MAX_CPU = 90`
			`|OPERATIONS_DELAY_INIT = 0" \
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
chunks_count_before_files_removal="$(find_all_chunks | wc -l)"
rm -f "$file" "$xorfile"

# Wait for removing all the chunks
timeout="3 minutes"
if ! wait_for '[[ $(find_all_chunks | wc -l) == 0 ]]' "$timeout"; then
	test_add_failure $'The following chunks were not removed:\n'"$(find_all_chunks)"
fi

# Ensure the "unlinked" files are trashed
trashed_chunks_count=$(find_all_trashed_chunks | wc -l)
if [ "${trashed_chunks_count}" -eq 0 ]; then
	test_add_failure $'The removed chunks were not moved to the trash folder'
fi

# Ensure the trashed chunks are removed after the trashing time
if [ "${trashed_chunks_count}" -ne  "${chunks_count_before_files_removal}" ]; then
	test_add_failure $'The removed chunks do not match the chunks number in the trash folder'
fi
