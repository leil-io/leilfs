timeout_set '5 minutes'

# Verifies the mock disk plugin (src/chunkserver/plugins/mockdisk):
# - a mock hdd.cfg line synthesizes chunks without creating chunk files,
# - the master learns all of them and they match seeded metadata,
# - reads of mock-backed files succeed end-to-end (CRC-valid pattern data),
# - the mock disk is never selected for new chunks.

CHUNK_COUNT=100000

CHUNKSERVERS=1 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

# Seed metadata offline with CHUNK_COUNT chunks: ids 1..CHUNK_COUNT, version 1,
# goal 1 -- exactly the chunks the mock disk will register.
saunafs_master_daemon stop
metadata_version=$(metadata_get_version "${info[master_data_path]}/metadata.sfs")
generate_mock_chunks_changelog "$CHUNK_COUNT" 10000 "$metadata_version" \
	> "${info[master_data_path]}/changelog.sfs"
assert_success sfsmetarestore -a -d "${info[master_data_path]}"
saunafs_master_daemon start
saunafs_wait_for_all_ready_chunkservers

# Attach the mock disk with the matching chunks to chunkserver 0
mock_dir="$RAMDISK_DIR/mock_disk_0"
mkdir -p "$mock_dir"
echo "mock:${CHUNK_COUNT}:1:${mock_dir}" >> "${info[chunkserver0_hdd]}"
saunafs_chunkserver_daemon 0 reload

# The master should eventually know all the fake chunk copies
# (list-chunkservers humanizes counts -- "100k" -- so use 'info' instead)
count_chunk_copies() {
	saunafs-admin info localhost "${info[matocl]}" 2>/dev/null \
		| awk '/^Chunk copies:/ {print $NF}'
}
assert_eventually_equals "echo $CHUNK_COUNT" 'count_chunk_copies' '3 minutes'

# No chunk data files were created for the fake chunks
assert_equals 0 "$(find "$mock_dir" -name "chunk_*" | wc -l)"

# Read mock-backed data through the mount: every 64KiB block contains the
# static pattern byte[i] = (i & 0xFF) ^ 0x5A
expected_head="5a5b58595e5f5c5d5253505156575455"
read_head() {
	dd if="$1" bs=16 count=1 skip="${2:-0}" iflag=skip_bytes 2>/dev/null \
		| od -An -tx1 | tr -d ' \n'
}
first_file="${info[mount0]}/mock_0000000"
assert_eventually_equals "echo $expected_head" 'read_head "$first_file"' '2 minutes'
# A block in the middle of another chunk of the same file (chunk index 3)
assert_equals "$expected_head" "$(read_head "$first_file" $((3 * 67108864 + 655360)))"
# A file whose chunks live further into the id space
last_file=$(ls "${info[mount0]}" | grep mock_ | tail -1)
assert_equals "$expected_head" "$(read_head "${info[mount0]}/$last_file")"

# Writes must succeed and land on the real disk, never on the mock disk
echo "canary data" > "${info[mount0]}/canary"
assert_equals "canary data" "$(cat "${info[mount0]}/canary")"
real_disk=$(head -1 "${info[chunkserver0_hdd]}")
assert_equals 1 "$(find "$real_disk" -name "chunk_*.dat" | wc -l)"
assert_equals 0 "$(find "$mock_dir" -name "chunk_*" | wc -l)"
