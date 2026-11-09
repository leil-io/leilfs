timeout_set '2 minutes'

# Reload the source disk as to-delete after its target was reported during
# pull registration. The sweep must revisit the chunk with its new flag,
# allowing the master to evacuate it to server 1 and remove the source copy.

CHUNK_COUNT=${CHUNK_COUNT:-30000}
REGISTRATION_CHUNKS_PER_SECOND=1000

CHUNKSERVERS=2 \
	DISK_PER_CHUNKSERVER=1 \
	MASTERSERVERS=1 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_EXTRA_CONFIG="CHUNK_REGISTRATION_CHUNKS_PER_SECOND = ${REGISTRATION_CHUNKS_PER_SECOND}`
			`|CHUNK_REGISTRATION_BULK_SIZE = 1`
			`|CHUNKS_LOOP_MIN_TIME = 1`
			`|CHUNKS_LOOP_MAX_CPU = 90`
			`|OPERATIONS_DELAY_INIT = 0`
			`|OPERATIONS_DELAY_DISCONNECT = 0" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

# Create the only target copy on server 0, then bring server 1 back as
# the evacuation destination.
saunafs_chunkserver_daemon 1 stop
mkdir "${info[mount0]}/target_dir"
assert_success saunafs setgoal 1 "${info[mount0]}/target_dir"
target_file="${info[mount0]}/target_dir/target"
FILE_SIZE=1M file-generate "$target_file"
assert_eventually_equals 'echo 1' "count_registered_parts '$target_file'"
saunafs_chunkserver_daemon 1 start
saunafs_wait_for_all_ready_chunkservers

# Mock chunks prolong registration; their IDs cannot overlap the real target.
mock_dir="$RAMDISK_DIR/mock_disk_0"
mkdir -p "$mock_dir"
echo "mock:${CHUNK_COUNT}:1000000:${mock_dir}" >> "${info[chunkserver0_hdd]}"
saunafs_chunkserver_daemon 0 reload
assert_eventually_equals "echo $((CHUNK_COUNT + 1))" 'count_chunk_copies'

# Restart the source and wait for the target's first report while inventory
# is still incomplete, so the reload must update an already-swept entry.
saunafs_chunkserver_daemon 0 restart

registration_in_progress() {
	local copies
	copies=$(count_chunk_copies)
	[[ -n "$copies" && "$copies" -gt 0 && "$copies" -lt $((CHUNK_COUNT + 1)) ]]
}
assert_eventually 'registration_in_progress'
assert_eventually_equals 'echo 1' "count_registered_parts '$target_file'"
MESSAGE="target must have been reported before its disk state changes" \
	assert_less_than "$(count_chunk_copies)" "$((CHUNK_COUNT + 1))"

# '*' marks the source disk to-delete and triggers chunk reannouncements.
sed -i '1s/^/*/' "${info[chunkserver0_hdd]}"
saunafs_chunkserver_daemon 0 reload
echo "PHASE: target disk marked to-delete during paced registration"

# After inventory converges, allow a separate window for background evacuation.
# The source disk must become empty while the target keeps one registered copy.
assert_eventually_equals "echo $((CHUNK_COUNT + 1))" 'count_chunk_copies' '45 seconds'
target_disk="$RAMDISK_DIR/hdd_0_0"
MESSAGE="to-delete re-announcement must relocate the already swept target" \
	assert_eventually_prints 0 "find '$target_disk' -type f -name 'chunk*' | wc -l" '30 seconds'
assert_eventually_equals 'echo 1' "count_registered_parts '$target_file'"
