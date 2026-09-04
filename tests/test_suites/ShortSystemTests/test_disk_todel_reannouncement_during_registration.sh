timeout_set '2 minutes'

# A disk configuration reload re-announces every chunk it owns when its
# to-delete state changes. During a pull registration, an already swept
# chunk must be revisited with that new state: suppressing the announcement
# without clearing its sweep mark leaves the master believing that the old
# (non-to-delete) copy is still current indefinitely.
#
# Create a real, goal-1 chunk while only chunkserver 0 is running, then add a
# large mock disk to make its subsequent registration long enough to exercise
# the race. The target is deliberately waited for *after* its first report,
# so changing its disk state can only reach the master through the revisited
# registration entry. A correct master marks it to-delete, replicates it to
# chunkserver 1, and removes the old local copy.

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

# With chunkserver 1 stopped, the target has exactly one copy on the first
# disk of chunkserver 0.  Start the other server before the test window so it
# is ready to receive the relocation after that disk becomes to-delete.
saunafs_chunkserver_daemon 1 stop
mkdir "${info[mount0]}/target_dir"
assert_success saunafs setgoal 1 "${info[mount0]}/target_dir"
target_file="${info[mount0]}/target_dir/target"
FILE_SIZE=1M file-generate "$target_file"
assert_eventually_equals 'echo 1' "count_registered_parts '$target_file'"
saunafs_chunkserver_daemon 1 start
saunafs_wait_for_all_ready_chunkservers

# The mock entries are only a pacing reservoir. Their ids are outside the
# allocator's range for the real target above, so none can alias it.
mock_dir="$RAMDISK_DIR/mock_disk_0"
mkdir -p "$mock_dir"
echo "mock:${CHUNK_COUNT}:1000000:${mock_dir}" >> "${info[chunkserver0_hdd]}"
saunafs_chunkserver_daemon 0 reload
assert_eventually_equals "echo $((CHUNK_COUNT + 1))" 'count_chunk_copies'

# Restart only the source chunkserver. Wait until the target has been
# reported once to this registration, while most of the mock registry remains
# unswept. This makes the following reload an update of an already-emitted
# record rather than an ordinary first report.
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

# Prefixing a configured disk with '*' changes it to to-delete and causes the
# chunkserver to re-announce every part it owns. The target has no second
# physical copy at this point, so it cannot disappear accidentally.
sed -i '1s/^/*/' "${info[chunkserver0_hdd]}"
saunafs_chunkserver_daemon 0 reload
echo "PHASE: target disk marked to-delete during paced registration"

# The terminal sweep pass should report the '$target_file' chunk with the to-delete flag.
# Wait for terminal sweep pass before giving normal repair its own bounded window to copy
# the target to chunkserver 1 and remove the source copy from hdd_0_0.
assert_eventually_equals "echo $((CHUNK_COUNT + 1))" 'count_chunk_copies' '45 seconds'
target_disk="$RAMDISK_DIR/hdd_0_0"
MESSAGE="to-delete re-announcement must relocate the already swept target" \
	assert_eventually_prints 0 "find '$target_disk' -type f -name 'chunk*' | wc -l" '30 seconds'
assert_eventually_equals 'echo 1' "count_registered_parts '$target_file'"
