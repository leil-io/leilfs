timeout_set '1 minute'

# Exercise reads, writes and unlink during paced re-registration.
# Restart only chunkserver 1, which owns the sole copies used by read probes.
# Chunkserver 0 stays online to provide capacity for new writes.

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

# Seed goal-1 metadata for disjoint chunkserver inventories.
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

# Restart chunkserver 1 to open the paced registration window.
saunafs_chunkserver_daemon 1 restart

# Require partial inventory from chunkserver 1, showing its scan has started
# populating the registry. Advertised capacity alone cannot establish this.
registration_in_progress() {
	local copies
	copies=$(count_chunk_copies)
	[[ -n "$copies" && "$copies" -gt "$CHUNK_COUNT" && "$copies" -lt $((2 * CHUNK_COUNT)) ]]
}
assert_eventually 'registration_in_progress'
echo "PHASE: chunkserver 1 restarted, registration window open" \
	"($(count_chunk_copies) / $((2 * CHUNK_COUNT)) copies known)"

# Read uncached files from chunkserver 1's range so the online server cannot
# satisfy the probes with its own copies.
total_files=$((2 * CHUNK_COUNT / CHUNKS_PER_FILE))
first_probe_file=$((CHUNK_COUNT / CHUNKS_PER_FILE))
last_probe_file=$((total_files - 1))
expected_head="5a5b58595e5f5c5d5253505156575455"
for fraction_idx in 0 1 2 3; do
	file_idx=$((first_probe_file + fraction_idx * (last_probe_file - first_probe_file) / 3))
	file="${info[mount0]}/$(printf 'mock_%07d' "$file_idx")"
	head_hex=$(dd if="$file" bs=16 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')
	MESSAGE="read of $file during registration" \
		assert_equals "$expected_head" "$head_hex"
done

# New writes succeed and read back correctly while chunkserver 1 registers.
for i in 1 2 3; do
	echo "data $i" > "${info[mount0]}/during_registration_$i"
	MESSAGE="write+read during registration" \
		assert_equals "data $i" "$(cat "${info[mount0]}/during_registration_$i")"
done

# Delete one mock file and one new file; -f avoids prompts for protected files.
assert_success rm -f "${info[mount0]}/mock_0000001"
assert_success rm -f "${info[mount0]}/during_registration_3"

# A deleted mock file removes CHUNKS_PER_FILE copies; two new chunks remain.
# Adjust the completed total before checking that registration is unfinished.
remaining_mock_copies=$((2 * CHUNK_COUNT - CHUNKS_PER_FILE))
expected_remaining_copies=$((remaining_mock_copies + 2))
copies_at_probe=$(count_chunk_copies)
MESSAGE="IO must be served while registration is still in progress" \
	assert_less_than "$copies_at_probe" "$expected_remaining_copies"

echo "PHASE: IO checks done at $copies_at_probe / $expected_remaining_copies copies," \
	"waiting for registration to converge"
# Wait for the remaining inventory. Deleted copies can linger until background
# cleanup, so allow counts above the adjusted total.
copies_at_least() {
	local copies
	copies=$(count_chunk_copies)
	[[ -n "$copies" && "$copies" -ge "$expected_remaining_copies" ]]
}
assert_eventually 'copies_at_least'

# Verify data after registration converges.
assert_equals "data 1" "$(cat "${info[mount0]}/during_registration_1")"
assert_equals "$expected_head" \
	"$(dd if="${info[mount0]}/mock_0000002" bs=16 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')"
