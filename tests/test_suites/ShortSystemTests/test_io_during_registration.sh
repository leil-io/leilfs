timeout_set '1 minute'

# Client IO must keep working while a chunkserver is (re-)registering its
# chunks: reads of not-yet-registered chunks are resolved on demand, writes
# create or update chunks normally, unlink works.
#
# Only chunkserver 1 is restarted, and that is what makes each of those claims
# testable:
#
#   reads  - chunkserver 1 holds the only copy of the chunks in its id range
#            (goal 1, disjoint ranges), so a read of one of them can only be
#            answered by resolving the location on demand. Probing a chunk from
#            chunkserver 0's range would prove nothing: it is registered the
#            whole time.
#
#   writes - a chunkserver reports no free space until its registration has
#            COMPLETED, so a cluster whose every chunkserver is registering can
#            place no new chunk at all and creating a file can only fail and be
#            retried. Keeping chunkserver 0 up gives new chunks somewhere to
#            land while chunkserver 1 is still registering.
#
# The same rule explains why the gate below cannot wait for chunkservers to
# report ready: readiness is that same post-registration space report, so
# waiting for it would close the window this test is about.

CHUNK_COUNT=${CHUNK_COUNT:-200000}
CHUNKS_PER_FILE=1000

CHUNKSERVERS=2 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_EXTRA_CONFIG="CHUNK_REGISTRATION_CHUNKS_PER_SECOND = 50000" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

# Seed metadata offline; disjoint id ranges per chunkserver at goal 1
saunafs_master_daemon stop
metadata_version=$(metadata_get_version "${info[master_data_path]}/metadata.sfs")
generate_mock_chunks_changelog "$((2 * CHUNK_COUNT))" "$CHUNKS_PER_FILE" "$metadata_version" \
	> "${info[master_data_path]}/changelog.sfs"
assert_success sfsmetarestore -a -d "${info[master_data_path]}"
saunafs_master_daemon start
saunafs_wait_for_all_ready_chunkservers

for csid in 0 1; do
	mock_dir="$RAMDISK_DIR/mock_disk_$csid"
	mkdir -p "$mock_dir"
	echo "mock:${CHUNK_COUNT}:$((csid * CHUNK_COUNT + 1)):${mock_dir}" \
		>> "${info[chunkserver${csid}_hdd]}"
	saunafs_chunkserver_daemon "$csid" reload
done

assert_eventually_equals "echo $((2 * CHUNK_COUNT))" 'count_chunk_copies'
echo "PHASE: initial registration complete ($(count_chunk_copies) copies)"

# Restart chunkserver 1: the master drops its copies and its re-registration
# window (paced by CHUNK_REGISTRATION_CHUNKS_PER_SECOND set above) begins.
# Chunkserver 0 keeps serving throughout.
saunafs_chunkserver_daemon 1 restart

# Wait until chunkserver 1 has registered at least one chunk, and no more than
# that. Chunkserver 0 never stops accounting for CHUNK_COUNT copies, so any
# count above that comes from chunkserver 1's sweep, which means it has scanned
# its disks and can answer on-demand location queries for the chunks it has not
# streamed yet. Waiting merely for "registration started" is not enough:
# the sweep begins before the disk scan finishes, and until the scan populates
# the registry the chunkserver truthfully answers that it does not have the
# chunk, so the probe would measure the client's retry loop instead.
registration_in_progress() {
	local copies
	copies=$(count_chunk_copies)
	[[ -n "$copies" && "$copies" -gt "$CHUNK_COUNT" && "$copies" -lt $((2 * CHUNK_COUNT)) ]]
}
assert_eventually 'registration_in_progress'
echo "PHASE: chunkserver 1 restarted, registration window open" \
	"($(count_chunk_copies) / $((2 * CHUNK_COUNT)) copies known)"

# IO during the registration window. The probes are spread across chunkserver 1's id
# range only. Those chunks have no other copy while it registers, so each read
# has to be resolved on demand. None of these files was read before, so the mount
# holds no cached location for them either.
total_files=$((2 * CHUNK_COUNT / CHUNKS_PER_FILE))
first_probe_file=$((CHUNK_COUNT / CHUNKS_PER_FILE))
last_probe_file=$((total_files - 1))
expected_head="5a5b58595e5f5c5d5253505156575455"
for fraction_idx in 0 1 2 3; do
	file_idx=$((first_probe_file + fraction_idx * (last_probe_file - first_probe_file) / 3))
	file="${info[mount0]}/$(printf 'mock_%07d' "$file_idx")"
	# reads of mock-backed chunks succeed with the right content
	head_hex=$(dd if="$file" bs=16 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')
	MESSAGE="read of $file during registration" \
		assert_equals "$expected_head" "$head_hex"
done

# writes work and read back correctly: their new chunks land on chunkserver 0's
# real disk, the only one with space to offer while chunkserver 1 registers
for i in 1 2 3; do
	echo "data $i" > "${info[mount0]}/during_registration_$i"
	MESSAGE="write+read during registration" \
		assert_equals "data $i" "$(cat "${info[mount0]}/during_registration_$i")"
done

# unlink works, both for mock-backed and fresh files (-f: the seeded mock
# files are write-protected for the test user, and a bare rm on an
# interactive TTY would prompt and hang the test)
assert_success rm -f "${info[mount0]}/mock_0000001"
assert_success rm -f "${info[mount0]}/during_registration_3"

# All of the IO above must have run while registration was still in progress,
# otherwise it exercised a converged cluster and this test measured nothing.
# Unlinking only ever lowers the count, so comparing against the full total
# is safe here.
copies_at_probe=$(count_chunk_copies)
MESSAGE="IO must be served while registration is still in progress" \
	assert_less_than "$copies_at_probe" "$((2 * CHUNK_COUNT))"

echo "PHASE: IO checks done at $copies_at_probe / $((2 * CHUNK_COUNT)) copies," \
	"waiting for registration to converge"
# Registration converges: all copies of the remaining mock chunks plus the
# two remaining new files' chunks are eventually known (>=: the copies of the
# unlinked chunks disappear later, via the regular deletion machinery)
remaining_mock_copies=$((2 * CHUNK_COUNT - CHUNKS_PER_FILE))
copies_at_least() {
	local copies
	copies=$(count_chunk_copies)
	[[ -n "$copies" && "$copies" -ge $((remaining_mock_copies + 2)) ]]
}
assert_eventually 'copies_at_least'

# And the data is still consistent afterwards
assert_equals "data 1" "$(cat "${info[mount0]}/during_registration_1")"
assert_equals "$expected_head" \
	"$(dd if="${info[mount0]}/mock_0000002" bs=16 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')"
