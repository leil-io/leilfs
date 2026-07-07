timeout_set '15 minutes'

# Client IO must keep working while chunkservers are (re-)registering their
# chunks: reads of not-yet-registered chunks are resolved on demand (M1),
# writes create or update chunks normally, unlink works. Exercises the window
# by restarting both chunkservers with re-registration throttled to one chunk
# per packet.

CHUNK_COUNT=${CHUNK_COUNT:-200000}
CHUNKS_PER_FILE=1000

CHUNKSERVERS=2 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
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

count_chunk_copies() {
	saunafs-admin info localhost "${info[matocl]}" 2>/dev/null \
		| awk '/^Chunk copies:/ {print $NF}'
}
assert_eventually_equals "echo $((2 * CHUNK_COUNT))" 'count_chunk_copies' '5 minutes'
echo "PHASE: initial registration complete ($(count_chunk_copies) copies)"

# Throttle re-registration, then restart both chunkservers: the master drops
# all their copies and the slow re-registration window begins
for csid in 0 1; do
	echo "HDD_CHUNK_BULK_SIZE = 1" >> "${info[chunkserver${csid}_cfg]}"
	saunafs_chunkserver_daemon "$csid" restart
done
saunafs_wait_for_all_ready_chunkservers
echo "PHASE: chunkservers restarted, registration window open"

# IO during the registration window, spread across the id space (none of
# these files was read before, so no location is cached in the mount):
total_files=$((2 * CHUNK_COUNT / CHUNKS_PER_FILE))
expected_head="5a5b58595e5f5c5d5253505156575455"
for fraction_idx in 0 1 2 3; do
	file_idx=$((fraction_idx * (total_files - 1) / 3))
	file="${info[mount0]}/$(printf 'mock_%07d' "$file_idx")"
	# reads of mock-backed chunks succeed with the right content
	head_hex=$(dd if="$file" bs=16 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')
	MESSAGE="read of $file during registration" \
		assert_equals "$expected_head" "$head_hex"
done

# writes work (land on the real disks) and read back correctly
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

echo "PHASE: IO checks done, waiting for registration to converge" \
	"(can take ~1 min: with HDD_CHUNK_BULK_SIZE=1 chunks found by a scan" \
	"which finished after the reconnect drain one per packet)"
# Registration converges: all copies of the remaining mock chunks plus the
# two remaining new files' chunks are eventually known (>=: the copies of the
# unlinked chunks disappear later, via the regular deletion machinery)
remaining_mock_copies=$((2 * CHUNK_COUNT - CHUNKS_PER_FILE))
copies_at_least() {
	local copies
	copies=$(count_chunk_copies)
	[[ -n "$copies" && "$copies" -ge $((remaining_mock_copies + 2)) ]]
}
assert_eventually 'copies_at_least' '10 minutes'

# And the data is still consistent afterwards
assert_equals "data 1" "$(cat "${info[mount0]}/during_registration_1")"
assert_equals "$expected_head" \
	"$(dd if="${info[mount0]}/mock_0000002" bs=16 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')"
