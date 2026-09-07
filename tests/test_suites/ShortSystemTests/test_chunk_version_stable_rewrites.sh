timeout_set 2 minutes

# Rewriting a chunk whose copies are all present must not change its version:
# the version exists to tell copies apart, not to count writes. Reference
# behavior for every metadata backend.
CHUNKSERVERS=2 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_EXTRA_CONFIG="CHUNKS_LOOP_MIN_TIME = 1 \
		|OPERATIONS_DELAY_INIT = 0 \
		|OPERATIONS_DELAY_DISCONNECT = 0" \
	setup_local_empty_saunafs info

get_chunk_version() {
	saunafs fileinfo "${1}" | awk '
		$1 == "chunk" && $2 == "0:" {
			if (match($0, /ver:[0-9]+/)) {
				print substr($0, RSTART + 4, RLENGTH - 4)
			}
		}
	'
}

copy_count() {
	saunafs fileinfo "${1}" | grep -c copy
}

cd "${info[mount0]}"
mkdir dir
saunafs setgoal 2 dir
reference="${TEMP_DIR}/reference"
FILE_SIZE=1M file-generate "${reference}"
cp "${reference}" dir/file
assert_eventually_prints 2 "copy_count dir/file"
assert_equals 1 "$(get_chunk_version dir/file)"

# Each dd is a separate write session on the same chunk with both copies online.
for session in 1 2 3; do
	sleep 3
	dd if=/dev/urandom of="${reference}" bs=64K count=1 seek="${session}" conv=notrunc status=none
	dd if="${reference}" of=dir/file bs=64K count=1 skip="${session}" seek="${session}" \
		conv=notrunc status=none
	sleep 3
	echo "session ${session} version $(get_chunk_version dir/file)"
done
assert_equals 1 "$(get_chunk_version dir/file)"
assert_eventually_prints 2 "copy_count dir/file"
assert_success cmp "${reference}" dir/file
